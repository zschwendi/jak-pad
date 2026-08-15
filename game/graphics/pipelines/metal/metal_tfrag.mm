/*!
 * @file metal_tfrag.mm
 * See metal_tfrag.h.
 */

#include "game/graphics/pipelines/metal/metal_tfrag.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "common/goal_constants.h"
#include "common/log/log.h"

#include "game/graphics/pipelines/metal/metal_vis_data.h"
#include "game/graphics/texture/TexturePool.h"

namespace {

// VU memory layout constants from TFragment.h - only used to check the
// double-buffer setup packet, which is what proves the walk stayed in sync.
constexpr int kBuffer0Start = 0;
constexpr int kBuffer1Start = 328;
constexpr int kTFragSetup = 0;

bool looks_like_tfragment_dma(const DmaFollower& follow) {
  return follow.current_tag_vifcode0().kind == VifCode::Kind::STCYCL;
}

bool looks_like_tfrag_init(const DmaFollower& follow) {
  return follow.current_tag_vifcode0().kind == VifCode::Kind::NOP &&
         follow.current_tag_vifcode1().kind == VifCode::Kind::DIRECT &&
         follow.current_tag_vifcode1().immediate == 2;
}

}  // namespace

MetalTFragment::MetalTFragment(const std::string& name,
                               int my_id,
                               const std::vector<tfrag3::TFragmentTreeKind>& tree_kinds,
                               int level_id,
                               bool does_vis_copy,
                               bool child_mode)
    : MetalBucketRenderer(name, my_id),
      m_tree_kinds(tree_kinds),
      m_level_id(level_id),
      m_child_mode(child_mode),
      m_does_vis_copy(does_vis_copy) {
  // Fixed maximum so the shaders' color indices line up regardless of how many
  // colors a level actually has (same as the GL renderer).
  m_color_result.resize(kMetalTimeOfDayColorCount);
  const char* log_visibility = std::getenv("GOALPAD_JAK2_DEBUG_LOG_TFRAG_BUCKET8_VISIBILITY");
  m_log_bucket8_visibility = my_id == 8 && log_visibility && std::strcmp(log_visibility, "1") == 0;
}

/*!
 * Port of TFragment::handle_initialization. The matrix and constant uploads
 * feed the VU1 program, which the PC path replaces entirely - they are read to
 * keep the walk in sync and checked so a chain that does not match is reported.
 * The PC port packet carries everything the renderer needs.
 *
 * Returns false if the packet was not the expected shape, in which case the
 * caller skips the rest of the bucket.
 */
bool MetalTFragment::handle_initialization(DmaFollower& dma, MetalBackgroundState* bg) {
  auto expect = [&](bool ok, const char* what) {
    return metal_background_expect(ok, m_name, what, bg);
  };

  auto setup_test = dma.read_and_advance();
  if (!expect(setup_test.vif0() == 0 && setup_test.vifcode1().kind == VifCode::Kind::DIRECT &&
                  setup_test.vifcode1().immediate == 2 && setup_test.size_bytes == 32,
              "a 32-byte DIRECT GS test setup")) {
    return false;
  }

  dma.read_and_advance();  // matrix 0
  dma.read_and_advance();  // matrix 1
  dma.read_and_advance();  // frame data

  auto mscal_setup = dma.read_and_advance();
  if (!expect(mscal_setup.vifcode1().kind == VifCode::Kind::MSCAL &&
                  mscal_setup.vifcode1().immediate == kTFragSetup,
              "an MSCAL of the tfrag setup program")) {
    return false;
  }

  auto pc_port_data = dma.read_and_advance();
  if (!expect(pc_port_data.size_bytes == sizeof(MetalTfragPcPortData),
              "a PC port packet of TfragPcPortData size")) {
    return false;
  }
  memcpy(&m_pc_port_data, pc_port_data.data, sizeof(MetalTfragPcPortData));
  m_pc_port_data.level_name[11] = '\0';
  if (bg) {
    bg->observe_camera(m_pc_port_data.camera, m_name);
  }

  auto db_setup = dma.read_and_advance();
  return expect(db_setup.size_bytes == 0 &&
                    db_setup.vifcode0().kind == VifCode::Kind::BASE &&
                    db_setup.vifcode0().immediate == kBuffer0Start &&
                    db_setup.vifcode1().kind == VifCode::Kind::OFFSET &&
                    db_setup.vifcode1().immediate == (kBuffer1Start - kBuffer0Start),
                "the VU double-buffer BASE/OFFSET setup");
}

void MetalTFragment::render(DmaFollower& dma,
                            MetalSharedRenderState* render_state,
                            MetalFrameContext& ctx) {
  m_stats = {};
  m_stats.level_id = m_level_id;
  auto* bg = render_state->background;
  auto expect = [&](bool ok, const char* what) {
    return metal_background_expect(ok, m_name, what, bg);
  };

  // First thing should be a NEXT with two nops - unless we are a child, in
  // which case the parent renderer already took it.
  if (!m_child_mode) {
    auto data0 = dma.read_and_advance();
    if (!expect(data0.vifcode1().kind == VifCode::Kind::NOP && data0.size_bytes == 0 &&
                    (data0.vif0() == 0 || data0.vifcode0().kind == VifCode::Kind::MARK),
                "the bucket to open with an empty NEXT")) {
      metal_finish_bucket(dma, *render_state);
      return;
    }
  }

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run
    metal_finish_bucket(dma, *render_state);
    return;
  }

  // The game hangs the per-level occlusion strings off one bucket.
  if (m_does_vis_copy && dma.current_tag_vifcode1().kind == VifCode::Kind::PC_PORT) {
    DmaTransfer transfers[jak1::LEVEL_MAX];
    for (int i = 0; i < jak1::LEVEL_MAX; i++) {
      transfers[i] = dma.read_and_advance();
      dma.read_and_advance();
    }
    for (int i = 0; i < jak1::LEVEL_MAX; i++) {
      if (transfers[i].size_bytes == 128 * 16) {
        if (bg && bg->use_occlusion_culling) {
          bg->visibility.levels[i].valid = true;
          memcpy(bg->visibility.levels[i].data.data(), transfers[i].data, 128 * 16);
        }
      } else {
        // 16 bytes means "this level has no visibility this frame"
        expect(transfers[i].size_bytes == 16, "an occlusion string of 2048 or 16 bytes");
      }
    }
  }

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    metal_finish_bucket(dma, *render_state);
    return;
  }

  std::string level_name;
  while (looks_like_tfrag_init(dma)) {
    if (!handle_initialization(dma, bg)) {
      metal_finish_bucket(dma, *render_state);
      return;
    }
    if (level_name.empty()) {
      level_name = m_pc_port_data.level_name;
    } else if (level_name != m_pc_port_data.level_name) {
      // one bucket is one level; if that ever stops holding, draw the first
      // and say so rather than mixing two levels' geometry
      expect(false, "one level name per bucket");
      break;
    }
    while (looks_like_tfragment_dma(dma)) {
      dma.read_and_advance();
    }
  }

  metal_finish_bucket(dma, *render_state);

  if (level_name.empty()) {
    return;
  }
  m_stats.level_name = level_name;

  if (!setup_for_level(level_name)) {
    m_stats.level_missing = true;
    if (bg) {
      bg->missing_levels++;
    }
    if (!m_warned_missing_level) {
      m_warned_missing_level = true;
      lg::warn("Metal {}: level '{}' is not loaded, its tfrag geometry is not drawn", m_name,
               level_name);
    }
    return;
  }

  MetalTfragRenderSettings settings;
  settings.camera = m_pc_port_data.camera;
  settings.tree_idx = 0;
  if (bg && bg->visibility.levels[m_level_id].valid) {
    settings.occlusion_culling = bg->visibility.levels[m_level_id].data.data();
  }
  m_stats.occlusion_valid = settings.occlusion_culling != nullptr;
  m_stats.all_visible_override = bg && bg->debug_all_visible;

  // lod: the GL renderer exposes lod_tfrag as a debug setting, default 0.
  render_matching_trees(0, settings, render_state, ctx);
  if (m_log_bucket8_visibility && m_bucket8_visibility_logs < 8) {
    lg::info("GOALPAD_JAK2_TFRAG_BUCKET8_VIS sample={} name={} level={} occ={} override={} "
             "nodes=v{}/f{}/o{}/t{} groups=v{}/a{}/t{} trees={} draws={} runs={} tris={}",
             m_bucket8_visibility_logs, m_stats.level_name, m_stats.level_id,
             m_stats.occlusion_valid, m_stats.all_visible_override, m_stats.visible_nodes,
             m_stats.frustum_visible_nodes, m_stats.occlusion_visible_nodes, m_stats.bvh_nodes,
             m_stats.visible_vis_groups, m_stats.always_visible_vis_groups, m_stats.vis_groups,
             m_stats.trees_rendered, m_stats.draws, m_stats.runs, m_stats.triangles);
    m_bucket8_visibility_logs++;
  }
}

/*!
 * Port of TFragment::setup_for_level: pick up the level's GPU data, rebuilding
 * the tree cache if the level (or its load) changed.
 */
bool MetalTFragment::setup_for_level(const std::string& level) {
  auto* level_data = metal_level_data::get(level);
  if (!level_data) {
    m_level = nullptr;
    m_level_name.clear();
    for (auto& trees : m_cached_trees) {
      trees.clear();
    }
    return false;
  }

  if (m_level_name != level || m_load_id != level_data->load_id) {
    update_load(level_data);
    m_level = level_data;
    m_level_name = level;
    m_load_id = level_data->load_id;
  }
  return true;
}

void MetalTFragment::update_load(MetalLevelData* level_data) {
  for (auto& trees : m_cached_trees) {
    trees.clear();
  }

  size_t vis_temp_len = 0;
  size_t max_draws = 0;
  size_t max_num_grps = 0;

  for (int geom = 0; geom < tfrag3::TFRAG_GEOS; geom++) {
    auto& in_trees = level_data->level->tfrag_trees[geom];
    for (size_t tree_idx = 0; tree_idx < in_trees.size(); tree_idx++) {
      const auto& tree = in_trees[tree_idx];
      if (std::find(m_tree_kinds.begin(), m_tree_kinds.end(), tree.kind) == m_tree_kinds.end()) {
        continue;
      }
      auto& cache = m_cached_trees[geom].emplace_back();
      cache.kind = tree.kind;
      cache.buffers = &level_data->tfrag[geom][tree_idx];
      cache.draws = &tree.draws;
      cache.colors = &tree.colors;
      cache.vis = &tree.bvh;
      cache.use_strips = tree.use_strips;

      max_draws = std::max(max_draws, tree.draws.size());
      size_t num_grps = 0;
      for (auto& draw : tree.draws) {
        num_grps += draw.vis_groups.size();
      }
      max_num_grps = std::max(max_num_grps, num_grps);
      vis_temp_len = std::max(vis_temp_len, tree.bvh.vis_nodes.size());
      // the palette texture is a fixed size so color indices line up; a level
      // with more colors than that would sample the wrong ones, so say so.
      metal_background_expect(tree.colors.color_count <= kMetalTimeOfDayColorCount, m_name,
                              "the tree's time-of-day palette to fit the palette texture",
                              nullptr);
    }
  }

  m_cache.vis_temp.resize(vis_temp_len);
  m_cache.draw_runs.resize(max_draws);
  // worst case: every vis group is its own run
  m_cache.runs.resize(max_num_grps);
}

void MetalTFragment::render_matching_trees(int geom,
                                           const MetalTfragRenderSettings& settings,
                                           MetalSharedRenderState* render_state,
                                           MetalFrameContext& ctx) {
  MetalTfragRenderSettings settings_copy = settings;
  for (size_t i = 0; i < m_cached_trees[geom].size(); i++) {
    settings_copy.tree_idx = (int)i;
    render_tree(geom, settings_copy, render_state, ctx);
    m_stats.trees_rendered++;
  }
}

void MetalTFragment::render_tree(int geom,
                                 const MetalTfragRenderSettings& settings,
                                 MetalSharedRenderState* render_state,
                                 MetalFrameContext& ctx) {
  auto& tree = m_cached_trees.at(geom).at(settings.tree_idx);
  auto* bg = render_state->background;
  id<MTLRenderCommandEncoder> enc = ctx.enc;

  // time of day: interpolate the packed palettes for this frame's itimes, then
  // refresh the tree's 1D palette texture the vertex shader reads.
  metal_interp_time_of_day(settings.camera.itimes, *tree.colors, m_color_result.data());
  metal_update_time_of_day_texture(tree.buffers->time_of_day, m_color_result.data(),
                                   tree.colors->color_count);

  const bool all_visible_override = bg && bg->debug_all_visible;
  if (!all_visible_override || m_log_bucket8_visibility) {
    metal_cull_check_all_slow(settings.camera.planes, tree.vis->vis_nodes,
                              settings.occlusion_culling, m_cache.vis_temp.data());
  }
  if (m_log_bucket8_visibility) {
    // Keep the natural cull result even when the all-visible debug override is
    // active so this diagnostic can distinguish visibility data from frustum
    // rejection without changing the rendered result.
    m_stats.bvh_nodes += (int)tree.vis->vis_nodes.size();
    for (size_t i = 0; i < tree.vis->vis_nodes.size(); i++) {
      const auto& node = tree.vis->vis_nodes[i];
      const bool frustum_visible = metal_sphere_in_view_ref(node.bsphere, settings.camera.planes);
      const bool occlusion_visible =
          !settings.occlusion_culling ||
          (node.my_id != UINT16_MAX &&
           node.my_id / 8 < metal_renderer::kMetalVisibilityBytes &&
           (settings.occlusion_culling[node.my_id / 8] & (1 << (7 - (node.my_id & 7)))));
      m_stats.frustum_visible_nodes += frustum_visible;
      m_stats.occlusion_visible_nodes += occlusion_visible;
      m_stats.visible_nodes += m_cache.vis_temp[i] != 0;
    }
    for (const auto& draw : *tree.draws) {
      for (const auto& group : draw.vis_groups) {
        m_stats.vis_groups++;
        if (group.vis_idx_in_pc_bvh == UINT16_MAX) {
          m_stats.always_visible_vis_groups++;
          m_stats.visible_vis_groups++;
        } else if (group.vis_idx_in_pc_bvh < m_cache.vis_temp.size() &&
                   m_cache.vis_temp[group.vis_idx_in_pc_bvh]) {
          m_stats.visible_vis_groups++;
        }
      }
    }
  }

  // visibility -> draw runs
  u32 total_tris;
  if (all_visible_override) {
    total_tris = metal_make_all_visible_draw_runs(m_cache.draw_runs.data(), m_cache.runs.data(),
                                                  *tree.draws);
  } else {
    total_tris = metal_make_draw_runs_from_vis_string(
        m_cache.draw_runs.data(), m_cache.runs.data(), *tree.draws, m_cache.vis_temp);
  }

  MetalBackgroundVsParams vs_params;
  metal_fill_background_vs_params(settings.camera, render_state->version, &vs_params);
  MetalBackgroundFsParams fs_params;
  metal_fill_background_fs_params(*render_state, &fs_params);

  [enc setVertexBuffer:tree.buffers->vertices offset:0 atIndex:0];
  [enc setVertexBytes:&vs_params length:sizeof(vs_params) atIndex:1];
  [enc setVertexTexture:tree.buffers->time_of_day atIndex:1];

  const auto primitive =
      tree.use_strips ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle;
  int draws_this_tree = 0;

  for (size_t draw_idx = 0; draw_idx < tree.draws->size(); draw_idx++) {
    const auto& draw = tree.draws->operator[](draw_idx);
    const auto& runs = m_cache.draw_runs[draw_idx];
    if (runs.second == 0) {
      continue;
    }

    auto settings_for_draw =
        metal_background_settings_from_draw_mode(draw.mode, MetalShaderId::TFRAG3, ctx, bg);
    id<MTLTexture> tex = metal_background_texture(*m_level, draw.tree_tex_id, render_state, bg);

    [enc setRenderPipelineState:ctx.pso_cache->get_pipeline(settings_for_draw.pso)];
    [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(settings_for_draw.depth)];
    [enc setFragmentTexture:tex atIndex:0];
    [enc setFragmentSamplerState:ctx.sampler_cache->get(settings_for_draw.sampler) atIndex:0];
    if (settings_for_draw.needs_blend_color) {
      [enc setBlendColorRed:0.5f green:0.5f blue:0.5f alpha:0.5f];
    }

    MetalBackgroundDrawParams draw_params;
    draw_params.decal = draw.mode.get_decal() ? 1 : 0;
    [enc setVertexBytes:&draw_params length:sizeof(draw_params) atIndex:2];

    fs_params.alpha_min = settings_for_draw.aref_first;
    fs_params.alpha_max = 10.f;
    [enc setFragmentBytes:&fs_params length:sizeof(fs_params) atIndex:0];

    for (u32 r = 0; r < runs.second; r++) {
      const auto& run = m_cache.runs[runs.first + r];
      [enc drawIndexedPrimitives:primitive
                      indexCount:run.index_count
                       indexType:MTLIndexTypeUInt32
                     indexBuffer:tree.buffers->indices
               indexBufferOffset:run.first_index * sizeof(u32)];
      m_stats.runs++;
    }
    draws_this_tree++;

    if (settings_for_draw.afail_double_draw) {
      // alpha-failing fragments again, without depth writes
      auto no_write = settings_for_draw.depth;
      no_write.depth_write = false;
      [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(no_write)];
      fs_params.alpha_min = -10.f;
      fs_params.alpha_max = settings_for_draw.aref_second;
      [enc setFragmentBytes:&fs_params length:sizeof(fs_params) atIndex:0];
      for (u32 r = 0; r < runs.second; r++) {
        const auto& run = m_cache.runs[runs.first + r];
        [enc drawIndexedPrimitives:primitive
                        indexCount:run.index_count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:tree.buffers->indices
                 indexBufferOffset:run.first_index * sizeof(u32)];
        m_stats.runs++;
      }
      draws_this_tree++;
    }
  }

  m_stats.draws += draws_this_tree;
  m_stats.triangles += (int)total_tris;
  ctx.draw_calls += draws_this_tree;
  ctx.triangles += (int)total_tris;
  if (bg) {
    bg->tfrag_draws += draws_this_tree;
    bg->tfrag_tris += (int)total_tris;
  }
}
