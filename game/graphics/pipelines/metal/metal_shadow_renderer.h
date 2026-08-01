#pragma once

/*!
 * @file metal_shadow_renderer.h
 * Jak 1 shadow renderer for the Metal backend. Objective-C++ only.
 *
 * Port of game/graphics/opengl_renderer/ShadowRenderer.cpp. The VU1 program
 * that builds the shadow volume is *shared*, not copied: it now lives on
 * ShadowVu (game/graphics/opengl_renderer/ShadowVu.h), which both the GL and
 * Metal renderers derive from, so the two backends cannot drift.
 *
 * What this file ports is the rest: the DMA walk that feeds the VU, and the
 * three-pass stencil draw - increment the stencil where front faces pass
 * depth, decrement it where back faces do, then darken the screen wherever the
 * count is non-zero.
 */

#include <string>

#include "game/graphics/opengl_renderer/ShadowVu.h"
#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"

#import <Metal/Metal.h>

class MetalShadowRenderer : public MetalBucketRenderer, public ShadowVu {
 public:
  struct Stats {
    int volumes = 0;         // xgkick runs that produced geometry
    int vertices = 0;
    int front_indices = 0;
    int back_indices = 0;
    int draw_calls = 0;
    int triangles = 0;
    int unexpected_dma = 0;  // the bucket did not match: consumed and reported
  };

  MetalShadowRenderer(const std::string& name, int my_id)
      : MetalBucketRenderer(name, my_id) {}

  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;
  const Stats& stats() const { return m_stats; }

 private:
  void draw(MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  bool expect(bool condition, const char* what);

  math::Vector4f m_color;
  Stats m_stats;
  bool m_failed = false;
  bool m_warned = false;
};
