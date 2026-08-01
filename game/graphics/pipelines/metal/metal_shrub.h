#pragma once

/*!
 * @file metal_shrub.h
 * Metal port of the Shrub bucket renderer
 * (game/graphics/opengl_renderer/background/Shrub.cpp). Objective-C++ only.
 *
 * Shrub is the small instanced vegetation and clutter: bushes, grass tufts,
 * flowers. It is the simplest of the three background renderers - the chain
 * carries only the PC port control block, and the GL renderer does no culling
 * at all (every draw of every tree is issued), so this port does the same.
 *
 * The one shrub-specific piece is the shader: shrub vertices carry a base
 * vertex color that is multiplied with the time-of-day palette color, and their
 * texture coordinates are stored scaled by 4096.
 *
 * Not ported: per-proto visibility toggles (Jak 2/3 only).
 */

#include <string>
#include <utility>
#include <vector>

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"

class MetalShrub : public MetalBucketRenderer {
 public:
  MetalShrub(const std::string& name, int my_id);

  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;

  struct Stats {
    int trees_rendered = 0;
    int draws = 0;
    int triangles = 0;
    bool level_missing = false;
    std::string level_name;
  };
  const Stats& stats() const { return m_stats; }

 private:
  bool setup_for_level(const std::string& level);
  void update_load(MetalLevelData* level_data);
  void render_tree(int idx, MetalSharedRenderState* render_state, MetalFrameContext& ctx);

  struct Tree {
    MetalLevelData::TreeBuffers* buffers = nullptr;
    const std::vector<tfrag3::ShrubDraw>* draws = nullptr;
    const tfrag3::PackedTimeOfDay* colors = nullptr;
  };

  MetalTfragPcPortData m_pc_port_data;
  MetalTfragRenderSettings m_settings;

  std::vector<Tree> m_trees;
  std::string m_level_name;
  MetalLevelData* m_level = nullptr;
  u64 m_load_id = 0;

  std::vector<math::Vector<u8, 4>> m_color_result;
  std::vector<std::pair<u32, u32>> m_draw_runs;
  std::vector<MetalDrawRun> m_runs;
  Stats m_stats;
  bool m_warned_missing_level = false;
};
