#pragma once

/*!
 * @file metal_pipeline.h
 * Experimental Metal graphics pipeline for Apple platforms.
 *
 * This implements the same GfxRendererModule interface that the OpenGL pipeline
 * (game/graphics/pipelines/opengl.cpp) sits behind. The frame scaffolding
 * (per-frame command buffer, offscreen game target, PSO cache, PCRTC-style
 * present pass) is in place; the game passes currently render a fixed
 * validation scene instead of the game's DMA chain.
 *
 * This header is plain C++ so tests can include it; the Objective-C++
 * implementation lives in metal_renderer.h/.mm and metal_pipeline.mm.
 */

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/custom_data/Tfrag3Data.h"
#include "common/math/Vector.h"

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"
#include "game/graphics/pipelines/metal/metal_camera_trace.h"
#include "game/graphics/pipelines/metal/metal_merc_skin_trace.h"
#include "game/graphics/pipelines/metal/metal_merc_transform_trace.h"

extern const GfxRendererModule gRendererMetal;

class TexturePool;

namespace tfrag3 {
struct Texture;
struct Level;
}

namespace metal_renderer {

inline constexpr std::size_t kTrackedDeferredBuckets = 4;
inline constexpr std::size_t kMercModelDiagnosticCapacity = 16;

struct MercModelDiagnostic {
  u32 bucket_id = 0;
  u64 model_name_hash = 0;
  u64 packets = 0;
  u64 draws = 0;
  u64 triangles = 0;
  u64 missing_models = 0;
};

// RGBA8 copy of a rendered frame, used by tests to verify that rendering
// actually happened. Origin is the top-left corner.
struct FramePixels {
  int width = 0;
  int height = 0;
  std::vector<u8> rgba;
};

// Cache, frame, and submission counters used by tests to verify readback identity, stream reuse,
// and that PSOs are reused rather than rebuilt per draw or per frame.
struct ScaffoldStats {
  u64 frames_rendered = 0;
  u64 last_internal_frame_submission = 0;
  u64 stream_reuse_waits = 0;
  u64 pso_count = 0;
  u64 depth_stencil_count = 0;
  u64 pso_misses = 0;
  u64 pso_hits = 0;
};

// Settings for present-pass verification (subset of the game's per-frame
// options; mirrors the fields of RenderOptions the present pass consumes).
struct PresentTestOptions {
  int window_w = 0;
  int window_h = 0;
  int draw_region_w = 0;
  int draw_region_h = 0;
  float pmode_alp = 1.f;
  int brightness_contrast_color = 0;
  int brightness_contrast_alpha = 128;
};

struct ExternalRenderTargetProofResult {
  FramePixels rendered_slice;
  u64 view_id = 0;
  bool internal_readback_identity_preserved = false;
  bool internal_readback_pixels_preserved = false;
  bool internal_readback_bookkeeping_preserved = false;
  bool stream_reuse_synchronized = false;
  bool invalid_descriptors_rejected = false;
  bool invalid_descriptors_preserved_stats = false;
  bool color_slice_zero_preserved = false;
  bool framebuffer_copy_used_selected_slice = false;
};

enum MercPaletteHealthIssue : u8 {
  MERC_PALETTE_HEALTH_NONFINITE = 1 << 0,
  MERC_PALETTE_HEALTH_DEGENERATE = 1 << 1,
  MERC_PALETTE_HEALTH_SOURCE_BASE = 1 << 2,
};

// Numeric evidence retained for an unhealthy matrix in an enabled, weighted Merc palette slot.
// This is diagnostic only: the renderer still uploads and draws the original matrix bytes.
struct MercPaletteHealthEvent {
  u8 issue_mask = 0;
  u32 nonfinite_lane_mask = 0;
  int bone_slot = -1;
  u64 model_name_hash = 0;
  u64 matrix_hash = 0;
  double axis_norm_x = 0.0;
  double axis_norm_y = 0.0;
  double axis_norm_z = 0.0;
  double normalized_abs_determinant = 0.0;
  u32 source_address = 0;
  u64 source_base = 0;
  u64 expected_source_base = 0;

  bool valid() const { return issue_mask != 0; }
};

struct DirectBatchStats {
  bool valid = false;
  bool textured = false;
  int vertices = 0;
  int nonzero_rgb_vertices = 0;
  u32 tex0_tbp = 0;
  bool tex0_tcc = false;
  bool tex0_decal = false;
  bool texture_lookup_hit = false;
  bool used_placeholder = false;
  bool write_rgb = false;
  bool blend_enabled = false;  // GS PRIM.ABE for the batch
  u8 blend_a = 0;
  u8 blend_b = 0;
  u8 blend_c = 0;
  u8 blend_d = 0;
  bool alpha_test_enabled = false;
  u8 alpha_test_mode = 0;
  u8 alpha_aref = 0;
  u8 alpha_afail = 0;
};

// Counters for the DMA-chain path, used by tests to verify that send_chain
// frames really dispatched buckets and that deferred content is counted.
struct ChainStats {
  u64 chains_rendered = 0;
  u64 command_buffers_committed = 0;
  u64 command_buffers_completed = 0;
  u64 command_buffer_errors = 0;
  int last_command_buffer_status = 0;
  s64 last_command_buffer_error_code = 0;
  u64 last_buckets_dispatched = 0;
  u64 drawables_acquired = 0;
  u64 drawable_misses = 0;
  u64 late_present_submissions = 0;
  // End-to-end provenance for the last chain. Live camera qwords are camera-temp[0...3] then
  // trans. Render qwords are camera-temp[0...3], hvdf-off, fog.x, trans, camera-rot[0...3], then
  // perspective[0...3].
  u64 last_host_tick_id = 0;
  u64 last_chain_ordinal = 0;
  u64 last_engine_frame_id = 0;
  u64 last_camera_fingerprint = 0;
  int last_camera_packets = 0;
  int last_live_camera_mismatches = 0;
  int last_packet_camera_mismatches = 0;
  u8 last_live_camera_mismatch_qwords = 0;
  u8 last_packet_camera_mismatch_qwords = 0;
  u64 live_camera_mismatches = 0;
  u64 packet_camera_mismatches = 0;
  u64 last_render_camera_fingerprint = 0;
  int last_render_camera_live_mismatches = 0;
  int last_render_camera_packet_mismatches = 0;
  u16 last_render_camera_live_mismatch_qwords = 0;
  u16 last_render_camera_packet_mismatch_qwords = 0;
  u64 render_camera_live_mismatches = 0;
  u64 render_camera_packet_mismatches = 0;
  u64 render_camera_alternations = 0;
  metal_camera_trace::RetainedAlternationObservation last_render_camera_alternation;
  u64 producer_camera_alternations = 0;
  u64 last_camera_alternation_older_frame_id = 0;
  u64 last_camera_alternation_previous_frame_id = 0;
  u64 last_camera_alternation_current_frame_id = 0;
  u64 last_camera_alternation_older_fingerprint = 0;
  u64 last_camera_alternation_previous_fingerprint = 0;
  u64 last_camera_alternation_current_fingerprint = 0;
  u64 last_camera_alternation_host_tick_id = 0;
  u64 last_camera_alternation_chain_ordinal = 0;
  u64 last_camera_alternation_packet_fingerprint = 0;
  u8 last_camera_alternation_live_mismatch_qwords = 0;
  u8 last_camera_alternation_packet_mismatch_qwords = 0;
  u16 last_camera_alternation_render_live_mismatch_qwords = 0;
  u16 last_camera_alternation_render_packet_mismatch_qwords = 0;
  double last_camera_older_to_previous_distance = 0.0;
  double last_camera_previous_to_current_distance = 0.0;
  double last_camera_older_to_current_distance = 0.0;
  u64 submissions = 0;
  u64 presentations_completed = 0;
  u64 presentation_drops = 0;
  u64 presentation_order_mismatches = 0;
  u64 last_submission_id = 0;
  u64 last_presented_submission_id = 0;
  u64 last_dropped_submission_id = 0;
  u64 last_drawable_id = 0;
  u64 last_presented_drawable_id = 0;
  u64 last_presented_engine_frame_id = 0;
  u64 last_presented_host_tick_id = 0;
  u64 last_presented_chain_ordinal = 0;
  double last_requested_presentation_time = 0.0;
  double last_actual_presentation_time = 0.0;
  // from the last chain frame
  int draw_calls = 0;
  int triangles = 0;
  int jak2_sky_draw_draws = 0;
  int jak2_sky_draw_triangles = 0;
  DirectBatchStats jak2_sky_draw_last_batch;
  bool jak2_blit_display_plan_valid = false;
  bool jak2_blit_display_snapshot_requested = false;
  bool jak2_blit_display_copy_back_requested = false;
  bool jak2_blit_display_copy_back_performed = false;
  bool jak2_blit_display_texture_lookup_hit = false;
  bool jak2_blit_display_used_placeholder = false;
  u64 jak2_blit_display_texture_handle = 0;
  u32 jak2_blit_display_texture_tbp = 0;
  u64 jak2_blit_display_unsupported_pc_ports = 0;
  int jak2_screen_filter_draws = 0;
  int jak2_screen_filter_triangles = 0;
  int jak2_progress_draws = 0;
  int jak2_progress_triangles = 0;
  int jak2_progress_textured_draws = 0;
  int jak2_progress_missing_texture_draws = 0;
  int jak2_debug_no_zbuf1_draws = 0;
  int jak2_debug_no_zbuf1_triangles = 0;
  int jak2_debug_no_zbuf1_textured_draws = 0;
  int jak2_debug_no_zbuf1_missing_texture_draws = 0;
  int jak2_debug_no_zbuf2_draws = 0;
  int jak2_debug_no_zbuf2_triangles = 0;
  int tex_uploads = 0;
  int sky_draws = 0;
  int sky_blends = 0;
  int cloud_draws = 0;
  int cloud_blends = 0;
  // sprite bucket, from the last chain frame
  int sprites_2d = 0;
  int sprites_3d = 0;
  int sprites_hud = 0;
  int sprites_distort = 0;  // DMA consumed; distort drawing is not ported
  int sprite_normal_submitted = 0;
  int sprite_glow_marked = 0;
  int sprite_glow_skipped = 0;
  int sprite_glow_parsed = 0;
  int sprite_glow_accepted = 0;
  int sprite_glow_rejected = 0;
  int sprite_glow_invalid_records = 0;
  // Legacy host-facing names: these are visibility-tested final flare counts.
  int sprite_glow_force_visible_submitted = 0;
  int sprite_glow_force_visible_drawn = 0;
  int sprite_glow_force_visible_draws = 0;
  int sprite_glow_force_visible_triangles = 0;
  int sprite_glow_force_visible_missing_textures = 0;
  int sprite_draws = 0;
  int sprite_triangles = 0;
  int sprite_missing_textures = 0;
  int sprite_placeholder_batches = 0;
  int sprite_placeholder_sprites = 0;
  bool sprite_first_placeholder_valid = false;
  u32 sprite_first_placeholder_tbp = 0;
  u32 sprite_first_placeholder_draw_mode = 0;
  u64 sprite_unsupported_bytes = 0;
  // ocean buckets, from the last chain frame
  int ocean_draws = 0;
  int ocean_triangles = 0;
  int ocean_texture_verts = 0;  // VU-produced vertices of the generated ocean texture
  int ocean_mid_verts = 0;
  int ocean_near_verts = 0;
  int ocean_missing_textures = 0;
  int ocean_command_buffers_committed = 0;
  int ocean_command_buffers_completed = 0;
  int ocean_command_buffer_errors = 0;
  int ocean_last_command_buffer_status = 0;
  // registry handles of the two generated ocean textures (mipmapped one from
  // ocean-mid-and-far, single-level one from ocean-near), so tests can read
  // them back
  u64 ocean_mid_texture = 0;
  u64 ocean_near_texture = 0;
  // merc buckets, from the last chain frame
  int merc_models = 0;
  int merc_missing_models = 0;  // the model's level is not loaded
  std::array<MercModelDiagnostic, kMercModelDiagnosticCapacity> merc_model_diagnostics = {};
  std::size_t merc_model_diagnostic_count = 0;
  u64 merc_model_diagnostic_overflow_packets = 0;
  int merc_malformed_dma = 0;
  u32 merc_preflight_rejection_reason = 0;
  int merc_draws = 0;
  int merc_triangles = 0;
  int merc_envmap_draws = 0;
  int merc_bone_vectors = 0;
  int merc_mod_vtx_uploads = 0;  // effects whose blerc / mod-vertex update was uploaded
  int merc_mod_vtx_skipped = 0;  // effects that asked for one but could not be updated
  int merc_anim_slot_draws = 0;
  int merc_anim_slot_placeholder_draws = 0;
  std::array<int, 4> merc_anim_slot_draws_by_slot = {};
  std::array<int, 4> merc_anim_slot_placeholder_draws_by_slot = {};
  std::array<u64, 4> merc_anim_slot_first_model_hashes = {};
  int merc_eye_draws = 0;        // draws whose texture the eye renderer composed
  int merc_eye_renderer_missing = 0;
  int merc_eye_lookup_failed = 0;
  int merc_eye_placeholder_draws = 0;
  int merc_missing_textures = 0;
  int merc_bad_bone_pointers = 0;  // bone pointer outside EE memory
  int merc_bad_draw_ranges = 0;    // draw range outside the level's index buffer
  int merc_missing_bone_slots = 0;
  int merc_models_with_missing_bone_slots = 0;
  int merc_nonfinite_bone_matrices = 0;
  int merc_degenerate_bone_matrices = 0;
  int merc_incoherent_bone_sources = 0;
  int merc_models_with_palette_health_issues = 0;
  int merc_eichar_palette_health_issues = 0;
  int merc_eichar_transform_discontinuities = 0;
  int merc_eichar_provenance_events = 0;
  int merc_eichar_output_composition_mismatches = 0;
  int merc_eichar_target_control_events = 0;
  int merc_eichar_target_control_divergences = 0;
  int merc_eichar_target_control_attack_boundaries = 0;
  int merc_eichar_target_control_capture_attempts = 0;
  int merc_eichar_target_control_valid_observations = 0;
  metal_merc_skin_trace::SkinStats merc_eichar_weighted_skin;
  metal_merc_skin_trace::DuplicationStats merc_eichar_duplication;
  jak1_target_control_capture::Stage last_merc_eichar_target_control_capture_stage =
      jak1_target_control_capture::Stage::NOT_ATTEMPTED;
  jak1_target_control_capture::Result last_merc_eichar_target_control_capture_result =
      jak1_target_control_capture::Result::NOT_ATTEMPTED;
  metal_merc_transform_trace::TargetControlObservation
      last_merc_eichar_target_control_observation;
  // Retained across clean frames so an intermittent one-frame failure remains inspectable.
  MercPaletteHealthEvent first_merc_palette_health_event;
  MercPaletteHealthEvent last_merc_palette_health_event;
  MercPaletteHealthEvent first_merc_eichar_palette_health_event;
  MercPaletteHealthEvent last_merc_eichar_palette_health_event;
  metal_merc_transform_trace::Event first_merc_eichar_transform_discontinuity;
  metal_merc_transform_trace::Event last_merc_eichar_transform_discontinuity;
  metal_merc_transform_trace::Event first_merc_eichar_provenance_event;
  metal_merc_transform_trace::Event last_merc_eichar_provenance_event;
  metal_merc_transform_trace::Event first_merc_eichar_output_composition_mismatch;
  metal_merc_transform_trace::Event last_merc_eichar_output_composition_mismatch;
  metal_merc_transform_trace::TargetControlEvent first_merc_eichar_target_control_event;
  metal_merc_transform_trace::TargetControlEvent last_merc_eichar_target_control_event;
  // eye renderer, from the last chain frame
  int eyes_composed = 0;
  int eye_draws = 0;
  int eye_triangles = 0;
  int eye_missing_textures = 0;
  int eye_unexpected_dma = 0;
  int eye_duplicate_slot_writes = 0;
  int eye_command_buffers_committed = 0;
  int eye_command_buffers_completed = 0;
  int eye_command_buffer_errors = 0;
  int eye_last_command_buffer_status = 0;
  // registry handle of the first eye composed this frame, so tests can read it
  u64 eye_texture = 0;
  // generic2 buckets, from the last chain frame
  int generic_fragments = 0;
  int generic_vertices = 0;
  int generic_adgifs = 0;
  int generic_draw_buckets = 0;
  int generic_draws = 0;
  int generic_triangles = 0;
  int generic_missing_textures = 0;
  int generic_unsupported_blends = 0;
  int generic_unexpected_dma = 0;
  int generic_overflow = 0;
  // Exact Jak II EFFECTS bucket 315, kept separate from normal Generic2 buckets.
  int effects315_fragments = 0;
  int effects315_vertices = 0;
  int effects315_adgifs = 0;
  int effects315_draw_buckets = 0;
  int effects315_draws = 0;
  int effects315_triangles = 0;
  int effects315_missing_textures = 0;
  int effects315_placeholder_draws = 0;
  int effects315_unsupported_blends = 0;
  int effects315_unexpected_dma = 0;
  int effects315_overflow = 0;
  // Exact Jak II GMERC_WARP bucket 317, kept separate from normal Generic2 buckets.
  int warp317_fragments = 0;
  int warp317_continued_fragments = 0;
  int warp317_vertices = 0;
  int warp317_adgifs = 0;
  int warp317_draw_buckets = 0;
  int warp317_draws = 0;
  int warp317_triangles = 0;
  int warp317_missing_textures = 0;
  int warp317_placeholder_draws = 0;
  int warp317_missing_publications = 0;
  int warp317_unsupported_blends = 0;
  int warp317_unexpected_dma = 0;
  int warp317_overflow = 0;
  u64 warp317_snapshot_publications = 0;
  u64 warp317_snapshot_copies = 0;
  u64 warp317_snapshot_allocations = 0;
  u64 warp317_snapshot_replacements = 0;
  u64 warp317_snapshot_failures = 0;
  u64 warp317_snapshot_texture = 0;
  // shadow renderer, from the last chain frame
  int shadow_volumes = 0;
  int shadow_vertices = 0;
  int shadow_draws = 0;
  int shadow_triangles = 0;
  int shadow_unexpected_dma = 0;
  // Exact Jak II SHADOW bucket 195, kept separate from Jak 1 ShadowVu telemetry.
  int shadow195_executions = 0;
  int shadow195_absent = 0;
  int shadow195_ready = 0;
  int shadow195_deferred_no_draw = 0;
  int shadow195_input_batches = 0;
  int shadow195_input_vertices = 0;
  int shadow195_input_records = 0;
  int shadow195_output_vertices = 0;
  int shadow195_front_triangles = 0;
  int shadow195_back_triangles = 0;
  int shadow195_draws = 0;
  int shadow195_triangles = 0;
  int shadow195_darken_draws = 0;
  int shadow195_lighten_draws = 0;
  int shadow195_unexpected_dma = 0;
  int shadow195_invalid_plan = 0;
  int shadow195_nonfinite_projection = 0;
  int shadow195_overflow = 0;
  int shadow195_pipeline_failures = 0;
  bool shadow195_reached_boundary = false;
  // cumulative
  u64 skipped_bucket_bytes = 0;    // DMA consumed by not-yet-ported bucket renderers
  u64 skipped_tfrag_bytes = 0;     // tfrag-trans content in the sky-blend buckets
  // largest deferred buckets in the last frame, descending by payload bytes
  int last_skipped_bucket_count = 0;
  std::array<u32, kTrackedDeferredBuckets> last_skipped_bucket_ids = {};
  std::array<u64, kTrackedDeferredBuckets> last_skipped_bucket_bytes = {};
  int direct_unsupported_blends = 0;
};

ChainStats get_chain_stats();

// Blocks until the last submitted frame finishes on the GPU, then reads back
// the offscreen game render target. Returns false if no frame has been rendered.
bool read_last_frame(FramePixels* out);

// Metal-proof hook: renders the most recently copied DMA chain into slice 1 of host-owned
// two-slice color/depth textures and reads that color slice back. Slice 0 is initialized with a
// sentinel and checked after rendering.
bool render_last_chain_to_external_target(int width,
                                          int height,
                                          ExternalRenderTargetProofResult* out);

// Renders the present (letterbox) pass into an offscreen window-sized target
// using the same encoder path the on-screen drawable gets, then reads it back.
bool read_present_frame(const PresentTestOptions& opts, FramePixels* out);

ScaffoldStats get_stats();

// Replay/test hook: use this GOAL symbol-table pointer instead of the live
// kernel's `offset_of_s7()`. A replay runs without a booted GOAL kernel, so the
// kernel's value has no meaning there, while a capture carries the value its
// frame actually ran with. 0 restores the kernel's value.
void set_s7_override(u32 s7_ptr);

// Create the next display's window hidden. The CAMetalLayer still renders and
// can be read back, so the proof and the chain replay run headless: no window
// appears and nothing steals focus. Must be called before make_display.
void set_window_hidden(bool hidden);

// Physical A/B diagnostic: disable only Jak 1 TIE's reflective second pass.
// The envmapped base geometry, Merc envmaps, camera, and all other buckets are
// unchanged. Enabled by default; callers must opt out explicitly.
void set_jak1_tie_envmap_second_pass_enabled(bool enabled);

// Physical A/B diagnostic: disable only the final color-writing composite of
// Jak 1 shadow volumes. The same shadow DMA and VU work are still consumed so
// this isolates shadow output from level material and depth behavior. Enabled
// by default; callers must opt out explicitly.
void set_jak1_shadow_output_enabled(bool enabled);
bool jak1_shadow_output_enabled();

// --- texture path (plain C++ mirror of the Objective-C++ API in
// metal_texture.h, so tests can drive it) ----------------------------------

// A textured-quad readback: sample `texture` over the uv range [u0,u1]x[v0,v1]
// into an out_w x out_h target with the requested sampler state. Row 0 of the
// result is the v0 edge. Minification (out size < sampled texel count) drives
// mip selection when mip_mode is enabled.
struct TextureSampleSpec {
  u64 texture = 0;  // registry handle (also what TexturePool::lookup returns)
  int out_w = 4;
  int out_h = 4;
  float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
  bool min_linear = false;
  bool mag_linear = false;
  int mip_mode = 0;  // 0 = mips off, 1 = nearest mip, 2 = linear between mips
  bool wrap_s_repeat = false;  // false = clamp to edge
  bool wrap_t_repeat = false;
  int max_aniso = 1;
};

// Renders and reads back the sample quad. Requires a created display.
bool read_texture_sample(const TextureSampleSpec& spec, FramePixels* out);

// The Metal pipeline's texture pool (null before make_display).
TexturePool* get_texture_pool();

// Uploads RGBA8888 pixels (with a full GPU-generated mip chain) and returns
// the registry handle, or 0 on failure.
u64 upload_texture_rgba8(const u8* data, int w, int h);

// Number of registry-owned MTLTexture handles. Used by lifecycle proofs to
// verify that failed and unloaded levels publish no residual resources.
size_t texture_registry_live_count();

// Mirror of the GL loader's add_texture against the pipeline's pool.
u64 pool_add_texture(const tfrag3::Texture& tex, bool is_common);

// --- level data (plain C++ mirror of metal_level_data.h) -------------------

// What load_level_fr3 uploaded, for reporting and tests.
struct LevelLoadResult {
  bool ok = false;
  std::string error;
  std::string level_name;
  int textures = 0;
  int tfrag_trees = 0;  // in the first geometry level of detail
  int tie_trees = 0;
  int shrub_trees = 0;
  u64 vertex_bytes = 0;  // across every geometry level of detail
  u64 index_bytes = 0;
};

// Loads one extracted level (.fr3) into the Metal renderer: textures into the
// pool and tfrag / tie / shrub geometry into GPU buffers. This is what the GL
// streaming Loader does for a level, in one call. Requires a created display.
LevelLoadResult load_level_fr3(const std::string& path, bool is_common);

// Frees every loaded background and Merc level and their texture handles.
void unload_all_levels();

// Per-frame background-renderer counters from the last chain frame.
struct BackgroundStats {
  int tfrag_draws = 0;
  int tfrag_tris = 0;
  int tie_draws = 0;
  int tie_tris = 0;
  int shrub_draws = 0;
  int shrub_tris = 0;
  int missing_levels = 0;
  int missing_textures = 0;
  int anim_slot_draws = 0;
  int tie_envmap_second_draws = 0;
  int tie_envmap_second_tris = 0;
  int tie_wind_draws_skipped = 0;
  int unexpected_dma = 0;
};
BackgroundStats get_background_stats();

// Test hooks for the portable ports of background_common's pure computations
// (metal_level_data.h). Exposed here because the GL originals live behind the
// OpenGL renderer's header chain, which Objective-C++ translation units in the
// Metal path do not include - so the two can only be diffed from plain C++.
void interp_time_of_day_for_test(const math::Vector<s32, 4> itimes[4],
                                 const tfrag3::PackedTimeOfDay& colors,
                                 math::Vector<u8, 4>* out);
void cull_check_all_slow_for_test(const math::Vector4f* planes,
                                  const std::vector<tfrag3::VisNode>& nodes,
                                  const u8* level_occlusion_string,
                                  u8* out);
void background_camera_matrix_for_test(const math::Vector4f rotation[4],
                                       const math::Vector4f perspective[4],
                                       float fog_constant,
                                       float hvdf_z,
                                       math::Vector4f out[4]);

// Sizes of the layout mirrors in metal_level_data.h, so the proof can check
// them against the GL definitions they duplicate.
size_t sizeof_pc_port_data_mirror();
size_t sizeof_camera_data_mirror();
// --- merc model data (see metal_merc_model_pool.h) --------------------------

// What one extracted level contributed to the merc model pool.
struct MercLevelLoad {
  std::string level_name;
  int textures = 0;
  int models = 0;
  u32 vertices = 0;
  u32 indices = 0;
};

// Loads an extracted level (.fr3): its textures go into the pool exactly as
// pool_add_texture does, and its merc models/geometry become available to the
// merc bucket renderers. Requires a created display. False on failure, with a
// message in `error`.
bool merc_load_fr3(const std::string& path,
                   bool is_common,
                   MercLevelLoad* out,
                   std::string* error);

// Same, for a level built in memory: the proof checks the merc path with a
// synthetic level so it needs no game data.
bool merc_add_level(std::unique_ptr<tfrag3::Level> level,
                    bool is_common,
                    MercLevelLoad* out,
                    std::string* error);

// --- frame pacing ----------------------------------------------------------

// How long each presented frame must stay on screen, in seconds; 0 presents at
// the next vsync, whatever the display's rate is. See MetalRenderOptions::
// min_present_duration for why a 60 fps game on a 120 Hz display needs this.
void set_present_pacing(double seconds);

// Presentation intervals since the last call, in milliseconds: this is what
// says whether frames are arriving regularly, which an average frame rate
// cannot.
struct PresentTiming {
  int frames = 0;
  double mean_ms = 0;
  double min_ms = 0;
  double max_ms = 0;
  double stddev_ms = 0;
  int late_frames = 0;  // intervals more than 1.5x the mean
};
PresentTiming take_present_timing();

// --- the level art the running game asks for (__pc-set-levels) --------------

// Where the extracted `.fr3` level art lives: the `fr3` directory of the
// player's own prepared data. Until this is set, `set_levels` has nothing to
// load from and says so once. The analog of the GL Loader's base path.
void set_level_art_directory(const std::string& path);

// What the game's own level requests did.
struct LevelArtStats {
  int requests = 0;         // set_levels calls whose list changed
  int levels_loaded = 0;    // levels loaded because the game asked for them
  int levels_evicted = 0;   // levels released after the game stopped asking for them
  int load_failures = 0;    // levels the game asked for that would not load
  double last_load_ms = 0;  // how long the last load took
  std::string wanted;       // the last list the game asked for, joined with '+'
  std::string loaded;       // every level currently on the GPU, joined with '+'
};
LevelArtStats get_level_art_stats();

// Loads the shared level (GAME.fr3) now, on the calling thread. A host calls this after
// set_level_art_directory and before the game starts: the game's own boot relocates textures out
// of it (`setup-font-texture!` while GAME.CGO links), so it has to be whole before the game runs
// at all rather than appearing underneath it.
bool load_common_level_art();

}  // namespace metal_renderer
