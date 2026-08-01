#pragma once

/*!
 * @file metal_tfrag.h
 * Metal port of the TFragment bucket renderer
 * (game/graphics/opengl_renderer/background/TFragment.cpp). Objective-C++ only.
 *
 * tfrag is the static terrain: the ground, cliffs and water of every Jak 1
 * level. The DMA chain carries only a control block - the camera, fog, the
 * level name and (in the level-0 bucket) the occlusion-visibility strings for
 * every loaded level. The geometry comes from the level's `.fr3`, uploaded by
 * the level-data stage (metal_level_data.h).
 *
 * The DMA walk, the per-frame BVH + occlusion culling, the visibility ->
 * draw-range conversion and the time-of-day palette interpolation are the GL
 * logic unchanged. The GL-specific parts are replaced at the draw boundary:
 * each StripDraw's DrawMode becomes a PSO key, a depth-stencil key and a
 * sampler key, and GL's one glMultiDrawElements per draw becomes one
 * drawIndexedPrimitives per visible run (Metal has no multidraw). Metal
 * restarts strips on the GL renderer's 0xFFFFFFFF index natively.
 *
 * Not ported: the BVH cull-debug overlay (an ImGui-only debug view), and the
 * texture-animator slots a negative texture index selects (Jak 2/3 only) -
 * those draws are counted and drawn with the pool's placeholder.
 */

#include <string>
#include <utility>
#include <vector>

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"

class MetalTFragment : public MetalBucketRenderer {
 public:
  // `does_vis_copy` marks the one bucket the game attaches the per-level
  // occlusion strings to (SharedRenderState::bucket_for_vis_copy; TFRAG_LEVEL0
  // on Jak 1).
  MetalTFragment(const std::string& name,
                 int my_id,
                 const std::vector<tfrag3::TFragmentTreeKind>& tree_kinds,
                 int level_id,
                 bool does_vis_copy);

  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;

  struct Stats {
    int trees_rendered = 0;
    int draws = 0;
    int runs = 0;
    int triangles = 0;
    bool level_missing = false;
    std::string level_name;
  };
  const Stats& stats() const { return m_stats; }

 private:
  bool handle_initialization(DmaFollower& dma, MetalBackgroundState* bg);
  bool setup_for_level(const std::string& level);
  void update_load(MetalLevelData* level_data);
  void render_matching_trees(int geom,
                             const MetalTfragRenderSettings& settings,
                             MetalSharedRenderState* render_state,
                             MetalFrameContext& ctx);
  void render_tree(int geom,
                   const MetalTfragRenderSettings& settings,
                   MetalSharedRenderState* render_state,
                   MetalFrameContext& ctx);

  struct TreeCache {
    tfrag3::TFragmentTreeKind kind = tfrag3::TFragmentTreeKind::INVALID;
    MetalLevelData::TreeBuffers* buffers = nullptr;
    const std::vector<tfrag3::StripDraw>* draws = nullptr;
    const tfrag3::PackedTimeOfDay* colors = nullptr;
    const tfrag3::BVH* vis = nullptr;
    bool use_strips = true;
  };

  std::vector<tfrag3::TFragmentTreeKind> m_tree_kinds;
  int m_level_id;
  bool m_does_vis_copy;

  MetalTfragPcPortData m_pc_port_data;
  std::string m_level_name;
  MetalLevelData* m_level = nullptr;
  u64 m_load_id = 0;

  std::array<std::vector<TreeCache>, tfrag3::TFRAG_GEOS> m_cached_trees;

  struct Cache {
    std::vector<u8> vis_temp;
    std::vector<std::pair<u32, u32>> draw_runs;
    std::vector<MetalDrawRun> runs;
  } m_cache;

  std::vector<math::Vector<u8, 4>> m_color_result;
  Stats m_stats;
  bool m_warned_missing_level = false;
};
