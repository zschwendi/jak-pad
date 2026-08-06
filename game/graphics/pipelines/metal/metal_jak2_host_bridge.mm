#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"
#include "common/util/Assert.h"

#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket4_mixed_executor.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_capture.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_plan.h"
#include "game/graphics/pipelines/metal/metal_jak2_sprite_texture_upload_plan.h"
#include "game/graphics/pipelines/metal/metal_kernel_bridge.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/pipelines/metal/metal_renderer.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/runtime.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

struct goal_jak2_metal_host {
  TexturePool textures{GameVersion::Jak2};
  MetalRenderer renderer;
  std::unique_ptr<metal_renderer::Jak2Bucket4MixedExecutor> bucket4_mixed_executor;
  FixedChunkDmaCopier copier{EE_MAIN_MEM_SIZE};
  goal_gfx_host callbacks = {};
  goal_jak2_metal_host_metrics metrics = {};
  MetalRenderOptions options;
  CAMetalLayer* layer = nil;
  std::string error;
  std::string fr3_directory;
  std::string fatal_chain_error;
  std::vector<std::string> requested_level_names;
  std::vector<std::string> loaded_level_keys;
  u64 placeholder_handle = 0;
  bool configuration_attempted = false;
  bool configured = false;
  bool callbacks_copied = false;
  bool inactive = false;
};

namespace {

goal_jak2_metal_host* g_active_host = nullptr;
std::mutex g_host_mutex;

constexpr int kJak2LevelSlotCount = 6;

goal_jak2_metal_host* active_host() {
  return g_active_host;
}

void record_failure(goal_jak2_metal_host* host, const char* message) {
  host->metrics.failed_chains++;
  host->error = message;
}

void fail_closed(goal_jak2_metal_host* host, const std::string& message) {
  if (host->fatal_chain_error.empty()) {
    host->fatal_chain_error = message;
  }
  host->error = message;
}

void fail_current_chain_closed(goal_jak2_metal_host* host, const std::string& message) {
  fail_closed(host, message);
  host->metrics.failed_chains++;
}

void record_send_chain_failure(goal_jak2_metal_host* host,
                               const std::string& message,
                               bool host_texture_mutated) {
  if (host_texture_mutated) {
    fail_current_chain_closed(host, message);
  } else {
    record_failure(host, message.c_str());
  }
}

std::string fr3_path(const goal_jak2_metal_host* host, const std::string& name) {
  const bool has_separator = !host->fr3_directory.empty() && host->fr3_directory.back() == '/';
  return host->fr3_directory + (has_separator ? "" : "/") + name + ".fr3";
}

bool is_level_basename(const char* name) {
  if (!name || !name[0] || std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
    return false;
  }
  for (const char* ch = name; *ch; ++ch) {
    if (*ch == '/' || *ch == '\\') {
      return false;
    }
  }
  return true;
}

bool already_requested(const goal_jak2_metal_host* host, const std::string& name) {
  return std::find(host->requested_level_names.begin(), host->requested_level_names.end(), name) !=
         host->requested_level_names.end();
}

void copy_renderer_metrics(goal_jak2_metal_host* host) {
  const auto stats = host->renderer.chain_stats();
  host->metrics.last_buckets_dispatched = stats.last_buckets_dispatched;
  host->metrics.command_buffers_committed = stats.command_buffers_committed;
  host->metrics.command_buffers_completed = stats.command_buffers_completed;
  host->metrics.command_buffer_errors = stats.command_buffer_errors;
  host->metrics.drawables_acquired = stats.drawables_acquired;
  host->metrics.drawable_misses = stats.drawable_misses;
  host->metrics.late_present_submissions = stats.late_present_submissions;
  host->metrics.draws = stats.draw_calls;
  host->metrics.triangles = stats.triangles;
  host->metrics.last_sky_draw_draws = stats.jak2_sky_draw_draws;
  host->metrics.last_sky_draw_triangles = stats.jak2_sky_draw_triangles;
  const auto& sky_batch = stats.jak2_sky_draw_last_batch;
  host->metrics.last_sky_draw_batch_valid = sky_batch.valid;
  host->metrics.last_sky_draw_batch_textured = sky_batch.textured;
  host->metrics.last_sky_draw_batch_vertices = sky_batch.vertices;
  host->metrics.last_sky_draw_batch_nonzero_rgb_vertices = sky_batch.nonzero_rgb_vertices;
  host->metrics.last_sky_draw_batch_tex0_tbp = sky_batch.tex0_tbp;
  host->metrics.last_sky_draw_batch_tex0_tcc = sky_batch.tex0_tcc;
  host->metrics.last_sky_draw_batch_tex0_decal = sky_batch.tex0_decal;
  host->metrics.last_sky_draw_batch_texture_lookup_hit = sky_batch.texture_lookup_hit;
  host->metrics.last_sky_draw_batch_used_placeholder = sky_batch.used_placeholder;
  host->metrics.last_sky_draw_batch_write_rgb = sky_batch.write_rgb;
  host->metrics.last_sky_draw_batch_blend_enabled = sky_batch.blend_enabled;
  host->metrics.last_sky_draw_batch_blend_a = sky_batch.blend_a;
  host->metrics.last_sky_draw_batch_blend_b = sky_batch.blend_b;
  host->metrics.last_sky_draw_batch_blend_c = sky_batch.blend_c;
  host->metrics.last_sky_draw_batch_blend_d = sky_batch.blend_d;
  host->metrics.last_sky_draw_batch_alpha_test_enabled = sky_batch.alpha_test_enabled;
  host->metrics.last_sky_draw_batch_alpha_test_mode = sky_batch.alpha_test_mode;
  host->metrics.last_sky_draw_batch_alpha_aref = sky_batch.alpha_aref;
  host->metrics.last_sky_draw_batch_alpha_afail = sky_batch.alpha_afail;
  host->metrics.last_screen_filter_draws = stats.jak2_screen_filter_draws;
  host->metrics.last_screen_filter_triangles = stats.jak2_screen_filter_triangles;
  host->metrics.last_debug_no_zbuf2_draws = stats.jak2_debug_no_zbuf2_draws;
  host->metrics.last_debug_no_zbuf2_triangles = stats.jak2_debug_no_zbuf2_triangles;
  host->metrics.last_sprites_2d = stats.sprites_2d;
  host->metrics.last_sprites_3d = stats.sprites_3d;
  host->metrics.last_sprites_hud = stats.sprites_hud;
  host->metrics.last_sprites_distort = stats.sprites_distort;
  host->metrics.last_sprite_normal_submitted = stats.sprite_normal_submitted;
  host->metrics.last_sprite_glow_marked = stats.sprite_glow_marked;
  host->metrics.last_sprite_glow_skipped = stats.sprite_glow_skipped;
  host->metrics.last_sprite_glow_parsed = stats.sprite_glow_parsed;
  host->metrics.last_sprite_glow_accepted = stats.sprite_glow_accepted;
  host->metrics.last_sprite_glow_rejected = stats.sprite_glow_rejected;
  host->metrics.last_sprite_glow_invalid_records = stats.sprite_glow_invalid_records;
  host->metrics.last_sprite_glow_force_visible_submitted =
      stats.sprite_glow_force_visible_submitted;
  host->metrics.last_sprite_glow_force_visible_drawn = stats.sprite_glow_force_visible_drawn;
  host->metrics.last_sprite_glow_force_visible_draws = stats.sprite_glow_force_visible_draws;
  host->metrics.last_sprite_glow_force_visible_triangles =
      stats.sprite_glow_force_visible_triangles;
  host->metrics.last_sprite_glow_force_visible_missing_textures =
      stats.sprite_glow_force_visible_missing_textures;
  host->metrics.last_sprite_draws = stats.sprite_draws;
  host->metrics.last_sprite_triangles = stats.sprite_triangles;
  host->metrics.last_sprite_missing_textures = stats.sprite_missing_textures;
  host->metrics.last_sprite_unsupported_bytes = stats.sprite_unsupported_bytes;
  host->metrics.submissions = stats.submissions;
  host->metrics.presentations = stats.presentations_completed;
  host->metrics.presentation_drops = stats.presentation_drops;
  host->metrics.presentation_order_mismatches = stats.presentation_order_mismatches;
  host->metrics.skipped_bucket_bytes = stats.skipped_bucket_bytes;
  host->metrics.unsupported_blends = stats.direct_unsupported_blends;
  host->metrics.last_command_buffer_status = stats.last_command_buffer_status;
  host->metrics.last_command_buffer_error_code = stats.last_command_buffer_error_code;
}

void copy_bucket4_texture_upload_metrics(
    goal_jak2_metal_host* host,
    const metal_renderer::Jak2Bucket4TextureUploadCapture& capture) {
  auto& out = host->metrics.last_bucket4_texture_upload;
  out = {};
  out.valid = capture.valid;
  out.present = capture.present;
  out.total_payload_bytes = capture.total_payload_bytes;
  out.dma_transfers = capture.dma_transfers;
  out.payload_transfers = capture.payload_transfers;
  out.inert_transfers = capture.inert_transfers;
  out.inert_cnt_transfers = capture.inert_cnt_transfers;
  out.inert_next_transfers = capture.inert_next_transfers;
  out.inert_state_mask = capture.inert_state_mask;
  out.ordinary_descriptors = capture.ordinary_descriptors;
  out.ordinary_page = capture.ordinary_page;
  out.ordinary_mode = capture.ordinary_mode;
  out.animator_arrays = capture.animator_arrays;
  out.animator_bytes = capture.animator_bytes;
  std::copy(capture.opcode_counts.begin(), capture.opcode_counts.end(), out.opcode_counts);
  out.cloud_destination = capture.cloud_destination;
  out.erase_width = capture.erase_width;
  out.erase_height = capture.erase_height;
  out.erase_destination = capture.erase_destination;
  out.erase_test = capture.erase_test;
  out.erase_alpha = capture.erase_alpha;
  out.erase_clamp = capture.erase_clamp;
  std::copy(capture.erase_clear.begin(), capture.erase_clear.end(), out.erase_clear);
  out.generic_source = capture.generic_source;
  out.generic_width = capture.generic_width;
  out.generic_height = capture.generic_height;
  out.generic_destination = capture.generic_destination;
  out.generic_format = capture.generic_format;
  out.generic_force_to_gpu = capture.generic_force_to_gpu;
  out.clut_source = capture.clut_source;
  out.clut_destination = capture.clut_destination;
  out.finishes = capture.finishes;
  out.malformed_transfers = capture.malformed_transfers;
  out.malformed_bytes = capture.malformed_bytes;
  out.unsupported_transfers = capture.unsupported_transfers;
  out.unsupported_bytes = capture.unsupported_bytes;
}

void copy_sprite_texture_upload_metrics(
    goal_jak2_metal_host* host,
    const std::optional<metal_renderer::Jak2SpriteTextureUploadPlan>& plan) {
  static_assert(GOAL_JAK2_SPRITE_TEXTURE_UPLOAD_MAX_GROUPS ==
                metal_renderer::kJak2SpriteTextureUploadMaximumGroups);
  auto& out = host->metrics.last_sprite_texture_upload;
  out = {};
  out.valid = plan.has_value();
  if (!plan) {
    return;
  }
  out.present = plan->present;
  const std::size_t upload_count =
      std::min(plan->upload_count, metal_renderer::kJak2SpriteTextureUploadMaximumGroups);
  out.upload_count = static_cast<uint32_t>(upload_count);
  for (std::size_t i = 0; i < upload_count; ++i) {
    out.pages[i] = plan->uploads[i].page_offset;
    out.modes[i] = plan->uploads[i].mode;
  }
}

bool execute_ordinary_texture_upload(
    goal_jak2_metal_host* host,
    const metal_renderer::Jak2Bucket4OrdinaryUploadPlan& ordinary,
    const u8* live_ee_memory,
    uint64_t* execution_count,
    const char* label) {
  if (ordinary.page_offset > EE_MAIN_MEM_SIZE - ordinary.page_header.size() ||
      ordinary.mode != -1 ||
      std::memcmp(live_ee_memory + ordinary.page_offset, ordinary.page_header.data(),
                  ordinary.page_header.size()) != 0) {
    fail_current_chain_closed(host, std::string(label) + " changed after planning");
    return false;
  }
  try {
    host->textures.handle_upload_now(live_ee_memory + ordinary.page_offset,
                                     static_cast<int>(ordinary.mode), g_ee_main_mem,
                                     metal_offset_of_s7(), false);
    (*execution_count)++;
    return true;
  } catch (const std::exception& error) {
    fail_current_chain_closed(host, error.what());
  } catch (...) {
    fail_current_chain_closed(host, std::string(label) + " threw");
  }
  return false;
}

bool execute_bucket4_plan(goal_jak2_metal_host* host,
                          const metal_renderer::Jak2Bucket4TextureUploadPlan& plan,
                          const u8* live_ee_memory) {
  if (std::holds_alternative<metal_renderer::Jak2Bucket4AbsentPlan>(plan)) {
    return true;
  }
  if (const auto* ordinary_only =
          std::get_if<metal_renderer::Jak2Bucket4OrdinaryOnlyPlan>(&plan)) {
    return execute_ordinary_texture_upload(
        host, ordinary_only->ordinary, live_ee_memory, &host->metrics.bucket4_ordinary_uploads,
        "Jak 2 bucket 4 ordinary texture upload");
  }

  const auto& mixed = std::get<metal_renderer::Jak2Bucket4MixedPlan>(plan);
  if (!execute_ordinary_texture_upload(
          host, mixed.ordinary, live_ee_memory, &host->metrics.bucket4_ordinary_uploads,
          "Jak 2 bucket 4 ordinary texture upload")) {
    return false;
  }
  if (!host->bucket4_mixed_executor || !host->bucket4_mixed_executor->execute(mixed)) {
    const char* detail = host->bucket4_mixed_executor
                             ? host->bucket4_mixed_executor->last_error()
                             : "executor is unavailable";
    fail_current_chain_closed(host,
                              std::string("Jak 2 bucket 4 mixed texture execution failed: ") +
                                  detail);
    return false;
  }

  const auto& stats = host->bucket4_mixed_executor->stats();
  host->metrics.bucket4_mixed_executions = stats.frames;
  host->metrics.bucket4_cloud_publications = stats.cloud_publications;
  host->metrics.bucket4_fog_publications = stats.fog_publications;
  host->metrics.bucket4_cloud_texture =
      host->textures.lookup(static_cast<u32>(mixed.sky.cloud_destination)).value_or(0);
  host->metrics.bucket4_fog_texture = host->textures.lookup(mixed.fog.destination).value_or(0);
  return true;
}

bool execute_sprite_texture_upload_plan(
    goal_jak2_metal_host* host,
    const metal_renderer::Jak2SpriteTextureUploadPlan& plan,
    const u8* live_ee_memory) {
  if (plan.present != (plan.upload_count > 0) ||
      plan.upload_count > metal_renderer::kJak2SpriteTextureUploadMaximumGroups) {
    fail_current_chain_closed(host, "Jak 2 bucket 312 texture-upload plan is inconsistent");
    return false;
  }
  for (std::size_t i = 0; i < plan.upload_count; ++i) {
    if (!execute_ordinary_texture_upload(
            host, plan.uploads[i], live_ee_memory, &host->metrics.sprite_texture_uploads,
            "Jak 2 bucket 312 ordinary texture upload")) {
      return false;
    }
  }
  return true;
}

bool update_draw_region(goal_jak2_metal_host* host) {
  if (!host->layer) {
    return true;
  }
  const CGSize size = host->layer.drawableSize;
  if (!std::isfinite(size.width) || !std::isfinite(size.height) || size.width < 1.0 ||
      size.height < 1.0) {
    return false;
  }
  const int width = static_cast<int>(size.width);
  const int height = static_cast<int>(size.height);
  if (width * 3 >= height * 4) {
    host->options.draw_region_h = height;
    host->options.draw_region_w = height * 4 / 3;
  } else {
    host->options.draw_region_w = width;
    host->options.draw_region_h = width * 3 / 4;
  }
  return host->options.draw_region_w > 0 && host->options.draw_region_h > 0;
}

void send_chain(const void* ee_base, uint32_t chain_offset) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  auto* host = active_host();
  if (!host) {
    return;
  }
  ASSERT(ee_base == g_ee_main_mem);
  ASSERT(chain_offset != 0);

  host->metrics.chains++;
  if (!host->fatal_chain_error.empty()) {
    record_failure(host, host->fatal_chain_error.c_str());
    return;
  }
  bool host_texture_mutated = false;
  try {
    host->options.host_tick_id = host->metrics.chains;
    host->options.chain_ordinal = host->metrics.chains;
    host->options.engine_frame_id = host->metrics.chains;
    if (!update_draw_region(host)) {
      record_failure(host, "Jak 2 CAMetalLayer has no finite drawable size");
      return;
    }
    metal_renderer::Jak2Bucket4TextureUploadCapture bucket4_capture;
    const auto bucket4_plan = metal_renderer::plan_jak2_bucket4_texture_upload(
        static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, chain_offset,
        static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, &bucket4_capture);
    copy_bucket4_texture_upload_metrics(host, bucket4_capture);
    if (!bucket4_plan) {
      record_failure(host, "Jak 2 bucket 4 texture-upload capture rejected malformed DMA");
      return;
    }
    const auto sprite_texture_plan = metal_renderer::plan_jak2_sprite_texture_upload(
        static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, chain_offset,
        static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE);
    copy_sprite_texture_upload_metrics(host, sprite_texture_plan);
    if (!sprite_texture_plan) {
      record_failure(host, "Jak 2 bucket 312 texture-upload plan rejected malformed DMA");
      return;
    }
    host_texture_mutated =
        !std::holds_alternative<metal_renderer::Jak2Bucket4AbsentPlan>(*bucket4_plan) ||
        sprite_texture_plan->present;
    if (!execute_bucket4_plan(host, *bucket4_plan, static_cast<const u8*>(ee_base))) {
      return;
    }
    if (!execute_sprite_texture_upload_plan(host, *sprite_texture_plan,
                                            static_cast<const u8*>(ee_base))) {
      return;
    }
    const auto& copied = host->copier.run(ee_base, chain_offset, false);
    host->metrics.last_copied_bytes = static_cast<uint32_t>(copied.data.size());

    const bool acquired = host->renderer.render_chain_frame(
        host->options, host->layer, copied.data.data(), copied.start_offset);
    copy_renderer_metrics(host);
    if (host->metrics.last_buckets_dispatched != metal_renderer::kJak2MetalBucketCount) {
      record_send_chain_failure(host, "Jak 2 Metal renderer violated its audited bucket policy",
                                host_texture_mutated);
      return;
    }
    const bool exact_presenting_commit_count =
        host->metrics.command_buffers_committed == host->metrics.chains;
    if (!host->layer) {
      if (acquired || host->metrics.command_buffers_committed != 0 ||
          host->metrics.command_buffers_completed != 0 || host->metrics.command_buffer_errors != 0 ||
          host->metrics.drawables_acquired != 0 || host->metrics.drawable_misses != 0 ||
          host->metrics.submissions != 0 || host->metrics.presentations != 0 ||
          host->metrics.presentation_drops != 0 ||
          host->metrics.presentation_order_mismatches != 0) {
        record_send_chain_failure(
            host, "Jak 2 nil-layer renderer violated the submission-free dispatch gate",
            host_texture_mutated);
        return;
      }
    } else if (!acquired || host->metrics.unsupported_blends != 0 ||
               !exact_presenting_commit_count ||
               host->metrics.drawables_acquired != host->metrics.chains ||
               host->metrics.drawable_misses != 0 ||
               host->metrics.submissions != host->metrics.chains ||
               host->metrics.late_present_submissions != 0 ||
               host->metrics.command_buffer_errors != 0) {
      record_send_chain_failure(
          host, acquired ? "Jak 2 layer-backed submission counters violated their gate"
                         : "Jak 2 CAMetalLayer did not provide a drawable",
          host_texture_mutated);
      return;
    }
    host->metrics.completed_chains++;
  } catch (const std::exception& error) {
    record_send_chain_failure(host, error.what(), host_texture_mutated);
  } catch (...) {
    record_send_chain_failure(host, "Jak 2 Metal send-chain threw an unknown exception",
                              host_texture_mutated);
  }
}

uint32_t vsync() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  auto* host = active_host();
  if (!host) {
    return 0;
  }
  host->metrics.vsyncs++;
  return static_cast<uint32_t>(host->metrics.vsyncs & 1);
}

uint32_t sync_path() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  auto* host = active_host();
  if (host) {
    host->metrics.sync_paths++;
  }
  return 0;
}

void texture_upload(const uint8_t* tpage, int mode, uint32_t s7_ptr) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  auto* host = active_host();
  if (!host) {
    return;
  }
  host->metrics.texture_uploads++;
  if (!tpage || !g_ee_main_mem) {
    fail_closed(host, "Jak 2 texture upload did not provide EE memory");
    return;
  }
  try {
    host->textures.handle_upload_now(tpage, mode, g_ee_main_mem, s7_ptr, false);
  } catch (const std::exception& error) {
    fail_closed(host, error.what());
  } catch (...) {
    fail_closed(host, "Jak 2 texture upload threw an unknown exception");
  }
}

void texture_relocate(uint32_t dst, uint32_t src, uint32_t format) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  auto* host = active_host();
  if (!host) {
    return;
  }
  host->metrics.texture_relocations++;
  try {
    host->textures.relocate(dst, src, format);
  } catch (const std::exception& error) {
    fail_closed(host, error.what());
  } catch (...) {
    fail_closed(host, "Jak 2 texture relocate threw an unknown exception");
  }
}

void set_levels(const char* const* names, int count) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  auto* host = active_host();
  if (!host || host->fr3_directory.empty() || !host->fatal_chain_error.empty()) {
    return;
  }
  if (count < 0 || count > kJak2LevelSlotCount || (count && !names)) {
    fail_closed(host, "Jak 2 set-levels supplied an invalid level list");
    return;
  }

  for (int i = 0; i < count; ++i) {
    if (!is_level_basename(names[i])) {
      fail_closed(host, "Jak 2 set-levels rejected a non-basename level name");
      return;
    }
    const std::string name = names[i];
    if (already_requested(host, name)) {
      continue;
    }
    host->requested_level_names.push_back(name);
    try {
      std::string error;
      auto* level = metal_level_data::load_fr3(host->renderer.device(), host->renderer.queue(),
                                               host->textures, fr3_path(host, name), false, &error);
      if (!level) {
        fail_closed(host, "Jak 2 level art load failed for " + name + ": " + error);
        return;
      }
      host->loaded_level_keys.push_back(level->level->level_name);
    } catch (const std::exception& error) {
      fail_closed(host, "Jak 2 level art load failed for " + name + ": " + error.what());
      return;
    } catch (...) {
      fail_closed(host, "Jak 2 level art load failed for " + name + ": unknown exception");
      return;
    }
  }
}
void set_active_levels(const char* const*, int) {}

void set_pmode_alpha(float alpha) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  if (auto* host = active_host()) {
    host->options.pmode_alp = alpha;
  }
}

bool policy_table_is_audited() {
  const auto& table = metal_renderer::jak2_metal_bucket_table();
  if (table.size() != metal_renderer::kJak2MetalBucketCount ||
      metal_renderer::jak2_metal_bucket_table_fingerprint() !=
          metal_renderer::kJak2MetalBucketExpectedFingerprint) {
    return false;
  }
  for (const auto& descriptor : table) {
    if (descriptor.behavior != metal_renderer::Jak2MetalBucketBehavior::DeferredSkip &&
        descriptor.behavior != metal_renderer::Jak2MetalBucketBehavior::StrictEmpty &&
        descriptor.behavior != metal_renderer::Jak2MetalBucketBehavior::Direct &&
        descriptor.behavior != metal_renderer::Jak2MetalBucketBehavior::HostTextureUpload &&
        descriptor.behavior != metal_renderer::Jak2MetalBucketBehavior::Visibility &&
        descriptor.behavior != metal_renderer::Jak2MetalBucketBehavior::Sprite &&
        descriptor.behavior != metal_renderer::Jak2MetalBucketBehavior::TFragment) {
      return false;
    }
  }
  return true;
}

goal_jak2_metal_host* create_host(CAMetalLayer* layer, bool presenting) {
  if (g_active_host || !policy_table_is_audited() || (presenting && !layer)) {
    return nullptr;
  }
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) {
    return nullptr;
  }

  if (presenting) {
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.opaque = YES;
    layer.presentsWithTransaction = NO;
    layer.allowsNextDrawableTimeout = YES;
  }

  auto* host = new goal_jak2_metal_host();
  host->layer = presenting ? layer : nil;
  if (!host->renderer.init(device) || !update_draw_region(host)) {
    delete host;
    return nullptr;
  }
  if (!metal_setup_placeholder(host->renderer.device(), host->renderer.queue(), host->textures)) {
    delete host;
    return nullptr;
  }
  host->placeholder_handle = host->textures.get_placeholder_texture();
  host->renderer.init_bucket_renderers(&host->textures, GameVersion::Jak2,
                                       /*host_texture_uploads=*/true);
  host->bucket4_mixed_executor = std::make_unique<metal_renderer::Jak2Bucket4MixedExecutor>(
      host->renderer.device(), host->renderer.queue(), &host->textures);
  host->callbacks.send_chain = send_chain;
  host->callbacks.vsync = vsync;
  host->callbacks.sync_path = sync_path;
  host->callbacks.texture_upload_now = texture_upload;
  host->callbacks.texture_relocate = texture_relocate;
  host->callbacks.set_levels = set_levels;
  host->callbacks.set_pmode_alp = set_pmode_alpha;
  host->callbacks.set_active_levels = set_active_levels;
  g_active_host = host;
  return host;
}

}  // namespace

extern "C" {

goal_jak2_metal_host* goal_jak2_metal_host_create(void) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  return create_host(nil, false);
}

goal_jak2_metal_host* goal_jak2_metal_host_create_presenting(
    goal_jak2_metal_host_layer layer) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  return create_host(layer, true);
}

int goal_jak2_metal_host_configure_level_art(goal_jak2_metal_host* host,
                                             const char* fr3_directory) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  if (!host || host != g_active_host || host->inactive || !fr3_directory) {
    return 0;
  }
  const std::string directory = fr3_directory;
  if (host->configuration_attempted) {
    if (host->configured && host->fr3_directory == directory) {
      return 1;
    }
    host->error = host->configured ? "Jak 2 level art is already configured for another directory"
                                   : "Jak 2 level art configuration already failed";
    return 0;
  }
  if (host->callbacks_copied) {
    fail_closed(host, "Jak 2 level art must be configured before graphics callbacks are copied");
    return 0;
  }

  host->configuration_attempted = true;
  host->fr3_directory = directory;
  if (directory.empty()) {
    host->error = "Jak 2 level art directory is empty";
    return 0;
  }

  try {
    std::string error;
    auto* common = metal_level_data::load_fr3(
        host->renderer.device(), host->renderer.queue(), host->textures, fr3_path(host, "GAME"),
        true, &error);
    if (!common) {
      host->error = "Jak 2 common level art load failed: " + error;
      return 0;
    }
    host->loaded_level_keys.push_back(common->level->level_name);
    host->configured = true;
    host->error.clear();
    return 1;
  } catch (const std::exception& error) {
    host->error = std::string("Jak 2 common level art load failed: ") + error.what();
  } catch (...) {
    host->error = "Jak 2 common level art load failed: unknown exception";
  }
  return 0;
}

int goal_jak2_metal_host_copy_gfx_host(goal_jak2_metal_host* host, goal_gfx_host* out) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  if (!host || host != g_active_host || host->inactive || !out ||
      (host->configuration_attempted && !host->configured)) {
    return 0;
  }
  host->callbacks_copied = true;
  *out = host->callbacks;
  return 1;
}

int goal_jak2_metal_host_get_metrics(goal_jak2_metal_host* host,
                                     goal_jak2_metal_host_metrics* out) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  if (!host || host != g_active_host || host->inactive || !out) {
    return 0;
  }
  copy_renderer_metrics(host);
  *out = host->metrics;
  return 1;
}

int goal_jak2_metal_host_metrics_pass_frame_gate(const goal_jak2_metal_host_metrics* metrics,
                                                 int require_presentation) {
  if (!metrics || metrics->chains == 0) {
    return 0;
  }
  const bool presentation_exact =
      metrics->presentations == metrics->submissions && metrics->presentation_drops == 0 &&
      metrics->presentation_order_mismatches == 0;
  return metrics->completed_chains == metrics->chains && metrics->failed_chains == 0 &&
         metrics->last_buckets_dispatched == metal_renderer::kJak2MetalBucketCount &&
         metrics->command_buffers_committed == metrics->chains &&
         metrics->command_buffers_completed == metrics->chains &&
         metrics->command_buffer_errors == 0 && metrics->drawables_acquired == metrics->chains &&
         metrics->drawable_misses == 0 && metrics->submissions == metrics->chains &&
         metrics->late_present_submissions == 0 && metrics->unsupported_blends == 0 &&
         (!require_presentation || presentation_exact);
}

int goal_jak2_metal_host_read_last_frame(goal_jak2_metal_host* host,
                                         goal_jak2_metal_frame_summary* out) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  if (!out) {
    return 0;
  }
  *out = {};
  if (!host || host != g_active_host || !host->layer) {
    return 0;
  }

  try {
    metal_renderer::FramePixels frame;
    if (!host->renderer.read_game_frame(&frame) || frame.width <= 0 || frame.height <= 0) {
      return 0;
    }

    const auto width = static_cast<std::size_t>(frame.width);
    const auto height = static_cast<std::size_t>(frame.height);
    if (height > std::numeric_limits<std::size_t>::max() / width ||
        width * height > std::numeric_limits<std::size_t>::max() / 4) {
      return 0;
    }
    const std::size_t expected_bytes = width * height * 4;
    if (frame.rgba.size() != expected_bytes) {
      return 0;
    }

    constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
    constexpr uint64_t kFnvPrime = 1099511628211ull;
    uint64_t hash = kFnvOffsetBasis;
    uint64_t non_black_pixels = 0;
    uint64_t nonzero_alpha_pixels = 0;
    uint32_t max_alpha = 0;
    for (std::size_t offset = 0; offset < frame.rgba.size(); offset += 4) {
      hash ^= frame.rgba[offset];
      hash *= kFnvPrime;
      hash ^= frame.rgba[offset + 1];
      hash *= kFnvPrime;
      hash ^= frame.rgba[offset + 2];
      hash *= kFnvPrime;
      hash ^= frame.rgba[offset + 3];
      hash *= kFnvPrime;
      non_black_pixels +=
          frame.rgba[offset] != 0 || frame.rgba[offset + 1] != 0 || frame.rgba[offset + 2] != 0;
      nonzero_alpha_pixels += frame.rgba[offset + 3] != 0;
      max_alpha = std::max(max_alpha, static_cast<uint32_t>(frame.rgba[offset + 3]));
    }

    out->width = static_cast<uint32_t>(frame.width);
    out->height = static_cast<uint32_t>(frame.height);
    out->byte_count = frame.rgba.size();
    out->hash = hash;
    out->non_black_pixels = non_black_pixels;
    out->nonzero_alpha_pixels = nonzero_alpha_pixels;
    out->max_alpha = max_alpha;
    return 1;
  } catch (...) {
    *out = {};
    return 0;
  }
}

int goal_jak2_metal_host_wait_for_last_frame(goal_jak2_metal_host* host,
                                             double timeout_seconds,
                                             int require_presentation) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  if (!host || host != g_active_host || host->inactive || !host->layer || timeout_seconds <= 0.0 ||
      host->metrics.chains == 0) {
    return 0;
  }
  if (!host->renderer.wait_for_last_chain_frame(timeout_seconds)) {
    copy_renderer_metrics(host);
    record_failure(host, "Jak 2 Metal command buffers did not complete successfully in time");
    return 0;
  }
  if (require_presentation && !host->renderer.wait_for_last_presentation(timeout_seconds)) {
    copy_renderer_metrics(host);
    record_failure(host, "Jak 2 Metal drawable presentation did not complete successfully in time");
    return 0;
  }
  copy_renderer_metrics(host);
  if (!goal_jak2_metal_host_metrics_pass_frame_gate(&host->metrics, require_presentation)) {
    record_failure(host, "Jak 2 completed Metal frame counters violated their exact gate");
    return 0;
  }
  return 1;
}

void goal_jak2_metal_host_destroy(goal_jak2_metal_host* host) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  if (!host || host != g_active_host || host->inactive) {
    return;
  }
  host->inactive = true;
  g_active_host = nullptr;
  for (auto key = host->loaded_level_keys.rbegin(); key != host->loaded_level_keys.rend(); ++key) {
    metal_level_data::unload(host->textures, *key);
  }
  host->loaded_level_keys.clear();
  if (host->bucket4_mixed_executor) {
    host->bucket4_mixed_executor->detach_pool();
  }
  if (host->placeholder_handle) {
    metal_texture_release(host->placeholder_handle);
    host->textures.set_placeholder(0);
    host->placeholder_handle = 0;
  }
  delete host;
}

const char* goal_jak2_metal_host_last_error(goal_jak2_metal_host* host) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  return host && host == g_active_host && !host->inactive ? host->error.c_str()
                                                         : "Jak 2 Metal host is not active";
}

}  // extern "C"
