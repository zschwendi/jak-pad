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

#include <memory>
#include <mutex>
#include <vector>

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/pipelines/metal/metal_pipeline.h"
#include "game/graphics/pipelines/metal/metal_pso_cache.h"
#include "game/graphics/pipelines/metal/metal_texture.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

class TexturePool;
class MetalSkyBlendHandler;
struct MetalPresentationState;

// Backs metal_renderer::set_s7_override (see metal_pipeline.h).
void metal_set_s7_override(u32 s7_ptr);

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
  // How long the drawable must stay on screen, in seconds, or 0 to present at the next vsync.
  // On a display whose refresh is a multiple of the game's rate this is what makes presentation
  // regular: at 0 a 60 fps game on a 120 Hz display lands on alternating 8.3 ms and 16.7 ms
  // vsyncs and judders even though the average rate is exactly right.
  double min_present_duration = 0.0;
  // Absolute Core Animation host time for this drawable. When nonzero this takes precedence over
  // min_present_duration, so a CADisplayLink-driven host can use the link as its only clock.
  double presentation_time = 0.0;

  // Diagnostic identity supplied by the host at the synchronous DMA-send seam. These fields do
  // not affect rendering or presentation.
  u64 host_tick_id = 0;
  u64 chain_ordinal = 0;
  u64 engine_frame_id = 0;
  bool expected_camera_valid = false;
  metal_camera_trace::Snapshot expected_camera;
  bool expected_render_camera_valid = false;
  metal_camera_trace::RenderSnapshot expected_render_camera;
};

class MetalRenderer {
 public:
  bool init(id<MTLDevice> device);
  id<MTLDevice> device() const { return m_device; }
  id<MTLCommandQueue> queue() const { return m_queue; }

  // Builds the selected game's bucket renderer table. Must be called once the
  // texture pool exists (the Jak 1 sky blender registers its output textures
  // with it).
  void init_bucket_renderers(TexturePool* pool, GameVersion version);

  // Renders one frame: game passes into the offscreen target, then the present
  // pass into the layer's next drawable, all in one command buffer.
  void render_frame(const MetalRenderOptions& opts, CAMetalLayer* layer);

  // Renders one frame from the game's DMA chain (the copied chain from
  // send_chain): walks the selected game's chain and hands each bucket to its
  // renderer, then runs the present pass when a layer is supplied. A nil layer
  // still commits the offscreen game pass without scheduling presentation.
  bool render_chain_frame(const MetalRenderOptions& opts,
                          CAMetalLayer* layer,
                          const u8* chain_data,
                          u32 chain_offset);

  // Wait for the most recently committed frame and require successful GPU completion. This is
  // the synchronization contract used by a nil-layer external host; it performs no readback.
  bool wait_for_last_frame();

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
  metal_renderer::ChainStats chain_stats();

  // Background (tfrag/tie/shrub) counters from the last chain frame.
  const MetalBackgroundState& background_state() const { return m_background; }

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
  void init_bucket_renderers_jak1();
  void init_bucket_renderers_jak2();
  void dispatch_buckets_jak1(DmaFollower dma, MetalFrameContext& ctx);
  void dispatch_buckets_jak2(DmaFollower dma, MetalFrameContext& ctx);
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

  // --- DMA chain path (stage 4) ---------------------------------------------
  MetalStreamBuffer m_stream;
  MetalSharedRenderState m_shared_state;
  std::vector<std::unique_ptr<MetalBucketRenderer>> m_bucket_renderers;
  TexturePool* m_texture_pool = nullptr;
  MetalSkyBlendHandler* m_sky_blend_handlers[2] = {nullptr, nullptr};
  // level-geometry frame state, shared with the tfrag/tie/shrub renderers
  MetalBackgroundState m_background;
  metal_renderer::ChainStats m_chain_stats;
  bool m_reported_camera_mismatch = false;
  std::shared_ptr<MetalPresentationState> m_presentation_state;
  u64 m_submission_count = 0;
};
