#pragma once

#include <stdint.h>

#include "game/kernel/core/gfx_host.h"

#ifdef __OBJC__
@class CAMetalLayer;
typedef CAMetalLayer* goal_jak2_metal_host_layer;
#else
typedef void* goal_jak2_metal_host_layer;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct goal_jak2_metal_host goal_jak2_metal_host;

typedef struct goal_jak2_bucket4_texture_upload_metrics {
  uint32_t valid;
  uint32_t present;
  uint32_t total_payload_bytes;
  uint32_t dma_transfers;
  uint32_t payload_transfers;
  uint32_t inert_transfers;
  uint32_t inert_cnt_transfers;
  uint32_t inert_next_transfers;
  uint32_t inert_state_mask;
  uint32_t ordinary_descriptors;
  uint64_t ordinary_page;
  int64_t ordinary_mode;
  uint32_t animator_arrays;
  uint32_t animator_bytes;
  uint32_t opcode_counts[44];
  int32_t cloud_destination;
  uint32_t erase_width;
  uint32_t erase_height;
  uint32_t erase_destination;
  uint64_t erase_test;
  uint64_t erase_alpha;
  uint64_t erase_clamp;
  uint32_t erase_clear[4];
  uint32_t generic_source;
  uint16_t generic_width;
  uint16_t generic_height;
  uint32_t generic_destination;
  uint8_t generic_format;
  uint8_t generic_force_to_gpu;
  uint32_t clut_source;
  uint32_t clut_destination;
  uint32_t finishes;
  uint32_t malformed_transfers;
  uint32_t malformed_bytes;
  uint32_t unsupported_transfers;
  uint32_t unsupported_bytes;
} goal_jak2_bucket4_texture_upload_metrics;

enum { GOAL_JAK2_SPRITE_TEXTURE_UPLOAD_MAX_GROUPS = 7 };
enum { GOAL_JAK2_MAP_TEXTURE_UPLOAD_MAX_GROUPS = 8 };

typedef struct goal_jak2_sprite_texture_upload_metrics {
  uint32_t valid;
  uint32_t present;
  uint32_t upload_count;
  uint64_t pages[GOAL_JAK2_SPRITE_TEXTURE_UPLOAD_MAX_GROUPS];
  int64_t modes[GOAL_JAK2_SPRITE_TEXTURE_UPLOAD_MAX_GROUPS];
} goal_jak2_sprite_texture_upload_metrics;

typedef struct goal_jak2_map_texture_upload_metrics {
  uint32_t valid;
  uint32_t present;
  uint32_t upload_count;
  uint64_t pages[GOAL_JAK2_MAP_TEXTURE_UPLOAD_MAX_GROUPS];
  int64_t modes[GOAL_JAK2_MAP_TEXTURE_UPLOAD_MAX_GROUPS];
} goal_jak2_map_texture_upload_metrics;

enum {
  GOAL_JAK2_TFRAG_TEXTURE_UPLOAD_BUCKET_COUNT = 6,
  GOAL_JAK2_SHRUB_TEXTURE_UPLOAD_BUCKET_COUNT = 7,
  GOAL_JAK2_ALPHA_TEXTURE_UPLOAD_BUCKET_COUNT = 6,
  GOAL_JAK2_PRIS_TEXTURE_UPLOAD_BUCKET_COUNT = 6,
  GOAL_JAK2_WATER_TEXTURE_UPLOAD_BUCKET_COUNT = 6,
  GOAL_JAK2_TFRAG_TEXTURE_UPLOAD_CLASS_COUNT = 7,
  GOAL_JAK2_TRACKED_DEFERRED_BUCKET_COUNT = 4,
};

typedef struct goal_jak2_tfrag_texture_upload_metrics {
  uint32_t bucket_id;
  uint64_t captures;
  uint64_t present_captures;
  uint64_t executions;
  uint64_t classifications[GOAL_JAK2_TFRAG_TEXTURE_UPLOAD_CLASS_COUNT];
  uint64_t transfers;
  uint64_t payload_bytes;
  uint64_t inert_transfers;
  uint64_t ordinary_descriptors;
  uint64_t direct_setup_transfers;
  uint64_t gs_setup_transfers;
  uint64_t animator_arrays;
  uint64_t animator_body_transfers;
  uint64_t animator_payload_bytes;
  uint64_t opcode_counts[44];
  uint64_t eye_markers;
  uint64_t other_transfers;
  uint64_t malformed_transfers;
  uint32_t last_nonordinary_payload_bytes;
  uint16_t last_nonordinary_qwc;
  uint8_t last_nonordinary_tag_kind;
  uint8_t last_nonordinary_vif0_kind;
  uint16_t last_nonordinary_vif0_immediate;
  uint8_t last_nonordinary_vif1_kind;
  uint16_t last_nonordinary_vif1_immediate;
} goal_jak2_tfrag_texture_upload_metrics;

typedef struct goal_jak2_metal_host_metrics {
  uint64_t chains;
  uint64_t completed_chains;
  uint64_t failed_chains;
  uint64_t sync_paths;
  uint64_t vsyncs;
  uint64_t texture_uploads;
  uint64_t texture_relocations;
  uint64_t bucket4_ordinary_uploads;
  uint64_t bucket4_mixed_executions;
  uint64_t bucket4_cloud_publications;
  uint64_t bucket4_fog_publications;
  uint64_t bucket4_cloud_texture;
  uint64_t bucket4_fog_texture;
  uint64_t sprite_texture_uploads;
  uint64_t map_texture_uploads;
  uint64_t raw_image_publications;
  uint64_t raw_image_texture;
  uint64_t raw_image_pixels;
  uint64_t common_tfrag_ordinary_uploads;
  uint64_t common_tfrag_skull_gem_preparations;
  uint64_t common_tfrag_skull_gem_publications;
  uint64_t common_tfrag_skull_gem_texture;
  uint32_t common_tfrag_skull_gem_destination_tbp;
  uint32_t common_tfrag_skull_gem_anim_slot;
  uint64_t last_pris_eye_dispatches;
  uint64_t last_pris_eye_present_dispatches;
  uint64_t last_pris_eye_chunks;
  uint64_t last_eye_composed;
  uint64_t last_eye_draws;
  uint64_t last_eye_triangles;
  uint64_t last_eye_missing_textures;
  uint64_t last_eye_unexpected_dma;
  uint64_t last_eye_duplicate_slot_writes;
  uint64_t last_eye_command_buffers_committed;
  uint64_t last_eye_command_buffers_completed;
  uint64_t last_eye_command_buffer_errors;
  uint32_t last_copied_bytes;
  uint64_t last_buckets_dispatched;
  uint64_t command_buffers_committed;
  uint64_t command_buffers_completed;
  uint64_t command_buffer_errors;
  uint64_t drawables_acquired;
  uint64_t drawable_misses;
  uint64_t late_present_submissions;
  uint64_t draws;
  uint64_t triangles;
  uint64_t last_tie_draws;
  uint64_t last_tie_triangles;
  uint64_t last_background_missing_levels;
  uint64_t last_background_missing_textures;
  uint64_t last_background_anim_slot_draws;
  uint64_t last_merc_models;
  uint64_t last_merc_draws;
  uint64_t last_merc_triangles;
  uint64_t last_merc_eye_draws;
  uint64_t last_merc_eye_renderer_missing;
  uint64_t last_merc_eye_lookup_failed;
  uint64_t last_merc_eye_placeholder_draws;
  uint64_t last_merc_missing_textures;
  uint64_t last_merc_malformed_dma;
  uint32_t last_merc_preflight_rejection_reason;
  uint64_t last_merc_missing_models;
  uint64_t last_merc_bad_bone_pointers;
  uint64_t last_merc_missing_bone_slots;
  uint64_t last_merc_nonfinite_bone_matrices;
  uint64_t last_merc_degenerate_bone_matrices;
  uint64_t last_merc_incoherent_bone_sources;
  uint64_t last_sky_draw_draws;
  uint64_t last_sky_draw_triangles;
  uint32_t last_sky_draw_batch_valid;
  uint32_t last_sky_draw_batch_textured;
  uint32_t last_sky_draw_batch_vertices;
  uint32_t last_sky_draw_batch_nonzero_rgb_vertices;
  uint32_t last_sky_draw_batch_tex0_tbp;
  uint32_t last_sky_draw_batch_tex0_tcc;
  uint32_t last_sky_draw_batch_tex0_decal;
  uint32_t last_sky_draw_batch_texture_lookup_hit;
  uint32_t last_sky_draw_batch_used_placeholder;
  uint32_t last_sky_draw_batch_write_rgb;
  uint32_t last_sky_draw_batch_blend_enabled;
  uint32_t last_sky_draw_batch_blend_a;
  uint32_t last_sky_draw_batch_blend_b;
  uint32_t last_sky_draw_batch_blend_c;
  uint32_t last_sky_draw_batch_blend_d;
  uint32_t last_sky_draw_batch_alpha_test_enabled;
  uint32_t last_sky_draw_batch_alpha_test_mode;
  uint32_t last_sky_draw_batch_alpha_aref;
  uint32_t last_sky_draw_batch_alpha_afail;
  uint64_t last_screen_filter_draws;
  uint64_t last_screen_filter_triangles;
  uint64_t last_progress_draws;
  uint64_t last_progress_triangles;
  uint64_t last_progress_textured_draws;
  uint64_t last_progress_missing_texture_draws;
  uint64_t last_debug_no_zbuf1_draws;
  uint64_t last_debug_no_zbuf1_triangles;
  uint64_t last_debug_no_zbuf1_textured_draws;
  uint64_t last_debug_no_zbuf1_missing_texture_draws;
  uint64_t last_debug_no_zbuf2_draws;
  uint64_t last_debug_no_zbuf2_triangles;
  uint64_t last_sprites_2d;
  uint64_t last_sprites_3d;
  uint64_t last_sprites_hud;
  uint64_t last_sprites_distort;
  uint64_t last_sprite_normal_submitted;
  uint64_t last_sprite_glow_marked;
  uint64_t last_sprite_glow_skipped;
  uint64_t last_sprite_glow_parsed;
  uint64_t last_sprite_glow_accepted;
  uint64_t last_sprite_glow_rejected;
  uint64_t last_sprite_glow_invalid_records;
  // Legacy ABI names: these are visibility-tested final flare counts.
  uint64_t last_sprite_glow_force_visible_submitted;
  uint64_t last_sprite_glow_force_visible_drawn;
  uint64_t last_sprite_glow_force_visible_draws;
  uint64_t last_sprite_glow_force_visible_triangles;
  uint64_t last_sprite_glow_force_visible_missing_textures;
  uint64_t last_sprite_draws;
  uint64_t last_sprite_triangles;
  uint64_t last_sprite_missing_textures;
  uint64_t last_sprite_placeholder_batches;
  uint64_t last_sprite_placeholder_sprites;
  uint32_t last_sprite_first_placeholder_valid;
  uint32_t last_sprite_first_placeholder_tbp;
  uint32_t last_sprite_first_placeholder_draw_mode;
  uint64_t last_sprite_unsupported_bytes;
  uint64_t submissions;
  uint64_t presentations;
  uint64_t presentation_drops;
  uint64_t presentation_order_mismatches;
  uint64_t skipped_bucket_bytes;
  uint32_t last_skipped_bucket_count;
  uint32_t last_skipped_bucket_ids[GOAL_JAK2_TRACKED_DEFERRED_BUCKET_COUNT];
  uint64_t last_skipped_bucket_bytes[GOAL_JAK2_TRACKED_DEFERRED_BUCKET_COUNT];
  uint64_t unsupported_blends;
  int32_t last_command_buffer_status;
  int64_t last_command_buffer_error_code;
  goal_jak2_bucket4_texture_upload_metrics last_bucket4_texture_upload;
  goal_jak2_sprite_texture_upload_metrics last_sprite_texture_upload;
  goal_jak2_map_texture_upload_metrics last_map_texture_upload;
  goal_jak2_tfrag_texture_upload_metrics
      tfrag_texture_uploads[GOAL_JAK2_TFRAG_TEXTURE_UPLOAD_BUCKET_COUNT];
  goal_jak2_tfrag_texture_upload_metrics
      shrub_texture_uploads[GOAL_JAK2_SHRUB_TEXTURE_UPLOAD_BUCKET_COUNT];
  goal_jak2_tfrag_texture_upload_metrics
      alpha_texture_uploads[GOAL_JAK2_ALPHA_TEXTURE_UPLOAD_BUCKET_COUNT];
  goal_jak2_tfrag_texture_upload_metrics
      pris_texture_uploads[GOAL_JAK2_PRIS_TEXTURE_UPLOAD_BUCKET_COUNT];
  goal_jak2_tfrag_texture_upload_metrics
      water_texture_uploads[GOAL_JAK2_WATER_TEXTURE_UPLOAD_BUCKET_COUNT];
  goal_jak2_tfrag_texture_upload_metrics common_tfrag_texture_upload;
  uint64_t ocean_draws;
  uint64_t ocean_triangles;
  uint64_t ocean_missing_textures;
  uint64_t ocean_command_buffers_committed;
  uint64_t ocean_command_buffers_completed;
  uint64_t ocean_command_buffer_errors;
  int32_t ocean_last_command_buffer_status;
  uint64_t last_generic_draw_buckets;
  uint64_t last_generic_draws;
  uint64_t last_generic_triangles;
  uint64_t last_generic_missing_textures;
  uint64_t last_generic_unexpected_dma;
  goal_jak2_tfrag_texture_upload_metrics common_pris_texture_upload;
} goal_jak2_metal_host_metrics;

typedef struct goal_jak2_metal_frame_summary {
  uint32_t width;
  uint32_t height;
  uint64_t byte_count;
  uint64_t hash;
  uint64_t non_black_pixels;
  uint64_t nonzero_alpha_pixels;
  uint32_t max_alpha;
} goal_jak2_metal_frame_summary;

/*! Create the process-singleton, nil-layer Jak 2 policy-dispatch host. */
goal_jak2_metal_host* goal_jak2_metal_host_create(void);

/*! Create the same external host with an app-owned layer for real-DMA presentation. */
goal_jak2_metal_host* goal_jak2_metal_host_create_presenting(
    goal_jak2_metal_host_layer layer);

/*! Set the minimum on-screen duration for future drawables. Zero disables presentation pacing. */
int goal_jak2_metal_host_set_present_pacing(goal_jak2_metal_host* host, double seconds);

/*!
 * Configure host-owned Jak 2 level art before copying the graphics callbacks. `fr3_directory` is
 * copied, then GAME.fr3 is synchronously loaded as common art. Repeating the same successful
 * configuration is harmless; a different directory, a late call, or a load failure is rejected.
 */
int goal_jak2_metal_host_configure_level_art(goal_jak2_metal_host* host,
                                             const char* fr3_directory);

/*! Copy the app-owned callback table into `out`; the runtime copies it again during start. */
int goal_jak2_metal_host_copy_gfx_host(goal_jak2_metal_host* host, goal_gfx_host* out);

int goal_jak2_metal_host_get_metrics(goal_jak2_metal_host* host,
                                     goal_jak2_metal_host_metrics* out);

/*!
 * Pure counter gate shared by the host wait and standalone proof. Drawable callbacks, drops, and
 * ordering decide the result only when `require_presentation` is nonzero.
 */
int goal_jak2_metal_host_metrics_pass_frame_gate(const goal_jak2_metal_host_metrics* metrics,
                                                 int require_presentation);

/*!
 * Summarize the last completed RGBA8 game frame without exposing C++ storage. `hash` is FNV-1a
 * over the RGBA bytes. `non_black_pixels` ignores alpha, while `nonzero_alpha_pixels` and
 * `max_alpha` summarize the alpha channel. Returns zero and clears `out` when no layer-backed
 * frame is available.
 */
int goal_jak2_metal_host_read_last_frame(goal_jak2_metal_host* host,
                                         goal_jak2_metal_frame_summary* out);

/*!
 * Wait for every layer-backed command buffer submitted so far. When `require_presentation` is
 * nonzero, also require every physical drawable callback with no drops or ordering mismatches.
 * Otherwise those asynchronous presentation facts remain diagnostic. The iOS Simulator passes
 * zero because its SDK does not expose that callback.
 */
int goal_jak2_metal_host_wait_for_last_frame(goal_jak2_metal_host* host,
                                             double timeout_seconds,
                                             int require_presentation);

/*! Call only after goal_jak2_runtime_shutdown, when the copied callbacks are no longer reachable. */
void goal_jak2_metal_host_destroy(goal_jak2_metal_host* host);

const char* goal_jak2_metal_host_last_error(goal_jak2_metal_host* host);

#ifdef __cplusplus
}  // extern "C"

namespace metal_renderer {

/*! Verify that the host explicitly recognizes every behavior in the fixed Jak 2 bucket table. */
bool jak2_metal_host_policy_table_is_audited();

}  // namespace metal_renderer
#endif
