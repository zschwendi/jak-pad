#include "metal_merc.h"

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/pipelines/metal/metal_eye_renderer.h"
#include "game/graphics/texture/TexturePool.h"

#include "fmt/format.h"

namespace {

constexpr float kGameHeightJak1 = 448.f;

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
  mod_effects_deferred += o.mod_effects_deferred;
  eye_draws += o.eye_draws;
  missing_textures += o.missing_textures;
  bad_bone_pointers += o.bad_bone_pointers;
  bad_draw_ranges += o.bad_draw_ranges;
}

MetalMerc2::MetalMerc2(id<MTLDevice> device, id<MTLCommandQueue> queue, TexturePool* texture_pool) {
  metal_merc_models().init(device, queue, texture_pool);
  for (int i = 0; i < MAX_LEVELS; i++) {
    auto& draws = m_level_draw_buckets.emplace_back();
    draws.draws.resize(MAX_DRAWS_PER_LEVEL);
    draws.envmap_draws.resize(MAX_DRAWS_PER_LEVEL);
  }
}

void MetalMerc2::render(DmaFollower& dma,
                        MetalSharedRenderState* render_state,
                        MetalFrameContext& ctx,
                        Stats* stats) {
  *stats = {};
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

  ASSERT(render_state->version == GameVersion::Jak1);
  auto second = dma.read_and_advance();
  ASSERT(second.size_bytes == 32);  // setting up test register.
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
  const int skip_count = 2;  // Jak 1

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
  ASSERT(strlen((const char*)input_data) < 127);
  char name[128];
  strcpy(name, (const char*)setup.data);
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

  LevelDrawBucket* lev_bucket = nullptr;
  for (u32 i = 0; i < m_next_free_level_bucket; i++) {
    if (m_level_draw_buckets[i].level == lev) {
      lev_bucket = &m_level_draw_buckets[i];
      break;
    }
  }
  if (!lev_bucket) {
    if (m_next_free_level_bucket >= m_level_draw_buckets.size()) {
      flush_draw_buckets(render_state, ctx, stats);
    }
    lev_bucket = &m_level_draw_buckets[m_next_free_level_bucket++];
    lev_bucket->reset();
    lev_bucket->level = lev;
  }

  if (lev_bucket->next_free_draw + model->max_draws >= lev_bucket->draws.size()) {
    lg::warn("Metal merc: out of draws, flushing");
    flush_draw_buckets(render_state, ctx, stats);
    ASSERT(model->max_draws < lev_bucket->draws.size());
  }
  if (lev_bucket->next_free_envmap_draw + model->max_draws >= lev_bucket->envmap_draws.size()) {
    lg::warn("Metal merc: out of envmap draws, flushing");
    flush_draw_buckets(render_state, ctx, stats);
    ASSERT(model->max_draws < lev_bucket->envmap_draws.size());
  }

  VuLights current_lights;
  memcpy(&current_lights, input_data, sizeof(VuLights));
  input_data += sizeof(VuLights);

  u64 uses_water = 0;
  // jak 1 figures out water at runtime
  memcpy(&uses_water, input_data, 8);
  input_data += 16;

  // The matrix slot string tells us which bones go where; the matrices
  // themselves live in EE main memory (bones runs after merc's DMA is built).
  ShaderMercMat skel_matrix_buffer[MAX_SKEL_BONES] = {};
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
    memcpy(&skel_matrix_buffer[input_data[i]], ee0 + addr, sizeof(MercMat));
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

  if (model_uses_pc_blerc) {
    input_data += 40 * sizeof(float);  // Merc2::kMaxBlerc weights
  }

  u8 fade_buffer[4 * kMaxEffect];
  for (int ei = 0; ei < num_effects; ei++) {
    for (int j = 0; j < 4; j++) {
      fade_buffer[ei * 4 + j] = input_data[ei * 4 + j];
    }
  }
  input_data += (((num_effects * 4) + 15) / 16) * 16;

  // Vertex modification (blerc and the mod-vertex path) is not ported: those
  // effects are drawn from the level's unmodified vertices instead of the
  // updated copy. Counted here and logged once, never silently dropped.
  bool model_wants_mod = model_uses_pc_blerc || model_uses_mod;

  stats->models++;
  for (const auto& effect : model->effects) {
    stats->effects++;
    if (model_wants_mod && effect.has_mod_draw) {
      stats->mod_effects_deferred++;
    }
  }
  if (model_wants_mod && !m_warned_mod) {
    lg::warn(
        "Metal merc: model '{}' asks for vertex modification (blerc/mod-vtx), which is not "
        "ported; drawing the unmodified vertices (logged once)",
        model->name);
    m_warned_mod = true;
  }

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

  for (size_t ei = 0; ei < model->effects.size(); ei++) {
    args.fade = fade_buffer + 4 * ei;

    if (!(current_effect_enable_bits & (1ull << ei))) {
      continue;
    }

    args.ignore_alpha = !!(current_ignore_alpha_bits & (1ull << ei));
    auto& effect = model->effects[ei];
    bool should_envmap = effect.has_envmap && !model_disables_envmap;

    for (auto& draw : effect.all_draws) {
      if (should_envmap) {
        try_alloc_envmap_draw(draw, effect.envmap_mode, effect.envmap_texture, args);
      }
      alloc_normal_draw(draw, args);
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
    vs.height_scale = 1.f;  // Jak 1
    vs.scissor_adjust = 512.f / kGameHeightJak1;

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
