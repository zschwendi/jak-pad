/*!
 * @file metal_shrub.mm
 * See metal_shrub.h.
 */

#include "game/graphics/pipelines/metal/metal_shrub.h"

#include <algorithm>
#include <cstring>

#include "common/log/log.h"

#include "game/graphics/texture/TexturePool.h"

namespace {

constexpr u32 kPcPortVif = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;

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

}  // namespace

MetalShrub::MetalShrub(const std::string& name, int my_id) : MetalBucketRenderer(name, my_id) {
  m_color_result.resize(kMetalTimeOfDayColorCount);
}

void MetalShrub::render(DmaFollower& dma,
                        MetalSharedRenderState* render_state,
                        MetalFrameContext& ctx) {
  m_stats = {};
  m_apply_proto_visibility = false;
  auto* bg = render_state->background;
  auto expect = [&](bool ok, const char* what) {
    return metal_background_expect(ok, m_name, what, bg);
  };

  const bool is_jak2 = render_state->version == GameVersion::Jak2;
  if (is_jak2) {
    const auto opening_tag = dma.current_tag();
    if (!expect(opening_tag.kind == DmaTag::Kind::NEXT && opening_tag.qwc == 0 &&
                    !opening_tag.spr && is_opening_vif0(dma.current_tag_vif0()) &&
                    is_nop_vif(dma.current_tag_vif1()),
                "an empty Jak 2 NEXT opening with NOP or MARK VIF state")) {
      metal_finish_bucket(dma, *render_state);
      return;
    }
  }

  auto data0 = dma.read_and_advance();
  if (!is_jak2 &&
      !expect(data0.size_bytes == 0 &&
                  (data0.vif1() == 0 || data0.vifcode1().kind == VifCode::Kind::NOP),
              "the bucket to open with an empty NEXT")) {
    metal_finish_bucket(dma, *render_state);
    return;
  }

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run
    metal_finish_bucket(dma, *render_state);
    return;
  }
  if (dma.current_tag_offset() == render_state->next_bucket) {
    return;
  }

  if (is_jak2) {
    const auto pc_tag = dma.current_tag();
    if (!expect(pc_tag.kind == DmaTag::Kind::CNT && pc_tag.qwc == 25 && pc_tag.addr == 0 &&
                    !pc_tag.spr && dma.current_tag_vif0() == 0 &&
                    dma.current_tag_vif1() == kPcPortVif,
                "an exact Jak 2 CNT qwc25 PC_PORT background block")) {
      metal_finish_bucket(dma, *render_state);
      return;
    }
  }

  auto pc_port_data = dma.read_and_advance();
  const bool have_data = is_jak2 ||
                         expect(pc_port_data.size_bytes == sizeof(MetalTfragPcPortData),
                                "a PC port packet of TfragPcPortData size");
  MetalTfragPcPortData parsed_pc_port_data = {};
  if (have_data) {
    memcpy(&parsed_pc_port_data, pc_port_data.data, sizeof(MetalTfragPcPortData));
    parsed_pc_port_data.level_name[11] = '\0';
  }

  std::vector<std::string> hidden_names;
  bool have_proto_mask = true;
  if (is_jak2 && have_data) {
    if (dma.current_tag_offset() == render_state->next_bucket) {
      expect(false, "one bounded CNT PC_PORT hidden-prototype-name transfer before the boundary");
      return;
    }

    const auto mask_offset = dma.current_tag_offset();
    const auto mask_tag = dma.current_tag();
    const u64 mask_end = static_cast<u64>(mask_offset) + 16 +
                         static_cast<u64>(mask_tag.qwc) * 16;
    const bool mask_fits_bucket = mask_offset < render_state->next_bucket &&
                                  mask_end < render_state->next_bucket;
    if (!expect(mask_tag.kind == DmaTag::Kind::CNT && mask_tag.addr == 0 && !mask_tag.spr &&
                    dma.current_tag_vif0() == 0 && dma.current_tag_vif1() == kPcPortVif &&
                    mask_fits_bucket,
                "one bounded CNT PC_PORT hidden-prototype-name transfer")) {
      metal_finish_bucket(dma, *render_state);
      return;
    }

    const auto proto_mask = dma.read_and_advance();
    const auto tail = dma.current_tag();
    const bool exact_tail = tail.kind == DmaTag::Kind::NEXT && tail.qwc == 0 && !tail.spr &&
                            tail.addr == render_state->next_bucket &&
                            dma.current_tag_vif0() == 0 && dma.current_tag_vif1() == 0;
    have_proto_mask =
        expect(exact_tail && parse_hidden_proto_names(proto_mask, &hidden_names),
               "one terminated PC_PORT hidden-prototype-name transfer and exact NEXT tail");
  }

  if (!is_jak2 && have_data) {
    m_pc_port_data = parsed_pc_port_data;
    if (bg) {
      bg->observe_camera(m_pc_port_data.camera, m_name);
    }
  }
  metal_finish_bucket(dma, *render_state);
  if (!have_data || !have_proto_mask) {
    return;
  }
  if (is_jak2) {
    m_pc_port_data = parsed_pc_port_data;
    if (bg) {
      bg->observe_camera(m_pc_port_data.camera, m_name);
    }
  }

  m_settings.camera = m_pc_port_data.camera;
  m_settings.tree_idx = 0;
  m_stats.level_name = m_pc_port_data.level_name;

  if (!setup_for_level(m_pc_port_data.level_name)) {
    m_stats.level_missing = true;
    if (bg) {
      bg->missing_levels++;
    }
    if (!m_warned_missing_level) {
      m_warned_missing_level = true;
      lg::warn("Metal {}: level '{}' is not loaded, its shrub geometry is not drawn", m_name,
               m_pc_port_data.level_name);
    }
    return;
  }

  if (is_jak2 && !configure_proto_visibility(hidden_names, bg)) {
    return;
  }

  for (size_t i = 0; i < m_trees.size(); i++) {
    render_tree((int)i, render_state, ctx);
  }
}

bool MetalShrub::setup_for_level(const std::string& level) {
  auto* level_data = metal_level_data::get(level);
  if (!level_data) {
    m_level = nullptr;
    m_level_name.clear();
    m_trees.clear();
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

void MetalShrub::update_load(MetalLevelData* level_data) {
  m_trees.clear();
  size_t max_draws = 0;
  for (size_t i = 0; i < level_data->level->shrub_trees.size(); i++) {
    const auto& in = level_data->level->shrub_trees[i];
    auto& out = m_trees.emplace_back();
    out.buffers = &level_data->shrub[i];
    out.draws = &in.static_draws;
    out.colors = &in.time_of_day_colors;
    out.proto_names = &in.proto_names;
    out.proto_visible.resize(in.proto_names.size(), true);
    max_draws = std::max(max_draws, in.static_draws.size());
    // the palette texture is a fixed size so color indices line up
    metal_background_expect(in.time_of_day_colors.color_count <= kMetalTimeOfDayColorCount,
                            m_name, "the tree's time-of-day palette to fit the palette texture",
                            nullptr);
  }
  m_draw_runs.resize(max_draws);
  m_runs.resize(max_draws);
}

bool MetalShrub::configure_proto_visibility(const std::vector<std::string>& hidden_names,
                                            MetalBackgroundState* background) {
  for (const auto& tree : m_trees) {
    for (const auto& draw : *tree.draws) {
      if (draw.proto_idx >= tree.proto_names->size()) {
        return metal_background_expect(false, m_name,
                                       "every shrub draw proto_idx to name an FR3 prototype",
                                       background);
      }
    }
  }

  for (auto& tree : m_trees) {
    for (std::size_t i = 0; i < tree.proto_names->size(); i++) {
      tree.proto_visible[i] =
          std::find(hidden_names.begin(), hidden_names.end(), tree.proto_names->at(i)) ==
          hidden_names.end();
    }
  }
  m_apply_proto_visibility = true;
  return true;
}

void MetalShrub::render_tree(int idx,
                             MetalSharedRenderState* render_state,
                             MetalFrameContext& ctx) {
  auto& tree = m_trees.at(idx);
  auto* bg = render_state->background;
  id<MTLRenderCommandEncoder> enc = ctx.enc;

  metal_interp_time_of_day(m_settings.camera.itimes, *tree.colors, m_color_result.data());
  metal_update_time_of_day_texture(tree.buffers->time_of_day, m_color_result.data(),
                                   tree.colors->color_count);

  // Shrub has no geometric visibility data, so every draw gets a complete run.
  // Jak 2's prototype-name visibility is applied while issuing those runs.
  metal_make_all_visible_draw_runs(m_draw_runs.data(), m_runs.data(), *tree.draws);

  MetalBackgroundVsParams vs_params;
  metal_fill_background_vs_params(m_settings.camera, render_state->version, &vs_params);
  MetalBackgroundFsParams fs_params;
  metal_fill_background_fs_params(*render_state, &fs_params);

  [enc setVertexBuffer:tree.buffers->vertices offset:0 atIndex:0];
  [enc setVertexBytes:&vs_params length:sizeof(vs_params) atIndex:1];
  [enc setVertexTexture:tree.buffers->time_of_day atIndex:1];

  int draws_this_tree = 0;
  int tris_this_tree = 0;

  for (size_t draw_idx = 0; draw_idx < tree.draws->size(); draw_idx++) {
    const auto& draw = tree.draws->operator[](draw_idx);
    if (m_apply_proto_visibility && !tree.proto_visible[draw.proto_idx]) {
      continue;
    }
    const auto& run = m_runs[m_draw_runs[draw_idx].first];
    if (run.index_count == 0) {
      continue;
    }

    auto settings =
        metal_background_settings_from_draw_mode(draw.mode, MetalShaderId::SHRUB, ctx, bg);
    id<MTLTexture> tex =
        metal_background_texture(*m_level, (s32)draw.tree_tex_id, render_state, bg);

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

    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                    indexCount:run.index_count
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:tree.buffers->indices
             indexBufferOffset:run.first_index * sizeof(u32)];
    draws_this_tree++;
    tris_this_tree += (int)draw.num_triangles;

    if (settings.afail_double_draw) {
      auto no_write = settings.depth;
      no_write.depth_write = false;
      [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(no_write)];
      fs_params.alpha_min = -10.f;
      fs_params.alpha_max = settings.aref_second;
      [enc setFragmentBytes:&fs_params length:sizeof(fs_params) atIndex:0];
      [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                      indexCount:run.index_count
                       indexType:MTLIndexTypeUInt32
                     indexBuffer:tree.buffers->indices
               indexBufferOffset:run.first_index * sizeof(u32)];
      draws_this_tree++;
    }
  }

  m_stats.trees_rendered++;
  m_stats.draws += draws_this_tree;
  m_stats.triangles += tris_this_tree;
  ctx.draw_calls += draws_this_tree;
  ctx.triangles += tris_this_tree;
  if (bg) {
    bg->shrub_draws += draws_this_tree;
    bg->shrub_tris += tris_this_tree;
  }
}
