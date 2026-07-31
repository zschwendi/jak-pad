#pragma once

/*!
 * @file metal_renderer.h
 * Frame scaffolding for the Metal backend. Objective-C++ only.
 *
 * Mirrors the frame structure of OpenGLRenderer (game/graphics/opengl_renderer/
 * OpenGLRenderer.cpp): each frame, game content is rendered into an offscreen
 * target at the game's internal resolution (setup_frame), then a PCRTC-style
 * final pass draws that target into the centered, letterboxed draw region of
 * the window (do_pcrtc_effects), including the brightness/contrast math and the
 * pmode-alp blackout. Depth follows the PS2 convention the GL renderer uses:
 * depth clears to 0.0 and closer geometry has larger depth values (GEQUAL).
 *
 * The game passes currently draw a fixed validation scene that exercises the
 * PSO cache with distinct blend / write-mask / depth states; bucket renderers
 * will replace it in later stages.
 */

#include <mutex>
#include <vector>

#include "game/graphics/pipelines/metal/metal_pipeline.h"
#include "game/graphics/pipelines/metal/metal_pso_cache.h"
#include "game/graphics/pipelines/metal/metal_texture.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

// Vertex layout for the scaffold shader. Must match ScaffoldVertexIn in
// shaders/scaffold.metal.
struct ScaffoldVertex {
  float pos[3];
  float uv[2];
  float color[4];
  float use_tex;
};

// The per-frame settings the game supplies (subset of the GL RenderOptions that
// the scaffolding stage needs).
struct MetalRenderOptions {
  int game_res_w = 640;
  int game_res_h = 480;
  int draw_region_w = 0;
  int draw_region_h = 0;
  float pmode_alp = 1.f;
  int brightness_contrast_color = 0;    // 0 = no change
  int brightness_contrast_alpha = 128;  // 128 = no change
};

class MetalRenderer {
 public:
  bool init(id<MTLDevice> device);
  id<MTLDevice> device() const { return m_device; }
  id<MTLCommandQueue> queue() const { return m_queue; }

  // Renders one frame: game passes into the offscreen target, then the present
  // pass into the layer's next drawable, all in one command buffer.
  void render_frame(const MetalRenderOptions& opts, CAMetalLayer* layer);

  // Waits for the last committed frame, then reads back the offscreen game
  // target. Returns false if no frame has been rendered yet.
  bool read_game_frame(metal_renderer::FramePixels* out);

  // Renders the present pass (the same encoder path the drawable gets) into an
  // offscreen window-sized target and reads it back. Requires at least one
  // rendered frame so the game target has content.
  bool read_present_frame(int window_w,
                          int window_h,
                          const MetalRenderOptions& opts,
                          metal_renderer::FramePixels* out);

  metal_renderer::ScaffoldStats stats();

  // Renders a quad sampling the given registry texture with the requested
  // sampler state into a small offscreen target and reads it back. Verifies
  // the texture path (upload, mips, sampler modes) by pixel readback.
  bool read_texture_sample(const metal_renderer::TextureSampleSpec& spec,
                           metal_renderer::FramePixels* out);

 private:
  // A draw of the validation scene: a vertex range plus the GL-style state that
  // the PSO / depth-stencil caches turn into baked Metal objects.
  struct SceneDraw {
    int first_vertex;
    int vertex_count;
    MetalPsoKey pso;             // color/depth formats filled in at encode time
    MetalDepthStencilKey depth;
  };

  void setup_frame(const MetalRenderOptions& opts);
  void encode_game_passes(id<MTLCommandBuffer> cmds);
  void encode_present_pass(id<MTLCommandBuffer> cmds,
                           id<MTLTexture> target,
                           const MetalRenderOptions& opts);
  void build_validation_scene();
  bool read_color_target(id<MTLTexture> tex, metal_renderer::FramePixels* out);

  id<MTLDevice> m_device;
  id<MTLCommandQueue> m_queue;
  MetalPsoCache m_pso_cache;
  MetalSamplerCache m_sampler_cache;

  // offscreen game render target (game internal resolution)
  id<MTLTexture> m_game_color;
  id<MTLTexture> m_game_depth;

  // validation scene resources
  id<MTLBuffer> m_scene_vertices;
  id<MTLTexture> m_checker_texture;
  std::vector<SceneDraw> m_scene_draws;

  id<MTLCommandBuffer> m_last_frame_cmds;
  std::mutex m_frame_mutex;
  u64 m_frame_count = 0;
};
