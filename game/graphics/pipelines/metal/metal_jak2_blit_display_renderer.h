#pragma once

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_blit_display_plan.h"
#include "game/graphics/texture/TextureID.h"

struct GpuTexture;

namespace metal_renderer {

class Jak2BlitDisplayExecutor {
 public:
  struct Stats {
    bool plan_valid = false;
    std::size_t transfer_count = 0;
    std::size_t unsupported_pc_port_count = 0;
    bool snapshot_requested = false;
    bool copy_back_requested = false;
    bool texture_lookup_hit = false;
    bool used_placeholder = false;
    bool copy_back_performed = false;
    u64 texture_handle = 0;
    u32 texture_tbp = 0;
  };

  explicit Jak2BlitDisplayExecutor(TexturePool* texture_pool);
  ~Jak2BlitDisplayExecutor();

  Jak2BlitDisplayExecutor(const Jak2BlitDisplayExecutor&) = delete;
  Jak2BlitDisplayExecutor& operator=(const Jak2BlitDisplayExecutor&) = delete;

  bool execute(const Jak2BlitDisplayPlan& plan,
               MetalSharedRenderState* render_state,
               MetalFrameContext& ctx);
  void finish_frame(MetalFrameContext& ctx);

  const Stats& stats() const { return m_stats; }

 private:
  bool ensure_snapshot(MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  void restart_with_clear(MetalFrameContext& ctx, bool copy_snapshot);
  void restore_snapshot(MetalFrameContext& ctx);

  TexturePool* m_texture_pool = nullptr;
  id<MTLTexture> m_snapshot = nil;
  u64 m_texture_handle = 0;
  PcTextureId m_texture_id;
  bool m_texture_id_allocated = false;
  GpuTexture* m_pool_texture = nullptr;
  bool m_copy_back_pending = false;
  Stats m_stats;
};

}  // namespace metal_renderer

class MetalJak2BlitDisplayRenderer : public MetalBucketRenderer {
 public:
  MetalJak2BlitDisplayRenderer(const std::string& name, int my_id, TexturePool* texture_pool);

  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;
  void finish_frame(MetalFrameContext& ctx) { m_executor.finish_frame(ctx); }

  const metal_renderer::Jak2BlitDisplayExecutor::Stats& stats() const { return m_executor.stats(); }

 private:
  metal_renderer::Jak2BlitDisplayExecutor m_executor;
};
