#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"
#include "common/util/Assert.h"

#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/pipelines/metal/metal_renderer.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/runtime.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

struct goal_jak2_metal_host {
  MetalRenderer renderer;
  TexturePool textures{GameVersion::Jak2};
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
  host->metrics.submissions = stats.submissions;
  host->metrics.presentations = stats.presentations_completed;
  host->metrics.presentation_drops = stats.presentation_drops;
  host->metrics.presentation_order_mismatches = stats.presentation_order_mismatches;
  host->metrics.skipped_bucket_bytes = stats.skipped_bucket_bytes;
  host->metrics.unsupported_blends = stats.direct_unsupported_blends;
  host->metrics.last_command_buffer_status = stats.last_command_buffer_status;
  host->metrics.last_command_buffer_error_code = stats.last_command_buffer_error_code;
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
  try {
    host->options.host_tick_id = host->metrics.chains;
    host->options.chain_ordinal = host->metrics.chains;
    host->options.engine_frame_id = host->metrics.chains;
    if (!update_draw_region(host)) {
      record_failure(host, "Jak 2 CAMetalLayer has no finite drawable size");
      return;
    }
    const auto& copied = host->copier.run(ee_base, chain_offset, false);
    host->metrics.last_copied_bytes = static_cast<uint32_t>(copied.data.size());

    const bool acquired = host->renderer.render_chain_frame(
        host->options, host->layer, copied.data.data(), copied.start_offset);
    copy_renderer_metrics(host);
    if (host->metrics.last_buckets_dispatched != metal_renderer::kJak2MetalBucketCount) {
      record_failure(host, "Jak 2 Metal renderer violated its audited bucket policy");
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
        record_failure(host, "Jak 2 nil-layer renderer violated the submission-free dispatch gate");
        return;
      }
    } else if (!acquired || host->metrics.unsupported_blends != 0 ||
               !exact_presenting_commit_count ||
               host->metrics.drawables_acquired != host->metrics.chains ||
               host->metrics.drawable_misses != 0 ||
               host->metrics.submissions != host->metrics.chains ||
               host->metrics.late_present_submissions != 0 ||
               host->metrics.command_buffer_errors != 0) {
      record_failure(host, acquired ? "Jak 2 layer-backed submission counters violated their gate"
                                    : "Jak 2 CAMetalLayer did not provide a drawable");
      return;
    }
    host->metrics.completed_chains++;
  } catch (const std::exception& error) {
    record_failure(host, error.what());
  } catch (...) {
    record_failure(host, "Jak 2 Metal send-chain threw an unknown exception");
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
        descriptor.behavior != metal_renderer::Jak2MetalBucketBehavior::Direct) {
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
  host->renderer.init_bucket_renderers(&host->textures, GameVersion::Jak2);
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
