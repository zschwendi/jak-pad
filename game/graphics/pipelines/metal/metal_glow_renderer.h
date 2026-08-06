#pragma once

#include <cstddef>

#include "game/graphics/sprite_glow_math.h"

struct MetalFrameContext;
struct MetalSharedRenderState;

/*!
 * Diagnostic-only Jak 2 final glow-flare draw.
 *
 * The caller owns glow parsing and transformation. This renderer accepts the
 * resulting records and deliberately treats every flare as fully visible with
 * a boost of one; it does not implement the depth probe or downsample passes.
 */
class MetalGlowRenderer {
 public:
  struct Stats {
    int sprites_submitted = 0;
    int sprites_drawn = 0;
    int draw_calls = 0;
    int triangles = 0;
    int missing_textures = 0;
  };

  void draw_force_visible(const SpriteGlowOutput* sprites,
                          std::size_t count,
                          MetalSharedRenderState* render_state,
                          MetalFrameContext& ctx);

  const Stats& stats() const { return m_stats; }

 private:
  Stats m_stats;
};
