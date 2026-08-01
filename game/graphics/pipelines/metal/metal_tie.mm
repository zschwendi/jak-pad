/*!
 * @file metal_tie.mm
 * See metal_tie.h.
 */

#include "game/graphics/pipelines/metal/metal_tie.h"

#include <algorithm>

#include "common/log/log.h"

#include "game/graphics/texture/TexturePool.h"

namespace {

// Jak 1's WindWork block, sent every frame. The wind-instanced draws that use
// it are not ported, but the transfer still has to be consumed to keep the walk
// in sync, and its size is what proves the packet is the one we think it is.
constexpr u32 kWindWorkBytes = 84 * 16;

}  // namespace

MetalTie3::MetalTie3(const std::string& name, int my_id, int level_id)
    : MetalBucketRenderer(name, my_id), m_level_id(level_id) {
  m_color_result.resize(kMetalTimeOfDayColorCount);
}

/*!
 * Port of Tie3::set_up_common_data_from_dma. Returns false when the bucket is
 * empty for this frame.
 */
bool MetalTie3::set_up_common_data_from_dma(DmaFollower& dma,
                                            MetalSharedRenderState* render_state) {
  auto* bg = render_state->background;
  auto expect = [&](bool ok, const char* what) {
    return metal_background_expect(ok, m_name, what, bg);
  };

  auto data0 = dma.read_and_advance();
  if (!expect(data0.size_bytes == 0 &&
                  (data0.vif1() == 0 || data0.vifcode1().kind == VifCode::Kind::NOP),
              "the bucket to open with an empty NEXT")) {
    return false;
  }

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run
    return false;
  }
  if (dma.current_tag_offset() == render_state->next_bucket) {
    return false;
  }

  auto gs_test = dma.read_and_advance();
  if (gs_test.size_bytes != 160) {
    if (!expect(gs_test.size_bytes == 32, "a 32- or 160-byte GS test setup")) {
      return false;
    }
    auto tie_consts = dma.read_and_advance();
    if (!expect(tie_consts.size_bytes == 9 * 16, "9 quadwords of tie constants")) {
      return false;
    }
  }

  auto mscalf = dma.read_and_advance();
  if (!expect(mscalf.size_bytes == 0, "an empty MSCALF packet")) {
    return false;
  }

  auto row = dma.read_and_advance();
  if (!expect(row.size_bytes == 32, "a 32-byte VIF row")) {
    return false;
  }

  auto next = dma.read_and_advance();
  if (next.size_bytes == 32) {
    next = dma.read_and_advance();
  }
  if (!expect(next.size_bytes == 0, "an empty packet before the PC port data")) {
    return false;
  }

  auto pc_port_data = dma.read_and_advance();
  if (!expect(pc_port_data.size_bytes == sizeof(MetalTfragPcPortData),
              "a PC port packet of TfragPcPortData size")) {
    return false;
  }
  memcpy(&m_pc_port_data, pc_port_data.data, sizeof(MetalTfragPcPortData));
  m_pc_port_data.level_name[11] = '\0';

  if (!expect(render_state->version == GameVersion::Jak1, "a Jak 1 chain")) {
    return false;
  }
  auto wind_data = dma.read_and_advance();
  if (!expect(wind_data.size_bytes == kWindWorkBytes, "a WindWork block")) {
    return false;
  }

  auto envmap_color = dma.read_and_advance();
  if (!expect(envmap_color.size_bytes == 16, "a one-quadword envmap color")) {
    return false;
  }

  m_settings.camera = m_pc_port_data.camera;
  m_settings.tree_idx = 0;
  m_settings.occlusion_culling =
      (bg && bg->occlusion_vis[m_level_id].valid) ? bg->occlusion_vis[m_level_id].data : nullptr;
  return true;
}

void MetalTie3::render(DmaFollower& dma,
                       MetalSharedRenderState* render_state,
                       MetalFrameContext& ctx) {
  m_stats = {};
  const bool have_data = set_up_common_data_from_dma(dma, render_state);
  metal_finish_bucket(dma, *render_state);
  if (!have_data) {
    return;
  }
  m_stats.level_name = m_pc_port_data.level_name;

  auto* bg = render_state->background;
  if (!setup_for_level(m_pc_port_data.level_name)) {
    m_stats.level_missing = true;
    if (bg) {
      bg->missing_levels++;
    }
    if (!m_warned_missing_level) {
      m_warned_missing_level = true;
      lg::warn("Metal {}: level '{}' is not loaded, its tie geometry is not drawn", m_name,
               m_pc_port_data.level_name);
    }
    return;
  }

  // lod: the GL renderer exposes lod_tie as a debug setting, default 0.
  constexpr int geom = 0;

  // Time of day + visibility, once per tree, before any category draws.
  for (size_t i = 0; i < m_trees[geom].size(); i++) {
    auto& tree = m_trees[geom][i];
    metal_interp_time_of_day(m_settings.camera.itimes, *tree.colors, m_color_result.data());
    metal_update_time_of_day_texture(tree.buffers->time_of_day, m_color_result.data(),
                                     tree.colors->color_count);
    if (bg && bg->debug_all_visible) {
      metal_make_all_visible_draw_runs(tree.draw_runs.data(), tree.runs.data(), *tree.draws,
                                       tree.draw_tris.data());
    } else {
      metal_cull_check_all_slow(m_settings.camera.planes, tree.vis->vis_nodes,
                                m_settings.occlusion_culling, tree.vis_temp.data());
      metal_make_draw_runs_from_vis_string(tree.draw_runs.data(), tree.runs.data(), *tree.draws,
                                           tree.vis_temp, tree.draw_tris.data());
    }
    m_stats.trees_rendered++;
    m_stats.wind_draws_skipped += (int)tree.wind_draws->size();
    const int second = (int)tfrag3::TieCategory::NORMAL_ENVMAP_SECOND_DRAW;
    m_stats.envmap_second_draws_skipped +=
        (int)(tree.category_draw_indices[second + 1] - tree.category_draw_indices[second]);
  }

  // Jak 1's TIE bucket draws the plain category, then the base draw of the
  // envmapped one (Tie3WithEnvmapJak1::render).
  render_all_trees(geom, tfrag3::TieCategory::NORMAL, render_state, ctx);
  render_all_trees(geom, tfrag3::TieCategory::NORMAL_ENVMAP, render_state, ctx);
}

bool MetalTie3::setup_for_level(const std::string& level) {
  auto* level_data = metal_level_data::get(level);
  if (!level_data) {
    m_level = nullptr;
    m_level_name.clear();
    for (auto& trees : m_trees) {
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

void MetalTie3::update_load(MetalLevelData* level_data) {
  for (int geo = 0; geo < tfrag3::TIE_GEOS; geo++) {
    auto& in_trees = level_data->level->tie_trees[geo];
    m_trees[geo].clear();
    m_trees[geo].resize(in_trees.size());
    for (size_t i = 0; i < in_trees.size(); i++) {
      const auto& in = in_trees[i];
      auto& out = m_trees[geo][i];
      out.buffers = &level_data->tie[geo][i];
      out.draws = &in.static_draws;
      out.wind_draws = &in.instanced_wind_draws;
      out.colors = &in.colors;
      out.vis = &in.bvh;
      out.category_draw_indices = in.category_draw_indices;
      out.use_strips = in.use_strips;

      size_t num_grps = 0;
      for (auto& draw : in.static_draws) {
        num_grps += draw.vis_groups.size();
      }
      out.vis_temp.resize(in.bvh.vis_nodes.size());
      out.draw_runs.resize(in.static_draws.size());
      out.draw_tris.resize(in.static_draws.size());
      out.runs.resize(num_grps);
      // the palette texture is a fixed size so color indices line up
      metal_background_expect(in.colors.color_count <= kMetalTimeOfDayColorCount, m_name,
                              "the tree's time-of-day palette to fit the palette texture",
                              nullptr);
      // per-proto visibility is Jak 2/3 only; this renderer is Jak 1
      metal_background_expect(!in.has_per_proto_visibility_toggle, m_name,
                              "a level without per-proto visibility toggles", nullptr);
    }
  }
}

void MetalTie3::render_all_trees(int geom,
                                 tfrag3::TieCategory category,
                                 MetalSharedRenderState* render_state,
                                 MetalFrameContext& ctx) {
  for (size_t i = 0; i < m_trees[geom].size(); i++) {
    render_tree(geom, (int)i, category, render_state, ctx);
  }
}

void MetalTie3::render_tree(int geom,
                            int tree_idx,
                            tfrag3::TieCategory category,
                            MetalSharedRenderState* render_state,
                            MetalFrameContext& ctx) {
  auto& tree = m_trees.at(geom).at(tree_idx);
  auto* bg = render_state->background;
  id<MTLRenderCommandEncoder> enc = ctx.enc;

  const bool use_envmap = tfrag3::is_envmap_first_draw_category(category);
  const auto shader = use_envmap ? MetalShaderId::ETIE_BASE : MetalShaderId::TFRAG3;

  const u32 first_draw = tree.category_draw_indices[(int)category];
  const u32 end_draw = tree.category_draw_indices[(int)category + 1];
  if (first_draw == end_draw) {
    return;
  }

  MetalBackgroundFsParams fs_params;
  metal_fill_background_fs_params(*render_state, &fs_params);

  [enc setVertexBuffer:tree.buffers->vertices offset:0 atIndex:0];
  [enc setVertexTexture:tree.buffers->time_of_day atIndex:1];
  if (use_envmap) {
    MetalEtieVsParams vs_params;
    metal_fill_etie_vs_params(m_settings.camera, render_state->version, &vs_params);
    [enc setVertexBytes:&vs_params length:sizeof(vs_params) atIndex:1];
  } else {
    MetalBackgroundVsParams vs_params;
    metal_fill_background_vs_params(m_settings.camera, render_state->version, &vs_params);
    [enc setVertexBytes:&vs_params length:sizeof(vs_params) atIndex:1];
  }

  const auto primitive =
      tree.use_strips ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle;
  int draws_this_tree = 0;
  int tris_this_tree = 0;

  for (u32 draw_idx = first_draw; draw_idx < end_draw; draw_idx++) {
    const auto& draw = tree.draws->operator[](draw_idx);
    const auto& runs = tree.draw_runs[draw_idx];
    if (runs.second == 0) {
      continue;
    }

    auto settings = metal_background_settings_from_draw_mode(draw.mode, shader, ctx, bg);
    // the GL renderer asserts that TIE never needs the alpha-fail double draw
    metal_background_expect(!settings.afail_double_draw, m_name,
                            "no TIE draw to need the alpha-fail double draw", bg);

    id<MTLTexture> tex = metal_background_texture(*m_level, draw.tree_tex_id, render_state, bg);
    [enc setRenderPipelineState:ctx.pso_cache->get_pipeline(settings.pso)];
    [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(settings.depth)];
    [enc setFragmentTexture:tex atIndex:0];
    [enc setFragmentSamplerState:ctx.sampler_cache->get(settings.sampler) atIndex:0];
    if (settings.needs_blend_color) {
      [enc setBlendColorRed:0.5f green:0.5f blue:0.5f alpha:0.5f];
    }

    MetalBackgroundDrawParams draw_params;
    draw_params.decal = draw.mode.get_decal() ? 1 : 0;
    [enc setVertexBytes:&draw_params length:sizeof(draw_params) atIndex:2];

    fs_params.alpha_min = settings.aref_first;
    fs_params.alpha_max = 10.f;
    [enc setFragmentBytes:&fs_params length:sizeof(fs_params) atIndex:0];

    for (u32 r = 0; r < runs.second; r++) {
      const auto& run = tree.runs[runs.first + r];
      [enc drawIndexedPrimitives:primitive
                      indexCount:run.index_count
                       indexType:MTLIndexTypeUInt32
                     indexBuffer:tree.buffers->indices
               indexBufferOffset:run.first_index * sizeof(u32)];
      m_stats.runs++;
    }
    draws_this_tree++;
    tris_this_tree += (int)tree.draw_tris[draw_idx];
  }

  m_stats.draws += draws_this_tree;
  m_stats.triangles += tris_this_tree;
  ctx.draw_calls += draws_this_tree;
  ctx.triangles += tris_this_tree;
  if (bg) {
    bg->tie_draws += draws_this_tree;
    bg->tie_tris += tris_this_tree;
  }
}
