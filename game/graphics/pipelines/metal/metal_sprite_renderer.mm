#include "metal_sprite_renderer.h"

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/opengl_renderer/dma_helpers.h"
#include "game/graphics/texture/TexturePool.h"

#include "fmt/format.h"

namespace {

// The GL renderer buffers up to 1920 * 12 sprites before flushing. Here the
// limit is what one page of the per-frame stream buffer holds, because a
// flush's vertices must live in a single allocation. Flushing earlier is
// behaviourally identical - the GL renderer already flushes mid-block at its
// own limit (Sprite3::do_block_common).
constexpr int kMaxSpritesPerFlush = 8192;

constexpr float kGameHeightJak1 = 448.f;

/*!
 * Does the next DMA transfer look like the start of a 2D group?
 */
bool looks_like_2d_chunk_start(const DmaFollower& dma) {
  return dma.current_tag().qwc == 1 && dma.current_tag().kind == DmaTag::Kind::CNT;
}

/*!
 * Does the next DMA transfer look like the frame data for sprite distort?
 */
bool looks_like_distort_frame_data(const DmaFollower& dma) {
  return dma.current_tag().kind == DmaTag::Kind::CNT &&
         dma.current_tag_vifcode0().kind == VifCode::Kind::NOP &&
         dma.current_tag_vifcode1().kind == VifCode::Kind::UNPACK_V4_32;
}

/*!
 * Read a sprite chunk header. Returns the number of sprites, advances one
 * transfer. (Sprite3.cpp's process_sprite_chunk_header.)
 */
u32 process_sprite_chunk_header(DmaFollower& dma) {
  auto transfer = dma.read_and_advance();
  bool ok = verify_unpack_with_stcycl(transfer, VifCode::Kind::UNPACK_V4_32, 4, 4, 1,
                                      SpriteDataMem::Header, false, true);
  ASSERT(ok);
  u32 header[4];
  memcpy(header, transfer.data, 16);
  ASSERT(header[0] <= MetalSpriteRenderer::SPRITES_PER_CHUNK);
  return header[0];
}

// Must match SpriteVsParams in shaders/sprite.metal.
struct SpriteVsParams {
  float camera[16];
  float hud_matrix[16];
  float hvdf_offset[4];
  float hud_hvdf_offset[4];
  float basis_x[4];
  float basis_y[4];
  float xy_array[8][4];
  float xyz_array[4][4];
  float st_array[4][4];
  float pfog0;
  float fog_min;
  float fog_max;
  float min_scale;
  float max_scale;
  float deg_to_rad;
  float inv_area;
  float height_scale;
  float scissor_adjust;
  float pad[3];
};
static_assert(sizeof(SpriteVsParams) == 496);

// Must match SpriteFsParams in shaders/sprite.metal.
struct SpriteFsParams {
  float alpha_min;
  float alpha_max;
};

/*!
 * Metal equivalent of setup_opengl_from_draw_mode
 * (game/graphics/opengl_renderer/background/background_common.cpp): the GL
 * function mutates global state, this one fills in the state keys the encoder
 * needs. Mipmaps are off, as in the GL sprite flush.
 */
struct SpriteDrawSettings {
  MetalPsoKey pso;
  MetalDepthStencilKey depth;
  MetalSamplerKey sampler;
  bool needs_blend_color = false;  // SRC_DST_FIX_DST uses a constant of 0.5
  bool afail_double_draw = false;
  float aref_first = 0.f;
  float aref_second = 0.f;
};

SpriteDrawSettings settings_from_draw_mode(DrawMode mode, MetalFrameContext& ctx) {
  SpriteDrawSettings out;
  out.pso.shader = MetalShaderId::SPRITE3;
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

  bool blend_enable = mode.get_ab_enable() && mode.get_alpha_blend() != DrawMode::AlphaBlend::DISABLED;
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
        // the GL path also computes a 0.5 color multiplier here, which the
        // sprite shader has no uniform for and the GL flush discards too
        out.pso.blend_src_rgb = MTLBlendFactorDestinationAlpha;
        out.pso.blend_dst_rgb = MTLBlendFactorOne;
        out.pso.blend_src_alpha = MTLBlendFactorDestinationAlpha;
        out.pso.blend_dst_alpha = MTLBlendFactorOne;
        break;
      default:
        ASSERT(false);
    }
  }
  out.pso.blend_enable = blend_enable;

  out.sampler.wrap_s = mode.get_clamp_s_enable() ? MTLSamplerAddressModeClampToEdge
                                                 : MTLSamplerAddressModeRepeat;
  out.sampler.wrap_t = mode.get_clamp_t_enable() ? MTLSamplerAddressModeClampToEdge
                                                 : MTLSamplerAddressModeRepeat;
  out.sampler.min_filter = mode.get_filt_enable() ? MTLSamplerMinMagFilterLinear
                                                  : MTLSamplerMinMagFilterNearest;
  out.sampler.mag_filter = out.sampler.min_filter;

  // the game sets atest NEVER + FB_ONLY to mean "no depth writes"
  bool alpha_hack_to_disable_z_write = false;
  float alpha_min = 0.f;
  if (mode.get_at_enable()) {
    switch (mode.get_alpha_test()) {
      case DrawMode::AlphaTest::ALWAYS:
        break;
      case DrawMode::AlphaTest::GEQUAL:
        alpha_min = mode.get_aref() / 127.f;
        switch (mode.get_alpha_fail()) {
          case GsTest::AlphaFail::KEEP:
            break;
          case GsTest::AlphaFail::FB_ONLY:
            if (mode.get_depth_write_enable()) {
              out.afail_double_draw = true;
              out.aref_second = alpha_min;
            } else {
              alpha_min = 0.f;
            }
            break;
          default:
            ASSERT(false);
        }
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
  out.aref_first = alpha_min;
  return out;
}

}  // namespace

MetalSpriteRenderer::MetalSpriteRenderer(const std::string& name, int my_id)
    : MetalBucketRenderer(name, my_id), m_direct(name, my_id, 1024) {
  m_vertices_3d.resize(kMaxSpritesPerFlush * 4);
  m_index_buffer_data.resize(kMaxSpritesPerFlush * 5);

  m_default_mode.disable_depth_write();
  m_default_mode.set_depth_test(GsTest::ZTest::GEQUAL);
  m_default_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
  m_default_mode.set_aref(38);
  m_default_mode.set_alpha_test(DrawMode::AlphaTest::GEQUAL);
  m_default_mode.set_alpha_fail(GsTest::AlphaFail::FB_ONLY);
  m_default_mode.set_at(true);
  m_default_mode.set_zt(true);
  m_default_mode.set_ab(true);
  m_current_mode = m_default_mode;
}

/*!
 * Mirror of Sprite3::render_jak1.
 */
void MetalSpriteRenderer::render(DmaFollower& dma,
                                 MetalSharedRenderState* render_state,
                                 MetalFrameContext& ctx) {
  ASSERT_MSG(render_state->version == GameVersion::Jak1,
             "Metal sprite renderer only supports Jak 1");
  m_stats = {};

  // NEXT with two nops: the jump from the bucket array into the sprite data
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0);
  ASSERT(data0.vif0() == 0);
  ASSERT(data0.size_bytes == 0);

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // the sprite renderer didn't run this frame
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
    return;
  }

  // some DirectRenderer DMA may come first
  if (render_direct(dma, render_state, ctx)) {
    return;
  }

  distort_dma(dma);
  handle_sprite_frame_setup(dma);
  render_3d(dma);

  render_2d_group0(dma, render_state, ctx);
  flush_sprites(render_state, ctx, false);

  render_fake_shadow(dma);

  render_2d_group1(dma, render_state, ctx);
  flush_sprites(render_state, ctx, true);

  // the GL renderer consumes the remainder of the bucket the same way
  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }
}

bool MetalSpriteRenderer::render_direct(DmaFollower& dma,
                                        MetalSharedRenderState* render_state,
                                        MetalFrameContext& ctx) {
  m_direct.reset_state();
  while (dma.current_tag().qwc != 7 && dma.current_tag_offset() != render_state->next_bucket) {
    auto direct_data = dma.read_and_advance();
    m_direct.render_vif(direct_data.vif0(), direct_data.vif1(), direct_data.data,
                        direct_data.size_bytes, render_state, ctx);
  }
  m_direct.flush_pending(render_state, ctx);

  // with sprites off there is nothing after the direct data
  return dma.current_tag_offset() == render_state->next_bucket;
}

/*!
 * Walks the distorter's DMA exactly like Sprite3::distort_dma so the chain
 * stays in sync, and counts the sprites. The distort *drawing* (a snapshot of
 * the framebuffer resampled through sine tables) is not ported yet.
 */
void MetalSpriteRenderer::distort_dma(DmaFollower& dma) {
  // GS setup
  auto setup = dma.read_and_advance();
  ASSERT(setup.vifcode0().kind == VifCode::Kind::NOP);
  ASSERT(setup.vifcode1().kind == VifCode::Kind::DIRECT);
  ASSERT(setup.vifcode1().immediate == 7);
  struct {
    GifTag gif_tag;
    GsZbuf zbuf;
    u64 zbuf_addr;
    GsTex0 tex0;
    u64 tex0_addr;
    GsTex1 tex1;
    u64 tex1_addr;
    u64 miptbp;
    u64 miptbp_addr;
    u64 clamp;
    u64 clamp_addr;
    GsAlpha alpha;
    u64 alpha_addr;
  } distorter_setup;
  static_assert(sizeof(distorter_setup) == 7 * 16);
  memcpy(&distorter_setup, setup.data, 7 * 16);

  ASSERT(distorter_setup.gif_tag.nloop() == 1);
  ASSERT(distorter_setup.gif_tag.eop() == true);
  ASSERT(distorter_setup.gif_tag.nreg() == 6);
  ASSERT(distorter_setup.gif_tag.reg(0) == GifTag::RegisterDescriptor::AD);
  ASSERT(distorter_setup.zbuf.zbp() == 0x1c0);
  ASSERT(distorter_setup.zbuf.zmsk() == true);
  ASSERT(distorter_setup.zbuf.psm() == TextureFormat::PSMZ24);
  ASSERT(distorter_setup.tex0.tbw() == 8);
  ASSERT(distorter_setup.tex0.tw() == 9);
  ASSERT(distorter_setup.tex0.th() == 8);
  ASSERT(distorter_setup.tex1.mmag() == 1);
  ASSERT(distorter_setup.tex1.mmin() == 1);
  ASSERT(distorter_setup.alpha.a_mode() == GsAlpha::BlendMode::SOURCE);
  ASSERT(distorter_setup.alpha.b_mode() == GsAlpha::BlendMode::DEST);
  ASSERT(distorter_setup.alpha.c_mode() == GsAlpha::BlendMode::SOURCE);
  ASSERT(distorter_setup.alpha.d_mode() == GsAlpha::BlendMode::DEST);

  // aspect the sine tables were built for (PC only)
  auto tables_aspect = dma.read_and_advance();
  ASSERT(tables_aspect.size_bytes == 16);
  ASSERT(tables_aspect.vifcode1().kind == VifCode::Kind::PC_PORT);

  // sine tables
  struct SineTables {
    math::Vector4f entry[128];
    math::Vector<u32, 4> ientry[9];
    GifTag gs_gif_tag;
    math::Vector<u32, 4> color;
  } sine_tables;
  static_assert(sizeof(SineTables) == 0x8b * 16);
  auto tables = dma.read_and_advance();
  unpack_to_stcycl(&sine_tables, tables, VifCode::Kind::UNPACK_V4_32, 4, 4, 0x8b * 16, 0x160, false,
                   false);
  ASSERT(GsPrim(sine_tables.gs_gif_tag.prim()).kind() == GsPrim::Kind::TRI_STRIP);

  // frame data packets
  while (looks_like_distort_frame_data(dma)) {
    math::Vector<u32, 4> num_sprites_vec{0, 0, 0, 0};
    do {
      int dest = dma.current_tag_vifcode1().immediate;
      auto distort_data = dma.read_and_advance();
      if (dest == 511) {
        unpack_to_no_stcycl(&num_sprites_vec, distort_data, VifCode::Kind::UNPACK_V4_32, 16, dest,
                            false, false);
      } else {
        ASSERT(dest >= 512);
      }
    } while (looks_like_distort_frame_data(dma));

    ASSERT(dma.current_tag().kind == DmaTag::Kind::CNT);
    ASSERT(dma.current_tag_vifcode0().kind == VifCode::Kind::MSCALF);
    ASSERT(dma.current_tag_vifcode1().kind == VifCode::Kind::FLUSH);
    dma.read_and_advance();

    m_stats.distort_sprites += num_sprites_vec.x();
  }

  if (m_stats.distort_sprites && !m_warned_distort) {
    lg::warn("Metal sprite {}: distort drawing not ported yet; {} distort sprites consumed",
             m_name, m_stats.distort_sprites);
    m_warned_distort = true;
  }
}

/*!
 * Mirror of Sprite3::handle_sprite_frame_setup, Jak 1 branch.
 */
void MetalSpriteRenderer::handle_sprite_frame_setup(DmaFollower& dma) {
  auto direct_data = dma.read_and_advance();
  ASSERT(direct_data.size_bytes == 3 * 16);
  memcpy(m_sprite_direct_setup, direct_data.data, 3 * 16);
  ASSERT(m_sprite_direct_setup[0] == 0x2000000000008001);
  ASSERT(m_sprite_direct_setup[1] == 0xEEEEEEEEEEEEEEEE);
  ASSERT(m_sprite_direct_setup[2] == 0x000000000005126B);
  ASSERT(m_sprite_direct_setup[3] == 0x0000000000000047);
  ASSERT(m_sprite_direct_setup[4] == 0x0000000000000005);
  ASSERT(m_sprite_direct_setup[5] == 0x0000000000000008);

  auto frame_data = dma.read_and_advance();
  ASSERT(frame_data.size_bytes == (int)sizeof(SpriteFrameDataJak1));
  ASSERT(frame_data.vifcode0().kind == VifCode::Kind::STCYCL);
  VifCodeStcycl frame_data_stcycl(frame_data.vifcode0());
  ASSERT(frame_data_stcycl.cl == 4);
  ASSERT(frame_data_stcycl.wl == 4);
  ASSERT(frame_data.vifcode1().kind == VifCode::Kind::UNPACK_V4_32);
  VifCodeUnpack frame_data_unpack(frame_data.vifcode1());
  ASSERT(frame_data_unpack.addr_qw == SpriteDataMem::FrameData);
  ASSERT(frame_data_unpack.use_tops_flag == false);
  SpriteFrameDataJak1 jak1_data;
  memcpy(&jak1_data, frame_data.data, sizeof(SpriteFrameDataJak1));
  m_frame_data.from_jak1(jak1_data);

  auto mscalf = dma.read_and_advance();
  ASSERT(mscalf.size_bytes == 0);
  ASSERT(mscalf.vifcode0().kind == VifCode::Kind::MSCALF);
  ASSERT(mscalf.vifcode0().immediate == SpriteProgMem::Init);
  ASSERT(mscalf.vifcode1().kind == VifCode::Kind::FLUSHE);

  auto base_offset = dma.read_and_advance();
  ASSERT(base_offset.size_bytes == 0);
  ASSERT(base_offset.vifcode0().kind == VifCode::Kind::BASE);
  ASSERT(base_offset.vifcode0().immediate == SpriteDataMem::Buffer0);
  ASSERT(base_offset.vifcode1().kind == VifCode::Kind::OFFSET);
  ASSERT(base_offset.vifcode1().immediate == SpriteDataMem::Buffer1);
}

void MetalSpriteRenderer::render_3d(DmaFollower& dma) {
  auto matrix_data = dma.read_and_advance();
  ASSERT(matrix_data.size_bytes == sizeof(Sprite3DMatrixData));
  bool unpack_ok = verify_unpack_with_stcycl(matrix_data, VifCode::Kind::UNPACK_V4_32, 4, 4, 5,
                                             SpriteDataMem::Matrix, false, false);
  ASSERT(unpack_ok);
  static_assert(sizeof(m_3d_matrix_data) == 5 * 16);
  memcpy(&m_3d_matrix_data, matrix_data.data, sizeof(m_3d_matrix_data));
}

void MetalSpriteRenderer::render_2d_group0(DmaFollower& dma,
                                           MetalSharedRenderState* render_state,
                                           MetalFrameContext& ctx) {
  u16 last_prog = -1;
  while (looks_like_2d_chunk_start(dma)) {
    m_stats.blocks_2d_grp0++;

    u32 sprite_count = process_sprite_chunk_header(dma);
    m_stats.count_2d_grp0 += sprite_count;

    u32 expected_vec_size = sizeof(SpriteVecData2d) * sprite_count;
    auto vec_data = dma.read_and_advance();
    ASSERT(expected_vec_size <= sizeof(m_vec_data_2d));
    unpack_to_no_stcycl(&m_vec_data_2d, vec_data, VifCode::Kind::UNPACK_V4_32, expected_vec_size,
                        SpriteDataMem::Vector, false, true);

    u32 expected_adgif_size = sizeof(AdGifData) * sprite_count;
    auto adgif_data = dma.read_and_advance();
    ASSERT(expected_adgif_size <= sizeof(m_adgif));
    unpack_to_no_stcycl(&m_adgif, adgif_data, VifCode::Kind::UNPACK_V4_32, expected_adgif_size,
                        SpriteDataMem::Adgif, false, true);

    auto run = dma.read_and_advance();
    ASSERT(run.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(run.vifcode1().kind == VifCode::Kind::MSCAL);

    if (run.vifcode1().immediate != last_prog) {
      flush_sprites(render_state, ctx, false);
    }
    if (run.vifcode1().immediate == SpriteProgMem::Sprites2dGrp0) {
      do_block_common(SpriteMode::Mode2D, sprite_count, render_state, ctx);
    } else {
      m_stats.sprites_3d += sprite_count;
      do_block_common(SpriteMode::Mode3D, sprite_count, render_state, ctx);
    }
    last_prog = run.vifcode1().immediate;
  }
}

void MetalSpriteRenderer::render_fake_shadow(DmaFollower& dma) {
  auto nop_flushe = dma.read_and_advance();
  ASSERT(nop_flushe.vifcode0().kind == VifCode::Kind::NOP);
  ASSERT(nop_flushe.vifcode1().kind == VifCode::Kind::FLUSHE);
}

void MetalSpriteRenderer::render_2d_group1(DmaFollower& dma,
                                           MetalSharedRenderState* render_state,
                                           MetalFrameContext& ctx) {
  auto mat_upload = dma.read_and_advance();
  bool mat_ok = verify_unpack_with_stcycl(mat_upload, VifCode::Kind::UNPACK_V4_32, 4, 4, 80,
                                          SpriteDataMem::Matrix, false, false);
  ASSERT(mat_ok);
  ASSERT(mat_upload.size_bytes == sizeof(m_hud_matrix_data));
  memcpy(&m_hud_matrix_data, mat_upload.data, sizeof(m_hud_matrix_data));

  while (looks_like_2d_chunk_start(dma)) {
    m_stats.blocks_2d_grp1++;

    u32 sprite_count = process_sprite_chunk_header(dma);
    m_stats.count_2d_grp1 += sprite_count;

    u32 expected_vec_size = sizeof(SpriteVecData2d) * sprite_count;
    auto vec_data = dma.read_and_advance();
    ASSERT(expected_vec_size <= sizeof(m_vec_data_2d));
    unpack_to_no_stcycl(&m_vec_data_2d, vec_data, VifCode::Kind::UNPACK_V4_32, expected_vec_size,
                        SpriteDataMem::Vector, false, true);

    u32 expected_adgif_size = sizeof(AdGifData) * sprite_count;
    auto adgif_data = dma.read_and_advance();
    ASSERT(expected_adgif_size <= sizeof(m_adgif));
    unpack_to_no_stcycl(&m_adgif, adgif_data, VifCode::Kind::UNPACK_V4_32, expected_adgif_size,
                        SpriteDataMem::Adgif, false, true);

    auto run = dma.read_and_advance();
    ASSERT(run.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(run.vifcode1().kind == VifCode::Kind::MSCAL);
    ASSERT(run.vifcode1().immediate == SpriteProgMem::Sprites2dHud_Jak1);

    do_block_common(SpriteMode::ModeHUD, sprite_count, render_state, ctx);
  }
}

void MetalSpriteRenderer::handle_tex0(u64 val) {
  GsTex0 reg(val);
  m_current_tbp = reg.tbp0();
  m_current_mode.set_tcc(reg.tcc());
  ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);
  ASSERT(reg.psm() != GsTex0::PSM::PSMT4HH);
}

void MetalSpriteRenderer::handle_tex1(u64 val) {
  GsTex1 reg(val);
  m_current_mode.set_filt_enable(reg.mmag());
}

void MetalSpriteRenderer::handle_zbuf(u64 val) {
  GsZbuf x(val);
  ASSERT(x.psm() == TextureFormat::PSMZ24);
  ASSERT(x.zbp() == 448);
  m_current_mode.set_depth_write_enable(!x.zmsk());
}

void MetalSpriteRenderer::handle_clamp(u64 val) {
  if (!(val == 0b101 || val == 0 || val == 1 || val == 0b100)) {
    ASSERT_MSG(false, fmt::format("clamp: 0x{:x}", val));
  }
  m_current_mode.set_clamp_s_enable(val & 0b001);
  m_current_mode.set_clamp_t_enable(val & 0b100);
}

void MetalSpriteRenderer::handle_alpha(u64 val) {
  GsAlpha reg(val);
  if (reg.a_mode() == GsAlpha::BlendMode::SOURCE && reg.b_mode() == GsAlpha::BlendMode::DEST &&
      reg.c_mode() == GsAlpha::BlendMode::SOURCE && reg.d_mode() == GsAlpha::BlendMode::DEST) {
    m_current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
  } else if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.b_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.c_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.d_mode() == GsAlpha::BlendMode::DEST) {
    m_current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_SRC_DST);
  } else if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.b_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.c_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.d_mode() == GsAlpha::BlendMode::DEST) {
    ASSERT(reg.fix() == 128);
    m_current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_FIX_DST);
  } else if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.b_mode() == GsAlpha::BlendMode::DEST &&
             reg.c_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.d_mode() == GsAlpha::BlendMode::DEST) {
    ASSERT(reg.fix() == 64);
    m_current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_FIX_DST);
  } else if (reg.a_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.b_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.c_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.d_mode() == GsAlpha::BlendMode::DEST) {
    m_current_mode.set_alpha_blend(DrawMode::AlphaBlend::ZERO_SRC_SRC_DST);
  } else {
    lg::error("Metal sprite: unsupported blend a {} b {} c {} d {}", (int)reg.a_mode(),
              (int)reg.b_mode(), (int)reg.c_mode(), (int)reg.d_mode());
    m_current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
    ASSERT(false);
  }
}

/*!
 * Mirror of Sprite3::do_block_common. Note that the GL view-frustum culling
 * needs the PC vis data (render_state->has_pc_data), which the Metal path does
 * not receive yet - the GL renderer skips culling in exactly that case too.
 */
void MetalSpriteRenderer::do_block_common(SpriteMode mode,
                                          u32 count,
                                          MetalSharedRenderState* render_state,
                                          MetalFrameContext& ctx) {
  m_current_mode = m_default_mode;
  for (u32 sprite_idx = 0; sprite_idx < count; sprite_idx++) {
    if (m_sprite_idx == kMaxSpritesPerFlush) {
      flush_sprites(render_state, ctx, mode == ModeHUD);
    }

    auto& adgif = m_adgif[sprite_idx];
    handle_tex0(adgif.tex0_data);
    handle_tex1(adgif.tex1_data);
    if (GsRegisterAddress(adgif.clamp_addr) == GsRegisterAddress::ZBUF_1) {
      handle_zbuf(adgif.clamp_data);
    } else {
      handle_clamp(adgif.clamp_data);
    }
    handle_alpha(adgif.alpha_data);

    u64 key = (((u64)m_current_tbp) << 32) | m_current_mode.as_int();
    Bucket* bucket;
    if (key == m_last_bucket_key) {
      bucket = m_last_bucket;
    } else {
      auto it = m_sprite_buckets.find(key);
      if (it == m_sprite_buckets.end()) {
        bucket = &m_sprite_buckets[key];
        bucket->key = key;
        m_bucket_list.push_back(bucket);
      } else {
        bucket = &it->second;
      }
      // the GL renderer leaves this fast path dead (it never records the key);
      // recording it here finds the same bucket, since std::map keeps pointers
      // stable across inserts
      m_last_bucket_key = key;
      m_last_bucket = bucket;
    }

    u32 start_vtx_id = m_sprite_idx * 4;
    bucket->ids.push_back(start_vtx_id);
    bucket->ids.push_back(start_vtx_id + 1);
    bucket->ids.push_back(start_vtx_id + 2);
    bucket->ids.push_back(start_vtx_id + 3);
    bucket->ids.push_back(UINT32_MAX);

    auto& vert1 = m_vertices_3d.at(start_vtx_id + 0);
    vert1.xyz_sx = m_vec_data_2d[sprite_idx].xyz_sx;
    vert1.quat_sy = m_vec_data_2d[sprite_idx].flag_rot_sy;
    // ftoi'd in the original game; the VIF discards the upper bits on pack
    vert1.rgba = m_vec_data_2d[sprite_idx].rgba;
    vert1.rgba.x() = (int)vert1.rgba.x() & 0xff;
    vert1.rgba.y() = (int)vert1.rgba.y() & 0xff;
    vert1.rgba.z() = (int)vert1.rgba.z() & 0xff;
    vert1.rgba.w() = (int)vert1.rgba.w() & 0xff;
    vert1.rgba /= 255;
    vert1.flags_matrix[0] = m_vec_data_2d[sprite_idx].flag();
    vert1.flags_matrix[1] = m_vec_data_2d[sprite_idx].matrix();
    vert1.info[0] = 0;
    vert1.info[1] = m_current_mode.get_tcc_enable();
    vert1.info[2] = 0;
    vert1.info[3] = mode;

    m_vertices_3d.at(start_vtx_id + 1) = vert1;
    m_vertices_3d.at(start_vtx_id + 2) = vert1;
    m_vertices_3d.at(start_vtx_id + 3) = vert1;
    m_vertices_3d.at(start_vtx_id + 1).info[2] = 1;
    m_vertices_3d.at(start_vtx_id + 2).info[2] = 3;
    m_vertices_3d.at(start_vtx_id + 3).info[2] = 2;

    ++m_sprite_idx;
  }
}

/*!
 * Mirror of Sprite3::flush_sprites: one indexed triangle-strip draw per
 * (texture, DrawMode) bucket, with 0xFFFFFFFF separating sprites. Metal
 * restarts strips on that index for UInt32 indices without any state to set.
 */
void MetalSpriteRenderer::flush_sprites(MetalSharedRenderState* render_state,
                                        MetalFrameContext& ctx,
                                        bool double_draw) {
  if (m_sprite_idx == 0 || m_bucket_list.empty()) {
    m_sprite_buckets.clear();
    m_bucket_list.clear();
    m_last_bucket_key = UINT64_MAX;
    m_last_bucket = nullptr;
    m_sprite_idx = 0;
    return;
  }

  // vertices
  const u32 vtx_bytes = (u32)(m_sprite_idx * 4 * sizeof(SpriteVertex3D));
  id<MTLBuffer> vbuf;
  u32 voffset;
  memcpy(ctx.stream->alloc(vtx_bytes, &vbuf, &voffset), m_vertices_3d.data(), vtx_bytes);

  // indices, packed bucket by bucket
  u32 idx_offset = 0;
  for (auto* bucket : m_bucket_list) {
    memcpy(&m_index_buffer_data[idx_offset], bucket->ids.data(), bucket->ids.size() * sizeof(u32));
    bucket->offset_in_idx_buffer = idx_offset;
    idx_offset += bucket->ids.size();
  }
  id<MTLBuffer> ibuf;
  u32 ioffset;
  memcpy(ctx.stream->alloc(idx_offset * sizeof(u32), &ibuf, &ioffset), m_index_buffer_data.data(),
         idx_offset * sizeof(u32));

  SpriteVsParams vs_params = {};
  memcpy(vs_params.camera, m_3d_matrix_data.camera.data(), sizeof(vs_params.camera));
  memcpy(vs_params.hud_matrix, m_hud_matrix_data.matrix.data(), sizeof(vs_params.hud_matrix));
  memcpy(vs_params.hvdf_offset, m_3d_matrix_data.hvdf_offset.data(), sizeof(vs_params.hvdf_offset));
  memcpy(vs_params.hud_hvdf_offset, m_hud_matrix_data.hvdf_offset.data(),
         sizeof(vs_params.hud_hvdf_offset));
  memcpy(vs_params.basis_x, m_frame_data.basis_x.data(), sizeof(vs_params.basis_x));
  memcpy(vs_params.basis_y, m_frame_data.basis_y.data(), sizeof(vs_params.basis_y));
  memcpy(vs_params.xy_array, m_frame_data.xy_array[0].data(), sizeof(vs_params.xy_array));
  memcpy(vs_params.xyz_array, m_frame_data.xyz_array[0].data(), sizeof(vs_params.xyz_array));
  memcpy(vs_params.st_array, m_frame_data.st_array[0].data(), sizeof(vs_params.st_array));
  vs_params.pfog0 = m_frame_data.pfog0;
  vs_params.fog_min = m_frame_data.fog_min;
  vs_params.fog_max = m_frame_data.fog_max;
  vs_params.min_scale = m_frame_data.min_scale;
  vs_params.max_scale = m_frame_data.max_scale;
  vs_params.deg_to_rad = m_frame_data.deg_to_rad;
  vs_params.inv_area = m_frame_data.inv_area;
  vs_params.height_scale = 1.f;  // Jak 1
  vs_params.scissor_adjust = 512.f / kGameHeightJak1;

  id<MTLRenderCommandEncoder> enc = ctx.enc;
  [enc setVertexBuffer:vbuf offset:voffset atIndex:0];
  [enc setVertexBytes:&vs_params length:sizeof(vs_params) atIndex:1];
  [enc setVertexBytes:m_hud_matrix_data.user_hvdf[0].data()
               length:sizeof(m_hud_matrix_data.user_hvdf)
              atIndex:2];

  int draw_count = 0;
  int tri_count = 0;
  for (auto* bucket : m_bucket_list) {
    u32 tbp = bucket->key >> 32;
    DrawMode mode;
    mode.as_int() = bucket->key & 0xffffffff;

    auto settings = settings_from_draw_mode(mode, ctx);

    std::optional<u64> tex = render_state->texture_pool->lookup(tbp);
    if (!tex) {
      m_stats.missing_textures++;
      lg::warn("Metal sprite {}: failed to find texture at {}, using placeholder", m_name, tbp);
      tex = render_state->texture_pool->get_placeholder_texture();
    }
    id<MTLTexture> mtl_tex = metal_texture_lookup(*tex);
    if (!mtl_tex) {
      mtl_tex = metal_texture_lookup(render_state->texture_pool->get_placeholder_texture());
    }
    ASSERT(mtl_tex);

    id<MTLRenderPipelineState> pso = ctx.pso_cache->get_pipeline(settings.pso);
    ASSERT(pso);
    [enc setRenderPipelineState:pso];
    [enc setFragmentTexture:mtl_tex atIndex:0];
    [enc setFragmentSamplerState:ctx.sampler_cache->get(settings.sampler) atIndex:0];
    if (settings.needs_blend_color) {
      [enc setBlendColorRed:0.5f green:0.5f blue:0.5f alpha:0.5f];
    }

    SpriteFsParams fs_params;
    fs_params.alpha_min = double_draw ? settings.aref_first : 0.016f;
    fs_params.alpha_max = 10.f;
    [enc setFragmentBytes:&fs_params length:sizeof(fs_params) atIndex:0];
    [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(settings.depth)];

    const u32 index_count = (u32)bucket->ids.size();
    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                    indexCount:index_count
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:ibuf
             indexBufferOffset:ioffset + bucket->offset_in_idx_buffer * sizeof(u32)];
    draw_count++;
    tri_count += 2 * (index_count / 5);

    if (double_draw && settings.afail_double_draw) {
      // alpha-failing fragments again, without depth writes
      auto no_write = settings.depth;
      no_write.depth_write = false;
      [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(no_write)];
      fs_params.alpha_min = -10.f;
      fs_params.alpha_max = settings.aref_second;
      [enc setFragmentBytes:&fs_params length:sizeof(fs_params) atIndex:0];
      [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                      indexCount:index_count
                       indexType:MTLIndexTypeUInt32
                     indexBuffer:ibuf
               indexBufferOffset:ioffset + bucket->offset_in_idx_buffer * sizeof(u32)];
      draw_count++;
      tri_count += 2 * (index_count / 5);
    }
  }

  ctx.draw_calls += draw_count;
  ctx.triangles += tri_count;
  m_stats.draw_calls += draw_count;
  m_stats.triangles += tri_count;

  m_sprite_buckets.clear();
  m_bucket_list.clear();
  m_last_bucket_key = UINT64_MAX;
  m_last_bucket = nullptr;
  m_sprite_idx = 0;
}
