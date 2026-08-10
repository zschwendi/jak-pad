#pragma once

#include <cstddef>

#include "game/graphics/sprite_glow_math.h"

#import <Metal/Metal.h>

struct MetalFrameContext;
struct MetalSharedRenderState;

/*! Metal port of Jak 2's glow visibility probe and final flare draw. */
class MetalGlowRenderer {
 public:
  struct Stats {
    int sprites_submitted = 0;
    int sprites_drawn = 0;
    int draw_calls = 0;
    int triangles = 0;
    int visibility_draw_calls = 0;
    int visibility_triangles = 0;
    int missing_textures = 0;
    int invalid_records = 0;
  };

  void draw(const SpriteGlowOutput* sprites,
            std::size_t count,
            MetalSharedRenderState* render_state,
            MetalFrameContext& ctx);

  const Stats& stats() const { return m_stats; }

 private:
  static constexpr int kDownsampleBatchWidth = 20;
  static constexpr int kDownsampleIterations = 5;
  static constexpr int kFirstDownsampleSize = 32;

  bool ensure_probe_targets(id<MTLDevice> device);
  bool ensure_game_depth_snapshot(id<MTLTexture> source);
  bool ensure_missing_texture_fallback(id<MTLDevice> device);

  id<MTLTexture> m_probe_color[kDownsampleIterations] = {};
  id<MTLTexture> m_probe_depth = nil;
  id<MTLTexture> m_game_depth_snapshot = nil;
  id<MTLTexture> m_missing_texture_fallback = nil;
  Stats m_stats;
};
