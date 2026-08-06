/*!
 * @file metal_tie.mm
 * See metal_tie.h.
 */

#include "game/graphics/pipelines/metal/metal_tie.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

#include "common/log/log.h"

#include "game/graphics/texture/TexturePool.h"

namespace {

// Jak 1's WindWork block, sent every frame. The wind-instanced draws that use
// it are not ported, but the transfer still has to be consumed to keep the walk
// in sync, and its size is what proves the packet is the one we think it is.
constexpr u32 kWindWorkBytes = 84 * 16;

constexpr u32 kPcPortVif = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
constexpr u32 kTieSetupStmodVif = static_cast<u32>(VifCode::Kind::STMOD) << 24;
constexpr u32 kTieSetupUnpackVif = (static_cast<u32>(VifCode::Kind::UNPACK_V4_32) << 24) |
                                    (10u << 16) | 0x3c6u;
constexpr u32 kTieSetupMscalfVif = (static_cast<u32>(VifCode::Kind::MSCALF) << 24) | 8u;
constexpr u32 kTieSetupFlushaVif = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
constexpr u32 kTieSetupStrowVif = static_cast<u32>(VifCode::Kind::STROW) << 24;
constexpr u32 kTieSetupDirectVif =
    (static_cast<u32>(VifCode::Kind::DIRECT) << 24) | 2u;

bool is_nop_vif(u32 vif) {
  return vif == 0 || VifCode(vif).kind == VifCode::Kind::NOP;
}

bool is_opening_vif0(u32 vif) {
  return is_nop_vif(vif) || VifCode(vif).kind == VifCode::Kind::MARK;
}

bool parse_hidden_proto_names(const DmaTransfer& transfer,
                              std::vector<std::string>* hidden_names) {
  std::vector<std::string> parsed;
  std::size_t offset = 0;
  while (offset < transfer.size_bytes) {
    while (offset < transfer.size_bytes && transfer.data[offset] == 0) {
      offset++;
    }
    if (offset == transfer.size_bytes) {
      *hidden_names = std::move(parsed);
      return true;
    }

    const u8* begin = transfer.data + offset;
    const u8* end = transfer.data + transfer.size_bytes;
    const u8* terminator = std::find(begin, end, 0);
    if (terminator == end) {
      return false;
    }
    parsed.emplace_back(reinterpret_cast<const char*>(begin),
                        static_cast<std::size_t>(terminator - begin));
    offset = static_cast<std::size_t>(terminator - transfer.data) + 1;
  }
  *hidden_names = std::move(parsed);
  return true;
}

std::atomic<bool> g_jak1_tie_envmap_second_pass_enabled{true};

}  // namespace

namespace metal_renderer {

void set_jak1_tie_envmap_second_pass_enabled(bool enabled) {
  g_jak1_tie_envmap_second_pass_enabled.store(enabled, std::memory_order_relaxed);
}

}  // namespace metal_renderer

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

  if (render_state->version == GameVersion::Jak2) {
    return set_up_jak2_common_data_from_dma(dma, render_state);
  }

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
  if (bg) {
    bg->observe_camera(m_pc_port_data.camera, m_name);
  }

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
  // Tie3::set_up_common_data_from_dma, Jak 1 branch: /128, then *2, then the
  // debug envmap-strength slider (1.0 here, as it is by default in GL).
  memcpy(m_envmap_color.data(), envmap_color.data, 16);
  m_envmap_color /= 128.f;
  m_envmap_color *= 2.f;

  m_settings.camera = m_pc_port_data.camera;
  m_settings.tree_idx = 0;
  m_settings.occlusion_culling =
      (bg && bg->visibility.levels[m_level_id].valid)
          ? bg->visibility.levels[m_level_id].data.data()
          : nullptr;
  return true;
}

bool MetalTie3::set_up_jak2_common_data_from_dma(DmaFollower& dma,
                                                 MetalSharedRenderState* render_state) {
  auto* bg = render_state->background;
  auto expect = [&](bool ok, const char* what) {
    return metal_background_expect(ok, m_name, what, bg);
  };
  auto expect_setup = [&](bool ok, const char* what) {
    if (ok) {
      return true;
    }
    const auto tag = dma.current_tag();
    return metal_background_expect(
        false, m_name,
        fmt::format("{} (got kind={} qwc={} addr={:#x} spr={} vif0={:#010x} vif1={:#010x})",
                    what, static_cast<int>(tag.kind), tag.qwc, tag.addr, tag.spr,
                    dma.current_tag_vif0(), dma.current_tag_vif1()),
        bg);
  };

  const auto opening_tag = dma.current_tag();
  const u32 opening_vif0 = dma.current_tag_vif0();
  const u32 opening_vif1 = dma.current_tag_vif1();
  const bool is_empty_bucket = opening_tag.kind == DmaTag::Kind::CNT && opening_tag.qwc == 0 &&
                               opening_tag.addr == 0 && !opening_tag.spr && opening_vif0 == 0 &&
                               opening_vif1 == 0;
  const bool is_populated_bucket =
      opening_tag.kind == DmaTag::Kind::NEXT && opening_tag.qwc == 0 && !opening_tag.spr &&
      is_opening_vif0(opening_vif0) && is_nop_vif(opening_vif1);
  if (!expect(is_empty_bucket || is_populated_bucket,
              "a source-shaped empty CNT or populated NEXT Jak 2 TIE opening")) {
    return false;
  }

  dma.read_and_advance();
  if (is_empty_bucket) {
    expect(dma.current_tag_offset() == render_state->next_bucket,
           "an empty Jak 2 TIE opening to land exactly at the next bucket");
    return false;
  }
  if (dma.current_tag_offset() == render_state->next_bucket) {
    expect(false, "the populated Jak 2 TIE opening to reach its setup segment");
    return false;
  }

  const auto constants_tag = dma.current_tag();
  if (!expect_setup(constants_tag.kind == DmaTag::Kind::CNT && constants_tag.qwc == 10 &&
                        constants_tag.addr == 0 && !constants_tag.spr &&
                        dma.current_tag_vif0() == kTieSetupStmodVif &&
                        dma.current_tag_vif1() == kTieSetupUnpackVif,
                    "the exact CNT qwc10 Jak 2 TIE constant setup")) {
    return false;
  }
  dma.read_and_advance();

  const auto mscalf_tag = dma.current_tag();
  if (!expect_setup(mscalf_tag.kind == DmaTag::Kind::CNT && mscalf_tag.qwc == 0 &&
                        mscalf_tag.addr == 0 && !mscalf_tag.spr &&
                        dma.current_tag_vif0() == kTieSetupMscalfVif &&
                        dma.current_tag_vif1() == kTieSetupFlushaVif,
                    "the exact zero-qword MSCALF/FLUSHA Jak 2 TIE setup")) {
    return false;
  }
  dma.read_and_advance();

  const auto row_tag = dma.current_tag();
  if (!expect_setup(row_tag.kind == DmaTag::Kind::CNT && row_tag.qwc == 2 && row_tag.addr == 0 &&
                        !row_tag.spr && dma.current_tag_vif0() == 0 &&
                        dma.current_tag_vif1() == kTieSetupStrowVif,
                    "the exact CNT qwc2 STROW Jak 2 TIE setup")) {
    return false;
  }
  dma.read_and_advance();

  const auto gs_tag = dma.current_tag();
  if (!expect_setup(gs_tag.kind == DmaTag::Kind::CNT && gs_tag.qwc == 2 && gs_tag.addr == 0 &&
                        !gs_tag.spr && dma.current_tag_vif0() == 0 &&
                        dma.current_tag_vif1() == kTieSetupDirectVif,
                    "the exact CNT qwc2 DIRECT Jak 2 TIE GS setup")) {
    return false;
  }
  dma.read_and_advance();

  const auto control_link = dma.current_tag();
  if (!expect(control_link.kind == DmaTag::Kind::NEXT && control_link.qwc == 0 &&
                  !control_link.spr && dma.current_tag_vif0() == 0 &&
                  dma.current_tag_vif1() == 0,
              "a zero-qword zero-VIF NEXT from setup to the first TIE control segment")) {
    return false;
  }
  dma.read_and_advance();
  if (dma.current_tag_offset() == render_state->next_bucket) {
    expect(false, "the Jak 2 TIE setup link to reach its first control segment");
    return false;
  }

  const auto pc_tag = dma.current_tag();
  if (!expect(pc_tag.kind == DmaTag::Kind::CNT && pc_tag.qwc == 25 && pc_tag.addr == 0 &&
                  !pc_tag.spr && dma.current_tag_vif0() == 0 &&
                  dma.current_tag_vif1() == kPcPortVif,
              "an exact Jak 2 CNT qwc25 PC_PORT TIE camera block")) {
    return false;
  }
  const auto pc_transfer = dma.read_and_advance();
  MetalTfragPcPortData parsed_pc = {};
  memcpy(&parsed_pc, pc_transfer.data, sizeof(parsed_pc));
  parsed_pc.level_name[11] = '\0';

  if (dma.current_tag_offset() == render_state->next_bucket) {
    expect(false, "one bounded TIE hidden-prototype-name transfer before the boundary");
    return false;
  }
  const auto mask_offset = dma.current_tag_offset();
  const auto mask_tag = dma.current_tag();
  const u64 mask_end = static_cast<u64>(mask_offset) + 16 +
                       static_cast<u64>(mask_tag.qwc) * 16;
  const bool mask_is_bounded = mask_end >= mask_offset &&
                               mask_end <= std::numeric_limits<u32>::max();
  if (!expect(mask_tag.kind == DmaTag::Kind::CNT && mask_tag.addr == 0 && !mask_tag.spr &&
                  dma.current_tag_vif0() == 0 && dma.current_tag_vif1() == kPcPortVif &&
                  mask_is_bounded,
              "one bounded CNT PC_PORT TIE hidden-prototype-name transfer")) {
    return false;
  }
  const auto mask_transfer = dma.read_and_advance();
  std::vector<std::string> hidden_names;
  if (!expect(parse_hidden_proto_names(mask_transfer, &hidden_names),
              "the TIE hidden-prototype names to be NUL terminated")) {
    return false;
  }

  if (dma.current_tag_offset() == render_state->next_bucket) {
    expect(false, "one TIE envmap tint transfer before the boundary");
    return false;
  }
  const auto tint_offset = dma.current_tag_offset();
  const auto tint_tag = dma.current_tag();
  const u64 tint_end = static_cast<u64>(tint_offset) + 16 +
                       static_cast<u64>(tint_tag.qwc) * 16;
  if (!expect(tint_tag.kind == DmaTag::Kind::CNT && tint_tag.qwc == 1 && tint_tag.addr == 0 &&
                  !tint_tag.spr && dma.current_tag_vif0() == 0 &&
                  dma.current_tag_vif1() == kPcPortVif &&
                  tint_end <= std::numeric_limits<u32>::max(),
              "one exact CNT qwc1 PC_PORT TIE envmap tint")) {
    return false;
  }
  const auto tint_transfer = dma.read_and_advance();
  const auto first_tail = dma.current_tag();
  if (!expect(dma.current_tag_offset() == static_cast<u32>(tint_end) &&
                  first_tail.kind == DmaTag::Kind::NEXT && first_tail.qwc == 0 &&
                  !first_tail.spr && dma.current_tag_vif0() == 0 &&
                  dma.current_tag_vif1() == 0,
              "the first TIE control segment to end at a zero-qword zero-VIF NEXT tail")) {
    return false;
  }

  math::Vector4f parsed_envmap_color;
  memcpy(parsed_envmap_color.data(), tint_transfer.data, sizeof(parsed_envmap_color));
  parsed_envmap_color /= 128.f;

  m_pc_port_data = parsed_pc;
  m_hidden_proto_names = std::move(hidden_names);
  m_envmap_color = parsed_envmap_color;
  m_settings.camera = parsed_pc.camera;
  m_settings.tree_idx = 0;
  m_settings.occlusion_culling =
      (bg && bg->visibility.levels[m_level_id].valid)
          ? bg->visibility.levels[m_level_id].data.data()
          : nullptr;
  return true;
}

void MetalTie3::render(DmaFollower& dma,
                       MetalSharedRenderState* render_state,
                       MetalFrameContext& ctx) {
  invalidate_parent_state();
  m_stats = {};
  m_apply_proto_visibility = false;
  m_hidden_proto_names.clear();
  const bool have_data = set_up_common_data_from_dma(dma, render_state);
  metal_finish_bucket(dma, *render_state);
  if (!have_data) {
    return;
  }
  m_stats.level_name = m_pc_port_data.level_name;
  if (render_state->version == GameVersion::Jak2 && render_state->background) {
    render_state->background->observe_camera(m_pc_port_data.camera, m_name);
  }

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

  if (render_state->version == GameVersion::Jak2 &&
      !configure_proto_visibility(m_hidden_proto_names, bg)) {
    return;
  }

  // lod: the GL renderer exposes lod_tie as a debug setting, default 0.
  constexpr int geom = 0;

  // Time of day + visibility, once per tree, before any category draws.
  prepare_trees(geom, render_state);

  // Both versions render NORMAL in the parent bucket.
  render_all_trees(geom, tfrag3::TieCategory::NORMAL, render_state, ctx);

  if (render_state->version == GameVersion::Jak2) {
    m_parent_state_valid = true;
    m_parent_state_frame = render_state->engine_frame_id;
    return;
  }

  // Tie3WithEnvmapJak1 completes both envmap draws for one tree before
  // advancing to the next, so a later tree's base draw can replace an earlier
  // tree's shine.
  const bool draw_envmap_second_pass =
      g_jak1_tie_envmap_second_pass_enabled.load(std::memory_order_relaxed);
  for (size_t i = 0; i < m_trees[geom].size(); i++) {
    render_tree(geom, (int)i, tfrag3::TieCategory::NORMAL_ENVMAP, render_state, ctx);
    if (draw_envmap_second_pass) {
      render_tree(geom, (int)i, tfrag3::TieCategory::NORMAL_ENVMAP_SECOND_DRAW, render_state, ctx);
    }
  }
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
      out.proto_names = &in.proto_names;
      out.category_draw_indices = in.category_draw_indices;
      out.use_strips = in.use_strips;
      out.has_proto_visibility = in.has_per_proto_visibility_toggle;
      out.proto_visible.resize(in.proto_names.size(), true);

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
    }
  }
}

bool MetalTie3::configure_proto_visibility(const std::vector<std::string>& hidden_names,
                                           MetalBackgroundState* background) {
  constexpr std::array<tfrag3::TieCategory, 3> kSupportedCategories = {
      tfrag3::TieCategory::NORMAL, tfrag3::TieCategory::NORMAL_ENVMAP,
      tfrag3::TieCategory::NORMAL_ENVMAP_SECOND_DRAW};
  constexpr int geom = 0;

  for (const auto& tree : m_trees[geom]) {
    if (!tree.has_proto_visibility) {
      continue;
    }
    for (const auto category : kSupportedCategories) {
      const u32 first = tree.category_draw_indices[(int)category];
      const u32 end = tree.category_draw_indices[(int)category + 1];
      if (first > end || end > tree.draws->size()) {
        return metal_background_expect(false, m_name,
                                       "supported TIE category ranges to fit their FR3 draws",
                                       background);
      }
    }
    for (const auto& draw : *tree.draws) {
      for (const auto& group : draw.vis_groups) {
        if (group.tie_proto_idx >= tree.proto_names->size()) {
          return metal_background_expect(
              false, m_name,
              "every TIE visibility group proto_idx to name an FR3 prototype", background);
        }
      }
    }
  }

  for (auto& tree : m_trees[geom]) {
    if (!tree.has_proto_visibility) {
      continue;
    }
    for (std::size_t i = 0; i < tree.proto_names->size(); i++) {
      tree.proto_visible[i] =
          std::find(hidden_names.begin(), hidden_names.end(), tree.proto_names->at(i)) ==
          hidden_names.end();
    }
  }
  m_apply_proto_visibility = true;
  return true;
}

void MetalTie3::prepare_trees(int geom, MetalSharedRenderState* render_state) {
  auto* bg = render_state->background;
  for (auto& tree : m_trees[geom]) {
    metal_interp_time_of_day(m_settings.camera.itimes, *tree.colors, m_color_result.data());
    metal_update_time_of_day_texture(tree.buffers->time_of_day, m_color_result.data(),
                                     tree.colors->color_count);
    const bool all_geometry_visible = bg && bg->debug_all_visible;
    if (!all_geometry_visible) {
      metal_cull_check_all_slow(m_settings.camera.planes, tree.vis->vis_nodes,
                                m_settings.occlusion_culling, tree.vis_temp.data());
    }
    make_draw_runs(tree, all_geometry_visible);
    m_stats.trees_rendered++;
    m_stats.wind_draws_skipped += (int)tree.wind_draws->size();
    if (bg) {
      bg->tie_wind_draws_skipped += (int)tree.wind_draws->size();
    }
  }
}

void MetalTie3::make_draw_runs(Tree& tree, bool all_geometry_visible) {
  u32 run_idx = 0;
  u32 sanity_check = 0;
  for (std::size_t draw_idx = 0; draw_idx < tree.draws->size(); draw_idx++) {
    const auto& draw = tree.draws->at(draw_idx);
    u32 index_idx = draw.unpacked.idx_of_first_idx_in_full_buffer;
    metal_background_expect(sanity_check == index_idx, "level index layout",
                            "the TIE draws to tile the index buffer without gaps", nullptr);
    std::pair<u32, u32> draw_run{run_idx, 0};
    u32 draw_tris = 0;
    bool building_run = false;
    u32 run_start = 0;
    for (const auto& group : draw.vis_groups) {
      sanity_check += group.num_inds;
      const bool geometry_visible =
          all_geometry_visible || group.vis_idx_in_pc_bvh == UINT16_MAX ||
          tree.vis_temp[group.vis_idx_in_pc_bvh];
      const bool proto_visible =
          !m_apply_proto_visibility || !tree.has_proto_visibility ||
          tree.proto_visible[group.tie_proto_idx];
      const bool visible = geometry_visible && proto_visible;
      if (visible) {
        draw_tris += group.num_tris;
      }
      if (building_run) {
        if (!visible) {
          tree.runs[run_idx] = {run_start, index_idx - run_start};
          draw_run.second++;
          run_idx++;
          building_run = false;
        }
      } else if (visible) {
        building_run = true;
        run_start = index_idx;
      }
      index_idx += group.num_inds;
    }
    if (building_run) {
      tree.runs[run_idx] = {run_start, index_idx - run_start};
      draw_run.second++;
      run_idx++;
    }
    tree.draw_runs[draw_idx] = draw_run;
    tree.draw_tris[draw_idx] = draw_tris;
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

void MetalTie3::render_envmap_from_parent(MetalSharedRenderState* render_state,
                                          MetalFrameContext& ctx) {
  if (!m_parent_state_valid || m_parent_state_frame != render_state->engine_frame_id) {
    invalidate_parent_state();
    return;
  }

  constexpr int geom = 0;
  for (std::size_t i = 0; i < m_trees[geom].size(); i++) {
    render_tree(geom, (int)i, tfrag3::TieCategory::NORMAL_ENVMAP, render_state, ctx);
    render_tree(geom, (int)i, tfrag3::TieCategory::NORMAL_ENVMAP_SECOND_DRAW, render_state, ctx);
  }
  invalidate_parent_state();
}

void MetalTie3::invalidate_parent_state() {
  m_parent_state_valid = false;
  m_parent_state_frame = 0;
}

void MetalTie3::render_tree(int geom,
                            int tree_idx,
                            tfrag3::TieCategory category,
                            MetalSharedRenderState* render_state,
                            MetalFrameContext& ctx) {
  auto& tree = m_trees.at(geom).at(tree_idx);
  auto* bg = render_state->background;
  id<MTLRenderCommandEncoder> enc = ctx.enc;

  const bool second_draw = category == tfrag3::TieCategory::NORMAL_ENVMAP_SECOND_DRAW;
  const bool use_envmap = second_draw || tfrag3::is_envmap_first_draw_category(category);
  const auto shader = second_draw ? MetalShaderId::ETIE
                                  : (use_envmap ? MetalShaderId::ETIE_BASE : MetalShaderId::TFRAG3);

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
    if (second_draw) {
      memcpy(vs_params.envmap_tod_tint, m_envmap_color.data(), sizeof(vs_params.envmap_tod_tint));
    }
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

  if (second_draw) {
    m_stats.envmap_second_draws += draws_this_tree;
    m_stats.envmap_second_triangles += tris_this_tree;
    if (bg) {
      bg->tie_envmap_second_draws += draws_this_tree;
      bg->tie_envmap_second_tris += tris_this_tree;
    }
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

void MetalTieEnvmap::render(DmaFollower& dma,
                            MetalSharedRenderState* render_state,
                            MetalFrameContext& ctx) {
  auto* bg = render_state->background;
  auto expect = [&](bool ok, const char* what) {
    return metal_background_expect(ok, m_name, what, bg);
  };

  const auto tag = dma.current_tag();
  const bool exact_empty = tag.kind == DmaTag::Kind::CNT && tag.qwc == 0 && tag.addr == 0 &&
                           !tag.spr && dma.current_tag_vif0() == 0 &&
                           dma.current_tag_vif1() == 0;
  if (!expect(exact_empty, "a source-empty Jak 2 ETIE CNT bucket")) {
    m_parent->invalidate_parent_state();
    metal_finish_bucket(dma, *render_state);
    return;
  }

  dma.read_and_advance();
  if (!expect(dma.current_tag_offset() == render_state->next_bucket,
              "the empty Jak 2 ETIE bucket to land exactly at the next bucket")) {
    m_parent->invalidate_parent_state();
    metal_finish_bucket(dma, *render_state);
    return;
  }

  m_parent->render_envmap_from_parent(render_state, ctx);
}
