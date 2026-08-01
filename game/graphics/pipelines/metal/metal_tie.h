#pragma once

/*!
 * @file metal_tie.h
 * Metal port of the Tie3 bucket renderer
 * (game/graphics/opengl_renderer/background/Tie3.cpp). Objective-C++ only.
 *
 * TIE is the instanced environment geometry: the huts, fences, bridges,
 * platforms and props of a level - most of what is visibly "built" in Sandover
 * village. Like tfrag, the chain carries only a control block (camera, fog,
 * wind state, envmap tint, level name) and the geometry comes from the level's
 * `.fr3` through the level-data stage (metal_level_data.h).
 *
 * TIE draws are grouped into categories (tfrag3::TieCategory). On Jak 1 the one
 * bucket per level draws the NORMAL category, the base draw of the envmapped
 * category, then the envmap second draw. This port covers all three:
 *  - NORMAL uses the tfrag3 shader, exactly as GL does.
 *  - NORMAL_ENVMAP's base draw uses the etie_base shader, exactly as GL does
 *    (GL uses the envmap-style math for the base draw to avoid a rounding
 *    mismatch with the second draw).
 *  - NORMAL_ENVMAP_SECOND_DRAW - the shiny reflective pass - uses the etie
 *    shader, with the frame's envmap tint from the chain.
 *
 * Not ported, and honestly missing rather than faked:
 *  - wind-instanced draws (trees/flags that sway). They need the per-instance
 *    matrix rebuild the GL renderer does on the CPU each frame.
 *  - per-proto visibility toggles (Jak 2/3 only).
 * Each of those is counted so what is missing from a frame is visible.
 */

#include <string>
#include <utility>
#include <vector>

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"

class MetalTie3 : public MetalBucketRenderer {
 public:
  MetalTie3(const std::string& name, int my_id, int level_id);

  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;

  struct Stats {
    int trees_rendered = 0;
    int draws = 0;
    int runs = 0;
    int triangles = 0;
    int wind_draws_skipped = 0;
    int envmap_second_draws = 0;
    int envmap_second_triangles = 0;
    bool level_missing = false;
    std::string level_name;
  };
  const Stats& stats() const { return m_stats; }

 private:
  bool set_up_common_data_from_dma(DmaFollower& dma, MetalSharedRenderState* render_state);
  bool setup_for_level(const std::string& level);
  void update_load(MetalLevelData* level_data);
  void render_all_trees(int geom,
                        tfrag3::TieCategory category,
                        MetalSharedRenderState* render_state,
                        MetalFrameContext& ctx);
  void render_tree(int geom,
                   int tree_idx,
                   tfrag3::TieCategory category,
                   MetalSharedRenderState* render_state,
                   MetalFrameContext& ctx);

  struct Tree {
    MetalLevelData::TreeBuffers* buffers = nullptr;
    const std::vector<tfrag3::StripDraw>* draws = nullptr;
    const std::vector<tfrag3::InstancedStripDraw>* wind_draws = nullptr;
    const tfrag3::PackedTimeOfDay* colors = nullptr;
    const tfrag3::BVH* vis = nullptr;
    std::array<u32, tfrag3::kNumTieCategories + 1> category_draw_indices = {};
    bool use_strips = true;

    // per-frame, filled by the visibility pass before the category draws
    std::vector<u8> vis_temp;
    std::vector<std::pair<u32, u32>> draw_runs;
    std::vector<u32> draw_tris;
    std::vector<MetalDrawRun> runs;
  };

  int m_level_id;
  MetalTfragPcPortData m_pc_port_data;
  MetalTfragRenderSettings m_settings;

  std::array<std::vector<Tree>, tfrag3::TIE_GEOS> m_trees;
  std::string m_level_name;
  MetalLevelData* m_level = nullptr;
  u64 m_load_id = 0;

  std::vector<math::Vector<u8, 4>> m_color_result;
  math::Vector4f m_envmap_color{1.f, 1.f, 1.f, 1.f};
  Stats m_stats;
  bool m_warned_missing_level = false;
};
