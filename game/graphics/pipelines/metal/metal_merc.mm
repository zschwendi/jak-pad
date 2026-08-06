#include "metal_merc.h"

#include <array>
#include <cstring>

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/fnv.h"

#include "game/graphics/pipelines/metal/metal_eye_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_merc_dma.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/mips2c/jak1_bones_provenance_trace.h"

#include "fmt/format.h"

namespace {

metal_merc_transform_trace::ProvenanceObservation make_bones_provenance_observation(
    u32 source_address,
    u64 source_base,
    int root_bone,
    const float* output_matrix) {
  metal_merc_transform_trace::ProvenanceObservation out;
  if (source_base > UINT32_MAX) {
    return out;
  }

  const auto calculation =
      jak1_bones_provenance_trace::registry().find_source_address(source_address);
  if (!calculation) {
    return out;
  }
  const auto& target = calculation->target_control;
  auto& control = out.target_control;
  control.capture_stage = target.capture_stage;
  control.capture_result = target.capture_result;
  out.mapping_checked = true;
  if (calculation->output_base != source_base || !calculation->camera.valid) {
    return out;
  }

  if (root_bone < 1 || root_bone > static_cast<int>(calculation->root_anchors.size()) ||
      !calculation->root_anchors[root_bone - 1].valid) {
    return out;
  }
  const auto& root = calculation->root_anchors[root_bone - 1];
  const auto& bind_pose = calculation->root_bind_poses[root_bone - 1];
  out.input_root_bone = root_bone;

  std::array<u64, jak1_bones_provenance_trace::kRootAnchorCount> root_hashes = {};
  for (std::size_t anchor = 0; anchor < calculation->root_anchors.size(); anchor++) {
    const auto& snapshot = calculation->root_anchors[anchor];
    if (snapshot.valid) {
      root_hashes[anchor] = fnv64(snapshot.bytes.data(), snapshot.bytes.size());
    }
  }

  metal_merc_transform_trace::MatrixSnapshot root_matrix = {};
  metal_merc_transform_trace::MatrixSnapshot camera_matrix = {};
  memcpy(root_matrix.data(), root.bytes.data(), root.bytes.size());
  memcpy(camera_matrix.data(), calculation->camera.bytes.data(), calculation->camera.bytes.size());

  out.producer_serial = calculation->serial;
  out.input_root_hash = fnv64(root_hashes.data(), sizeof(root_hashes));
  out.camera_hash = fnv64(calculation->camera.bytes.data(), calculation->camera.bytes.size());
  out.input_root_basis = metal_merc_transform_trace::make_basis_snapshot(root_matrix.data());
  out.camera_basis = metal_merc_transform_trace::make_basis_snapshot(camera_matrix.data());
  out.output_basis = metal_merc_transform_trace::make_basis_snapshot(output_matrix);
  if (bind_pose.valid) {
    metal_merc_transform_trace::MatrixSnapshot bind_pose_matrix = {};
    memcpy(bind_pose_matrix.data(), bind_pose.bytes.data(), bind_pose.bytes.size());
    out.bind_pose_hash = fnv64(bind_pose.bytes.data(), bind_pose.bytes.size());
    out.output_expected_distance = metal_merc_transform_trace::output_composition_distance(
        camera_matrix.data(), root_matrix.data(), bind_pose_matrix.data(), output_matrix);
    out.expected_output_valid = std::isfinite(out.output_expected_distance);
  }
  out.input_translation_x = root_matrix[12];
  out.input_translation_y = root_matrix[13];
  out.input_translation_z = root_matrix[14];
  if (target.valid) {
    control.valid = true;
    control.producer_serial = calculation->serial;
    control.source_base = calculation->output_base;
    control.camera_hash = out.camera_hash;
    control.target_state_id = target.target_state_id;
    control.target_attack_id = target.target_attack_id;
    control.button0_abs = target.button0_abs;
    control.button0_rel = target.button0_rel;
    control.left_x = target.left_x;
    control.left_y = target.left_y;
    control.stick_direction = target.stick_direction;
    control.stick_speed = target.stick_speed;
    control.pad_magnitude = target.pad_magnitude;
    control.raw_dir_targ = target.raw_dir_targ;
    control.raw_quat_for_control = target.raw_quat_for_control;
    control.raw_render_quat = target.raw_render_quat;
    control.raw_turn_to_target = target.raw_turn_to_target;
    control.intent_forward = {target.intent_forward.valid, target.intent_forward.x,
                              target.intent_forward.z};
    control.desired_forward = {target.desired_forward.valid, target.desired_forward.x,
                               target.desired_forward.z};
    control.control_forward = {target.control_forward.valid, target.control_forward.x,
                               target.control_forward.z};
    control.render_forward = {target.render_forward.valid, target.render_forward.x,
                              target.render_forward.z};
    control.root_forward = metal_merc_transform_trace::make_facing_snapshot(
        out.input_root_basis.components[6], out.input_root_basis.components[8]);
    control.camera_basis = out.camera_basis;
    control.input_root_deformation =
        metal_merc_transform_trace::make_deformation_snapshot(root_matrix.data());
    control.output_deformation =
        metal_merc_transform_trace::make_deformation_snapshot(output_matrix);
    for (std::size_t anchor = 0; anchor < calculation->root_anchors.size(); anchor++) {
      const auto& anchor_transform = calculation->root_anchors[anchor];
      const auto& anchor_scale = calculation->root_scales[anchor];
      auto& anchor_observation = control.root_anchors[anchor];
      if (!anchor_transform.valid || !anchor_scale.valid) {
        continue;
      }
      metal_merc_transform_trace::MatrixSnapshot anchor_matrix = {};
      memcpy(anchor_matrix.data(), anchor_transform.bytes.data(), anchor_transform.bytes.size());
      anchor_observation = metal_merc_transform_trace::make_root_anchor_observation(
          anchor_matrix.data(), anchor_scale.x, anchor_scale.y, anchor_scale.z,
          anchor_scale.w_bits);
    }
    control.node3_parent_scale_cancellation_selected =
        control.root_anchors[1].valid && control.root_anchors[1].scale_w_bits != 0;
    control.input_root_translation_x = out.input_translation_x;
    control.input_root_translation_y = out.input_translation_y;
    control.input_root_translation_z = out.input_translation_z;
    const bool facing_valid = control.desired_forward.valid && control.control_forward.valid &&
                              control.render_forward.valid && control.root_forward.valid;
    const bool deformation_valid = control.camera_basis.valid &&
                                   control.input_root_deformation.valid &&
                                   control.output_deformation.valid;
    control.valid = facing_valid && deformation_valid;
    if (!facing_valid) {
      control.capture_stage = jak1_target_control_capture::Stage::FACING;
      control.capture_result = jak1_target_control_capture::Result::INVALID_FACING;
    } else if (!deformation_valid) {
      control.capture_stage = jak1_target_control_capture::Stage::DEFORMATION;
      control.capture_result = jak1_target_control_capture::Result::INVALID_DEFORMATION;
    }
  }
  out.mapping_valid =
      out.input_root_basis.valid && out.camera_basis.valid && out.output_basis.valid;
  return out;
}

// Must match MercVsParams in shaders/merc2.metal.
struct MercVsParams {
  float perspective[16];
  float hvdf_offset[4];
  float fog_constants[4];
  float light_dir0_fade[4];
  float light_dir1_fade_en[4];
  float light_dir2[4];
  float light_col0[4];
  float light_col1[4];
  float light_col2[4];
  float light_ambient[4];
  float fade[4];
  float height_scale;
  float scissor_adjust;
  float pad[2];
};
static_assert(sizeof(MercVsParams) == 240);

// Must match MercFsParams in shaders/merc2.metal.
struct MercFsParams {
  float fog_color[4];
  float light_dir0_fade[4];
  float light_dir1_fade_en[4];
  int ignore_alpha;
  int decal_enable;
  int gfx_hack_no_tex;
  int pad;
};
static_assert(sizeof(MercFsParams) == 64);

/*!
 * Metal equivalent of setup_opengl_from_draw_mode
 * (game/graphics/opengl_renderer/background/background_common.cpp), for the
 * subset merc uses: the GL function mutates global state, this one fills in the
 * state keys the encoder needs. Merc's shaders have no alpha_min/alpha_max
 * uniforms - the fragment shader discards below a fixed 0.128 - so the GL
 * renderer ignores the returned DoubleDraw, and so does this.
 */
struct MercDrawSettings {
  MetalPsoKey pso;
  MetalDepthStencilKey depth;
  MetalSamplerKey sampler;
  bool needs_blend_color = false;  // SRC_DST_FIX_DST uses a constant of 0.5
};

MercDrawSettings settings_from_draw_mode(DrawMode mode,
                                         MetalFrameContext& ctx,
                                         bool mipmap,
                                         bool envmap) {
  MercDrawSettings out;
  out.pso.shader = envmap ? MetalShaderId::EMERC : MetalShaderId::MERC2;
  out.pso.color_format = ctx.color_format;
  out.pso.depth_format = ctx.depth_format;

  if (mode.get_zt_enable()) {
    out.depth.depth_test = true;
    switch (mode.get_depth_test()) {
      case GsTest::ZTest::NEVER:
        out.depth.compare = MTLCompareFunctionNever;
        break;
      case GsTest::ZTest::ALWAYS:
        out.depth.compare = MTLCompareFunctionAlways;
        break;
      case GsTest::ZTest::GEQUAL:
        out.depth.compare = MTLCompareFunctionGreaterEqual;
        break;
      case GsTest::ZTest::GREATER:
        out.depth.compare = MTLCompareFunctionGreater;
        break;
      default:
        ASSERT(false);
    }
  } else {
    out.depth.depth_test = false;
    out.depth.compare = MTLCompareFunctionAlways;
  }

  bool blend_enable =
      mode.get_ab_enable() && mode.get_alpha_blend() != DrawMode::AlphaBlend::DISABLED;
  if (blend_enable) {
    out.pso.blend_src_alpha = MTLBlendFactorOne;
    out.pso.blend_dst_alpha = MTLBlendFactorZero;
    out.pso.blend_op_rgb = MTLBlendOperationAdd;
    out.pso.blend_op_alpha = MTLBlendOperationAdd;
    switch (mode.get_alpha_blend()) {
      case DrawMode::AlphaBlend::SRC_SRC_SRC_SRC:
        // (SRC - SRC) * alpha + SRC = SRC: no blend
        blend_enable = false;
        break;
      case DrawMode::AlphaBlend::SRC_DST_SRC_DST:
        out.pso.blend_src_rgb = MTLBlendFactorSourceAlpha;
        out.pso.blend_dst_rgb = MTLBlendFactorOneMinusSourceAlpha;
        break;
      case DrawMode::AlphaBlend::SRC_0_SRC_DST:
        out.pso.blend_src_rgb = MTLBlendFactorSourceAlpha;
        out.pso.blend_dst_rgb = MTLBlendFactorOne;
        break;
      case DrawMode::AlphaBlend::SRC_0_FIX_DST:
        out.pso.blend_src_rgb = MTLBlendFactorOne;
        out.pso.blend_dst_rgb = MTLBlendFactorOne;
        break;
      case DrawMode::AlphaBlend::SRC_DST_FIX_DST:
        // Cv = (Cs - Cd) * FIX + Cd, with the GL renderer's constant of 0.5
        out.pso.blend_src_rgb = MTLBlendFactorBlendColor;
        out.pso.blend_dst_rgb = MTLBlendFactorBlendColor;
        out.needs_blend_color = true;
        break;
      case DrawMode::AlphaBlend::ZERO_SRC_SRC_DST:
        out.pso.blend_src_rgb = MTLBlendFactorSourceAlpha;
        out.pso.blend_dst_rgb = MTLBlendFactorOne;
        out.pso.blend_op_rgb = MTLBlendOperationReverseSubtract;
        break;
      case DrawMode::AlphaBlend::SRC_0_DST_DST:
        // the GL path also computes a 0.5 color multiplier here, which merc's
        // shaders have no uniform for and the GL renderer discards too
        out.pso.blend_src_rgb = MTLBlendFactorDestinationAlpha;
        out.pso.blend_dst_rgb = MTLBlendFactorOne;
        // setup_opengl_from_draw_mode uses glBlendFunc (not glBlendFuncSeparate)
        // for this mode, so alpha uses the same factors as RGB.
        out.pso.blend_src_alpha = MTLBlendFactorDestinationAlpha;
        out.pso.blend_dst_alpha = MTLBlendFactorOne;
        break;
      default:
        ASSERT(false);
    }
  }
  out.pso.blend_enable = blend_enable;

  out.sampler.wrap_s =
      mode.get_clamp_s_enable() ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
  out.sampler.wrap_t =
      mode.get_clamp_t_enable() ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
  if (mode.get_filt_enable()) {
    out.sampler.min_filter = MTLSamplerMinMagFilterLinear;
    out.sampler.mag_filter = MTLSamplerMinMagFilterLinear;
    out.sampler.mip_filter =
        mipmap ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNotMipmapped;
  } else {
    out.sampler.min_filter = MTLSamplerMinMagFilterNearest;
    out.sampler.mag_filter = MTLSamplerMinMagFilterNearest;
    out.sampler.mip_filter = MTLSamplerMipFilterNotMipmapped;
  }

  // the game sets atest NEVER + FB_ONLY to mean "no depth writes"
  bool alpha_hack_to_disable_z_write = false;
  if (mode.get_at_enable()) {
    switch (mode.get_alpha_test()) {
      case DrawMode::AlphaTest::ALWAYS:
      case DrawMode::AlphaTest::GEQUAL:
        break;
      case DrawMode::AlphaTest::NEVER:
        if (mode.get_alpha_fail() == GsTest::AlphaFail::FB_ONLY) {
          alpha_hack_to_disable_z_write = true;
        } else {
          ASSERT(false);
        }
        break;
      default:
        ASSERT(false);
    }
  }
  out.depth.depth_write = mode.get_depth_write_enable() && !alpha_hack_to_disable_z_write;
  return out;
}

bool tag_is_nothing_next(const DmaFollower& dma) {
  return dma.current_tag().kind == DmaTag::Kind::NEXT && dma.current_tag().qwc == 0 &&
         dma.current_tag_vif0() == 0 && dma.current_tag_vif1() == 0;
}

// Sub-allocates from the frame's stream buffer with a stricter alignment than
// the stream's own 16 bytes (bone views are bound as buffer offsets).
void* alloc_aligned(MetalStreamBuffer* stream,
                    u32 size,
                    u32 align,
                    id<MTLBuffer>* out_buffer,
                    u32* out_offset) {
  u8* base = (u8*)stream->alloc(size + align, out_buffer, out_offset);
  u32 pad = (align - (*out_offset % align)) % align;
  *out_offset += pad;
  return base + pad;
}

/*!
 * Modify vertices for blerc: the GL renderer's blerc_avx (Merc2.cpp), as the
 * plain four-lane loop the SSE intrinsics compute. Per vertex, the int data is
 * [tgt0_idx, tgt1_idx, ..., terminator, dest] and the float data is
 * [base, tgt0, tgt1, ...]; the result is base + sum(tgtN * weights[tgtN_idx]).
 */
void blerc_vertices(const u32* i_data,
                    const u32* i_data_end,
                    const tfrag3::BlercFloatData* floats,
                    const float* weights,
                    tfrag3::MercVertex* out) {
  while (i_data != i_data_end) {
    float pos[4];
    float nrm[4];
    memcpy(pos, floats->v, sizeof(pos));
    memcpy(nrm, floats->v + 4, sizeof(nrm));
    floats++;

    while (*i_data != tfrag3::Blerc::kTargetIdxTerminator) {
      const float w = weights[*i_data];
      for (int i = 0; i < 4; i++) {
        pos[i] += floats->v[i] * w;
        nrm[i] += floats->v[4 + i] * w;
      }
      floats++;
      i_data++;
    }
    i_data++;

    // 16-byte stores, exactly like the GL renderer's: the fourth lane lands in
    // the vertex padding after pos / normal
    memcpy(out[*i_data].pos, pos, sizeof(pos));
    memcpy(out[*i_data].normal, nrm, sizeof(nrm));
    i_data++;
  }
}

}  // namespace

void MetalMerc2::Stats::add(const Stats& o) {
  models += o.models;
  missing_models += o.missing_models;
  effects += o.effects;
  draws += o.draws;
  triangles += o.triangles;
  envmap_draws += o.envmap_draws;
  bone_vectors += o.bone_vectors;
  lights += o.lights;
  mod_vtx_uploads += o.mod_vtx_uploads;
  mod_vtx_skipped += o.mod_vtx_skipped;
  eye_draws += o.eye_draws;
  missing_textures += o.missing_textures;
  malformed_dma += o.malformed_dma;
  bad_bone_pointers += o.bad_bone_pointers;
  bad_draw_ranges += o.bad_draw_ranges;
  missing_bone_slots += o.missing_bone_slots;
  models_with_missing_bone_slots += o.models_with_missing_bone_slots;
  nonfinite_bone_matrices += o.nonfinite_bone_matrices;
  degenerate_bone_matrices += o.degenerate_bone_matrices;
  incoherent_bone_sources += o.incoherent_bone_sources;
  models_with_palette_health_issues += o.models_with_palette_health_issues;
  eichar_palette_health_issues += o.eichar_palette_health_issues;
  eichar_transform_discontinuities += o.eichar_transform_discontinuities;
  eichar_provenance_events += o.eichar_provenance_events;
  eichar_output_composition_mismatches += o.eichar_output_composition_mismatches;
  eichar_target_control_events += o.eichar_target_control_events;
  eichar_target_control_divergences += o.eichar_target_control_divergences;
  eichar_target_control_attack_boundaries += o.eichar_target_control_attack_boundaries;
  eichar_target_control_capture_attempts += o.eichar_target_control_capture_attempts;
  eichar_target_control_valid_observations += o.eichar_target_control_valid_observations;
  eichar_weighted_skin.add(o.eichar_weighted_skin);
  eichar_duplication.add(o.eichar_duplication);
  if (o.eichar_target_control_capture_attempts > 0) {
    last_eichar_target_control_capture_stage = o.last_eichar_target_control_capture_stage;
    last_eichar_target_control_capture_result = o.last_eichar_target_control_capture_result;
  }
  if (o.eichar_target_control_valid_observations > 0) {
    last_eichar_target_control_observation = o.last_eichar_target_control_observation;
  }
  if (!first_palette_health_event.valid() && o.first_palette_health_event.valid()) {
    first_palette_health_event = o.first_palette_health_event;
  }
  if (o.last_palette_health_event.valid()) {
    last_palette_health_event = o.last_palette_health_event;
  }
  if (!first_eichar_palette_health_event.valid() && o.first_eichar_palette_health_event.valid()) {
    first_eichar_palette_health_event = o.first_eichar_palette_health_event;
  }
  if (o.last_eichar_palette_health_event.valid()) {
    last_eichar_palette_health_event = o.last_eichar_palette_health_event;
  }
  if (!first_eichar_transform_discontinuity.valid() &&
      o.first_eichar_transform_discontinuity.valid()) {
    first_eichar_transform_discontinuity = o.first_eichar_transform_discontinuity;
  }
  if (o.last_eichar_transform_discontinuity.valid()) {
    last_eichar_transform_discontinuity = o.last_eichar_transform_discontinuity;
  }
  if (!first_eichar_provenance_event.valid() && o.first_eichar_provenance_event.valid()) {
    first_eichar_provenance_event = o.first_eichar_provenance_event;
  }
  if (o.last_eichar_provenance_event.valid()) {
    last_eichar_provenance_event = o.last_eichar_provenance_event;
  }
  if (!first_eichar_output_composition_mismatch.valid() &&
      o.first_eichar_output_composition_mismatch.valid()) {
    first_eichar_output_composition_mismatch = o.first_eichar_output_composition_mismatch;
  }
  if (o.last_eichar_output_composition_mismatch.valid()) {
    last_eichar_output_composition_mismatch = o.last_eichar_output_composition_mismatch;
  }
  if (!first_eichar_target_control_event.valid() && o.first_eichar_target_control_event.valid()) {
    first_eichar_target_control_event = o.first_eichar_target_control_event;
  }
  if (o.last_eichar_target_control_event.valid()) {
    last_eichar_target_control_event = o.last_eichar_target_control_event;
  }
}

MetalMerc2::MetalMerc2(id<MTLDevice> device, id<MTLCommandQueue> queue, TexturePool* texture_pool) {
  metal_merc_models().init(device, queue, texture_pool);
  for (int i = 0; i < MAX_LEVELS; i++) {
    auto& draws = m_level_draw_buckets.emplace_back();
    draws.draws.resize(MAX_DRAWS_PER_LEVEL);
    draws.envmap_draws.resize(MAX_DRAWS_PER_LEVEL);
  }
  m_mod_vtx_unpack_temp.resize(MAX_MOD_VTX * 2);
}

void MetalMerc2::render(DmaFollower& dma,
                        MetalSharedRenderState* render_state,
                        MetalFrameContext& ctx,
                        Stats* stats) {
  *stats = {};
  if (render_state->version == GameVersion::Jak2) {
    metal_jak2_merc_dma::Bucket packet;
    std::string error;
    bool valid = metal_jak2_merc_dma::validate_bucket(
        dma, render_state->next_bucket, EE_MAIN_MEM_SIZE, &packet, &error);
    if (valid) {
      for (const auto& model_packet : packet.models) {
        const auto model = metal_merc_models().get_merc_model(model_packet.name.c_str());
        if (model && model_packet.effect_count != model->model->effects.size()) {
          error = "the packet effect count to match its loaded Merc model";
          valid = false;
          break;
        }
      }
    }
    if (!valid) {
      stats->malformed_dma++;
      if (!m_warned_malformed_dma) {
        lg::warn("Metal Jak 2 merc: expected {}; the bucket is skipped (logged once)", error);
        m_warned_malformed_dma = true;
      }
      if (!metal_jak2_merc_dma::recover_to_boundary(&dma, render_state->next_bucket)) {
        metal_finish_bucket(dma, *render_state);
      }
      return;
    }
  }
  handle_all_dma(dma, render_state, ctx, stats);
  flush_draw_buckets(render_state, ctx, stats);
}

void MetalMerc2::handle_all_dma(DmaFollower& dma,
                                MetalSharedRenderState* render_state,
                                MetalFrameContext& ctx,
                                Stats* stats) {
  // process the first tag. this is just jumping to the merc-specific dma.
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0 || data0.vifcode1().kind == VifCode::Kind::NOP);
  ASSERT(data0.vif0() == 0 || data0.vifcode0().kind == VifCode::Kind::NOP ||
         data0.vifcode0().kind == VifCode::Kind::MARK);
  ASSERT(data0.size_bytes == 0);
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
    return;
  }

  if (dma.current_tag_offset() == render_state->next_bucket) {
    return;
  }

  handle_setup_dma(dma, render_state);

  while (dma.current_tag_offset() != render_state->next_bucket) {
    handle_merc_chain(dma, render_state, ctx, stats);
  }
  ASSERT(dma.current_tag_offset() == render_state->next_bucket);
}

void MetalMerc2::handle_setup_dma(DmaFollower& dma, MetalSharedRenderState* render_state) {
  auto first = dma.read_and_advance();

  // 10 quadword setup packet
  ASSERT(first.size_bytes == 10 * 16);

  // transferred vifcodes
  {
    auto vif0 = first.vifcode0();
    auto vif1 = first.vifcode1();
    ASSERT(vif0.kind == VifCode::Kind::STCYCL);
    auto vif0_st = VifCodeStcycl(vif0);
    ASSERT(vif0_st.cl == 4 && vif0_st.wl == 4);
    ASSERT(vif1.kind == VifCode::Kind::STMOD);
    ASSERT(vif1.immediate == 0);
  }

  // 1 qw with 4 vifcodes.
  u32 vifcode_data[4];
  memcpy(vifcode_data, first.data, 16);
  {
    auto vif0 = VifCode(vifcode_data[0]);
    ASSERT(vif0.kind == VifCode::Kind::BASE);
    ASSERT(vif0.immediate == MercDataMemory::BUFFER_BASE);
    auto vif1 = VifCode(vifcode_data[1]);
    ASSERT(vif1.kind == VifCode::Kind::OFFSET);
    ASSERT((s16)vif1.immediate == MercDataMemory::BUFFER_OFFSET);
    auto vif2 = VifCode(vifcode_data[2]);
    ASSERT(vif2.kind == VifCode::Kind::NOP);
    auto vif3 = VifCode(vifcode_data[3]);
    ASSERT(vif3.kind == VifCode::Kind::UNPACK_V4_32);
    VifCodeUnpack up(vif3);
    ASSERT(up.addr_qw == MercDataMemory::LOW_MEMORY);
    ASSERT(!up.use_tops_flag);
    ASSERT(vif3.num == 8);
  }

  // 8 qw's of low memory data
  memcpy(&m_low_memory, first.data + 16, sizeof(LowMemory));

  // 1 qw with another 4 vifcodes.
  u32 vifcode_final_data[4];
  memcpy(vifcode_final_data, first.data + 16 + sizeof(LowMemory), 16);
  {
    ASSERT(VifCode(vifcode_final_data[0]).kind == VifCode::Kind::FLUSHE);
    ASSERT(vifcode_final_data[1] == 0);
    ASSERT(vifcode_final_data[2] == 0);
    VifCode mscal(vifcode_final_data[3]);
    ASSERT(mscal.kind == VifCode::Kind::MSCAL);
    ASSERT(mscal.immediate == 0);
  }

  auto second = dma.read_and_advance();
  ASSERT(second.size_bytes ==
         (render_state->version == GameVersion::Jak1 ? 32 : 48));  // test/zbuf registers
  auto nothing = dma.read_and_advance();
  ASSERT(nothing.size_bytes == 0);
  ASSERT(nothing.vif0() == 0);
  ASSERT(nothing.vif1() == 0);
}

void MetalMerc2::handle_merc_chain(DmaFollower& dma,
                                   MetalSharedRenderState* render_state,
                                   MetalFrameContext& ctx,
                                   Stats* stats) {
  while (tag_is_nothing_next(dma)) {
    auto nothing = dma.read_and_advance();
    ASSERT(nothing.size_bytes == 0);
  }
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    return;
  }

  auto init = dma.read_and_advance();
  const int skip_count = render_state->version == GameVersion::Jak1 ? 2 : 1;

  while (init.vifcode1().kind == VifCode::Kind::PC_PORT) {
    handle_pc_model(init, render_state, ctx, stats);
    for (int i = 0; i < skip_count; i++) {
      auto link = dma.read_and_advance();
      ASSERT(link.vifcode0().kind == VifCode::Kind::NOP);
      ASSERT(link.vifcode1().kind == VifCode::Kind::NOP);
      ASSERT(link.size_bytes == 0);
    }
    init = dma.read_and_advance();
  }

  if (init.vifcode0().kind == VifCode::Kind::FLUSHA) {
    int num_skipped = 0;
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
      num_skipped++;
    }
    ASSERT(num_skipped < 4);
    return;
  }
}

void* MetalMerc2::alloc_mod_vtx_buffer(size_t vertex_count,
                                       const char* model_name,
                                       MetalFrameContext& ctx,
                                       ModBuffers* out,
                                       Stats* stats) {
  const size_t bytes = vertex_count * sizeof(tfrag3::MercVertex);
  if (vertex_count == 0 ||
      bytes + alignof(tfrag3::MercVertex) > MetalStreamBuffer::kPageSize) {
    stats->mod_vtx_skipped++;
    if (!m_warned_mod_skip) {
      lg::warn("Metal merc: model '{}' has {} modifiable vertices, which does not fit a stream "
               "page; drawing the unmodified vertices (logged once)",
               model_name, vertex_count);
      m_warned_mod_skip = true;
    }
    return nullptr;
  }
  id<MTLBuffer> buffer = nil;
  u32 offset = 0;
  void* data = alloc_aligned(ctx.stream, static_cast<u32>(bytes),
                             alignof(tfrag3::MercVertex), &buffer, &offset);
  ASSERT(offset % alignof(tfrag3::MercVertex) == 0);
  ASSERT(reinterpret_cast<uintptr_t>(data) % alignof(tfrag3::MercVertex) == 0);
  out->buffer = buffer;
  out->offset = offset;
  out->vertex_count = static_cast<u32>(vertex_count);
  return data;
}

/*!
 * Update vertices from the DMA's blend-shape weights: the GL renderer's
 * model_mod_blerc_draws, with the per-effect GL buffer replaced by a range of
 * the frame's stream buffer, written in place.
 */
void MetalMerc2::model_mod_blerc_draws(int num_effects,
                                       const tfrag3::MercModel* model,
                                       MetalFrameContext& ctx,
                                       ModBuffers* mod_buffers,
                                       const float* blerc_weights,
                                       Stats* stats) {
  for (int ei = 0; ei < num_effects; ei++) {
    const auto& effect = model->effects[ei];
    // some effects might have no mod draw info, and no modifiable vertices
    if (effect.mod.mod_draw.empty()) {
      continue;
    }

    auto* verts = (tfrag3::MercVertex*)alloc_mod_vtx_buffer(effect.mod.vertices.size(),
                                                            model->name.c_str(), ctx,
                                                            &mod_buffers[ei], stats);
    if (!verts) {
      continue;
    }

    // start with the correct vertices from the model data, then blerc in place
    memcpy(verts, effect.mod.vertices.data(),
           sizeof(tfrag3::MercVertex) * effect.mod.vertices.size());
    const u32* i_data = effect.mod.blerc.int_data.data();
    blerc_vertices(i_data, i_data + effect.mod.blerc.int_data.size(),
                   effect.mod.blerc.float_data.data(), blerc_weights, verts);
    stats->mod_vtx_uploads++;
  }
}

/*!
 * Update vertices from the merc fragment data the game modified in EE memory
 * (texture scrolling, ripple): the GL renderer's model_mod_draws. The GL code
 * reaches EE memory through `setup.data - setup.data_offset`; here the chain
 * is the copier's compacted copy, so the game's addresses are resolved against
 * EE memory directly, like the bone matrices above. Every address comes from
 * the chain or from game-written memory, so it is bounded before it is
 * dereferenced: a malformed frame reports and keeps the unmodified vertices.
 */
void MetalMerc2::model_mod_draws(int num_effects,
                                 const tfrag3::MercModel* model,
                                 const u8* input_data,
                                 const u8* ee0,
                                 MetalFrameContext& ctx,
                                 ModBuffers* mod_buffers,
                                 Stats* stats) {
  const u8* ee_end = ee0 + EE_MAIN_MEM_SIZE;
  // headroom covering every in-fragment read below: the u8 quadword counts
  // bound offsets to mm_qwc_off * 16 + 12 < 4096
  constexpr size_t kFragReadSpan = 4096;
  constexpr size_t kFragCtrlReadSpan = 4 + 2 * 255;

  for (int ei = 0; ei < num_effects; ei++) {
    const auto& effect = model->effects[ei];
    if (effect.mod.mod_draw.empty()) {
      continue;
    }

    auto report_skip = [&](const char* why) {
      stats->mod_vtx_skipped++;
      if (!m_warned_mod_skip) {
        lg::warn("Metal merc: model '{}' mod-vertex update skipped ({}); drawing the unmodified "
                 "vertices (logged once)",
                 model->name, why);
        m_warned_mod_skip = true;
      }
      mod_buffers[ei] = {};
    };

    if (effect.mod.expect_vidx_end > MAX_MOD_VTX) {
      report_skip("more mod vertices than MAX_MOD_VTX");
      continue;
    }

    auto* verts = (tfrag3::MercVertex*)alloc_mod_vtx_buffer(effect.mod.vertices.size(),
                                                            model->name.c_str(), ctx,
                                                            &mod_buffers[ei], stats);
    if (!verts) {
      continue;
    }

    // start with the "correct" vertices from the model data
    memcpy(verts, effect.mod.vertices.data(),
           sizeof(tfrag3::MercVertex) * effect.mod.vertices.size());

    // get pointers to the fragment and fragment control data
    u32 goal_addr;
    memcpy(&goal_addr, input_data + 4 * ei, 4);
    if (goal_addr == 0 || goal_addr + 22 > EE_MAIN_MEM_SIZE) {
      report_skip("effect pointer outside EE memory");
      continue;
    }
    const u8* merc_effect = ee0 + goal_addr;
    u16 frag_cnt;
    memcpy(&frag_cnt, merc_effect + 18, 2);
    if (frag_cnt < effect.mod.fragment_mask.size()) {
      report_skip("fewer fragments than the model expects");
      continue;
    }
    u32 frag_goal;
    memcpy(&frag_goal, merc_effect, 4);
    u32 frag_ctrl_goal;
    memcpy(&frag_ctrl_goal, merc_effect + 4, 4);
    if (frag_goal >= EE_MAIN_MEM_SIZE || frag_ctrl_goal >= EE_MAIN_MEM_SIZE) {
      report_skip("fragment pointer outside EE memory");
      continue;
    }
    const u8* frag = ee0 + frag_goal;
    const u8* frag_ctrl = ee0 + frag_ctrl_goal;

    // loop over frags
    u32 vidx = 0;
    const float xyz_scale = model->xyz_scale;
    bool walked = true;
    for (u32 fi = 0; fi < effect.mod.fragment_mask.size(); fi++) {
      if (frag + kFragReadSpan > ee_end || frag_ctrl + kFragCtrlReadSpan > ee_end) {
        report_skip("fragment walk left EE memory");
        walked = false;
        break;
      }
      u8 mat_xfer_count = frag_ctrl[3];

      // we have a mask of fragments to skip because they have no vertices;
      // the indexing data assumes we skip the other fragments
      if (effect.mod.fragment_mask[fi]) {
        // read fragment metadata
        u8 unsigned_four_count = frag_ctrl[0];
        u8 lump_four_count = frag_ctrl[1];
        u32 mm_qwc_off = frag[10];
        float float_offsets[3];
        memcpy(float_offsets, &frag[mm_qwc_off * 16], 12);
        u32 my_u4_count = ((unsigned_four_count + 3) / 4) * 16;
        u32 my_l4_count = my_u4_count + ((lump_four_count + 3) / 4) * 16;

        // loop over vertices in the fragment and unpack. The GL loop's
        // `w < my_l4_count / 4 - 2` is compared signed here so an empty
        // fragment cannot underflow it.
        for (u32 w = my_u4_count / 4; (s64)w + 2 < (s64)(my_l4_count / 4); w += 3) {
          if (vidx >= m_mod_vtx_unpack_temp.size()) {
            break;  // expect_vidx_end mismatch, reported after the walk
          }
          // positions
          u32 q0w = 0x4b010000 + frag[w * 4 + (0 * 4) + 3];
          u32 q1w = 0x4b010000 + frag[w * 4 + (1 * 4) + 3];
          u32 q2w = 0x4b010000 + frag[w * 4 + (2 * 4) + 3];

          // normals
          u32 q0z = 0x47800000 + frag[w * 4 + (0 * 4) + 2];
          u32 q1z = 0x47800000 + frag[w * 4 + (1 * 4) + 2];
          u32 q2z = 0x47800000 + frag[w * 4 + (2 * 4) + 2];

          // uvs
          u32 q2x = model->st_vif_add + frag[w * 4 + (2 * 4) + 0];
          u32 q2y = model->st_vif_add + frag[w * 4 + (2 * 4) + 1];

          auto* pos_array = m_mod_vtx_unpack_temp[vidx].pos;
          memcpy(&pos_array[0], &q0w, 4);
          memcpy(&pos_array[1], &q1w, 4);
          memcpy(&pos_array[2], &q2w, 4);
          pos_array[0] += float_offsets[0];
          pos_array[1] += float_offsets[1];
          pos_array[2] += float_offsets[2];
          pos_array[0] *= xyz_scale;
          pos_array[1] *= xyz_scale;
          pos_array[2] *= xyz_scale;

          auto* nrm_array = m_mod_vtx_unpack_temp[vidx].nrm;
          memcpy(&nrm_array[0], &q0z, 4);
          memcpy(&nrm_array[1], &q1z, 4);
          memcpy(&nrm_array[2], &q2z, 4);
          nrm_array[0] += -65537;
          nrm_array[1] += -65537;
          nrm_array[2] += -65537;

          auto* uv_array = m_mod_vtx_unpack_temp[vidx].uv;
          memcpy(&uv_array[0], &q2x, 4);
          memcpy(&uv_array[1], &q2y, 4);
          uv_array[0] += model->st_magic;
          uv_array[1] += model->st_magic;

          vidx++;
        }
      }

      // next control
      frag_ctrl += 4 + 2 * mat_xfer_count;

      // next frag
      u32 mm_qwc_count = frag[11];
      frag += mm_qwc_count * 16;
    }
    if (!walked) {
      continue;
    }
    if (effect.mod.expect_vidx_end != vidx) {
      report_skip("unpacked vertex count does not match the model");
      continue;
    }

    // now copy the data in merc original vertex order to the output
    for (u32 vi = 0; vi < effect.mod.vertices.size(); vi++) {
      u32 addr = effect.mod.vertex_lump4_addr[vi];
      if (addr < vidx) {
        memcpy(&verts[vi], &m_mod_vtx_unpack_temp[addr], 32);
        verts[vi].st[0] = m_mod_vtx_unpack_temp[addr].uv[0];
        verts[vi].st[1] = m_mod_vtx_unpack_temp[addr].uv[1];
      }
    }
    stats->mod_vtx_uploads++;
  }
}

/*!
 * Setup draws for a model, given the DMA data generated by the GOAL code.
 * Byte-for-byte the GL Merc2::handle_pc_model walk.
 */
void MetalMerc2::handle_pc_model(const DmaTransfer& setup,
                                 MetalSharedRenderState* render_state,
                                 MetalFrameContext& ctx,
                                 Stats* stats) {
  //  ;; name   (128 char, 8 qw)
  //  ;; lights (7 qw x 1)
  //  ;; matrix slot string (128 char, 8 qw)
  //  ;; matrices (7 qw x N)
  //  ;; flags    (num-effects, effect-alpha-ignore, effect-disable)
  //  ;; fades    (u32 x N), padding to qw aligned
  //  ;; pointers (u32 x N), padding
  const u8* input_data = setup.data;
  const auto* name_end = static_cast<const u8*>(std::memchr(input_data, 0, 128));
  ASSERT(name_end);
  ASSERT(render_state->version != GameVersion::Jak1 || name_end - input_data < 127);
  char name[128] = {};
  memcpy(name, input_data, static_cast<size_t>(name_end - input_data));
  input_data += 128;

  auto model_ref = metal_merc_models().get_merc_model(name);
  if (!model_ref) {
    // the level holding this model is not loaded: don't draw, and say so.
    stats->missing_models++;
    return;
  }

  // Bone matrices live in EE main memory, not in the chain (the game's `bones`
  // runs after merc's DMA is built). The GL renderer reaches them with
  // `setup.data - setup.data_offset + addr`, which works there only because the
  // GL pipeline walks the original EE memory (`run_dma_copy = false` in
  // pipelines/opengl.cpp). The Metal pipeline walks the FixedChunkDmaCopier's
  // compacted copy, where chunk indices are not preserved, so the address must
  // be resolved against EE memory directly - the same thing the Metal texture
  // upload handler does with its texture-page pointers.
  const u8* ee0 = render_state->ee_memory;
  if (!ee0) {
    stats->missing_models++;
    if (!m_warned_no_ee) {
      lg::warn("Metal merc: no EE main memory, so bone matrices cannot be read; "
               "skipping models (logged once)");
      m_warned_no_ee = true;
    }
    return;
  }

  const MetalMercLevel* lev = model_ref->level;
  const tfrag3::MercModel* model = model_ref->model;

  if (m_next_free_light >= MAX_LIGHTS) {
    lg::warn("Metal merc: out of lights, flushing");
    flush_draw_buckets(render_state, ctx, stats);
  }

  int bone_count = model->max_bones + 1;
  if (m_next_free_bone_vector + kBoneVectorAlignment + bone_count * 8 > MAX_SHADER_BONE_VECTORS) {
    lg::warn("Metal merc: out of bones, flushing");
    flush_draw_buckets(render_state, ctx, stats);
  }
  ASSERT(kBoneVectorAlignment + bone_count * 8 <= MAX_SHADER_BONE_VECTORS);

  // A flush retires every level bucket, so the bucket has to be (re)acquired
  // after one: holding the old pointer would keep appending draws to a bucket
  // that is no longer in the live range, and those draws would never be issued.
  auto acquire_level_bucket = [&]() {
    for (u32 i = 0; i < m_next_free_level_bucket; i++) {
      if (m_level_draw_buckets[i].level == lev) {
        return &m_level_draw_buckets[i];
      }
    }
    if (m_next_free_level_bucket >= m_level_draw_buckets.size()) {
      flush_draw_buckets(render_state, ctx, stats);
    }
    LevelDrawBucket* b = &m_level_draw_buckets[m_next_free_level_bucket++];
    b->reset();
    b->level = lev;
    return b;
  };

  LevelDrawBucket* lev_bucket = acquire_level_bucket();

  if (lev_bucket->next_free_draw + model->max_draws >= lev_bucket->draws.size() ||
      lev_bucket->next_free_envmap_draw + model->max_draws >= lev_bucket->envmap_draws.size()) {
    lg::warn("Metal merc: out of draws, flushing");
    flush_draw_buckets(render_state, ctx, stats);
    lev_bucket = acquire_level_bucket();
    ASSERT(model->max_draws < lev_bucket->draws.size());
    ASSERT(model->max_draws < lev_bucket->envmap_draws.size());
  }

  VuLights current_lights;
  memcpy(&current_lights, input_data, sizeof(VuLights));
  input_data += sizeof(VuLights);

  u64 uses_water = 0;
  if (render_state->version == GameVersion::Jak1) {
    // Jak 1 figures out water at runtime. Jak 2 omits this quadword.
    memcpy(&uses_water, input_data, 8);
    input_data += 16;
  }

  // The matrix slot string tells us which bones go where; the matrices
  // themselves live in EE main memory (bones runs after merc's DMA is built).
  ShaderMercMat skel_matrix_buffer[MAX_SKEL_BONES] = {};
  MetalMercBoneSlotMask populated_bone_slots = {};
  u32 bone_source_addresses[MAX_SKEL_BONES] = {};
  auto* matrix_array = (const u32*)(input_data + 128);
  int i;
  for (i = 0; i < 128; i++) {
    if (input_data[i] == 0xff) {  // indicates end of string.
      break;
    }
    u32 addr;
    memcpy(&addr, &matrix_array[i * 4], 4);
    ASSERT(input_data[i] < MAX_SKEL_BONES);
    // the address comes from the chain, so bound it before dereferencing: a
    // replayed or malformed frame must report, not fault
    if (addr == 0 || addr + sizeof(MercMat) > EE_MAIN_MEM_SIZE) {
      stats->bad_bone_pointers++;
      if (!m_warned_bad_bone) {
        lg::warn("Metal merc: model '{}' bone {} points at {:#x}, outside EE memory; "
                 "using an identity bone (logged once)",
                 model->name, i, addr);
        m_warned_bad_bone = true;
      }
      continue;
    }
    const u8 destination_slot = input_data[i];
    memcpy(&skel_matrix_buffer[destination_slot], ee0 + addr, sizeof(MercMat));
    bone_source_addresses[destination_slot] = addr;
    populated_bone_slots[destination_slot / 64] |= 1ull << (destination_slot % 64);
  }
  input_data += 128 + 16 * i;

  struct PcMercFlags {
    u64 enable_mask;
    u64 ignore_alpha_mask;
    u8 effect_count;
    u8 bitflags;
  };
  auto* flags = (const PcMercFlags*)input_data;
  int num_effects = flags->effect_count;
  ASSERT(num_effects < kMaxEffect);
  u64 current_ignore_alpha_bits = flags->ignore_alpha_mask;
  u64 current_effect_enable_bits = flags->enable_mask;
  bool model_uses_mod = flags->bitflags & 1;
  bool model_disables_fog = flags->bitflags & 2;
  bool model_uses_pc_blerc = flags->bitflags & 4;
  bool model_disables_envmap = flags->bitflags & 8;
  input_data += 32;

  MetalMercBoneSlotMask required_bone_slots = {};
  const auto& effect_masks = *model_ref->required_bone_slots_by_effect;
  ASSERT(effect_masks.size() == model->effects.size());
  for (size_t effect_idx = 0; effect_idx < effect_masks.size() && effect_idx < 64; effect_idx++) {
    if (!(current_effect_enable_bits & (1ull << effect_idx))) {
      continue;
    }
    for (size_t word = 0; word < required_bone_slots.size(); word++) {
      required_bone_slots[word] |= effect_masks[effect_idx][word];
    }
  }

  MetalMercBoneSlotMask missing_bone_slots = {};
  int missing_slot_count = 0;
  for (size_t word = 0; word < missing_bone_slots.size(); word++) {
    missing_bone_slots[word] = required_bone_slots[word] & ~populated_bone_slots[word];
    missing_slot_count += __builtin_popcountll(missing_bone_slots[word]);
  }
  if (missing_slot_count) {
    stats->missing_bone_slots += missing_slot_count;
    stats->models_with_missing_bone_slots++;
    if (!m_reported_missing_bone_slots) {
      std::string missing_slots;
      for (int slot = 0; slot < 256; slot++) {
        if (!(missing_bone_slots[slot / 64] & (1ull << (slot % 64)))) {
          continue;
        }
        if (!missing_slots.empty()) {
          missing_slots += ",";
        }
        missing_slots += fmt::format("{}", slot);
      }
      lg::error("Metal merc: model '{}' is missing {} required bone slot(s): {} (logged once)",
                model->name, missing_slot_count, missing_slots);
      m_reported_missing_bone_slots = true;
    }
  }

  bool model_has_palette_health_issue = false;
  bool expected_source_base_valid = false;
  u64 expected_source_base = 0;
  int provenance_probe_slot = -1;
  if (model->name == "eichar-lod0") {
    const auto slot_is_available = [&](int slot) {
      const u64 bit = 1ull << (slot % 64);
      return (required_bone_slots[slot / 64] & bit) && (populated_bone_slots[slot / 64] & bit);
    };
    for (int slot = 3; slot >= 1; slot--) {
      if (slot_is_available(slot)) {
        provenance_probe_slot = slot;
        break;
      }
    }
  }
  for (size_t word = 0; word < required_bone_slots.size(); word++) {
    u64 slots_to_inspect = required_bone_slots[word] & populated_bone_slots[word];
    while (slots_to_inspect) {
      const int bit = __builtin_ctzll(slots_to_inspect);
      slots_to_inspect &= slots_to_inspect - 1;
      const int slot = (int)(word * 64 + bit);
      ASSERT(slot < MAX_SKEL_BONES);

      MercMat matrix;
      memcpy(&matrix, &skel_matrix_buffer[slot], sizeof(matrix));

      static_assert(sizeof(matrix) ==
                    metal_merc_transform_trace::kMatrixLaneCount * sizeof(float));
      std::array<float, metal_merc_transform_trace::kMatrixLaneCount> matrix_lanes;
      memcpy(matrix_lanes.data(), &matrix, sizeof(matrix));
      const u32 nonfinite_lane_mask =
          metal_merc_transform_trace::nonfinite_lane_mask(matrix_lanes.data());
      const bool matrix_is_finite = nonfinite_lane_mask == 0;

      auto axis_norm = [](const math::Vector4f& axis) {
        const double x = axis.x();
        const double y = axis.y();
        const double z = axis.z();
        return std::sqrt(x * x + y * y + z * z);
      };
      const double axis_norm_x = axis_norm(matrix.tmat[0]);
      const double axis_norm_y = axis_norm(matrix.tmat[1]);
      const double axis_norm_z = axis_norm(matrix.tmat[2]);
      const auto& x = matrix.tmat[0];
      const auto& y = matrix.tmat[1];
      const auto& z = matrix.tmat[2];
      const double determinant =
          (double)x.x() * ((double)y.y() * z.z() - (double)y.z() * z.y()) -
          (double)y.x() * ((double)x.y() * z.z() - (double)x.z() * z.y()) +
          (double)z.x() * ((double)x.y() * y.z() - (double)x.z() * y.y());
      const double norm_product = axis_norm_x * axis_norm_y * axis_norm_z;
      const double normalized_abs_determinant =
          norm_product > 0.0 ? std::abs(determinant) / norm_product : 0.0;
      constexpr double kMinimumAxisNorm = 1e-9;
      constexpr double kMinimumNormalizedDeterminant = 1e-6;
      const bool matrix_is_degenerate =
          matrix_is_finite &&
          (axis_norm_x <= kMinimumAxisNorm || axis_norm_y <= kMinimumAxisNorm ||
           axis_norm_z <= kMinimumAxisNorm ||
           normalized_abs_determinant <= kMinimumNormalizedDeterminant);

      const u32 source_address = bone_source_addresses[slot];
      const u64 slot_offset = (u64)slot * sizeof(ShaderMercMat);
      const bool source_base_valid = source_address >= slot_offset;
      const u64 source_base = source_base_valid ? source_address - slot_offset : 0;
      bool source_base_is_incoherent = !source_base_valid;
      if (source_base_valid) {
        if (!expected_source_base_valid) {
          expected_source_base = source_base;
          expected_source_base_valid = true;
        } else {
          source_base_is_incoherent = source_base != expected_source_base;
        }
      }

      if (matrix_is_finite && model->name == "eichar-lod0") {
        metal_merc_transform_trace::ProvenanceObservation provenance;
        if (slot == provenance_probe_slot) {
          provenance =
              make_bones_provenance_observation(source_address, source_base, provenance_probe_slot,
                                                reinterpret_cast<const float*>(&matrix));
          const auto target_control_trace = m_eichar_target_control_tracker.observe_with_status(
              render_state->engine_frame_id, slot, provenance.target_control);
          if (target_control_trace.capture_attempted) {
            stats->eichar_target_control_capture_attempts++;
            stats->last_eichar_target_control_capture_stage =
                target_control_trace.capture_stage;
            stats->last_eichar_target_control_capture_result =
                target_control_trace.capture_result;
          }
          if (target_control_trace.valid_observation) {
            stats->eichar_target_control_valid_observations++;
            stats->last_eichar_target_control_observation = target_control_trace.observation;
          }
          const auto& target_control = target_control_trace.event;
          if (target_control.valid()) {
            stats->eichar_target_control_events++;
            if (target_control.issue_mask &
                metal_merc_transform_trace::TARGET_CONTROL_FACING_DIVERGENCE) {
              stats->eichar_target_control_divergences++;
            }
            if (target_control.issue_mask &
                metal_merc_transform_trace::TARGET_CONTROL_ATTACK_BOUNDARY) {
              stats->eichar_target_control_attack_boundaries++;
            }
            if (!stats->first_eichar_target_control_event.valid()) {
              stats->first_eichar_target_control_event = target_control;
            }
            stats->last_eichar_target_control_event = target_control;
          }
        }
        const auto discontinuity = m_eichar_transform_tracker.observe(
            render_state->engine_frame_id, slot, fnv64(model->name), fnv64(&matrix, sizeof(matrix)),
            axis_norm_x, axis_norm_y, axis_norm_z, source_base, provenance);
        if (discontinuity.valid()) {
          stats->eichar_transform_discontinuities++;
          if (!stats->first_eichar_transform_discontinuity.valid()) {
            stats->first_eichar_transform_discontinuity = discontinuity;
          }
          stats->last_eichar_transform_discontinuity = discontinuity;
          metal_merc_transform_trace::retain_provenance_event(
              discontinuity, &stats->eichar_provenance_events,
              &stats->first_eichar_provenance_event, &stats->last_eichar_provenance_event);
          if (discontinuity.issue_mask & metal_merc_transform_trace::OUTPUT_COMPOSITION_MISMATCH) {
            stats->eichar_output_composition_mismatches++;
            if (!stats->first_eichar_output_composition_mismatch.valid()) {
              stats->first_eichar_output_composition_mismatch = discontinuity;
            }
            stats->last_eichar_output_composition_mismatch = discontinuity;
          }
          if (!m_reported_eichar_transform_discontinuity) {
            lg::error(
                "Metal merc: model '{}' required bone slot {} has consecutive-frame transform "
                "discontinuity {:#x} from engine frame {} to {}; matrix hashes {:#x}->{:#x}, "
                "axis norms [{:.9g}, {:.9g}, {:.9g}]->[{:.9g}, {:.9g}, {:.9g}], scale "
                "{:.9g}->{:.9g}, aspect {:.9g}->{:.9g}, source bases {:#x}->{:#x} (logged once)",
                model->name, slot, discontinuity.issue_mask, discontinuity.previous_frame_id,
                discontinuity.current_frame_id, discontinuity.previous_matrix_hash,
                discontinuity.current_matrix_hash, discontinuity.previous_axis_norm_x,
                discontinuity.previous_axis_norm_y, discontinuity.previous_axis_norm_z,
                discontinuity.current_axis_norm_x, discontinuity.current_axis_norm_y,
                discontinuity.current_axis_norm_z, discontinuity.previous_scale,
                discontinuity.current_scale, discontinuity.previous_aspect,
                discontinuity.current_aspect, discontinuity.previous_source_base,
                discontinuity.current_source_base);
            m_reported_eichar_transform_discontinuity = true;
          }
        }
      }

      u8 issue_mask = 0;
      if (!matrix_is_finite) {
        stats->nonfinite_bone_matrices++;
        issue_mask |= metal_renderer::MERC_PALETTE_HEALTH_NONFINITE;
      } else if (matrix_is_degenerate) {
        stats->degenerate_bone_matrices++;
        issue_mask |= metal_renderer::MERC_PALETTE_HEALTH_DEGENERATE;
      }
      if (source_base_is_incoherent) {
        stats->incoherent_bone_sources++;
        issue_mask |= metal_renderer::MERC_PALETTE_HEALTH_SOURCE_BASE;
      }
      if (!issue_mask) {
        continue;
      }

      model_has_palette_health_issue = true;
      metal_renderer::MercPaletteHealthEvent event;
      event.issue_mask = issue_mask;
      event.nonfinite_lane_mask = nonfinite_lane_mask;
      event.bone_slot = slot;
      event.model_name_hash = fnv64(model->name);
      event.matrix_hash = fnv64(&matrix, sizeof(matrix));
      event.axis_norm_x = axis_norm_x;
      event.axis_norm_y = axis_norm_y;
      event.axis_norm_z = axis_norm_z;
      event.normalized_abs_determinant = normalized_abs_determinant;
      event.source_address = source_address;
      event.source_base = source_base;
      event.expected_source_base = expected_source_base_valid ? expected_source_base : 0;
      if (!stats->first_palette_health_event.valid()) {
        stats->first_palette_health_event = event;
      }
      stats->last_palette_health_event = event;
      if (model->name == "eichar-lod0") {
        stats->eichar_palette_health_issues++;
        if (!stats->first_eichar_palette_health_event.valid()) {
          stats->first_eichar_palette_health_event = event;
        }
        stats->last_eichar_palette_health_event = event;
      }

      if (!m_reported_palette_health_issue) {
        lg::error(
            "Metal merc: model '{}' required bone slot {} has palette health issue {:#x}; "
            "matrix hash {:#x}, non-finite lanes {:#x}, axis norms [{:.9g}, {:.9g}, "
            "{:.9g}], normalized |det| {:.9g}, source {:#x}, source base {:#x}, expected base "
            "{:#x} (logged once)",
            model->name, slot, issue_mask, event.matrix_hash, nonfinite_lane_mask, axis_norm_x,
            axis_norm_y, axis_norm_z, normalized_abs_determinant, source_address, source_base,
            event.expected_source_base);
        m_reported_palette_health_issue = true;
      }
    }
  }
  if (model_has_palette_health_issue) {
    stats->models_with_palette_health_issues++;
  }

  const bool trace_eichar = model->name == "eichar-lod0" &&
                            model_ref->eichar_skin_profiles_by_effect &&
                            model_ref->eichar_skin_profiles_by_effect->size() ==
                                model->effects.size();
  u64 eichar_packet_palette_hash = 0;
  metal_merc_skin_trace::PacketResult eichar_packet;

  float blerc_weights[kMaxBlerc];
  if (model_uses_pc_blerc) {
    memcpy(blerc_weights, input_data, kMaxBlerc * sizeof(float));
    input_data += kMaxBlerc * sizeof(float);
  }

  u8 fade_buffer[4 * kMaxEffect];
  for (int ei = 0; ei < num_effects; ei++) {
    for (int j = 0; j < 4; j++) {
      fade_buffer[ei * 4 + j] = input_data[ei * 4 + j];
    }
  }
  input_data += (((num_effects * 4) + 15) / 16) * 16;

  // input_data is now at the per-effect EE pointers the mod-vertex path reads.
  // Vertex modification: blerc updates come from the weights in the chain,
  // mod-vertex updates from the fragment data the game modified in EE memory.
  ModBuffers mod_buffers[kMaxEffect];
  if (model_uses_pc_blerc) {
    model_mod_blerc_draws(num_effects, model, ctx, mod_buffers, blerc_weights, stats);
  } else if (model_uses_mod) {
    model_mod_draws(num_effects, model, input_data, ee0, ctx, mod_buffers, stats);
  }

  const auto uses_modified_draw_path = [&](size_t effect_index) {
    const auto& effect = model->effects[effect_index];
    bool use_modified_path =
        (model_uses_pc_blerc || model_uses_mod) && effect.has_mod_draw;
    if (use_modified_path && !effect.mod.mod_draw.empty() &&
        mod_buffers[effect_index].buffer == nil) {
      // The update was skipped (reported), so the draw path falls back to all_draws.
      use_modified_path = false;
    }
    return use_modified_path;
  };

  if (trace_eichar) {
    std::array<u64, 2> packet_used_bone_slots = {};
    for (size_t effect_index = 0; effect_index < model->effects.size(); effect_index++) {
      const auto& profiles = model_ref->eichar_skin_profiles_by_effect->at(effect_index);
      metal_merc_skin_trace::merge_active_draw_slots(
          current_effect_enable_bits & (1ull << effect_index),
          uses_modified_draw_path(effect_index), profiles.fixed_draws,
          profiles.modified_draws, profiles.all_draws, &packet_used_bone_slots);
    }
    eichar_packet_palette_hash = metal_merc_skin_trace::hash_palette(
        reinterpret_cast<const float*>(skel_matrix_buffer), static_cast<size_t>(bone_count),
        packet_used_bone_slots);
    eichar_packet = m_eichar_skin_tracker.observe_packet(
        render_state->engine_frame_id, expected_source_base_valid, expected_source_base,
        eichar_packet_palette_hash);
    metal_merc_skin_trace::retain_packet_result(eichar_packet, &stats->eichar_duplication);
  }

  stats->models++;
  stats->effects += (int)model->effects.size();

  u32 first_bone = alloc_bones(bone_count, skel_matrix_buffer);

  if (current_lights.w1) {
    // force off merc fade in jak1/2 - a bunch of stuff uses this
    current_lights.w1 = 0;
  }
  u32 lights = alloc_lights(current_lights);
  stats->lights++;

  DrawArgs args;
  args.lev_bucket = lev_bucket;
  args.jak1_water_mode = uses_water;
  args.disable_fog = model_disables_fog;
  args.lights = lights;
  args.first_bone = first_bone;
  args.skin_profile = nullptr;
  args.trace_source_base = expected_source_base;
  args.trace_source_base_valid = expected_source_base_valid;
  args.trace_packet_palette_hash = eichar_packet_palette_hash;
  args.trace_packet_sequence = eichar_packet.packet_sequence;
  args.trace_bone_count = static_cast<u16>(bone_count);
  args.trace_effect_index = 0;
  const auto retain_trace_profile = [&](const metal_merc_skin_trace::DrawProfile* profile) {
    args.skin_profile = profile;
    args.trace_packet_palette_hash =
        profile ? metal_merc_skin_trace::hash_draw_palette(
                      *profile, reinterpret_cast<const float*>(skel_matrix_buffer),
                      static_cast<size_t>(bone_count))
                : 0;
  };

  for (size_t ei = 0; ei < model->effects.size(); ei++) {
    args.fade = fade_buffer + 4 * ei;

    if (!(current_effect_enable_bits & (1ull << ei))) {
      continue;
    }

    args.ignore_alpha = !!(current_ignore_alpha_bits & (1ull << ei));
    auto& effect = model->effects[ei];
    const MetalMercEffectSkinProfiles* effect_profiles =
        trace_eichar ? &model_ref->eichar_skin_profiles_by_effect->at(ei) : nullptr;
    args.trace_effect_index = static_cast<u16>(ei);
    bool should_envmap = effect.has_envmap && !model_disables_envmap;
    const bool should_mod = uses_modified_draw_path(ei);

    if (should_mod) {
      // draw as two parts, fixed and mod

      // do fixed draws:
      ASSERT(!effect_profiles || effect_profiles->fixed_draws.size() == effect.mod.fix_draw.size());
      for (size_t draw_idx = 0; draw_idx < effect.mod.fix_draw.size(); draw_idx++) {
        auto& fdraw = effect.mod.fix_draw[draw_idx];
        retain_trace_profile(effect_profiles ? &effect_profiles->fixed_draws[draw_idx] : nullptr);
        alloc_normal_draw(fdraw, args);
        if (should_envmap) {
          try_alloc_envmap_draw(fdraw, effect.envmap_mode, effect.envmap_texture, args);
        }
      }

      // do mod draws:
      ASSERT(!effect_profiles ||
             effect_profiles->modified_draws.size() == effect.mod.mod_draw.size());
      for (size_t draw_idx = 0; draw_idx < effect.mod.mod_draw.size(); draw_idx++) {
        auto& mdraw = effect.mod.mod_draw[draw_idx];
        retain_trace_profile(effect_profiles ? &effect_profiles->modified_draws[draw_idx]
                                             : nullptr);
        auto* n = alloc_normal_draw(mdraw, args);
        // modify the draw, set the mod flag and point it at this frame's vertices
        n->flags |= MOD_VTX;
        n->mod_vtx = mod_buffers[ei];
        if (should_envmap) {
          auto* e = try_alloc_envmap_draw(mdraw, effect.envmap_mode, effect.envmap_texture, args);
          if (e) {
            e->flags |= MOD_VTX;
            e->mod_vtx = mod_buffers[ei];
          }
        }
      }
    } else {
      // no mod, just do all_draws
      ASSERT(!effect_profiles || effect_profiles->all_draws.size() == effect.all_draws.size());
      for (size_t draw_idx = 0; draw_idx < effect.all_draws.size(); draw_idx++) {
        auto& draw = effect.all_draws[draw_idx];
        retain_trace_profile(effect_profiles ? &effect_profiles->all_draws[draw_idx] : nullptr);
        if (should_envmap) {
          try_alloc_envmap_draw(draw, effect.envmap_mode, effect.envmap_texture, args);
        }
        alloc_normal_draw(draw, args);
      }
    }
  }
}

u32 MetalMerc2::alloc_lights(const VuLights& lights) {
  ASSERT(m_next_free_light < MAX_LIGHTS);
  u32 light_idx = m_next_free_light;
  m_lights_buffer[m_next_free_light++] = lights;
  return light_idx;
}

u32 MetalMerc2::alloc_bones(int count, ShaderMercMat* data) {
  u32 first_bone_vector = m_next_free_bone_vector;
  ASSERT(count * 8 + first_bone_vector <= MAX_SHADER_BONE_VECTORS);
  ASSERT(count <= MAX_SKEL_BONES);

  for (int i = 0; i < count; i++) {
    auto& skel_mat = data[i];
    auto* shader_mat = &m_shader_bone_vector_buffer[m_next_free_bone_vector];
    int bv = 0;
    for (int j = 0; j < 4; j++) {
      shader_mat[bv++] = skel_mat.tmat[j];
    }
    for (int j = 0; j < 3; j++) {
      shader_mat[bv++] = skel_mat.nmat[j];
    }
    m_next_free_bone_vector += 8;
  }

  auto b0 = m_next_free_bone_vector;
  m_next_free_bone_vector += kBoneVectorAlignment - 1;
  m_next_free_bone_vector /= kBoneVectorAlignment;
  m_next_free_bone_vector *= kBoneVectorAlignment;
  ASSERT(b0 <= m_next_free_bone_vector);
  ASSERT(first_bone_vector + count * 8 <= m_next_free_bone_vector);
  return first_bone_vector;
}

MetalMerc2::Draw* MetalMerc2::alloc_normal_draw(const tfrag3::MercDraw& mdraw,
                                                const DrawArgs& args) {
  Draw* draw = &args.lev_bucket->draws[args.lev_bucket->next_free_draw++];
  draw->flags = 0;
  draw->mod_vtx = {};
  draw->skin_profile = args.skin_profile;
  draw->trace_source_base = args.trace_source_base;
  draw->trace_source_base_valid = args.trace_source_base_valid;
  draw->trace_packet_palette_hash = args.trace_packet_palette_hash;
  draw->trace_packet_sequence = args.trace_packet_sequence;
  draw->trace_bone_count = args.trace_bone_count;
  draw->trace_effect_index = args.trace_effect_index;
  draw->first_index = mdraw.first_index;
  draw->index_count = mdraw.index_count;
  draw->mode = mdraw.mode;
  if (args.jak1_water_mode) {
    draw->mode.set_ab(true);
    draw->mode.disable_depth_write();
  }
  if (args.disable_fog) {
    draw->mode.set_fog(false);
  }
  draw->texture = mdraw.eye_id == 0xff ? mdraw.tree_tex_id : (0xefffff00 | mdraw.eye_id);
  draw->first_bone = args.first_bone;
  draw->light_idx = args.lights;
  draw->num_triangles = mdraw.num_triangles;
  draw->no_strip = mdraw.no_strip;
  if (args.ignore_alpha) {
    draw->flags |= IGNORE_ALPHA;
  }
  for (int i = 0; i < 4; i++) {
    draw->fade[i] = 0;
  }
  return draw;
}

MetalMerc2::Draw* MetalMerc2::try_alloc_envmap_draw(const tfrag3::MercDraw& mdraw,
                                                    const DrawMode& envmap_mode,
                                                    u32 envmap_texture,
                                                    const DrawArgs& args) {
  bool nonzero_fade = false;
  for (int i = 0; i < 4; i++) {
    if (args.fade[i]) {
      nonzero_fade = true;
      break;
    }
  }
  if (!nonzero_fade) {
    return nullptr;
  }

  Draw* draw = &args.lev_bucket->envmap_draws[args.lev_bucket->next_free_envmap_draw++];
  draw->flags = 0;
  draw->mod_vtx = {};
  draw->skin_profile = args.skin_profile;
  draw->trace_source_base = args.trace_source_base;
  draw->trace_source_base_valid = args.trace_source_base_valid;
  draw->trace_packet_palette_hash = args.trace_packet_palette_hash;
  draw->trace_packet_sequence = args.trace_packet_sequence;
  draw->trace_bone_count = args.trace_bone_count;
  draw->trace_effect_index = args.trace_effect_index;
  draw->first_index = mdraw.first_index;
  draw->index_count = mdraw.index_count;
  draw->mode = envmap_mode;
  if (args.jak1_water_mode) {
    draw->mode.enable_ab();
    draw->mode.disable_depth_write();
  }
  draw->texture = envmap_texture;
  draw->first_bone = args.first_bone;
  draw->light_idx = args.lights;
  draw->num_triangles = mdraw.num_triangles;
  draw->no_strip = mdraw.no_strip;
  for (int i = 0; i < 4; i++) {
    draw->fade[i] = args.fade[i];
  }
  return draw;
}

void MetalMerc2::flush_draw_buckets(MetalSharedRenderState* render_state,
                                    MetalFrameContext& ctx,
                                    Stats* stats) {
  if (m_next_free_bone_vector > 0) {
    stats->bone_vectors += m_next_free_bone_vector;

    // The GL renderer keeps one persistent 512 kB uniform buffer and rewrites it
    // per flush; here each flush gets its own range out of the frame's stream
    // buffer, because earlier draws in this encoder still reference the old one.
    // The extra MAX_SKEL_BONES of headroom keeps every draw's 128-matrix view
    // inside the allocation, as it is inside the GL buffer.
    const u32 bone_bytes =
        (m_next_free_bone_vector + MAX_SKEL_BONES * 8) * (u32)sizeof(math::Vector4f);
    id<MTLBuffer> bone_buffer = nil;
    u32 bone_base = 0;
    void* dst = alloc_aligned(ctx.stream, bone_bytes, kBoneVectorAlignment * 16, &bone_buffer,
                              &bone_base);
    memset(dst, 0, bone_bytes);
    memcpy(dst, m_shader_bone_vector_buffer,
           m_next_free_bone_vector * sizeof(math::Vector4f));

    for (u32 li = 0; li < m_next_free_level_bucket; li++) {
      const auto& lev_bucket = m_level_draw_buckets[li];
      do_draws(lev_bucket.draws.data(), lev_bucket.next_free_draw, lev_bucket.level, false,
               render_state, ctx, bone_buffer, bone_base, stats);
      if (lev_bucket.next_free_envmap_draw) {
        do_draws(lev_bucket.envmap_draws.data(), lev_bucket.next_free_envmap_draw,
                 lev_bucket.level, true, render_state, ctx, bone_buffer, bone_base, stats);
      }
    }
  }

  m_next_free_light = 0;
  m_next_free_bone_vector = 0;
  m_next_free_level_bucket = 0;
}

void MetalMerc2::do_draws(const Draw* draw_array,
                          u32 num_draws,
                          const MetalMercLevel* lev,
                          bool envmap,
                          MetalSharedRenderState* render_state,
                          MetalFrameContext& ctx,
                          id<MTLBuffer> bone_buffer,
                          u32 bone_base,
                          Stats* stats) {
  if (!num_draws || !lev->vertices || !lev->indices) {
    return;
  }
  id<MTLRenderCommandEncoder> enc = ctx.enc;
  [enc setVertexBuffer:lev->vertices offset:0 atIndex:0];
  bool normal_vtx_buffer_bound = true;
  id<MTLBuffer> bound_vertex_buffer = lev->vertices;
  u32 bound_vertex_offset = 0;
  size_t bound_vertex_count = lev->level->merc_data.vertices.size();

  const u64 placeholder = render_state->texture_pool->get_placeholder_texture();

  const u32 index_count_in_level = (u32)lev->level->merc_data.indices.size();

  for (u32 di = 0; di < num_draws; di++) {
    const auto& draw = draw_array[di];
    if ((u64)draw.first_index + draw.index_count > index_count_in_level) {
      // the model's draw does not fit its level's index buffer; report instead
      // of handing the GPU an out-of-range range
      stats->bad_draw_ranges++;
      continue;
    }

    // mod draws read this frame's updated vertices; their index ranges index
    // into the per-effect buffer, the level's index buffer stays bound
    if (draw.flags & MOD_VTX) {
      [enc setVertexBuffer:draw.mod_vtx.buffer offset:draw.mod_vtx.offset atIndex:0];
      normal_vtx_buffer_bound = false;
      bound_vertex_buffer = draw.mod_vtx.buffer;
      bound_vertex_offset = draw.mod_vtx.offset;
      bound_vertex_count = draw.mod_vtx.vertex_count;
    } else if (!normal_vtx_buffer_bound) {
      [enc setVertexBuffer:lev->vertices offset:0 atIndex:0];
      normal_vtx_buffer_bound = true;
      bound_vertex_buffer = lev->vertices;
      bound_vertex_offset = 0;
      bound_vertex_count = lev->level->merc_data.vertices.size();
    }

    bool use_mipmaps = true;
    id<MTLTexture> tex = nil;
    if (draw.texture >= 0 && draw.texture < (int)lev->textures.size()) {
      tex = metal_texture_lookup(lev->textures[draw.texture]);
    } else if ((draw.texture & 0xffffff00) == 0xefffff00) {
      // eye textures are composed by the eye renderer (Jak 1 indexes by eye id;
      // the hash form is Jak 3 only, like the GL renderer)
      stats->eye_draws++;
      use_mipmaps = false;
      if (render_state->eye_renderer) {
        auto maybe_eye = render_state->eye_renderer->lookup_eye_texture(draw.texture & 0xff);
        if (maybe_eye) {
          tex = metal_texture_lookup(*maybe_eye);
        }
      }
      if (!tex && !m_warned_eyes) {
        lg::warn("Metal merc: no eye texture for draw {}; using the placeholder (logged once)",
                 draw.texture & 0xff);
        m_warned_eyes = true;
      }
    } else {
      stats->missing_textures++;
    }
    if (!tex) {
      tex = metal_texture_lookup(placeholder);
    }
    ASSERT(tex);

    auto settings = settings_from_draw_mode(draw.mode, ctx, use_mipmaps, envmap);

    MercVsParams vs = {};
    memcpy(vs.perspective, &m_low_memory.perspective[0].x(), sizeof(vs.perspective));
    memcpy(vs.hvdf_offset, m_low_memory.hvdf_offset.data(), sizeof(vs.hvdf_offset));
    memcpy(vs.fog_constants, m_low_memory.fog.data(), sizeof(vs.fog_constants));

    const auto& lights = m_lights_buffer[draw.light_idx];
    float fade = 1.f;
    float fade_enable = 0.f;
    if (lights.w1) {
      fade = lights.w2 / 128.f;
      fade_enable = 1.f;
    }
    const math::Vector4f l0(lights.direction0.x(), lights.direction0.y(), lights.direction0.z(),
                            fade);
    const math::Vector4f l1(lights.direction1.x(), lights.direction1.y(), lights.direction1.z(),
                            fade_enable);
    memcpy(vs.light_dir0_fade, l0.data(), sizeof(vs.light_dir0_fade));
    memcpy(vs.light_dir1_fade_en, l1.data(), sizeof(vs.light_dir1_fade_en));
    memcpy(vs.light_dir2, lights.direction2.data(), 3 * sizeof(float));
    memcpy(vs.light_col0, lights.color0.data(), sizeof(vs.light_col0));
    memcpy(vs.light_col1, lights.color1.data(), sizeof(vs.light_col1));
    memcpy(vs.light_col2, lights.color2.data(), sizeof(vs.light_col2));
    memcpy(vs.light_ambient, lights.ambient.data(), sizeof(vs.light_ambient));
    for (int i = 0; i < 4; i++) {
      vs.fade[i] = draw.fade[i] / 255.f;
    }
    vs.height_scale = metal_height_scale(render_state->version);
    vs.scissor_adjust = metal_scissor_adjust(render_state->version);

    MercFsParams fs = {};
    const float fog_alpha =
        draw.mode.get_fog_enable() ? render_state->fog_intensity / 255.f : 0.f;
    fs.fog_color[0] = render_state->fog_color[0] / 255.f;
    fs.fog_color[1] = render_state->fog_color[1] / 255.f;
    fs.fog_color[2] = render_state->fog_color[2] / 255.f;
    fs.fog_color[3] = fog_alpha;
    memcpy(fs.light_dir0_fade, vs.light_dir0_fade, sizeof(fs.light_dir0_fade));
    memcpy(fs.light_dir1_fade_en, vs.light_dir1_fade_en, sizeof(fs.light_dir1_fade_en));
    fs.ignore_alpha = (draw.flags & IGNORE_ALPHA) != 0;
    fs.decal_enable = draw.mode.get_decal();
    fs.gfx_hack_no_tex = (draw.flags & NO_TEXTURE) != 0;

    if (envmap) {
      ASSERT(draw.mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_0_DST_DST);
    }

    id<MTLRenderPipelineState> pso = ctx.pso_cache->get_pipeline(settings.pso);
    ASSERT(pso);
    [enc setRenderPipelineState:pso];
    [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(settings.depth)];
    if (settings.needs_blend_color) {
      [enc setBlendColorRed:0.5f green:0.5f blue:0.5f alpha:0.5f];
    }
    [enc setVertexBytes:&vs length:sizeof(vs) atIndex:1];
    [enc setVertexBuffer:bone_buffer
                  offset:bone_base + (u32)sizeof(math::Vector4f) * draw.first_bone
                 atIndex:2];
    [enc setFragmentBytes:&fs length:sizeof(fs) atIndex:0];
    [enc setFragmentTexture:tex atIndex:0];
    [enc setFragmentSamplerState:ctx.sampler_cache->get(settings.sampler) atIndex:0];

    // EICHAR's grouped profile makes this proportional to its distinct
    // influence tuples, not its roughly ten thousand vertices. Read the exact
    // shared vertex and palette buffers currently bound for this base draw.
    // Envmap is a deliberate second pass and is excluded from every duplicate
    // and weighted-skin count.
    if (!envmap && draw.skin_profile) {
      const u32 palette_offset =
          bone_base + static_cast<u32>(sizeof(math::Vector4f)) * draw.first_bone;
      const size_t palette_bytes = draw.trace_bone_count * sizeof(ShaderMercMat);
      const bool vertex_range_valid = bound_vertex_buffer &&
                                      bound_vertex_offset <= [bound_vertex_buffer length] &&
                                      bound_vertex_count <=
                                          ([bound_vertex_buffer length] - bound_vertex_offset) /
                                              sizeof(tfrag3::MercVertex);
      const bool palette_range_valid =
          bone_buffer && static_cast<u64>(palette_offset) + palette_bytes <= [bone_buffer length];
      if (vertex_range_valid && palette_range_valid) {
        const auto* bound_vertices =
            static_cast<const u8*>([bound_vertex_buffer contents]) + bound_vertex_offset;
        const auto* bound_palette_bytes =
            static_cast<const u8*>([bone_buffer contents]) + palette_offset;
        const auto* bound_palette = reinterpret_cast<const float*>(bound_palette_bytes);
        const u64 bound_palette_hash = metal_merc_skin_trace::hash_draw_palette(
            *draw.skin_profile, bound_palette, draw.trace_bone_count);

        auto skin = metal_merc_skin_trace::analyze_draw(
            *draw.skin_profile, bound_vertices, bound_vertex_count, bound_palette,
            draw.trace_bone_count);
        if (skin.affected_draws) {
          const auto effect = m_eichar_skin_tracker.observe_affected_effect(
              render_state->engine_frame_id, draw.trace_source_base_valid,
              draw.trace_source_base, draw.trace_packet_sequence, draw.trace_effect_index);
          skin.affected_effects = effect.unique;
          stats->eichar_duplication.capacity_drops += effect.capacity_dropped;
        }
        stats->eichar_weighted_skin.add(skin);

        const auto duplicate = m_eichar_skin_tracker.observe_base_draw(
            render_state->engine_frame_id, draw.trace_source_base_valid,
            draw.trace_source_base, draw.trace_packet_sequence, draw.skin_profile->identity,
            bound_palette_hash);
        metal_merc_skin_trace::retain_draw_result(duplicate, &stats->eichar_duplication);
        if (bound_palette_hash != draw.trace_packet_palette_hash) {
          stats->eichar_duplication.bound_palette_hash_mismatches++;
        }
      } else {
        stats->eichar_weighted_skin.profile_tuple_mismatches++;
      }
    }

    [enc drawIndexedPrimitives:(draw.no_strip ? MTLPrimitiveTypeTriangle
                                              : MTLPrimitiveTypeTriangleStrip)
                    indexCount:draw.index_count
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:lev->indices
             indexBufferOffset:sizeof(u32) * draw.first_index];

    stats->draws++;
    stats->triangles += draw.num_triangles;
    if (envmap) {
      stats->envmap_draws++;
    }
    ctx.draw_calls++;
    ctx.triangles += draw.num_triangles;
  }
}

void MetalMercBucketRenderer::render(DmaFollower& dma,
                                     MetalSharedRenderState* render_state,
                                     MetalFrameContext& ctx) {
  m_renderer->render(dma, render_state, ctx, &m_stats);
}
