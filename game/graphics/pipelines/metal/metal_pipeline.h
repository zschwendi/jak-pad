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

#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/custom_data/Tfrag3Data.h"
#include "common/math/Vector.h"

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"

extern const GfxRendererModule gRendererMetal;

class TexturePool;

namespace tfrag3 {
struct Texture;
}

namespace metal_renderer {

// RGBA8 copy of a rendered frame, used by tests to verify that rendering
// actually happened. Origin is the top-left corner.
struct FramePixels {
  int width = 0;
  int height = 0;
  std::vector<u8> rgba;
};

// Cache and frame counters, used by tests to verify that PSOs are created once
// and reused rather than rebuilt per draw or per frame.
struct ScaffoldStats {
  u64 frames_rendered = 0;
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

// Counters for the DMA-chain path, used by tests to verify that send_chain
// frames really dispatched buckets and that deferred content is counted.
struct ChainStats {
  u64 chains_rendered = 0;
  // from the last chain frame
  int draw_calls = 0;
  int triangles = 0;
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
  int sprite_draws = 0;
  int sprite_missing_textures = 0;
  // cumulative
  u64 skipped_bucket_bytes = 0;    // DMA consumed by not-yet-ported bucket renderers
  u64 skipped_tfrag_bytes = 0;     // tfrag-trans content in the sky-blend buckets
  int direct_unsupported_blends = 0;
};

ChainStats get_chain_stats();

// Blocks until the last submitted frame finishes on the GPU, then reads back
// the offscreen game render target. Returns false if no frame has been rendered.
bool read_last_frame(FramePixels* out);

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

// Frees every loaded level.
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

// Sizes of the layout mirrors in metal_level_data.h, so the proof can check
// them against the GL definitions they duplicate.
size_t sizeof_pc_port_data_mirror();
size_t sizeof_camera_data_mirror();

}  // namespace metal_renderer
