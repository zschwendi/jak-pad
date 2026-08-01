#pragma once

#include "game/common/vu.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/ocean/CommonOceanRenderer.h"
#include "game/graphics/opengl_renderer/ocean/OceanTexture.h"
#include "game/graphics/opengl_renderer/ocean/OceanVu.h"

class OceanNear : public BucketRenderer, public OceanNearVu {
 public:
  OceanNear(const std::string& name, int my_id);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  void render_jak1(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof);
  void render_jak2(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof);
  void draw_debug_window() override;
  void init_textures(TexturePool& pool, GameVersion version) override;

 private:
  void xgkick(u16 addr) override;

  OceanTexture m_texture_renderer;
  CommonOceanRenderer m_common_ocean_renderer;
};
