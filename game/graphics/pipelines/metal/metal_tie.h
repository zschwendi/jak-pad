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
 * category, then the envmap second draw. Jak 2 keeps NORMAL in the populated
 * parent bucket and drives the two envmap draws from the immediately following
 * empty ETIE child bucket. For static draws with non-animated textures, this
 * slice covers all three:
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
 *  - negative tree_tex_id texture-animator slots. Until the Jak 2 texture
 *    animator is connected, these draws use the existing placeholder and
 *    increment both anim_slot_draws and missing_textures.
 * Jak 2's per-prototype visibility mask is supported for these static draws.
 * Wind skips remain counted so what is missing from a frame is visible.
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
  friend class MetalTieEnvmap;
  struct Tree;

  bool set_up_common_data_from_dma(DmaFollower& dma, MetalSharedRenderState* render_state);
  bool set_up_jak2_common_data_from_dma(DmaFollower& dma,
                                        MetalSharedRenderState* render_state);
  bool setup_for_level(const std::string& level);
  void update_load(MetalLevelData* level_data);
  bool configure_proto_visibility(const std::vector<std::string>& hidden_names,
                                  MetalBackgroundState* background);
  void prepare_trees(int geom,
                     MetalSharedRenderState* render_state);
  void make_draw_runs(Tree& tree, bool all_geometry_visible);
  void render_all_trees(int geom,
                        tfrag3::TieCategory category,
                        MetalSharedRenderState* render_state,
                        MetalFrameContext& ctx);
  void render_tree(int geom,
                   int tree_idx,
                   tfrag3::TieCategory category,
                   MetalSharedRenderState* render_state,
                   MetalFrameContext& ctx);
  void render_envmap_from_parent(MetalSharedRenderState* render_state,
                                 MetalFrameContext& ctx);
  void invalidate_parent_state();

  struct Tree {
    MetalLevelData::TreeBuffers* buffers = nullptr;
    const std::vector<tfrag3::StripDraw>* draws = nullptr;
    const std::vector<tfrag3::InstancedStripDraw>* wind_draws = nullptr;
    const tfrag3::PackedTimeOfDay* colors = nullptr;
    const tfrag3::BVH* vis = nullptr;
    const std::vector<std::string>* proto_names = nullptr;
    std::array<u32, tfrag3::kNumTieCategories + 1> category_draw_indices = {};
    bool use_strips = true;
    bool has_proto_visibility = false;

    // per-frame, filled by the visibility pass before the category draws
    std::vector<u8> proto_visible;
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
  std::vector<std::string> m_hidden_proto_names;
  math::Vector4f m_envmap_color{1.f, 1.f, 1.f, 1.f};
  Stats m_stats;
  bool m_apply_proto_visibility = false;
  bool m_parent_state_valid = false;
  u64 m_parent_state_frame = 0;
  bool m_warned_missing_level = false;
};

class MetalTieEnvmap : public MetalBucketRenderer {
 public:
  MetalTieEnvmap(const std::string& name, int my_id, MetalTie3* parent)
      : MetalBucketRenderer(name, my_id), m_parent(parent) {}

  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;

 private:
  MetalTie3* m_parent = nullptr;
};
