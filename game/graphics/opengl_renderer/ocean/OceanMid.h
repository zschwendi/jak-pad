#pragma once

#include "game/common/vu.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/DirectRenderer.h"
#include "game/graphics/opengl_renderer/ocean/CommonOceanRenderer.h"
#include "game/graphics/opengl_renderer/ocean/OceanVu.h"

class OceanMid : public OceanMidVu {
 public:
  OceanMid() = default;
  void run(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof);
  void run_jak2(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof);

 private:
  void run_call0();
  void xgkick(u16 addr) override;

  CommonOceanRenderer m_common_ocean_renderer;
};
