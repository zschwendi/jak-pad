#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"

#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket4_mixed_executor.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_capture.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_plan.h"
#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"
#include "game/graphics/pipelines/metal/metal_jak2_opcode27_skull_gem_executor.h"
#include "game/graphics/pipelines/metal/metal_jak2_raw_image_upload_executor.h"
#include "game/graphics/pipelines/metal/metal_jak2_raw_image_upload_plan.h"
#include "game/graphics/pipelines/metal/metal_jak2_chain_validation.h"
#include "game/graphics/pipelines/metal/metal_jak2_sprite_texture_upload_plan.h"
#include "game/graphics/pipelines/metal/metal_kernel_bridge.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/pipelines/metal/metal_merc_model_pool.h"
#include "game/graphics/pipelines/metal/metal_renderer.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/runtime.h"

#include "fmt/format.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

struct goal_jak2_metal_host {
  TexturePool textures{GameVersion::Jak2};
  MetalRenderer renderer;
  std::unique_ptr<metal_renderer::Jak2Bucket4MixedExecutor> bucket4_mixed_executor;
  std::unique_ptr<metal_renderer::Jak2Opcode27SkullGemExecutor> skull_gem_executor;
  std::unique_ptr<metal_renderer::Jak2RawImageUploadExecutor> raw_image_upload_executor;
  FixedChunkDmaCopier copier{EE_MAIN_MEM_SIZE};
  goal_gfx_host callbacks = {};
  goal_jak2_metal_host_metrics metrics = {};
  MetalRenderOptions options;
  CAMetalLayer* layer = nil;
  std::string error;
  std::string fr3_directory;
  std::string fatal_chain_error;
  MetalLevelData* common_level = nullptr;
  std::vector<std::string> requested_level_names;
  std::vector<std::string> loaded_level_keys;
  u64 placeholder_handle = 0;
  bool configuration_attempted = false;
  bool configured = false;
  bool callbacks_copied = false;
  bool inactive = false;
};

namespace metal_renderer {

static_assert(GOAL_JAK2_TRACKED_DEFERRED_BUCKET_COUNT == kTrackedDeferredBuckets);

bool jak2_metal_host_policy_table_is_audited() {
  const auto& table = jak2_metal_bucket_table();
  if (table.size() != kJak2MetalBucketCount ||
      jak2_metal_bucket_table_fingerprint() != kJak2MetalBucketExpectedFingerprint) {
    return false;
  }
  for (const auto& descriptor : table) {
    if (descriptor.behavior != Jak2MetalBucketBehavior::DeferredSkip &&
        descriptor.behavior != Jak2MetalBucketBehavior::StrictEmpty &&
        descriptor.behavior != Jak2MetalBucketBehavior::Direct &&
        descriptor.behavior != Jak2MetalBucketBehavior::HostTextureUpload &&
        descriptor.behavior != Jak2MetalBucketBehavior::HostTextureUploadDirect &&
        descriptor.behavior != Jak2MetalBucketBehavior::Visibility &&
        descriptor.behavior != Jak2MetalBucketBehavior::Sprite &&
        descriptor.behavior != Jak2MetalBucketBehavior::TFragment &&
        descriptor.behavior != Jak2MetalBucketBehavior::TFragmentTrans &&
        descriptor.behavior != Jak2MetalBucketBehavior::TFragmentWater &&
        descriptor.behavior != Jak2MetalBucketBehavior::Shrub &&
        descriptor.behavior != Jak2MetalBucketBehavior::Tie &&
        descriptor.behavior != Jak2MetalBucketBehavior::TieEnvmap &&
        descriptor.behavior != Jak2MetalBucketBehavior::TieTrans &&
        descriptor.behavior != Jak2MetalBucketBehavior::TieTransEnvmap &&
        descriptor.behavior != Jak2MetalBucketBehavior::TieWater &&
        descriptor.behavior != Jak2MetalBucketBehavior::TieWaterEnvmap &&
        descriptor.behavior != Jak2MetalBucketBehavior::Merc &&
        descriptor.behavior != Jak2MetalBucketBehavior::BlitDisplay &&
        descriptor.behavior != Jak2MetalBucketBehavior::MercAlpha &&
        descriptor.behavior != Jak2MetalBucketBehavior::MercWater &&
        descriptor.behavior != Jak2MetalBucketBehavior::Generic2 &&
        descriptor.behavior != Jak2MetalBucketBehavior::OceanMidFar &&
        descriptor.behavior != Jak2MetalBucketBehavior::OceanNear &&
        descriptor.behavior != Jak2MetalBucketBehavior::PrisEye) {
      return false;
    }
  }
  return true;
}

}  // namespace metal_renderer

namespace {

goal_jak2_metal_host* g_active_host = nullptr;
std::mutex g_host_mutex;

constexpr int kJak2LevelSlotCount = 6;

goal_jak2_metal_host* active_host() {
  return g_active_host;
}

bool counter_advanced_by(u64 before, u64 after, u64 expected) {
  return after >= before && after - before == expected;
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

bool load_level_art_pair(goal_jak2_metal_host* host,
                         const std::string& name,
                         bool is_common,
                         std::string* error) {
  const auto path = fr3_path(host, name);
  std::string background_error;
  auto* level = metal_level_data::load_fr3(host->renderer.device(), host->renderer.queue(),
                                           host->textures, path, is_common, &background_error);
  if (!level) {
    *error = "background level load failed: " + background_error;
    return false;
  }

  const std::string level_key = level->level->level_name;
  MetalMercModelPool::LoadResult merc_result;
  bool merc_loaded = false;
  try {
    std::string merc_error;
    if (!metal_merc_models().load_fr3(path, is_common, &merc_result, &merc_error)) {
      const bool rolled_back = metal_level_data::unload(host->textures, level_key);
      *error = "Merc model load failed: " + merc_error;
      if (!rolled_back) {
        *error += "; background rollback failed for " + level_key;
      }
      return false;
    }
    merc_loaded = true;
    if (merc_result.level_name != level_key) {
      metal_merc_models().remove_level(merc_result.level_name);
      metal_level_data::unload(host->textures, level_key);
      *error = "FR3 loaders disagreed on serialized level key";
      return false;
    }
    if (is_common) {
      host->common_level = level;
    }
    host->loaded_level_keys.push_back(level_key);
    return true;
  } catch (...) {
    if (merc_loaded) {
      metal_merc_models().remove_level(level_key);
    }
    metal_level_data::unload(host->textures, level_key);
    throw;
  }
}

void copy_renderer_metrics(goal_jak2_metal_host* host) {
  const auto stats = host->renderer.chain_stats();
  const auto& background = host->renderer.background_state();
  host->metrics.last_buckets_dispatched = stats.last_buckets_dispatched;
  host->metrics.command_buffers_committed = stats.command_buffers_committed;
  host->metrics.command_buffers_completed = stats.command_buffers_completed;
  host->metrics.command_buffer_errors = stats.command_buffer_errors;
  host->metrics.ocean_draws = stats.ocean_draws;
  host->metrics.ocean_triangles = stats.ocean_triangles;
  host->metrics.ocean_missing_textures = stats.ocean_missing_textures;
  host->metrics.ocean_command_buffers_committed = stats.ocean_command_buffers_committed;
  host->metrics.ocean_command_buffers_completed = stats.ocean_command_buffers_completed;
  host->metrics.ocean_command_buffer_errors = stats.ocean_command_buffer_errors;
  host->metrics.ocean_last_command_buffer_status = stats.ocean_last_command_buffer_status;
  host->metrics.drawables_acquired = stats.drawables_acquired;
  host->metrics.drawable_misses = stats.drawable_misses;
  host->metrics.late_present_submissions = stats.late_present_submissions;
  host->metrics.draws = stats.draw_calls;
  host->metrics.triangles = stats.triangles;
  host->metrics.last_tie_draws = background.tie_draws;
  host->metrics.last_tie_triangles = background.tie_tris;
  host->metrics.last_background_missing_levels = background.missing_levels;
  host->metrics.last_background_missing_textures = background.missing_textures;
  host->metrics.last_background_anim_slot_draws = background.anim_slot_draws;
  host->metrics.last_merc_models = stats.merc_models;
  host->metrics.last_merc_draws = stats.merc_draws;
  host->metrics.last_merc_triangles = stats.merc_triangles;
  host->metrics.last_merc_eye_draws = stats.merc_eye_draws;
  host->metrics.last_merc_eye_renderer_missing = stats.merc_eye_renderer_missing;
  host->metrics.last_merc_eye_lookup_failed = stats.merc_eye_lookup_failed;
  host->metrics.last_merc_eye_placeholder_draws = stats.merc_eye_placeholder_draws;
  host->metrics.last_merc_missing_textures = stats.merc_missing_textures;
  host->metrics.last_merc_malformed_dma = stats.merc_malformed_dma;
  host->metrics.last_merc_preflight_rejection_reason = stats.merc_preflight_rejection_reason;
  host->metrics.last_merc_missing_models = stats.merc_missing_models;
  host->metrics.last_merc_bad_bone_pointers = stats.merc_bad_bone_pointers;
  host->metrics.last_merc_missing_bone_slots = stats.merc_missing_bone_slots;
  host->metrics.last_merc_nonfinite_bone_matrices = stats.merc_nonfinite_bone_matrices;
  host->metrics.last_merc_degenerate_bone_matrices = stats.merc_degenerate_bone_matrices;
  host->metrics.last_merc_incoherent_bone_sources = stats.merc_incoherent_bone_sources;
  host->metrics.last_generic_draw_buckets = stats.generic_draw_buckets;
  host->metrics.last_generic_draws = stats.generic_draws;
  host->metrics.last_generic_triangles = stats.generic_triangles;
  host->metrics.last_generic_missing_textures = stats.generic_missing_textures;
  host->metrics.last_generic_unexpected_dma = stats.generic_unexpected_dma;
  host->metrics.last_eye_composed = stats.eyes_composed;
  host->metrics.last_eye_draws = stats.eye_draws;
  host->metrics.last_eye_triangles = stats.eye_triangles;
  host->metrics.last_eye_missing_textures = stats.eye_missing_textures;
  host->metrics.last_eye_unexpected_dma = stats.eye_unexpected_dma;
  host->metrics.last_eye_duplicate_slot_writes = stats.eye_duplicate_slot_writes;
  host->metrics.last_eye_command_buffers_committed = stats.eye_command_buffers_committed;
  host->metrics.last_eye_command_buffers_completed = stats.eye_command_buffers_completed;
  host->metrics.last_eye_command_buffer_errors = stats.eye_command_buffer_errors;
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
  host->metrics.last_progress_draws = stats.jak2_progress_draws;
  host->metrics.last_progress_triangles = stats.jak2_progress_triangles;
  host->metrics.last_progress_textured_draws = stats.jak2_progress_textured_draws;
  host->metrics.last_progress_missing_texture_draws =
      stats.jak2_progress_missing_texture_draws;
  host->metrics.last_debug_no_zbuf1_draws = stats.jak2_debug_no_zbuf1_draws;
  host->metrics.last_debug_no_zbuf1_triangles = stats.jak2_debug_no_zbuf1_triangles;
  host->metrics.last_debug_no_zbuf1_textured_draws =
      stats.jak2_debug_no_zbuf1_textured_draws;
  host->metrics.last_debug_no_zbuf1_missing_texture_draws =
      stats.jak2_debug_no_zbuf1_missing_texture_draws;
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
  host->metrics.last_sprite_placeholder_batches = stats.sprite_placeholder_batches;
  host->metrics.last_sprite_placeholder_sprites = stats.sprite_placeholder_sprites;
  host->metrics.last_sprite_first_placeholder_valid = stats.sprite_first_placeholder_valid;
  host->metrics.last_sprite_first_placeholder_tbp = stats.sprite_first_placeholder_tbp;
  host->metrics.last_sprite_first_placeholder_draw_mode =
      stats.sprite_first_placeholder_draw_mode;
  host->metrics.last_sprite_unsupported_bytes = stats.sprite_unsupported_bytes;
  host->metrics.submissions = stats.submissions;
  host->metrics.presentations = stats.presentations_completed;
  host->metrics.presentation_drops = stats.presentation_drops;
  host->metrics.presentation_order_mismatches = stats.presentation_order_mismatches;
  host->metrics.skipped_bucket_bytes = stats.skipped_bucket_bytes;
  host->metrics.last_skipped_bucket_count = stats.last_skipped_bucket_count;
  std::copy(stats.last_skipped_bucket_ids.begin(), stats.last_skipped_bucket_ids.end(),
            host->metrics.last_skipped_bucket_ids);
  std::copy(stats.last_skipped_bucket_bytes.begin(), stats.last_skipped_bucket_bytes.end(),
            host->metrics.last_skipped_bucket_bytes);
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

void copy_map_texture_upload_metrics(
    goal_jak2_metal_host* host,
    const std::optional<metal_renderer::Jak2MapTextureUploadPlan>& plan) {
  static_assert(GOAL_JAK2_MAP_TEXTURE_UPLOAD_MAX_GROUPS ==
                metal_renderer::kJak2MapTextureUploadMaximumGroups);
  auto& out = host->metrics.last_map_texture_upload;
  out = {};
  out.valid = plan.has_value();
  if (!plan) {
    return;
  }
  out.present = plan->present;
  const std::size_t upload_count =
      std::min(plan->upload_count, metal_renderer::kJak2MapTextureUploadMaximumGroups);
  out.upload_count = static_cast<uint32_t>(upload_count);
  for (std::size_t i = 0; i < upload_count; ++i) {
    out.pages[i] = plan->uploads[i].page_offset;
    out.modes[i] = plan->uploads[i].mode;
  }
}

const char* dma_tag_kind_name(u8 kind) {
  switch (static_cast<DmaTag::Kind>(kind)) {
    case DmaTag::Kind::REFE:
      return "REFE";
    case DmaTag::Kind::CNT:
      return "CNT";
    case DmaTag::Kind::NEXT:
      return "NEXT";
    case DmaTag::Kind::REF:
      return "REF";
    case DmaTag::Kind::REFS:
      return "REFS";
    case DmaTag::Kind::CALL:
      return "CALL";
    case DmaTag::Kind::RET:
      return "RET";
    case DmaTag::Kind::END:
      return "END";
  }
  return "UNKNOWN";
}

std::string map_texture_upload_rejection_message(
    const metal_renderer::Jak2MapTextureUploadDiagnostic& diagnostic) {
  std::string message = fmt::format(
      "Jak 2 bucket 319 texture-upload plan rejected malformed DMA "
      "[stage={} offset=0x{:08x} transfers={} rejected=",
      metal_renderer::jak2_map_texture_upload_rejection_stage_name(diagnostic.rejection_stage),
      diagnostic.failure_offset, diagnostic.transfer_count);
  if (diagnostic.rejected_transfer == metal_renderer::kJak2MapTextureUploadNoRejectedTransfer) {
    message += "none";
  } else {
    message += std::to_string(diagnostic.rejected_transfer);
  }
  if (diagnostic.rejected_transfer < diagnostic.transfer_count) {
    const auto& transfer = diagnostic.transfers[diagnostic.rejected_transfer];
    message += fmt::format(" failed={}:q{}:b{}:v0=0x{:08x}:v1=0x{:08x}:spr{}",
                           dma_tag_kind_name(transfer.tag_kind), transfer.qwc,
                           transfer.payload_bytes, transfer.vif0, transfer.vif1,
                           static_cast<u32>(transfer.spr));
  }
  message += " trace=";
  for (std::size_t i = 0; i < diagnostic.transfer_count; ++i) {
    const auto& transfer = diagnostic.transfers[i];
    if (i) {
      message += ",";
    }
    message += fmt::format("{}@{:08x}:{}:q{}:b{}:v0={:08x}:v1={:08x}:spr{}", i,
                           transfer.tag_offset, dma_tag_kind_name(transfer.tag_kind), transfer.qwc,
                           transfer.payload_bytes, transfer.vif0, transfer.vif1,
                           static_cast<u32>(transfer.spr));
  }
  message += "]";
  return message;
}

void record_texture_upload_metrics(
    goal_jak2_tfrag_texture_upload_metrics* out,
    u32 bucket_id,
    const metal_renderer::Jak2CommonTfragTextureUploadCapture& capture) {
  static_assert(GOAL_JAK2_TFRAG_TEXTURE_UPLOAD_BUCKET_COUNT ==
                metal_renderer::kJak2NormalTfragTextureUploadBuckets.size());
  static_assert(GOAL_JAK2_TFRAG_TEXTURE_UPLOAD_CLASS_COUNT == 7);
  out->bucket_id = bucket_id;
  out->captures++;
  out->present_captures += capture.present;
  const auto classification = static_cast<std::size_t>(capture.classification);
  if (classification < GOAL_JAK2_TFRAG_TEXTURE_UPLOAD_CLASS_COUNT) {
    out->classifications[classification]++;
  }
  out->transfers += capture.transfer_count;
  out->payload_bytes += capture.total_payload_bytes;
  out->inert_transfers += capture.inert_transfers;
  out->ordinary_descriptors += capture.ordinary_descriptors;
  out->direct_setup_transfers += capture.direct_setup_transfers;
  out->gs_setup_transfers += capture.gs_setup_transfers;
  out->animator_arrays += capture.animator_arrays;
  out->animator_body_transfers += capture.animator_body_transfers;
  out->animator_payload_bytes += capture.animator_payload_bytes;
  static_assert(metal_renderer::kJak2CommonTfragTextureAnimatorOpcodeCount == 44);
  for (std::size_t i = 0; i < capture.opcode_counts.size(); ++i) {
    out->opcode_counts[i] += capture.opcode_counts[i];
  }
  out->eye_markers += capture.eye_markers;
  out->other_transfers += capture.other_transfers;
  out->malformed_transfers += capture.malformed_transfers;
  for (std::size_t i = 0; i < capture.transfer_count; ++i) {
    const auto& transfer = capture.transfers[i];
    const bool ordinary =
        transfer.payload_bytes == 16 && transfer.qwc == 1 &&
        transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) &&
        transfer.vif0_kind == static_cast<u8>(VifCode::Kind::PC_PORT) &&
        transfer.vif0_immediate == 0 &&
        transfer.vif1_kind == static_cast<u8>(VifCode::Kind::NOP) &&
        transfer.vif1_immediate == 3;
    if (transfer.payload_bytes != 0 && !ordinary) {
      out->last_nonordinary_payload_bytes = transfer.payload_bytes;
      out->last_nonordinary_qwc = transfer.qwc;
      out->last_nonordinary_tag_kind = transfer.tag_kind;
      out->last_nonordinary_vif0_kind = transfer.vif0_kind;
      out->last_nonordinary_vif0_immediate = transfer.vif0_immediate;
      out->last_nonordinary_vif1_kind = transfer.vif1_kind;
      out->last_nonordinary_vif1_immediate = transfer.vif1_immediate;
    }
  }
}

void execute_ordinary_texture_upload_or_throw(
    goal_jak2_metal_host* host,
    const metal_renderer::Jak2Bucket4OrdinaryUploadPlan& ordinary,
    const u8* live_ee_memory,
    uint64_t* execution_count,
    const char* label,
    bool* mutation_started = nullptr) {
  if (ordinary.page_offset > EE_MAIN_MEM_SIZE - ordinary.page_header.size() ||
      ordinary.mode != -1 ||
      std::memcmp(live_ee_memory + ordinary.page_offset, ordinary.page_header.data(),
                  ordinary.page_header.size()) != 0) {
    throw std::runtime_error(std::string(label) + " changed after planning");
  }
  if (mutation_started) {
    *mutation_started = true;
  }
  host->textures.handle_upload_now(live_ee_memory + ordinary.page_offset,
                                   static_cast<int>(ordinary.mode), g_ee_main_mem,
                                   metal_offset_of_s7(), false);
  (*execution_count)++;
}

bool execute_ordinary_texture_upload(
    goal_jak2_metal_host* host,
    const metal_renderer::Jak2Bucket4OrdinaryUploadPlan& ordinary,
    const u8* live_ee_memory,
    uint64_t* execution_count,
    const char* label) {
  try {
    execute_ordinary_texture_upload_or_throw(host, ordinary, live_ee_memory, execution_count,
                                             label);
    return true;
  } catch (const std::exception& error) {
    fail_current_chain_closed(host, error.what());
  } catch (...) {
    fail_current_chain_closed(host, std::string(label) + " threw");
  }
  return false;
}

using Jak2TfragTextureUploadPlans =
    std::array<metal_renderer::Jak2NormalTfragTextureUploadPlan,
               metal_renderer::kJak2NormalTfragTextureUploadBuckets.size()>;
using Jak2ShrubTextureUploadPlans =
    std::array<metal_renderer::Jak2NormalShrubTextureUploadPlan,
               metal_renderer::kJak2NormalShrubTextureUploadBuckets.size()>;
using Jak2AlphaTextureUploadPlans =
    std::array<metal_renderer::Jak2AlphaTextureUploadPlan,
               metal_renderer::kJak2AlphaTextureUploadBuckets.size()>;
using Jak2WaterTextureUploadPlans =
    std::array<metal_renderer::Jak2WaterTextureUploadPlan,
               metal_renderer::kJak2WaterTextureUploadBuckets.size()>;
using Jak2PrisEyeTextureUploadPlans =
    std::array<metal_renderer::Jak2PrisEyeTextureUploadPlan,
               metal_renderer::kJak2PrisTextureUploadBuckets.size()>;

struct Jak2TextureUploadDispatch {
  goal_jak2_metal_host* host = nullptr;
  const metal_renderer::Jak2RawImageUploadPlan* raw_image_plan = nullptr;
  const Jak2TfragTextureUploadPlans* tfrag_plans = nullptr;
  const Jak2ShrubTextureUploadPlans* shrub_plans = nullptr;
  const Jak2AlphaTextureUploadPlans* alpha_plans = nullptr;
  const Jak2PrisEyeTextureUploadPlans* pris_eye_plans = nullptr;
  const Jak2WaterTextureUploadPlans* water_plans = nullptr;
  const metal_renderer::Jak2CommonTfragTextureUploadPlan* common_tfrag_plan = nullptr;
  const metal_renderer::Jak2SpriteTextureUploadPlan* sprite_plan = nullptr;
  const metal_renderer::Jak2MapTextureUploadPlan* map_plan = nullptr;
  const metal_renderer::Jak2Opcode27SkullGemExecutor::Prepared* skull_gem_prepared = nullptr;
  const metal_renderer::Jak2Opcode27SkullGemExecutor::PreparedSecurity* security_prepared =
      nullptr;
  const u8* live_ee_memory = nullptr;
  bool* host_texture_mutated = nullptr;
  bool* sprite_callback_executed = nullptr;
  bool* raw_image_callback_executed = nullptr;
  std::array<bool, metal_renderer::kJak2PrisTextureUploadBuckets.size()>*
      pris_eye_callbacks_executed = nullptr;
};

void execute_planned_texture_upload(void* opaque, u32 bucket_id) {
  auto* dispatch = static_cast<Jak2TextureUploadDispatch*>(opaque);
  const auto pris = std::find(metal_renderer::kJak2PrisTextureUploadBuckets.begin(),
                              metal_renderer::kJak2PrisTextureUploadBuckets.end(), bucket_id);
  if (pris != metal_renderer::kJak2PrisTextureUploadBuckets.end()) {
    if (!dispatch->pris_eye_plans || !dispatch->pris_eye_callbacks_executed) {
      throw std::runtime_error("Jak 2 PRIS eye texture dispatch is incomplete");
    }
    const std::size_t index = static_cast<std::size_t>(
        pris - metal_renderer::kJak2PrisTextureUploadBuckets.begin());
    if ((*dispatch->pris_eye_callbacks_executed)[index]) {
      throw std::runtime_error(
          fmt::format("Jak 2 PRIS eye bucket {} callback repeated", bucket_id));
    }
    (*dispatch->pris_eye_callbacks_executed)[index] = true;
    dispatch->host->metrics.last_pris_eye_dispatches++;
    const auto& plan = (*dispatch->pris_eye_plans)[index];
    if (plan.bucket_id != bucket_id) {
      throw std::runtime_error("Jak 2 PRIS eye texture dispatch order is inconsistent");
    }
    if (plan.present) {
      execute_ordinary_texture_upload_or_throw(
          dispatch->host, plan.ordinary, dispatch->live_ee_memory,
          &dispatch->host->metrics.pris_texture_uploads[index].executions,
          "Jak 2 PRIS eye ordinary texture upload", dispatch->host_texture_mutated);
      dispatch->host->metrics.last_pris_eye_present_dispatches++;
    }
    return;
  }
  if (bucket_id == static_cast<u32>(jak2::BucketId::TEX_ALL_SPRITE)) {
    if (!dispatch->sprite_callback_executed) {
      throw std::runtime_error("Jak 2 bucket 312 texture-upload marker tracking is unavailable");
    }
    if (*dispatch->sprite_callback_executed) {
      throw std::runtime_error("Jak 2 bucket 312 texture-upload marker repeated");
    }
    *dispatch->sprite_callback_executed = true;
    if (!dispatch->sprite_plan) {
      throw std::runtime_error("Jak 2 bucket 312 texture-upload dispatch is incomplete");
    }
    for (std::size_t i = 0; i < dispatch->sprite_plan->upload_count; ++i) {
      execute_ordinary_texture_upload_or_throw(
          dispatch->host, dispatch->sprite_plan->uploads[i], dispatch->live_ee_memory,
          &dispatch->host->metrics.sprite_texture_uploads,
          "Jak 2 bucket 312 ordinary texture upload", dispatch->host_texture_mutated);
    }
    return;
  }
  if (bucket_id == metal_renderer::kJak2RawImageUploadBucket) {
    if (!dispatch->raw_image_callback_executed) {
      throw std::runtime_error("Jak 2 bucket 318 raw-image marker tracking is unavailable");
    }
    if (*dispatch->raw_image_callback_executed) {
      throw std::runtime_error("Jak 2 bucket 318 raw-image publication marker repeated");
    }
    *dispatch->raw_image_callback_executed = true;
    if (!dispatch->raw_image_plan || !dispatch->raw_image_plan->present) {
      throw std::runtime_error("Jak 2 bucket 318 raw-image publication marker was unexpected");
    }
    *dispatch->host_texture_mutated = true;
    if (!dispatch->host->raw_image_upload_executor ||
        !dispatch->host->raw_image_upload_executor->execute(*dispatch->raw_image_plan)) {
      const char* detail = dispatch->host->raw_image_upload_executor
                               ? dispatch->host->raw_image_upload_executor->last_error()
                               : "executor is unavailable";
      throw std::runtime_error(std::string("Jak 2 raw-image publication failed: ") + detail);
    }
    const auto& stats = dispatch->host->raw_image_upload_executor->stats();
    dispatch->host->metrics.raw_image_publications = stats.publications;
    dispatch->host->metrics.raw_image_texture = stats.texture_handle;
    dispatch->host->metrics.raw_image_pixels = stats.pixel_count;
    return;
  }
  if (bucket_id == metal_renderer::kJak2MapTextureUploadBucket) {
    if (!dispatch->map_plan || !dispatch->map_plan->present) {
      return;
    }
    if (dispatch->map_plan->upload_count >
        metal_renderer::kJak2MapTextureUploadMaximumGroups) {
      throw std::runtime_error("Jak 2 bucket 319 texture-upload plan is inconsistent");
    }
    for (std::size_t i = 0; i < dispatch->map_plan->upload_count; ++i) {
      execute_ordinary_texture_upload_or_throw(
          dispatch->host, dispatch->map_plan->uploads[i], dispatch->live_ee_memory,
          &dispatch->host->metrics.map_texture_uploads,
          "Jak 2 bucket 319 ordinary texture upload", dispatch->host_texture_mutated);
    }
    return;
  }
  if (bucket_id == metal_renderer::kJak2CommonTfragTextureUploadBucket) {
    if (!dispatch->common_tfrag_plan || !dispatch->common_tfrag_plan->present) {
      return;
    }
    if (!dispatch->skull_gem_prepared || !dispatch->host->skull_gem_executor) {
      throw std::runtime_error("Jak 2 common TFRAG texture dispatch is incomplete");
    }
    execute_ordinary_texture_upload_or_throw(
        dispatch->host, dispatch->common_tfrag_plan->ordinary, dispatch->live_ee_memory,
        &dispatch->host->metrics.common_tfrag_ordinary_uploads,
        "Jak 2 common TFRAG ordinary texture upload", dispatch->host_texture_mutated);
    if (!dispatch->host->skull_gem_executor->publish(*dispatch->skull_gem_prepared)) {
      throw std::runtime_error(
          std::string("Jak 2 skull-gem publication failed: ") +
          dispatch->host->skull_gem_executor->last_error());
    }
    dispatch->host->metrics.common_tfrag_texture_upload.executions++;
    const auto& stats = dispatch->host->skull_gem_executor->stats();
    dispatch->host->metrics.common_tfrag_skull_gem_preparations = stats.preparations;
    dispatch->host->metrics.common_tfrag_skull_gem_publications = stats.publications;
    dispatch->host->metrics.common_tfrag_skull_gem_texture = stats.texture_handle;
    dispatch->host->metrics.common_tfrag_skull_gem_destination_tbp = stats.destination_tbp;
    dispatch->host->metrics.common_tfrag_skull_gem_anim_slot =
        metal_renderer::kJak2SkullGemAnimatedTextureSlot;
    return;
  }
  const auto found = std::find(metal_renderer::kJak2NormalTfragTextureUploadBuckets.begin(),
                               metal_renderer::kJak2NormalTfragTextureUploadBuckets.end(),
                               bucket_id);
  if (found != metal_renderer::kJak2NormalTfragTextureUploadBuckets.end()) {
    const std::size_t index = static_cast<std::size_t>(
        found - metal_renderer::kJak2NormalTfragTextureUploadBuckets.begin());
    const auto& plan = (*dispatch->tfrag_plans)[index];
    if (!plan.present) {
      return;
    }
    if (plan.bucket_id != bucket_id) {
      throw std::runtime_error("Jak 2 normal TFRAG texture-upload dispatch order is inconsistent");
    }
    const std::string label =
        "Jak 2 normal TFRAG texture upload bucket " + std::to_string(bucket_id);
    execute_ordinary_texture_upload_or_throw(
        dispatch->host, plan.ordinary, dispatch->live_ee_memory,
        &dispatch->host->metrics.tfrag_texture_uploads[index].executions, label.c_str(),
        dispatch->host_texture_mutated);
    return;
  }

  const auto shrub = std::find(metal_renderer::kJak2NormalShrubTextureUploadBuckets.begin(),
                               metal_renderer::kJak2NormalShrubTextureUploadBuckets.end(),
                               bucket_id);
  if (shrub != metal_renderer::kJak2NormalShrubTextureUploadBuckets.end()) {
    const std::size_t index = static_cast<std::size_t>(
        shrub - metal_renderer::kJak2NormalShrubTextureUploadBuckets.begin());
    const auto& plan = (*dispatch->shrub_plans)[index];
    if (!plan.present) {
      return;
    }
    if (plan.bucket_id != bucket_id) {
      throw std::runtime_error(
          "Jak 2 normal/common SHRUB texture-setup dispatch order is inconsistent");
    }
    dispatch->host->metrics.shrub_texture_uploads[index].executions++;
    return;
  }

  const auto alpha = std::find(metal_renderer::kJak2AlphaTextureUploadBuckets.begin(),
                               metal_renderer::kJak2AlphaTextureUploadBuckets.end(), bucket_id);
  if (alpha != metal_renderer::kJak2AlphaTextureUploadBuckets.end()) {
    const std::size_t index = static_cast<std::size_t>(
        alpha - metal_renderer::kJak2AlphaTextureUploadBuckets.begin());
    const auto& plan = (*dispatch->alpha_plans)[index];
    if (!plan.present) {
      return;
    }
    if (plan.bucket_id != bucket_id) {
      throw std::runtime_error("Jak 2 alpha texture-setup dispatch order is inconsistent");
    }
    dispatch->host->metrics.alpha_texture_uploads[index].executions++;
    return;
  }

  const auto water = std::find(metal_renderer::kJak2WaterTextureUploadBuckets.begin(),
                               metal_renderer::kJak2WaterTextureUploadBuckets.end(), bucket_id);
  if (water == metal_renderer::kJak2WaterTextureUploadBuckets.end()) {
    return;
  }
  const std::size_t index = static_cast<std::size_t>(
      water - metal_renderer::kJak2WaterTextureUploadBuckets.begin());
  const auto& plan = (*dispatch->water_plans)[index];
  if (!plan.present) {
    return;
  }
  if (plan.bucket_id != bucket_id) {
    throw std::runtime_error("Jak 2 water texture-upload dispatch order is inconsistent");
  }
  const std::string label = "Jak 2 water texture upload bucket " + std::to_string(bucket_id);
  execute_ordinary_texture_upload_or_throw(
      dispatch->host, plan.ordinary, dispatch->live_ee_memory,
      &dispatch->host->metrics.water_texture_uploads[index].executions, label.c_str(),
      dispatch->host_texture_mutated);
  if (plan.has_security_animator) {
    if (!dispatch->security_prepared || !dispatch->host->skull_gem_executor ||
        !dispatch->host->skull_gem_executor->publish_security(
            *dispatch->security_prepared)) {
      const char* detail = dispatch->host->skull_gem_executor
                               ? dispatch->host->skull_gem_executor->last_error()
                               : "executor is unavailable";
      throw std::runtime_error(std::string("Jak 2 security publication failed: ") + detail);
    }
  }
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
  if (ee_base != g_ee_main_mem) {
    record_failure(host, "Jak 2 DMA chain does not use the active EE memory base");
    return;
  }
  if (chain_offset == 0) {
    record_failure(host, "Jak 2 DMA chain has a null start offset");
    return;
  }

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
    const auto live_chain_validation =
        metal_renderer::validate_jak2_metal_dma_chain(ee_base, EE_MAIN_MEM_SIZE, chain_offset);
    if (!live_chain_validation) {
      const std::string error =
          std::string("Jak 2 live DMA chain validation failed before bucket ") +
          std::to_string(live_chain_validation.failed_bucket + 1) + ": " +
          metal_renderer::jak2_metal_chain_validation_error_message(live_chain_validation.error) +
          " (" + dma_chain_validation_error_message(live_chain_validation.dma.error) + ")";
      record_failure(host, error.c_str());
      return;
    }
    const auto raw_image_plan = metal_renderer::plan_jak2_raw_image_upload(
        static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, chain_offset,
        static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE);
    if (!raw_image_plan) {
      record_failure(host, "Jak 2 bucket 318 raw-image upload plan rejected malformed DMA");
      return;
    }
    Jak2TfragTextureUploadPlans tfrag_texture_plans;
    for (std::size_t i = 0; i < metal_renderer::kJak2NormalTfragTextureUploadBuckets.size();
         ++i) {
      const u32 bucket_id = metal_renderer::kJak2NormalTfragTextureUploadBuckets[i];
      metal_renderer::Jak2CommonTfragTextureUploadCapture capture;
      const auto plan = metal_renderer::plan_jak2_normal_tfrag_texture_upload(
          static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, chain_offset, bucket_id,
          static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, &capture);
      record_texture_upload_metrics(&host->metrics.tfrag_texture_uploads[i], bucket_id, capture);
      if (!plan) {
        const std::string error = "Jak 2 TFRAG texture-upload plan rejected bucket " +
                                  std::to_string(bucket_id) + " DMA";
        record_failure(host, error.c_str());
        return;
      }
      tfrag_texture_plans[i] = *plan;
    }
    Jak2ShrubTextureUploadPlans shrub_texture_plans;
    static_assert(GOAL_JAK2_SHRUB_TEXTURE_UPLOAD_BUCKET_COUNT ==
                  metal_renderer::kJak2NormalShrubTextureUploadBuckets.size());
    for (std::size_t i = 0; i < metal_renderer::kJak2NormalShrubTextureUploadBuckets.size();
         ++i) {
      const u32 bucket_id = metal_renderer::kJak2NormalShrubTextureUploadBuckets[i];
      metal_renderer::Jak2CommonTfragTextureUploadCapture capture;
      const auto plan = metal_renderer::plan_jak2_normal_shrub_texture_upload(
          static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, chain_offset, bucket_id, &capture);
      record_texture_upload_metrics(&host->metrics.shrub_texture_uploads[i], bucket_id, capture);
      if (!plan) {
        const std::string error = "Jak 2 normal/common SHRUB texture-setup plan rejected bucket " +
                                  std::to_string(bucket_id) + " DMA";
        record_failure(host, error.c_str());
        return;
      }
      shrub_texture_plans[i] = *plan;
    }
    Jak2AlphaTextureUploadPlans alpha_texture_plans;
    static_assert(GOAL_JAK2_ALPHA_TEXTURE_UPLOAD_BUCKET_COUNT ==
                  metal_renderer::kJak2AlphaTextureUploadBuckets.size());
    for (std::size_t i = 0; i < metal_renderer::kJak2AlphaTextureUploadBuckets.size(); ++i) {
      const u32 bucket_id = metal_renderer::kJak2AlphaTextureUploadBuckets[i];
      metal_renderer::Jak2CommonTfragTextureUploadCapture capture;
      const auto plan = metal_renderer::plan_jak2_alpha_texture_upload(
          static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, chain_offset, bucket_id, &capture);
      record_texture_upload_metrics(&host->metrics.alpha_texture_uploads[i], bucket_id, capture);
      if (!plan) {
        const std::string error = "Jak 2 alpha texture-setup plan rejected bucket " +
                                  std::to_string(bucket_id) + " DMA";
        record_failure(host, error.c_str());
        return;
      }
      alpha_texture_plans[i] = *plan;
    }
    Jak2PrisEyeTextureUploadPlans live_pris_eye_plans;
    static_assert(GOAL_JAK2_PRIS_TEXTURE_UPLOAD_BUCKET_COUNT ==
                  metal_renderer::kJak2PrisTextureUploadBuckets.size());
    for (std::size_t i = 0; i < metal_renderer::kJak2PrisTextureUploadBuckets.size(); ++i) {
      const u32 bucket_id = metal_renderer::kJak2PrisTextureUploadBuckets[i];
      metal_renderer::Jak2CommonTfragTextureUploadCapture capture;
      const auto plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
          static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, chain_offset, bucket_id,
          static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, &capture);
      record_texture_upload_metrics(&host->metrics.pris_texture_uploads[i], bucket_id, capture);
      if (!plan) {
        const std::string error = "Jak 2 PRIS eye texture plan rejected bucket " +
                                  std::to_string(bucket_id) + " DMA";
        record_failure(host, error.c_str());
        return;
      }
      live_pris_eye_plans[i] = *plan;
    }
    Jak2WaterTextureUploadPlans water_texture_plans;
    static_assert(GOAL_JAK2_WATER_TEXTURE_UPLOAD_BUCKET_COUNT ==
                  metal_renderer::kJak2WaterTextureUploadBuckets.size());
    for (std::size_t i = 0; i < metal_renderer::kJak2WaterTextureUploadBuckets.size(); ++i) {
      const u32 bucket_id = metal_renderer::kJak2WaterTextureUploadBuckets[i];
      metal_renderer::Jak2CommonTfragTextureUploadCapture capture;
      const auto plan = metal_renderer::plan_jak2_water_texture_upload(
          static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, chain_offset, bucket_id,
          static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, &capture);
      record_texture_upload_metrics(&host->metrics.water_texture_uploads[i], bucket_id, capture);
      if (!plan) {
        const std::string error = "Jak 2 water texture-upload plan rejected bucket " +
                                  std::to_string(bucket_id) + " DMA";
        record_failure(host, error.c_str());
        return;
      }
      water_texture_plans[i] = *plan;
    }
    metal_renderer::Jak2CommonTfragTextureUploadCapture common_tfrag_texture_capture;
    const auto common_tfrag_texture_plan =
        metal_renderer::plan_jak2_common_tfrag_texture_upload(
            static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, chain_offset,
            static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE,
            &common_tfrag_texture_capture);
    record_texture_upload_metrics(&host->metrics.common_tfrag_texture_upload,
                                  metal_renderer::kJak2CommonTfragTextureUploadBucket,
                                  common_tfrag_texture_capture);
    if (!common_tfrag_texture_plan) {
      record_failure(host, "Jak 2 common TFRAG texture plan rejected bucket 187 DMA");
      return;
    }
    metal_renderer::Jak2Opcode27SkullGemExecutor::Prepared skull_gem_prepared;
    metal_renderer::Jak2Opcode27SkullGemExecutor::PreparedSecurity security_prepared;
    const metal_renderer::Jak2WaterTextureUploadPlan* security_plan = nullptr;
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
    if (sprite_texture_plan->present != (sprite_texture_plan->upload_count > 0) ||
        sprite_texture_plan->upload_count >
            metal_renderer::kJak2SpriteTextureUploadMaximumGroups) {
      record_failure(host, "Jak 2 bucket 312 texture-upload plan is inconsistent");
      return;
    }
    metal_renderer::Jak2MapTextureUploadDiagnostic map_texture_diagnostic;
    const auto map_texture_plan = metal_renderer::plan_jak2_map_texture_upload(
        static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, chain_offset,
        static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE, &map_texture_diagnostic);
    copy_map_texture_upload_metrics(host, map_texture_plan);
    if (!map_texture_plan) {
      const std::string message = map_texture_upload_rejection_message(map_texture_diagnostic);
      record_failure(host, message.c_str());
      return;
    }
    const auto& copied = host->copier.run(ee_base, chain_offset, false);
    host->metrics.last_copied_bytes = static_cast<uint32_t>(copied.data.size());

    const auto copied_chain_validation = metal_renderer::validate_jak2_metal_dma_chain(
        copied.data.data(), copied.data.size(), copied.start_offset);
    if (!copied_chain_validation) {
      record_send_chain_failure(
          host,
          std::string("Jak 2 copied DMA chain validation failed before bucket ") +
              std::to_string(copied_chain_validation.failed_bucket + 1) + ": " +
              metal_renderer::jak2_metal_chain_validation_error_message(
                  copied_chain_validation.error) +
              " (" + dma_chain_validation_error_message(copied_chain_validation.dma.error) + ")",
          host_texture_mutated);
      return;
    }

    Jak2PrisEyeTextureUploadPlans copied_pris_eye_plans;
    for (std::size_t i = 0; i < metal_renderer::kJak2PrisTextureUploadBuckets.size(); ++i) {
      const u32 bucket_id = metal_renderer::kJak2PrisTextureUploadBuckets[i];
      const auto copied_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
          copied.data.data(), copied.data.size(), copied.start_offset, bucket_id,
          static_cast<const u8*>(ee_base), EE_MAIN_MEM_SIZE);
      if (!copied_plan || !metal_renderer::jak2_pris_eye_texture_upload_plans_match(
                              live_pris_eye_plans[i], *copied_plan)) {
        record_send_chain_failure(
            host,
            "Jak 2 copied PRIS eye texture plan did not match live bucket " +
                std::to_string(bucket_id),
            false);
        return;
      }
      copied_pris_eye_plans[i] = *copied_plan;
    }

    if (!metal_renderer::copied_jak2_raw_image_upload_markers_match_plan(
            copied.data.data(), copied.data.size(), copied.start_offset,
            raw_image_plan->present)) {
      record_send_chain_failure(
          host, "Jak 2 copied bucket 318 raw-image marker count did not match its plan",
          false);
      return;
    }

    if (common_tfrag_texture_plan->present &&
        (!host->common_level || !host->common_level->level || !host->skull_gem_executor ||
         !host->skull_gem_executor->prepare(common_tfrag_texture_plan->skull_gem,
                                            *host->common_level->level,
                                            &skull_gem_prepared))) {
      const char* detail = !host->common_level || !host->common_level->level
                               ? "common level art is unavailable"
                               : host->skull_gem_executor
                                     ? host->skull_gem_executor->last_error()
                                     : "executor is unavailable";
      record_failure(host, (std::string("Jak 2 skull-gem preparation failed: ") + detail).c_str());
      return;
    }
    for (const auto& water_plan : water_texture_plans) {
      if (!water_plan.has_security_animator) {
        continue;
      }
      if (security_plan) {
        record_failure(host, "Jak 2 security animator appeared in multiple water buckets");
        return;
      }
      security_plan = &water_plan;
    }
    auto* ctywide_level = security_plan ? metal_level_data::get("ctywide") : nullptr;
    if (security_plan &&
        (!host->common_level || !host->common_level->level || !ctywide_level ||
         !ctywide_level->level || !host->skull_gem_executor ||
         !host->skull_gem_executor->prepare_security(
             security_plan->security, *host->common_level->level,
             *ctywide_level->level,
             &security_prepared))) {
      const char* detail =
          !host->common_level || !host->common_level->level
              ? "common level art is unavailable"
              : !ctywide_level || !ctywide_level->level
                    ? "ctywide level art is unavailable"
                    : host->skull_gem_executor ? host->skull_gem_executor->last_error()
                                               : "executor is unavailable";
      record_failure(host,
                     (std::string("Jak 2 security preparation failed: ") + detail).c_str());
      return;
    }
    if (!execute_bucket4_plan(host, *bucket4_plan, static_cast<const u8*>(ee_base))) {
      return;
    }
    host_texture_mutated =
        !std::holds_alternative<metal_renderer::Jak2Bucket4AbsentPlan>(*bucket4_plan);

    bool sprite_callback_executed = false;
    bool raw_image_callback_executed = false;
    std::array<bool, metal_renderer::kJak2PrisTextureUploadBuckets.size()>
        pris_eye_callbacks_executed = {};
    host->metrics.last_pris_eye_dispatches = 0;
    host->metrics.last_pris_eye_present_dispatches = 0;
    host->metrics.last_pris_eye_chunks = 0;
    Jak2TextureUploadDispatch texture_dispatch{
        host,
        &*raw_image_plan,
        &tfrag_texture_plans,
        &shrub_texture_plans,
        &alpha_texture_plans,
        &copied_pris_eye_plans,
        &water_texture_plans,
        &*common_tfrag_texture_plan,
        &*sprite_texture_plan,
        &*map_texture_plan,
        common_tfrag_texture_plan->present ? &skull_gem_prepared : nullptr,
        security_plan ? &security_prepared : nullptr,
        static_cast<const u8*>(ee_base),
        &host_texture_mutated,
        &sprite_callback_executed,
        &raw_image_callback_executed,
        &pris_eye_callbacks_executed};
    auto render_options = host->options;
    const auto& animated_texture_slots = host->skull_gem_executor->animated_texture_slots();
    render_options.animated_texture_slots = animated_texture_slots.data();
    render_options.animated_texture_slot_count = animated_texture_slots.size();
    render_options.host_bucket_context = &texture_dispatch;
    render_options.host_bucket_callback = execute_planned_texture_upload;
    render_options.jak2_pris_eye_plans = copied_pris_eye_plans.data();
    render_options.jak2_pris_eye_plan_count = copied_pris_eye_plans.size();
    const auto renderer_before = host->renderer.chain_stats();
    const bool acquired = host->renderer.render_chain_frame(
        render_options, host->layer, copied.data.data(), copied.start_offset, copied.data.size());
    const auto renderer_after = host->renderer.chain_stats();
    copy_renderer_metrics(host);
    std::size_t expected_pris_eye_chunks = 0;
    std::size_t expected_pris_eye_present_dispatches = 0;
    for (const auto& plan : copied_pris_eye_plans) {
      expected_pris_eye_chunks += plan.chunk_count;
      expected_pris_eye_present_dispatches += plan.present;
    }
    if (!std::all_of(pris_eye_callbacks_executed.begin(), pris_eye_callbacks_executed.end(),
                     [](bool executed) { return executed; }) ||
        host->metrics.last_pris_eye_dispatches != copied_pris_eye_plans.size() ||
        host->metrics.last_pris_eye_present_dispatches !=
            expected_pris_eye_present_dispatches) {
      record_send_chain_failure(host, "Jak 2 PRIS eye bucket callback dispatch was incomplete",
                                host_texture_mutated);
      return;
    }
    if (host->metrics.last_eye_composed != expected_pris_eye_chunks * 2 ||
        host->metrics.last_eye_missing_textures != 0 ||
        host->metrics.last_eye_unexpected_dma != 0 ||
        host->metrics.last_eye_duplicate_slot_writes != 0 ||
        host->metrics.last_eye_command_buffers_committed != expected_pris_eye_chunks ||
        host->metrics.last_eye_command_buffers_completed != expected_pris_eye_chunks ||
        host->metrics.last_eye_command_buffer_errors != 0) {
      record_send_chain_failure(host, "Jak 2 PRIS eye renderer execution violated its exact gate",
                                host_texture_mutated);
      return;
    }
    host->metrics.last_pris_eye_chunks = expected_pris_eye_chunks;
    if (!sprite_callback_executed) {
      record_send_chain_failure(
          host, "Jak 2 bucket 312 texture-upload marker was not dispatched",
          host_texture_mutated);
      return;
    }
    if (raw_image_callback_executed != raw_image_plan->present) {
      record_send_chain_failure(
          host, "Jak 2 bucket 318 raw-image publication marker did not match its plan",
          host_texture_mutated);
      return;
    }
    if (host->metrics.last_buckets_dispatched != metal_renderer::kJak2MetalBucketCount) {
      record_send_chain_failure(host, "Jak 2 Metal renderer violated its audited bucket policy",
                                host_texture_mutated);
      return;
    }
    const bool exact_render_attempt =
        counter_advanced_by(renderer_before.chains_rendered, renderer_after.chains_rendered, 1);
    const bool exact_presenting_commit_count = counter_advanced_by(
        renderer_before.command_buffers_committed, renderer_after.command_buffers_committed, 1);
    const bool exact_drawable_acquisition_count = counter_advanced_by(
        renderer_before.drawables_acquired, renderer_after.drawables_acquired, 1);
    const bool exact_submission_count =
        counter_advanced_by(renderer_before.submissions, renderer_after.submissions, 1);
    const bool ocean_buffers_completed = host->metrics.ocean_command_buffers_committed ==
                                             host->metrics.ocean_command_buffers_completed &&
                                         host->metrics.ocean_command_buffer_errors == 0;
    if (!ocean_buffers_completed) {
      record_send_chain_failure(host, "Jak 2 ocean command buffer failed its completion gate",
                                host_texture_mutated);
      return;
    }
    if (!host->layer) {
      if (acquired || host->metrics.command_buffers_committed != 0 ||
          host->metrics.command_buffers_completed != 0 ||
          host->metrics.command_buffer_errors != 0 || host->metrics.drawables_acquired != 0 ||
          host->metrics.drawable_misses != 0 || host->metrics.submissions != 0 ||
          host->metrics.presentations != 0 || host->metrics.presentation_drops != 0 ||
          host->metrics.presentation_order_mismatches != 0) {
        record_send_chain_failure(
            host, "Jak 2 nil-layer renderer violated the submission-free dispatch gate",
            host_texture_mutated);
        return;
      }
    } else if (!acquired || host->metrics.unsupported_blends != 0 || !exact_render_attempt ||
               !exact_presenting_commit_count || !exact_drawable_acquisition_count ||
               host->metrics.drawable_misses != 0 || !exact_submission_count ||
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
    try {
      std::string error;
      if (!load_level_art_pair(host, name, false, &error)) {
        fail_closed(host, "Jak 2 level art load failed for " + name + ": " + error);
        return;
      }
      host->requested_level_names.push_back(name);
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

goal_jak2_metal_host* create_host(CAMetalLayer* layer, bool presenting) {
  if (g_active_host || !metal_renderer::jak2_metal_host_policy_table_is_audited() ||
      (presenting && !layer)) {
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
  host->skull_gem_executor =
      std::make_unique<metal_renderer::Jak2Opcode27SkullGemExecutor>(
          host->renderer.device(), host->renderer.queue(), &host->textures);
  host->raw_image_upload_executor =
      std::make_unique<metal_renderer::Jak2RawImageUploadExecutor>(
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

int goal_jak2_metal_host_set_present_pacing(goal_jak2_metal_host* host, double seconds) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  if (!host || host != g_active_host || host->inactive || !std::isfinite(seconds) || seconds < 0.0) {
    return 0;
  }
  host->options.min_present_duration = seconds;
  host->options.target_fps = seconds > 0.0 ? static_cast<float>(1.0 / seconds) : 60.f;
  return 1;
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
    if (!load_level_art_pair(host, "GAME", true, &error)) {
      host->error = "Jak 2 common level art load failed: " + error;
      return 0;
    }
    host->requested_level_names.push_back("GAME");
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
         metrics->command_buffer_errors == 0 &&
         metrics->ocean_command_buffers_committed ==
             metrics->ocean_command_buffers_completed &&
         metrics->ocean_command_buffer_errors == 0 &&
         metrics->drawables_acquired == metrics->chains &&
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
    metal_merc_models().remove_level(*key);
    metal_level_data::unload(host->textures, *key);
  }
  host->loaded_level_keys.clear();
  host->common_level = nullptr;
  if (host->bucket4_mixed_executor) {
    host->bucket4_mixed_executor->detach_pool();
  }
  if (host->skull_gem_executor) {
    host->skull_gem_executor->detach_pool();
  }
  if (host->raw_image_upload_executor) {
    host->raw_image_upload_executor->detach_pool();
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
