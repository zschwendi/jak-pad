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

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_external_submission_gate.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/pipelines/metal/metal_pipeline.h"
#include "game/graphics/pipelines/metal/metal_pso_cache.h"
#include "game/graphics/pipelines/metal/metal_stereo_eye_marker.h"
#include "game/graphics/pipelines/metal/metal_texture.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

class TexturePool;
class MetalSkyBlendCPU;
class MetalSkyBlendHandler;
class MetalJak2BlitDisplayRenderer;
struct MetalPresentationState;

namespace metal_renderer {
class Jak2Shadow195FrameCapture;
class MetalJak2Shadow2Renderer;
}

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
  // Jak 2/3 background draws encode texture-animator slots as negative texture IDs.
  const u64* animated_texture_slots = nullptr;
  std::size_t animated_texture_slot_count = 0;
  // Optional synchronous host work that must stay ordered with Jak II bucket dispatch.
  void* host_bucket_context = nullptr;
  MetalHostBucketCallback host_bucket_callback = nullptr;
  const metal_renderer::Jak2PrisEyeTextureUploadPlan* jak2_pris_eye_plans = nullptr;
  std::size_t jak2_pris_eye_plan_count = 0;
  const metal_renderer::Jak2CommonPrisTextureUploadPlan* jak2_common_pris_plan = nullptr;
  const metal_renderer::Jak2GmercWarpBucket317Plan* jak2_gmerc_warp_bucket317_plan = nullptr;
  const metal_renderer::Jak2ShadowBucket195Plan* jak2_shadow_bucket195_plan = nullptr;
  metal_renderer::Jak2Shadow195FrameCapture* jak2_shadow195_frame_capture = nullptr;
  float target_fps = 60.f;
};

// Borrowed render attachments for one host-owned view. The submitted command buffer retains the
// textures through GPU completion; ownership stays with the host. Jak 2 retains color between
// submissions only while view_id and the live color/depth texture slices remain identical. A host
// that discards an attachment's contents must supply a new view_id or attachment identity.
struct MetalExternalRenderTargetDescriptor {
  u64 view_id = 0;
  id<MTLTexture> color_texture = nil;
  NSUInteger color_slice = 0;
  id<MTLTexture> depth_texture = nil;
  NSUInteger depth_slice = 0;
  // Current pass-split support requires the full attachment: origin zero, attachment dimensions,
  // and a 0...1 depth range.
  MTLViewport viewport = {0.0, 0.0, 0.0, 0.0, 0.0, 1.0};
  double clear_depth = 0.0;
  // Host-owned transform from the game's final Metal clip coordinates into this view.
  metal_renderer::ViewTransform view_transform;
};

enum class MetalExternalFrameReservation {
  reserved,
  busy,
  failed,
};

class MetalRenderer {
 public:
  MetalRenderer();
  ~MetalRenderer();

  bool init(id<MTLDevice> device);
  id<MTLDevice> device() const { return m_device; }
  id<MTLCommandQueue> queue() const { return m_queue; }

  // Builds the selected game's bucket renderer table. Must be called once the
  // texture pool exists (the Jak 1 sky blender registers its output textures
  // with it).
  void init_bucket_renderers(TexturePool* pool,
                             GameVersion version,
                             bool host_texture_uploads = false);

  // Renders one frame: game passes into the offscreen target, then the present
  // pass into the layer's next drawable, all in one command buffer.
  void render_frame(const MetalRenderOptions& opts, CAMetalLayer* layer);

  // Renders one frame from the game's DMA chain (the copied chain from
  // send_chain): walks the selected game's chain and hands each bucket to its
  // renderer, then runs the present pass when a layer is supplied. A nil layer
  // validates dispatch and encoding without committing a command buffer.
  bool render_chain_frame(const MetalRenderOptions& opts,
                          CAMetalLayer* layer,
                          const u8* chain_data,
                          u32 chain_offset,
                          std::size_t chain_size);

  // Renders one DMA-chain view directly into host-owned color/depth texture slices, without a
  // CAMetalLayer or present pass. Invalid descriptors return before renderer state is mutated.
  bool render_chain_frame_to_external_target(
      const MetalRenderOptions& opts,
      const MetalExternalRenderTargetDescriptor& target,
      const u8* chain_data,
      u32 chain_offset,
      std::size_t chain_size);

  // Validates both views before mutation, then renders the same copied chain into both targets.
  // The primary owns frame-global uploads, callbacks, diagnostics, and aggregate frame stats; the
  // secondary replay only supplies the second view and cannot advance game simulation.
  // World geometry consumes each view's transform: background, Merc, 3D sprites, sky, ocean,
  // non-HUD generic draws, and shadow volumes. Authored screen content is deliberately duplicated
  // without parallax: Direct debug/subtitles, 2D/HUD/distortion sprites, generic HUD, and the
  // shadow fullscreen overlay. Texture uploads and eye/ocean/sky preparation run once and are
  // reused by both views.
  bool render_chain_frame_to_external_stereo_targets(
      const MetalRenderOptions& opts,
      const MetalExternalRenderTargetDescriptor& left,
      const MetalExternalRenderTargetDescriptor& right,
      id<MTLCommandBuffer> command_buffer,
      const u8* chain_data,
      u32 chain_offset,
      std::size_t chain_size);

  // Reserves the existing stream allocator without waiting. The caller must reserve before
  // acquiring its external textures, then either submit the borrowed command buffer or cancel.
  MetalExternalFrameReservation reserve_external_stereo_frame();
  void cancel_external_stereo_frame();
  bool submit_external_stereo_frame(id<MTLCommandBuffer> command_buffer);
  // Detach a still-in-flight external frame from the stream allocator before falling back to the
  // ordinary presentation path. The submitted command buffer retains its old stream pages.
  void prepare_external_stereo_fallback();

  // Waits for the most recently committed chain frame's completion handler, with a timeout.
  bool wait_for_last_chain_frame(double timeout_seconds);

  // Physical-device gate: waits for the most recently submitted drawable callback.
  bool wait_for_last_presentation(double timeout_seconds);

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
  void set_detailed_frame_stats_enabled(bool enabled) {
    m_detailed_frame_stats_enabled = enabled;
  }

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

  struct ExternalTargetHistory {
    __weak id<MTLTexture> color_texture = nil;
    NSUInteger color_slice = 0;
    __weak id<MTLTexture> depth_texture = nil;
    NSUInteger depth_slice = 0;
  };

  void setup_frame(const MetalRenderOptions& opts);
  void encode_game_passes(id<MTLCommandBuffer> cmds,
                          id<MTLTexture> color,
                          id<MTLTexture> depth);
  void init_bucket_renderers_jak1();
  void init_bucket_renderers_jak2();
  void dispatch_buckets_jak1(DmaFollower dma, MetalFrameContext& ctx);
  void dispatch_buckets_jak2(DmaFollower dma, MetalFrameContext& ctx);
  void encode_present_pass(id<MTLCommandBuffer> cmds,
                           id<MTLTexture> target,
                           const MetalRenderOptions& opts,
                           id<MTLTexture> source);
  void build_validation_scene();
  bool read_color_target(id<MTLTexture> tex, metal_renderer::FramePixels* out);
  bool render_chain_frame_impl(const MetalRenderOptions& opts,
                               CAMetalLayer* layer,
                               id<MTLTexture> game_color,
                               NSUInteger color_slice,
                               id<MTLTexture> game_depth,
                               NSUInteger depth_slice,
                               const MTLViewport* viewport,
                               double clear_depth,
                               u64 view_id,
                               const metal_renderer::ViewTransform& view_transform,
                               bool frame_global_side_effects,
                               id<MTLCommandBuffer> borrowed_command_buffer,
                               const u8* chain_data,
                               u32 chain_offset,
                               std::size_t chain_size);
#if GOALPAD_VISION_STEREO_EYE_MARKERS
  void encode_stereo_eye_markers(
      id<MTLCommandBuffer> command_buffer,
      const MetalExternalRenderTargetDescriptor& left,
      const MetalExternalRenderTargetDescriptor& right);
#endif

  id<MTLDevice> m_device;
  id<MTLCommandQueue> m_queue;
  MetalPsoCache m_pso_cache;
  MetalSamplerCache m_sampler_cache;

  // offscreen game render target (game internal resolution)
  id<MTLTexture> m_game_color;
  id<MTLTexture> m_game_depth;
  id<MTLTexture> m_jak2_fallback_color;
  id<MTLTexture> m_jak2_fallback_depth;

  // validation scene resources
  id<MTLBuffer> m_scene_vertices;
  id<MTLTexture> m_checker_texture;
  std::vector<SceneDraw> m_scene_draws;
#if GOALPAD_VISION_STEREO_EYE_MARKERS
  id<MTLBuffer> m_left_eye_marker;
  id<MTLBuffer> m_right_eye_marker;
#endif

  id<MTLCommandBuffer> m_last_internal_frame_cmds;
  std::mutex m_frame_mutex;
  u64 m_frame_count = 0;
  u64 m_command_submission_count = 0;
  u64 m_last_internal_frame_submission = 0;
  MetalOrdinaryFrameSlotRing m_ordinary_stream_slots;
  u64 m_external_stream_submission = 0;
  bool m_external_stereo_disabled = false;
  u64 m_stream_reuse_wait_count = 0;

  // --- DMA chain path (stage 4) ---------------------------------------------
  std::array<MetalStreamBuffer, kMetalOrdinaryFrameResourceSlotCount> m_ordinary_streams;
  MetalStreamBuffer m_external_stream;
  MetalSharedRenderState m_shared_state;
  std::vector<std::unique_ptr<MetalBucketRenderer>> m_bucket_renderers;
  std::unique_ptr<metal_renderer::MetalJak2Shadow2Renderer>
      m_jak2_shadow195_capture_renderer;
  std::unique_ptr<MetalEyeRenderer> m_jak2_eye_renderer;
  TexturePool* m_texture_pool = nullptr;
  bool m_host_texture_uploads = false;
  MetalJak2BlitDisplayRenderer* m_jak2_blit_display = nullptr;
  MetalSkyBlendHandler* m_sky_blend_handlers[2] = {nullptr, nullptr};
  std::shared_ptr<MetalSkyBlendCPU> m_sky_cpu_blender;
  // level-geometry frame state, shared with the tfrag/tie/shrub renderers
  MetalBackgroundState m_background;
  metal_renderer::ChainStats m_chain_stats;
  bool m_detailed_frame_stats_enabled = true;
  bool m_reported_camera_mismatch = false;
  std::shared_ptr<MetalPresentationState> m_presentation_state;
  u64 m_submission_count = 0;
  bool m_game_target_fresh = true;
  std::unordered_map<u64, std::unique_ptr<ExternalTargetHistory>> m_external_target_history;
};
