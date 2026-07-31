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

#include <vector>

#include "common/common_types.h"

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

// Blocks until the last submitted frame finishes on the GPU, then reads back
// the offscreen game render target. Returns false if no frame has been rendered.
bool read_last_frame(FramePixels* out);

// Renders the present (letterbox) pass into an offscreen window-sized target
// using the same encoder path the on-screen drawable gets, then reads it back.
bool read_present_frame(const PresentTestOptions& opts, FramePixels* out);

ScaffoldStats get_stats();

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

}  // namespace metal_renderer
