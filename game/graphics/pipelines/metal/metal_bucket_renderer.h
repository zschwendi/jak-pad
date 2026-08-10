#pragma once

/*!
 * @file metal_bucket_renderer.h
 * Bucket-renderer framework for the Metal backend. Objective-C++ only.
 *
 * Mirror of the GL BucketRenderer / SharedRenderState structure
 * (game/graphics/opengl_renderer/BucketRenderer.h): the top-level renderer
 * walks the game's DMA chain and hands each bucket to a renderer. Ported
 * renderers draw into the frame's single game-target render encoder; buckets
 * whose renderer is not ported yet are consumed by MetalSkipRenderer, which
 * counts the skipped bytes and logs once so deferred content is never
 * silently dropped.
 */

#include <string>
#include <vector>

#include "common/dma/dma_chain_read.h"
#include "common/math/Vector.h"
#include "common/versions/versions.h"

#include "game/graphics/pipelines/metal/metal_pso_cache.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/pipelines/metal/metal_texture_upload_handler.h"
#include "game/graphics/pipelines/metal/metal_view_transform.h"
#import <Metal/Metal.h>

class TexturePool;
// level-geometry frame state, owned by MetalRenderer (metal_level_data.h)
struct MetalBackgroundState;
// composes eye textures other renderers sample (metal_eye_renderer.h)
class MetalEyeRenderer;

/*!
 * Per-frame bump allocator for dynamic vertex data. The GL renderers stream
 * vertices with glBufferData per draw; in Metal the data must live in an
 * MTLBuffer until the frame's command buffer completes, so batches are
 * sub-allocated from shared pages that are reset once the previous frame's
 * GPU work is known to be finished.
 */
class MetalStreamBuffer {
 public:
  // largest single allocation; callers with data-driven sizes check against it
  static constexpr u32 kPageSize = 2 * 1024 * 1024;

  void init(id<MTLDevice> device) { m_device = device; }

  // start of frame; only call once the previous frame's command buffer has
  // completed (the pages are reused in place).
  void reset() {
    m_page = 0;
    m_offset = 0;
  }

  // If an external presentation submission stops completing, permanently retain its pages while
  // detaching them from this allocator. The ordinary renderer then allocates a fresh page set
  // without reusing storage that the stalled command buffer may still reference.
  void quarantine_in_flight_pages() {
    if (!m_pages.empty()) {
      m_quarantined_page_sets.emplace_back(std::move(m_pages));
    }
    m_pages.clear();
    reset();
  }

  // Returns a CPU-writable pointer of `size` bytes and the buffer/offset to
  // bind. Alignment is 16 bytes.
  void* alloc(u32 size, id<MTLBuffer>* out_buffer, u32* out_offset);

 private:
  id<MTLDevice> m_device;
  std::vector<id<MTLBuffer>> m_pages;
  std::vector<std::vector<id<MTLBuffer>>> m_quarantined_page_sets;
  size_t m_page = 0;
  u32 m_offset = 0;
};

/*!
 * The Metal analog of SharedRenderState: chain layout info plus the shared
 * texture pool. Fields the ported renderers actually use; more arrive with
 * later bucket renderers.
 */
struct MetalSharedRenderState {
  u32 buckets_base = 0;
  u32 next_bucket = 0;
  u32 default_regs_buffer = 0;
  math::Vector<u8, 4> fog_color = math::Vector<u8, 4>{0, 0, 0, 0};
  float fog_intensity = 1.f;
  TexturePool* texture_pool = nullptr;
  // shared by the background (tfrag/tie/shrub) renderers: the occlusion
  // visibility strings one bucket copies out of the chain, plus their stats.
  MetalBackgroundState* background = nullptr;
  // merc resolves its eye draws through this, like the GL renderer does
  MetalEyeRenderer* eye_renderer = nullptr;
  const u8* ee_memory = nullptr;
  u32 offset_of_s7 = 0;
  u64 engine_frame_id = 0;
  GameVersion version = GameVersion::Jak1;
  int game_res_w = 640;
  int game_res_h = 480;
  metal_renderer::ViewTransform view_transform;
  // The secondary view replays the already-copied chain only to encode another target. Persistent
  // uploads, diagnostics, callbacks, and aggregate frame statistics stay owned by the primary.
  bool secondary_view = false;
};

/*!
 * Everything a bucket renderer needs to encode draws into the current frame:
 * the game-target render encoder plus the state caches and the per-frame
 * vertex stream.
 */
struct MetalFrameContext {
  id<MTLRenderCommandEncoder> enc;
  MetalPsoCache* pso_cache = nullptr;
  MetalSamplerCache* sampler_cache = nullptr;
  MetalStreamBuffer* stream = nullptr;
  u32 color_format = 0;  // MTLPixelFormat of the game target
  u32 depth_format = 0;
  // the frame's command buffer and game targets, for renderers that must
  // split the game pass (sprite distort's framebuffer snapshot). nil in
  // contexts that never dispatch such buckets (the validation scene).
  id<MTLCommandBuffer> cmds;
  id<MTLTexture> game_color;
  NSUInteger game_color_slice = 0;
  id<MTLTexture> game_depth;
  NSUInteger game_depth_slice = 0;
  // Full-target viewport for the selected attachments. Restored after a bucket splits the pass.
  MTLViewport game_viewport = {0.0, 0.0, 0.0, 0.0, 0.0, 1.0};
  // frame stats (mirror of the GL profiler counters the tests read)
  int draw_calls = 0;
  int triangles = 0;

  // The Metal analog of the GL distorter's glBlitFramebuffer: ends the open
  // game pass, copies the color target into `snapshot`, and reopens the pass
  // loading color/depth/stencil, replacing `enc`. Requires cmds/game_color/
  // game_depth to be set and the first pass to store its depth.
  void resume_pass_with_framebuffer_copy(id<MTLTexture> snapshot);
};

class MetalBucketRenderer {
 public:
  MetalBucketRenderer(const std::string& name, int my_id) : m_name(name), m_my_id(my_id) {}
  virtual ~MetalBucketRenderer() = default;
  virtual void render(DmaFollower& dma,
                      MetalSharedRenderState* render_state,
                      MetalFrameContext& ctx) = 0;
  const std::string& name() const { return m_name; }
  // note: not named id() - that would shadow the Objective-C id type in
  // subclasses' member functions
  int bucket_id() const { return m_my_id; }

 protected:
  std::string m_name;
  int m_my_id;
};

// Walks one bucket without invoking its renderer. Secondary-view replay uses this for buckets
// whose output was already prepared by the primary (texture uploads and eye composition).
void metal_consume_bucket_without_side_effects(DmaFollower& dma,
                                               const MetalSharedRenderState& state);

/*!
 * Mirror of the GL EmptyBucketRenderer: accepts Jak 1's default-register
 * envelope or Jak 2's strict zero-qwc CNT slot and consumes exactly one bucket.
 */
class MetalEmptyBucketRenderer : public MetalBucketRenderer {
 public:
  MetalEmptyBucketRenderer(const std::string& name, int my_id)
      : MetalBucketRenderer(name, my_id) {}
  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;
};

/*!
 * Consumes a bucket whose renderer is not ported yet. Counts the skipped
 * payload bytes and logs the first occurrence per bucket, so deferred content
 * is visible rather than silently dropped.
 */
class MetalSkipRenderer : public MetalBucketRenderer {
 public:
  MetalSkipRenderer(const std::string& name, int my_id) : MetalBucketRenderer(name, my_id) {}
  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;
  u64 skipped_bytes() const { return m_skipped_bytes; }

 private:
  u64 m_skipped_bytes = 0;
  bool m_warned = false;
};

/*!
 * Bucket adapter over the plain-C++ MetalTextureUploadHandler.
 */
class MetalTextureBucketRenderer : public MetalBucketRenderer {
 public:
  MetalTextureBucketRenderer(const std::string& name, int my_id)
      : MetalBucketRenderer(name, my_id) {}
  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;
  const MetalTextureUploadHandler::Stats& last_stats() const { return m_last_stats; }

 private:
  MetalTextureUploadHandler m_handler;
  MetalTextureUploadHandler::Stats m_last_stats;
};
