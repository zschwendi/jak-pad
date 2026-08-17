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

typedef struct goal_jak2_metal_presentation_geometry {
  uint32_t game_width;
  uint32_t game_height;
  uint32_t draw_region_width;
  uint32_t draw_region_height;
} goal_jak2_metal_presentation_geometry;

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
  GOAL_JAK2_PRIS2_CAPTURE_BUCKET_COUNT = 2,
  GOAL_JAK2_PRISON_CLUT_OUTPUT_COUNT = 6,
  GOAL_JAK2_WATER_TEXTURE_UPLOAD_BUCKET_COUNT = 6,
  GOAL_JAK2_TFRAG_TEXTURE_UPLOAD_CLASS_COUNT = 7,
  GOAL_JAK2_TRACKED_DEFERRED_BUCKET_COUNT = 4,
  GOAL_JAK2_MERC_ANIM_SLOT_DIAGNOSTIC_COUNT = 4,
  GOAL_JAK2_MERC_MODEL_DIAGNOSTIC_COUNT = 16,
  GOAL_JAK2_SUBTITLE322_VARIANT_COUNT = 5,
  GOAL_JAK2_SUBTITLE322_REJECT_REASON_COUNT = 7,
};

enum {
  GOAL_JAK2_SUBTITLE322_STATUS_MATCHED = 0,
  GOAL_JAK2_SUBTITLE322_STATUS_REJECTED = 1,
  GOAL_JAK2_SUBTITLE322_STATUS_SEMANTIC_MISMATCH = 2,
  GOAL_JAK2_SUBTITLE322_NO_TRANSFER_INDEX = UINT32_MAX,
};

typedef struct goal_jak2_merc_model_diagnostic_metrics {
  uint32_t bucket_id;
  uint64_t model_name_hash;
  uint64_t packets;
  uint64_t draws;
  uint64_t triangles;
  uint64_t missing_models;
} goal_jak2_merc_model_diagnostic_metrics;

// Numeric identity for a retained unhealthy required Merc palette slot.
// issue_mask zero means no event has been observed in this host session.
// Lane bits 0..15 are the transform and 16..27 are the three padded normal vectors;
// normal-vector W lanes 19, 23, and 27 are not consumed by either Merc shader.
typedef struct goal_jak2_merc_palette_health_event {
  uint32_t issue_mask;
  uint32_t nonfinite_lane_mask;
  int32_t bone_slot;
  uint32_t source_address;
  uint64_t model_name_hash;
  uint64_t matrix_hash;
} goal_jak2_merc_palette_health_event;

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

typedef struct goal_jak2_effects_bucket315_metrics {
  uint64_t captures;
  uint64_t valid_captures;
  uint64_t malformed_captures;
  uint64_t absent_captures;
  uint64_t lightning_captures;
  uint64_t other_captures;
  uint64_t payload_bytes;
  uint32_t last_transfer_count;
  uint32_t last_fragment_count;
  uint32_t last_vertex_count;
  uint64_t last_payload_bytes;
  uint64_t last_semantic_fingerprint;
  uint8_t last_classification;
} goal_jak2_effects_bucket315_metrics;

typedef struct goal_jak2_effects_bucket315_execution_metrics {
  uint64_t callback_dispatches;
  uint64_t completed_executions;
  uint32_t last_expected_fragments;
  uint32_t last_expected_vertices;
  uint32_t last_expected_adgifs;
  uint32_t last_expected_draws;
  uint32_t last_actual_fragments;
  uint32_t last_actual_vertices;
  uint32_t last_actual_adgifs;
  uint32_t last_actual_draw_buckets;
  uint32_t last_actual_draws;
  uint32_t last_actual_missing_textures;
  uint32_t last_actual_placeholder_draws;
  uint32_t last_actual_unsupported_blends;
  uint32_t last_actual_unexpected_dma;
  uint32_t last_actual_overflow;
} goal_jak2_effects_bucket315_execution_metrics;

typedef struct goal_jak2_warp_texture_upload_metrics {
  uint64_t observations;
  uint64_t absent;
  uint64_t ordinary;
  uint64_t unclassified;
  uint32_t last_upload_count;
  uint32_t last_transfer_count;
  uint64_t last_payload_bytes;
  uint64_t last_semantic_fingerprint;
} goal_jak2_warp_texture_upload_metrics;

typedef struct goal_jak2_shadow_bucket195_metrics {
  uint64_t observations;
  uint64_t absent;
  uint64_t observed;
  uint64_t malformed;
  uint64_t limit_exceeded;
  uint32_t last_transfer_count;
  uint32_t last_v4_32_transfer_count;
  uint32_t last_v4_8_transfer_count;
  uint32_t last_v4_32_unpack_count;
  uint32_t last_v4_8_unpack_count;
  uint32_t last_direct_transfer_count;
  uint64_t last_total_payload_bytes;
  uint64_t last_direct_payload_bytes;
  uint64_t last_flusha_direct_payload_bytes;
  uint64_t last_semantic_fingerprint;
  uint16_t last_terminal_qwc;
  uint8_t last_terminal_tag_kind;
  uint8_t last_terminal_vif0_kind;
  uint8_t last_terminal_vif1_kind;
  uint8_t last_status;
  uint8_t last_reached_boundary;
} goal_jak2_shadow_bucket195_metrics;

typedef struct goal_jak2_shadow_bucket195_execution_metrics {
  uint64_t completed_executions;
  uint32_t last_expected_disposition;
  uint32_t last_expected_batches;
  uint32_t last_expected_vertices;
  uint32_t last_expected_records;
  uint32_t last_actual_executions;
  uint32_t last_actual_absent;
  uint32_t last_actual_ready;
  uint32_t last_actual_deferred_no_draw;
  uint32_t last_actual_input_batches;
  uint32_t last_actual_input_vertices;
  uint32_t last_actual_input_records;
  uint32_t last_actual_output_vertices;
  uint32_t last_actual_front_triangles;
  uint32_t last_actual_back_triangles;
  uint32_t last_actual_draws;
  uint32_t last_actual_triangles;
  uint32_t last_actual_darken_draws;
  uint32_t last_actual_lighten_draws;
  uint32_t last_actual_unexpected_dma;
  uint32_t last_actual_invalid_plan;
  uint32_t last_actual_nonfinite_projection;
  uint32_t last_actual_overflow;
  uint32_t last_actual_pipeline_failures;
  uint32_t last_actual_reached_boundary;
} goal_jak2_shadow_bucket195_execution_metrics;

typedef struct goal_jak2_gmerc_warp_bucket317_metrics {
  uint64_t observations;
  uint64_t absent;
  uint64_t setup_only;
  uint64_t fragments;
  uint64_t malformed;
  uint32_t last_transfer_count;
  uint32_t last_fragment_count;
  uint32_t last_continued_fragment_count;
  uint32_t last_vertex_count;
  uint32_t last_adgif_count;
  uint64_t last_payload_bytes;
  uint64_t last_semantic_fingerprint;
  uint8_t last_variant;
} goal_jak2_gmerc_warp_bucket317_metrics;

typedef struct goal_jak2_gmerc_warp_bucket317_execution_metrics {
  uint64_t callback_dispatches;
  uint64_t completed_executions;
  uint32_t last_expected_fragments;
  uint32_t last_expected_continued_fragments;
  uint32_t last_expected_vertices;
  uint32_t last_expected_adgifs;
  uint32_t last_actual_fragments;
  uint32_t last_actual_continued_fragments;
  uint32_t last_actual_vertices;
  uint32_t last_actual_adgifs;
  uint32_t last_actual_draw_buckets;
  uint32_t last_actual_draws;
  uint32_t last_actual_triangles;
  uint32_t last_actual_missing_textures;
  uint32_t last_actual_placeholder_draws;
  uint32_t last_actual_missing_publications;
  uint32_t last_actual_unsupported_blends;
  uint32_t last_actual_unexpected_dma;
  uint32_t last_actual_overflow;
  uint32_t last_snapshot_publications;
  uint32_t last_snapshot_copies;
  uint32_t last_snapshot_allocations;
  uint32_t last_snapshot_replacements;
  uint32_t last_snapshot_failures;
  uint64_t last_snapshot_texture;
} goal_jak2_gmerc_warp_bucket317_execution_metrics;

typedef struct goal_jak2_subtitle_bucket322_typed_metrics {
  uint64_t observations;
  uint64_t matched;
  uint64_t rejected;
  uint64_t semantic_mismatches;
  uint64_t variants[GOAL_JAK2_SUBTITLE322_VARIANT_COUNT];
  uint64_t reject_reasons[GOAL_JAK2_SUBTITLE322_REJECT_REASON_COUNT];
  uint32_t last_status;
  uint32_t last_variant;
  uint32_t last_reject_reason;
  uint32_t last_reject_transfer_index;
  uint32_t last_transfer_count;
  uint32_t last_linker_transfers;
  uint32_t last_direct_transfers;
  uint32_t last_opaque_direct_transfers;
  uint32_t last_hud_sprite_pairs;
  uint32_t last_image_upload_count;
  uint64_t last_direct_payload_bytes;
  uint64_t last_semantic_fingerprint;
} goal_jak2_subtitle_bucket322_typed_metrics;

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
  uint64_t prison_clut_preparations;
  uint64_t prison_clut_publications;
  uint64_t prison_clut_textures[GOAL_JAK2_PRISON_CLUT_OUTPUT_COUNT];
  uint32_t prison_clut_destination_tbps[GOAL_JAK2_PRISON_CLUT_OUTPUT_COUNT];
  uint32_t prison_clut_anim_slots[GOAL_JAK2_PRISON_CLUT_OUTPUT_COUNT];
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
  uint64_t last_merc_anim_slot_draws;
  uint64_t last_merc_anim_slot_placeholder_draws;
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
  // Keep diagnostic growth at the tail so every established metrics field retains its offset.
  goal_jak2_tfrag_texture_upload_metrics
      pris2_bucket_captures[GOAL_JAK2_PRIS2_CAPTURE_BUCKET_COUNT];
  uint64_t
      last_merc_anim_slot_draws_by_slot[GOAL_JAK2_MERC_ANIM_SLOT_DIAGNOSTIC_COUNT];
  uint64_t last_merc_anim_slot_placeholder_draws_by_slot
      [GOAL_JAK2_MERC_ANIM_SLOT_DIAGNOSTIC_COUNT];
  uint64_t last_merc_anim_slot_first_model_hashes
      [GOAL_JAK2_MERC_ANIM_SLOT_DIAGNOSTIC_COUNT];
  // Append-only live preflight telemetry for exact bucket 315.
  goal_jak2_effects_bucket315_metrics effects_bucket315;
  // Append-only execution telemetry for exact typed bucket 309 sky-post uploads.
  uint64_t sky_post_texture_upload_executions;
  // Append-only capture telemetry for exact bucket 316 ordinary upload plans.
  goal_jak2_warp_texture_upload_metrics warp_texture_upload;
  // Append-only capture and execution telemetry for exact common-water bucket 306.
  goal_jak2_tfrag_texture_upload_metrics common_water_texture_upload;
  // Append-only passive metadata. Bucket 322 remains DeferredSkip with no execution route.
  goal_jak2_tfrag_texture_upload_metrics subtitle_capture;
  // Append-only passive metadata. Bucket 195 remains DeferredSkip with no execution route.
  goal_jak2_shadow_bucket195_metrics shadow_bucket195;
  // Append-only passive metadata. Bucket 317 remains DeferredSkip with no execution route.
  goal_jak2_gmerc_warp_bucket317_metrics gmerc_warp_bucket317;
  // Append-only runtime diagnostics for the active BlitDisplays renderer.
  uint32_t last_blit_display_snapshot_requested;
  uint32_t last_blit_display_copy_back_requested;
  uint32_t last_blit_display_copy_back_performed;
  uint32_t last_blit_display_texture_tbp;
  uint32_t last_blit_display_texture_lookup_hit;
  uint32_t last_blit_display_used_placeholder;
  // Append-only execution count for source-ordered bucket 316 ordinary uploads.
  uint64_t warp_texture_upload_executions;
  // Append-only exact execution telemetry for source Lightning bucket 315.
  goal_jak2_effects_bucket315_execution_metrics effects_bucket315_execution;
  // Append-only exact execution telemetry for source framebuffer-warp bucket 317.
  goal_jak2_gmerc_warp_bucket317_execution_metrics gmerc_warp_bucket317_execution;
  // Append-only exact execution telemetry for source Shadow2 bucket 195.
  goal_jak2_shadow_bucket195_execution_metrics shadow_bucket195_execution;
  // Append-only bounded standard-Merc packet and encoded-draw identity for the last chain.
  uint32_t last_merc_model_diagnostic_count;
  uint64_t last_merc_model_diagnostic_overflow_packets;
  goal_jak2_merc_model_diagnostic_metrics
      last_merc_model_diagnostics[GOAL_JAK2_MERC_MODEL_DIAGNOSTIC_COUNT];
  // Append-only retained identities for the first and latest palette-health events.
  goal_jak2_merc_palette_health_event first_merc_palette_health_event;
  goal_jak2_merc_palette_health_event last_merc_palette_health_event;
  // Append-only passive typed telemetry. Bucket 322 remains DeferredSkip with no execution route.
  goal_jak2_subtitle_bucket322_typed_metrics subtitle_bucket322_typed;
  // Append-only renderer A/B telemetry. These distinguish generated ocean work from loaded
  // background geometry and identify the merged eye renderer's transient-stream vertex upload.
  uint64_t ocean_texture_vertices;
  uint64_t ocean_mid_vertices;
  uint64_t ocean_near_vertices;
  uint64_t last_tfrag_draws;
  uint64_t last_tfrag_triangles;
  uint64_t last_background_unexpected_dma;
  uint64_t last_eye_vertex_stream_uploads;
  uint64_t last_eye_vertex_bytes;
  uint32_t last_eye_vertex_buffer_offset;
  uint64_t last_eye_vertex_fingerprint;
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

enum {
  GOAL_JAK2_SHADOW195_CAPTURE_MATCH_HOST_TICK = 1u << 0,
  GOAL_JAK2_SHADOW195_CAPTURE_MATCH_ENGINE_FRAME = 1u << 1,
  GOAL_JAK2_SHADOW195_CAPTURE_MATCH_PLAN_FINGERPRINT = 1u << 2,
};

enum {
  GOAL_JAK2_SHADOW195_CAPTURE_IDLE = 0,
  GOAL_JAK2_SHADOW195_CAPTURE_ARMED = 1,
  GOAL_JAK2_SHADOW195_CAPTURE_SELECTED = 2,
  GOAL_JAK2_SHADOW195_CAPTURE_COMPLETE = 3,
  GOAL_JAK2_SHADOW195_CAPTURE_FAILED = 4,
};

enum {
  GOAL_JAK2_SHADOW195_CAPTURE_FAILURE_NONE = 0,
  GOAL_JAK2_SHADOW195_CAPTURE_FAILURE_INVALID_CONTEXT = 1,
  GOAL_JAK2_SHADOW195_CAPTURE_FAILURE_UNSUPPORTED_TARGET = 2,
  GOAL_JAK2_SHADOW195_CAPTURE_FAILURE_ALLOCATION = 3,
  GOAL_JAK2_SHADOW195_CAPTURE_FAILURE_RENDERER = 4,
  GOAL_JAK2_SHADOW195_CAPTURE_FAILURE_COMMAND_BUFFER = 5,
  GOAL_JAK2_SHADOW195_CAPTURE_FAILURE_READBACK = 6,
  GOAL_JAK2_SHADOW195_CAPTURE_FAILURE_PLAN_NOT_READY = 7,
};

typedef struct goal_jak2_shadow195_frame_capture_selector {
  uint32_t match;
  uint64_t host_tick_id;
  uint64_t engine_frame_id;
  uint64_t plan_fingerprint;
} goal_jak2_shadow195_frame_capture_selector;

typedef struct goal_jak2_shadow195_pixel_bounds {
  uint32_t valid;
  uint32_t min_x;
  uint32_t min_y;
  uint32_t max_x_exclusive;
  uint32_t max_y_exclusive;
} goal_jak2_shadow195_pixel_bounds;

typedef struct goal_jak2_shadow195_depth_summary {
  uint64_t hash;
  uint64_t finite_pixels;
  uint64_t nonfinite_pixels;
  uint32_t finite_range_valid;
  float min_finite;
  float max_finite;
} goal_jak2_shadow195_depth_summary;

typedef struct goal_jak2_shadow195_stencil_summary {
  uint64_t hash;
  uint64_t nonzero_pixels;
  goal_jak2_shadow195_pixel_bounds nonzero_bounds;
} goal_jak2_shadow195_stencil_summary;

typedef struct goal_jak2_shadow195_frame_capture_result {
  uint32_t status;
  uint32_t failure_reason;
  goal_jak2_shadow195_frame_capture_selector selector;
  uint64_t selected_host_tick_id;
  uint64_t selected_chain_ordinal;
  uint64_t selected_engine_frame_id;
  uint64_t selected_plan_fingerprint;
  uint64_t target_view_id;
  uint32_t target_external;
  uint32_t target_width;
  uint32_t target_height;
  uint32_t target_pixel_format;
  uint32_t target_color_slice;
  uint32_t target_depth_pixel_format;
  uint32_t target_depth_slice;
  uint32_t target_stencil_pixel_format;
  uint32_t target_stencil_slice;
  uint32_t target_texture_type;
  uint32_t target_storage_mode;
  uint32_t target_sample_count;
  uint32_t target_array_length;
  uint32_t target_mipmap_level_count;
  double target_viewport_origin_x;
  double target_viewport_origin_y;
  double target_viewport_width;
  double target_viewport_height;
  double target_viewport_znear;
  double target_viewport_zfar;
  uint32_t target_scissor_x;
  uint32_t target_scissor_y;
  uint32_t target_scissor_width;
  uint32_t target_scissor_height;
  uint32_t target_scissor_explicit;
  uint32_t target_color_load_action;
  uint32_t target_color_store_action;
  uint32_t target_depth_load_action;
  uint32_t target_depth_store_action;
  uint32_t target_stencil_load_action;
  uint32_t target_stencil_store_action;
  double target_render_scale_x;
  double target_render_scale_y;
  uint64_t before_hash;
  uint64_t before_nonzero_pixels;
  goal_jak2_shadow195_pixel_bounds before_nonzero_bounds;
  uint64_t after_hash;
  uint64_t after_nonzero_pixels;
  goal_jak2_shadow195_pixel_bounds after_nonzero_bounds;
  uint64_t changed_pixels;
  goal_jak2_shadow195_pixel_bounds changed_bounds;
  goal_jak2_shadow195_depth_summary before_depth;
  goal_jak2_shadow195_depth_summary after_depth;
  uint64_t changed_depth_pixels;
  goal_jak2_shadow195_pixel_bounds changed_depth_bounds;
  goal_jak2_shadow195_stencil_summary before_stencil;
  goal_jak2_shadow195_stencil_summary volume_stencil;
  goal_jak2_shadow195_stencil_summary after_stencil;
  uint64_t volume_stencil_changed_pixels;
  goal_jak2_shadow195_pixel_bounds volume_stencil_changed_bounds;
  uint64_t final_stencil_changed_pixels;
  goal_jak2_shadow195_pixel_bounds final_stencil_changed_bounds;
  goal_jak2_shadow_bucket195_execution_metrics renderer;
} goal_jak2_shadow195_frame_capture_result;

/*! Create the process-singleton, nil-layer Jak 2 policy-dispatch host. */
goal_jak2_metal_host* goal_jak2_metal_host_create(void);

/*! Create the same external host with an app-owned layer for real-DMA presentation. */
goal_jak2_metal_host* goal_jak2_metal_host_create_presenting(
    goal_jak2_metal_host_layer layer);

/*! Rebind a running presenting host to a replacement app-owned layer after view recreation. */
int goal_jak2_metal_host_rebind_presenting_layer(goal_jak2_metal_host* host,
                                                 goal_jak2_metal_host_layer layer);

/*! Set the game target and centered drawable region selected by the app presentation policy. */
int goal_jak2_metal_host_set_presentation_geometry(
    goal_jak2_metal_host* host,
    const goal_jak2_metal_presentation_geometry* geometry);

/*! Copy the current game-target and draw-region geometry. */
int goal_jak2_metal_host_get_presentation_geometry(
    goal_jak2_metal_host* host,
    goal_jak2_metal_presentation_geometry* out);

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
 * Query or copy the first non-absent, source-exact Shadow2 bucket-195 plan retained by this host.
 * A null `out_bytes` with zero capacity queries availability and required size. An undersized
 * buffer is not modified, but `required_size` and `serialized_fingerprint` are still reported.
 * The fingerprint is FNV-1a over the complete versioned serialization. Returns one only for an
 * available size query or a complete copy; no capture, invalid arguments, and insufficient
 * capacity return zero. Output metadata is cleared when no capture is available.
 */
int goal_jak2_metal_host_copy_shadow_bucket195_plan_capture(goal_jak2_metal_host* host,
                                                            uint8_t* out_bytes,
                                                            uint64_t capacity,
                                                            uint64_t* required_size,
                                                            uint64_t* serialized_fingerprint);

/*!
 * Arm one private, in-memory Shadow195 comparison. At least one exact selector field is required.
 * A matching Ready plan executes through the existing renderer without changing bucket policy;
 * its color, depth, and stencil checkpoints are reduced to numeric summaries, then discarded.
 */
int goal_jak2_metal_host_arm_shadow195_frame_capture(
    goal_jak2_metal_host* host,
    const goal_jak2_shadow195_frame_capture_selector* selector);

/*! Copy the current fixed-size numeric result. Raw attachment pixels are never exposed. */
int goal_jak2_metal_host_get_shadow195_frame_capture(
    goal_jak2_metal_host* host,
    goal_jak2_shadow195_frame_capture_result* out);

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
