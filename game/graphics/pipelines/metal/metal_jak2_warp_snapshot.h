#pragma once

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/texture/TextureID.h"

struct GpuTexture;
class TexturePool;

namespace metal_renderer {

constexpr u32 kJak2WarpTextureTbp = 1216;

// Private publication proof for Jak II's GMERC_WARP source. No production
// bucket owns this class while buckets 316 and 317 remain deferred. Its
// TexturePool must outlive it so reset can retire the published slot.
class Jak2WarpSnapshotPublisher {
 public:
  struct Stats {
    u32 publications = 0;
    u32 copies = 0;
    u32 allocations = 0;
    u32 replacements = 0;
    u32 failures = 0;
  };

  explicit Jak2WarpSnapshotPublisher(TexturePool* texture_pool);
  ~Jak2WarpSnapshotPublisher();

  Jak2WarpSnapshotPublisher(const Jak2WarpSnapshotPublisher&) = delete;
  Jak2WarpSnapshotPublisher& operator=(const Jak2WarpSnapshotPublisher&) = delete;

  bool publish(MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  void reset();

  u64 texture_handle() const { return m_texture_handle; }
  PcTextureId texture_id() const { return m_texture_id; }
  const Stats& stats() const { return m_stats; }

 private:
  bool ensure_snapshot(MetalFrameContext& ctx);

  TexturePool* m_texture_pool = nullptr;
  id<MTLTexture> m_snapshot = nil;
  u64 m_texture_handle = 0;
  PcTextureId m_texture_id;
  bool m_texture_id_allocated = false;
  GpuTexture* m_pool_texture = nullptr;
  Stats m_stats;
};

}  // namespace metal_renderer
