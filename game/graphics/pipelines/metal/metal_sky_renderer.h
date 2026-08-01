#pragma once

/*!
 * @file metal_sky_renderer.h
 * Metal ports of the Jak 1 sky path (game/graphics/opengl_renderer/
 * SkyRenderer.{h,cpp} and SkyBlendCPU.{h,cpp}). Objective-C++ only.
 *
 * MetalSkyBlendCPU blends level sky/cloud textures on the CPU and publishes
 * the results into the TexturePool at the fixed sky VRAM addresses. Note: the
 * GL SkyBlendCPU's blend kernels are x86-only and compile to no-ops on arm64;
 * this port uses portable scalar math with the same fixed-point semantics, so
 * the sky actually blends on Apple silicon.
 *
 * MetalSkyRenderer draws the sky via the DirectRenderer port.
 * MetalSkyBlendHandler owns the sky-blend bucket; its trailing tfrag-trans
 * content belongs to the background renderers (stage 5) and is skipped with a
 * counted, once-logged warning.
 */

#include "game/graphics/opengl_renderer/SkyBlendCommon.h"
#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_direct_renderer.h"

class MetalSkyBlendCPU {
 public:
  MetalSkyBlendCPU(id<MTLDevice> device);
  void init_textures(TexturePool& tex_pool, GameVersion version);
  SkyBlendStats do_sky_blends(DmaFollower& dma, MetalSharedRenderState* render_state);

 private:
  static constexpr int m_sizes[2] = {32, 64};
  std::vector<u8> m_texture_data[2];

  struct TexInfo {
    id<MTLTexture> texture;
    u64 handle = 0;
    u32 tbp = 0;
    GpuTexture* pool_tex = nullptr;
  } m_textures[2];
};

class MetalSkyRenderer : public MetalBucketRenderer {
 public:
  MetalSkyRenderer(const std::string& name, int my_id);
  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;
  const MetalDirectRenderer::Stats& direct_stats() const { return m_direct_renderer.stats(); }

 private:
  MetalDirectRenderer m_direct_renderer;
};

class MetalSkyBlendHandler : public MetalBucketRenderer {
 public:
  MetalSkyBlendHandler(const std::string& name,
                       int my_id,
                       std::shared_ptr<MetalSkyBlendCPU> shared_blender);
  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;
  const SkyBlendStats& last_stats() const { return m_stats; }
  u64 skipped_tfrag_bytes() const { return m_skipped_tfrag_bytes; }

 private:
  std::shared_ptr<MetalSkyBlendCPU> m_shared_blender;
  SkyBlendStats m_stats;
  u64 m_skipped_tfrag_bytes = 0;
  bool m_warned_tfrag = false;
};
