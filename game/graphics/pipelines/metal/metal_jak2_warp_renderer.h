#pragma once

#include <memory>

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_generic2.h"
#include "game/graphics/texture/TextureID.h"

struct GpuTexture;
class TexturePool;

namespace metal_renderer {

constexpr u32 kJak2WarpTextureTbp = 1216;

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

  bool capture_and_publish(MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  void reset();

  u64 texture_handle() const { return m_texture_handle; }
  PcTextureId texture_id() const { return m_texture_id; }
  const Stats& stats() const { return m_stats; }

 private:
  id<MTLTexture> make_candidate(MetalFrameContext& ctx, bool* replacement) const;
  bool publish_candidate(id<MTLTexture> candidate, bool replacement);

  TexturePool* m_texture_pool = nullptr;
  id<MTLTexture> m_snapshot = nil;
  u64 m_texture_handle = 0;
  PcTextureId m_texture_id;
  bool m_texture_id_allocated = false;
  GpuTexture* m_pool_texture = nullptr;
  Stats m_stats;
};

class MetalJak2WarpBucketRenderer final : public MetalBucketRenderer {
 public:
  MetalJak2WarpBucketRenderer(const std::string& name,
                              int my_id,
                              std::shared_ptr<MetalGeneric2> generic,
                              TexturePool* texture_pool);

  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;

  const MetalGeneric2::Stats& generic_stats() const { return m_generic_stats; }
  const Jak2WarpSnapshotPublisher::Stats& snapshot_stats() const {
    return m_snapshot.stats();
  }
  u64 texture_handle() const { return m_snapshot.texture_handle(); }

 private:
  std::shared_ptr<MetalGeneric2> m_generic;
  Jak2WarpSnapshotPublisher m_snapshot;
  MetalGeneric2::Stats m_generic_stats;
};

}  // namespace metal_renderer
