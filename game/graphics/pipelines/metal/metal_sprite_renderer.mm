#include "metal_sprite_renderer.h"

#include <array>
#include <utility>

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/opengl_renderer/dma_helpers.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/texture/TexturePool.h"

#include "fmt/format.h"

namespace {

// The GL renderer buffers up to 1920 * 12 sprites before flushing. Here the
// limit is what one page of the per-frame stream buffer holds, because a
// flush's vertices must live in a single allocation. Flushing earlier is
// behaviourally identical - the GL renderer already flushes mid-block at its
// own limit (Sprite3::do_block_common).
constexpr int kMaxSpritesPerFlush = 8192;

constexpr PerGameVersion<u32> kNormalZbp(448, 304, 304, 304);

constexpr u16 kGlowConstantsAddress = 980;
constexpr u16 kGlowTemplate0Address = 800;
constexpr u16 kGlowTemplate1Address = 884;
constexpr u16 kGlowBufferOffset = 400;
constexpr u16 kGlowControlAddress = 0;
constexpr u16 kGlowVectorAddress = 1;
constexpr u16 kGlowAdgifAddress = 145;
constexpr u16 kGlowProgramAddress = 10;
constexpr int kMaxGlowRecords = 400;
constexpr bool kGlowNewMode = true;

constexpr u32 vif_code(VifCode::Kind kind, u16 immediate = 0, u8 num = 0) {
  return (static_cast<u32>(kind) << 24) | (static_cast<u32>(num) << 16) | immediate;
}

constexpr u32 vif_stcycl(u16 cl, u16 wl) {
  return vif_code(VifCode::Kind::STCYCL, cl | (wl << 8));
}

constexpr u32 vif_unpack_v4_32(u8 qwc, u16 address, bool tops) {
  return vif_code(VifCode::Kind::UNPACK_V4_32, address | (tops ? (1 << 15) : 0), qwc);
}

bool is_exact_stcycl_unpack(const DmaTransfer& transfer, u8 qwc, u16 address, bool tops) {
  return transfer.size_bytes == qwc * 16 && transfer.vif0() == vif_stcycl(4, 4) &&
         transfer.vif1() == vif_unpack_v4_32(qwc, address, tops);
}

bool is_exact_glow_template_1(const DmaTransfer& transfer) {
  constexpr u8 kTemplateQwc = 0x54;
  return transfer.size_bytes == kTemplateQwc * 16 &&
         transfer.vif0() == vif_code(VifCode::Kind::MSCAL, 0) &&
         transfer.vif1() == vif_unpack_v4_32(kTemplateQwc, kGlowTemplate1Address, false);
}

bool is_exact_base_offset(const DmaTransfer& transfer) {
  return transfer.size_bytes == 0 && transfer.vif0() == vif_code(VifCode::Kind::BASE, 0) &&
         transfer.vif1() == vif_code(VifCode::Kind::OFFSET, kGlowBufferOffset);
}

bool is_exact_nop_nop(const DmaTransfer& transfer) {
  return transfer.size_bytes == 0 && transfer.vif0() == 0 && transfer.vif1() == 0;
}

bool is_exact_nop_flushe(const DmaTransfer& transfer) {
  return transfer.size_bytes == 0 && transfer.vif0() == 0 &&
         transfer.vif1() == vif_code(VifCode::Kind::FLUSHE);
}

bool is_exact_glow_call(const DmaTransfer& transfer) {
  return transfer.size_bytes == 0 &&
         transfer.vif0() == vif_code(VifCode::Kind::MSCALF, kGlowProgramAddress) &&
         transfer.vif1() == vif_code(VifCode::Kind::FLUSHE);
}

bool is_exact_direct10(const DmaTransfer& transfer) {
  return transfer.size_bytes == 10 * 16 && transfer.vif0() == 0 &&
         transfer.vif1() == vif_code(VifCode::Kind::DIRECT, 10);
}

// size of sprite-aux-list in GOAL code * SPRITE_MAX_AMOUNT_MULT (as in GL)
constexpr int kMaxDistortSprites = 256 * 12;

// Must match SpriteDistortParams in shaders/sprite.metal.
struct SpriteDistortParams {
  float color[4];
  float height_scale;
  float fb_v_offset;
  float pad[2];
};
static_assert(sizeof(SpriteDistortParams) == 32);

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
  m_distort_frame_data.resize(kMaxDistortSprites);

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

void MetalSpriteRenderer::render(DmaFollower& dma,
                                 MetalSharedRenderState* render_state,
                                 MetalFrameContext& ctx) {
  m_stats = {};
  m_pending_glow_outputs.clear();

  switch (render_state->version) {
    case GameVersion::Jak1:
      render_jak1(dma, render_state, ctx);
      break;
    case GameVersion::Jak2:
      render_jak2(dma, render_state, ctx);
      break;
    default:
      ASSERT_MSG(false, "Metal sprite renderer only supports Jak 1 and Jak 2");
  }
}

/*!
 * Mirror of Sprite3::render_jak1.
 */
void MetalSpriteRenderer::render_jak1(DmaFollower& dma,
                                      MetalSharedRenderState* render_state,
                                      MetalFrameContext& ctx) {
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

  if (!render_normal_path(dma, render_state, ctx)) {
    return;
  }

  // the GL renderer consumes the remainder of the bucket the same way
  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }
}

/*!
 * Mirror of Sprite3::render_jak2 through the normal sprite path. A complete,
 * constants-led glow packet is parsed into pending backend-neutral records.
 */
void MetalSpriteRenderer::render_jak2(DmaFollower& dma,
                                      MetalSharedRenderState* render_state,
                                      MetalFrameContext& ctx) {
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif0() == 0 || data0.vifcode0().kind == VifCode::Kind::MARK);
  ASSERT(data0.vif1() == 0 || data0.vifcode1().kind == VifCode::Kind::NOP);
  ASSERT(data0.size_bytes == 0);

  if (dma.current_tag_offset() == render_state->next_bucket) {
    return;
  }

  if (!render_normal_path(dma, render_state, ctx)) {
    return;
  }

  auto nop_flushe = dma.read_and_advance();
  ASSERT(nop_flushe.vifcode0().kind == VifCode::Kind::NOP);
  ASSERT(nop_flushe.vifcode1().kind == VifCode::Kind::FLUSHE);
  parse_jak2_glow_and_residual(dma, render_state);

  const auto& glow_outputs = pending_glow_outputs();
  m_glow_renderer.draw_force_visible(glow_outputs.empty() ? nullptr : glow_outputs.data(),
                                     glow_outputs.size(), render_state, ctx);
  const auto& glow_stats = m_glow_renderer.stats();
  m_stats.glow_invalid_records = glow_stats.invalid_records;
  m_stats.glow_force_visible_submitted = glow_stats.sprites_submitted;
  m_stats.glow_force_visible_drawn = glow_stats.sprites_drawn;
  m_stats.glow_force_visible_draw_calls = glow_stats.draw_calls;
  m_stats.glow_force_visible_triangles = glow_stats.triangles;
  m_stats.glow_force_visible_missing_textures = glow_stats.missing_textures;
  ASSERT(m_stats.glow_force_visible_drawn <= m_stats.glow_sprites_parsed);
  m_stats.glow_sprites_skipped =
      m_stats.glow_sprites_parsed - m_stats.glow_force_visible_drawn;
  m_stats.draw_calls += glow_stats.draw_calls;
  m_stats.triangles += glow_stats.triangles;
  m_stats.missing_textures += glow_stats.missing_textures;
}

bool MetalSpriteRenderer::render_normal_path(DmaFollower& dma,
                                             MetalSharedRenderState* render_state,
                                             MetalFrameContext& ctx) {
  // some DirectRenderer DMA may come first
  if (render_direct(dma, render_state, ctx)) {
    return false;
  }

  // the distorter: DMA, vertex build and draw, like Sprite3::render_distorter
  distort_dma(render_state->version, dma);
  distort_setup();
  distort_draw(render_state, ctx);

  handle_sprite_frame_setup(render_state->version, dma);
  render_3d(dma);

  render_2d_group0(dma, render_state, ctx);
  flush_sprites(render_state, ctx, false);

  render_fake_shadow(dma);

  render_2d_group1(dma, render_state, ctx);
  flush_sprites(render_state, ctx, true);
  return true;
}

void MetalSpriteRenderer::parse_jak2_glow_and_residual(DmaFollower& dma,
                                                       MetalSharedRenderState* render_state) {
  std::vector<DmaTransfer> packet_transfers;
  std::vector<SpriteGlowOutput> parsed_outputs;
  parsed_outputs.reserve(kMaxGlowRecords);
  int parsed_count = 0;
  int accepted_count = 0;
  int rejected_count = 0;
  std::array<int, static_cast<std::size_t>(SpriteGlowRejectReason::COUNT)> reject_reasons = {};

  auto read_packet_transfer = [&](DmaTransfer* transfer) {
    if (dma.current_tag_offset() == render_state->next_bucket) {
      return false;
    }
    *transfer = dma.read_and_advance();
    packet_transfers.push_back(*transfer);
    return true;
  };

  auto parse_constants_led_packet = [&]() {
    DmaTransfer constants_transfer;
    if (!read_packet_transfer(&constants_transfer) ||
        !is_exact_stcycl_unpack(constants_transfer, sizeof(SpriteGlowConsts) / 16,
                                kGlowConstantsAddress, false)) {
      return false;
    }

    SpriteGlowConsts constants;
    memcpy(&constants, constants_transfer.data, sizeof(constants));

    DmaTransfer transfer;
    if (!read_packet_transfer(&transfer) ||
        !is_exact_stcycl_unpack(transfer, 0x54, kGlowTemplate0Address, false)) {
      return false;
    }
    if (!read_packet_transfer(&transfer) || !is_exact_glow_template_1(transfer)) {
      return false;
    }
    if (!read_packet_transfer(&transfer) || !is_exact_base_offset(transfer)) {
      return false;
    }
    if (!read_packet_transfer(&transfer) || !is_exact_nop_flushe(transfer)) {
      return false;
    }
    if (!read_packet_transfer(&transfer)) {
      return false;
    }

    while (is_exact_nop_nop(transfer)) {
      if (!read_packet_transfer(&transfer)) {
        return false;
      }
    }

    while (transfer.size_bytes == 16) {
      if (!is_exact_stcycl_unpack(transfer, 1, kGlowControlAddress, true)) {
        return false;
      }
      u32 sprite_count = 0;
      memcpy(&sprite_count, transfer.data, sizeof(sprite_count));
      if (sprite_count != 1) {
        return false;
      }
      if (parsed_count == kMaxGlowRecords) {
        return false;
      }

      DmaTransfer vector_transfer;
      DmaTransfer adgif_transfer;
      DmaTransfer call_transfer;
      if (!read_packet_transfer(&vector_transfer) ||
          !is_exact_stcycl_unpack(vector_transfer, 4, kGlowVectorAddress, true) ||
          !read_packet_transfer(&adgif_transfer) ||
          !is_exact_stcycl_unpack(adgif_transfer, 5, kGlowAdgifAddress, true) ||
          !read_packet_transfer(&call_transfer) || !is_exact_glow_call(call_transfer)) {
        return false;
      }

      parsed_count++;
      SpriteGlowOutput output;
      SpriteGlowRejectReason reject_reason = SpriteGlowRejectReason::NONE;
      if (glow_math(&constants, kGlowNewMode, vector_transfer.data, adgif_transfer.data, &output,
                    &reject_reason)) {
        parsed_outputs.push_back(output);
        accepted_count++;
      } else {
        rejected_count++;
        reject_reasons.at(static_cast<std::size_t>(reject_reason))++;
      }

      if (!read_packet_transfer(&transfer)) {
        return false;
      }
      while (is_exact_nop_nop(transfer)) {
        if (!read_packet_transfer(&transfer)) {
          return false;
        }
      }
    }

    return is_exact_nop_flushe(transfer);
  };

  auto drain_remaining = [&]() {
    while (dma.current_tag_offset() != render_state->next_bucket) {
      const auto tag_kind = dma.current_tag().kind;
      const auto transfer = dma.read_and_advance();
      if (is_exact_direct10(transfer) ||
          (tag_kind == DmaTag::Kind::NEXT && transfer.size_bytes == 0)) {
        m_stats.post_glow_residual_transfers++;
        m_stats.post_glow_residual_bytes += transfer.size_bytes;
      } else {
        m_stats.glow_transfers_skipped++;
        m_stats.glow_bytes_skipped += transfer.size_bytes;
      }
      m_stats.unsupported_bytes += transfer.size_bytes;
    }
  };

  const bool parsed_packet =
      dma.current_tag_offset() != render_state->next_bucket && parse_constants_led_packet();
  if (parsed_packet) {
    m_stats.glow_sprites_parsed = parsed_count;
    m_stats.glow_sprites_accepted = accepted_count;
    m_stats.glow_sprites_rejected = rejected_count;
    m_pending_glow_outputs = std::move(parsed_outputs);
    drain_remaining();
  } else {
    for (const auto& transfer : packet_transfers) {
      m_stats.glow_transfers_skipped++;
      m_stats.glow_bytes_skipped += transfer.size_bytes;
      m_stats.unsupported_bytes += transfer.size_bytes;
    }

    drain_remaining();
  }

  m_unsupported_bytes_total += m_stats.unsupported_bytes;

  if (m_stats.glow_sprites_rejected > 0 && !m_warned_rejected_glow_math) {
    for (std::size_t reason = 0; reason < reject_reasons.size(); reason++) {
      if (reject_reasons[reason] > 0) {
        lg::warn("Metal sprite {}: rejected {} Jak 2 glow record(s): {}", m_name,
                 reject_reasons[reason],
                 sprite_glow_reject_reason_name(static_cast<SpriteGlowRejectReason>(reason)));
      }
    }
    m_warned_rejected_glow_math = true;
  }

  if (m_stats.unsupported_bytes > 0 && !m_warned_unsupported_glow) {
    lg::warn("Metal sprite {}: parsed {}/accepted {}/rejected {} Jak 2 glow sprites; left {} "
             "control-led transfers/{} payload bytes and {} residual transfers/{} payload bytes "
             "unsupported ({} payload bytes total, logged once)",
             m_name, m_stats.glow_sprites_parsed, m_stats.glow_sprites_accepted,
             m_stats.glow_sprites_rejected, m_stats.glow_transfers_skipped,
             m_stats.glow_bytes_skipped, m_stats.post_glow_residual_transfers,
             m_stats.post_glow_residual_bytes, m_stats.unsupported_bytes);
    m_warned_unsupported_glow = true;
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
 * Mirror of Sprite3::distort_dma: walks the distorter's DMA,
 * keeping the sine tables and the per-sprite frame data for distort_setup.
 */
void MetalSpriteRenderer::distort_dma(GameVersion version, DmaFollower& dma) {
  u32 expected_zbp = 0;
  u32 expected_th = 0;
  switch (version) {
    case GameVersion::Jak1:
      expected_zbp = 0x1c0;
      expected_th = 8;
      break;
    case GameVersion::Jak2:
      expected_zbp = 0x130;
      expected_th = 9;
      break;
    default:
      ASSERT_NOT_REACHED();
  }

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
  ASSERT(distorter_setup.zbuf.zbp() == expected_zbp);
  ASSERT(distorter_setup.zbuf.zmsk() == true);
  ASSERT(distorter_setup.zbuf.psm() == TextureFormat::PSMZ24);
  ASSERT(distorter_setup.tex0.tbw() == 8);
  ASSERT(distorter_setup.tex0.tw() == 9);
  ASSERT(distorter_setup.tex0.th() == expected_th);
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
  auto tables = dma.read_and_advance();
  unpack_to_stcycl(&m_distort_sine_tables, tables, VifCode::Kind::UNPACK_V4_32, 4, 4, 0x8b * 16,
                   0x160, false, false);
  ASSERT(GsPrim(m_distort_sine_tables.gs_gif_tag.prim()).kind() == GsPrim::Kind::TRI_STRIP);

  // frame data packets
  int sprite_idx = 0;
  m_distort_sprite_count = 0;
  while (looks_like_distort_frame_data(dma)) {
    math::Vector<u32, 4> num_sprites_vec{0, 0, 0, 0};
    do {
      int qwc = dma.current_tag().qwc;
      int dest = dma.current_tag_vifcode1().immediate;
      auto distort_data = dma.read_and_advance();
      if (dest == 511) {
        // VU address 511 specifies the number of sprites
        unpack_to_no_stcycl(&num_sprites_vec, distort_data, VifCode::Kind::UNPACK_V4_32, 16, dest,
                            false, false);
      } else {
        // VU address >= 512 is the actual vertex data
        ASSERT(dest >= 512);
        ASSERT(sprite_idx + (qwc / 3) <= (int)m_distort_frame_data.size());
        unpack_to_no_stcycl(&m_distort_frame_data.at(sprite_idx), distort_data,
                            VifCode::Kind::UNPACK_V4_32, qwc * 16, dest, false, false);
        sprite_idx += qwc / 3;
      }
    } while (looks_like_distort_frame_data(dma));

    ASSERT(dma.current_tag().kind == DmaTag::Kind::CNT);
    ASSERT(dma.current_tag_vifcode0().kind == VifCode::Kind::MSCALF);
    ASSERT(dma.current_tag_vifcode1().kind == VifCode::Kind::FLUSH);
    dma.read_and_advance();

    m_distort_sprite_count += num_sprites_vec.x();
  }

  ASSERT(m_distort_sprite_count <= kMaxDistortSprites);
  m_stats.distort_sprites += m_distort_sprite_count;
}

/*!
 * Mirror of Sprite3::distort_setup (the non-instanced path): expands each
 * sprite through the sine tables into triangle-strip slices, sharing the
 * center vertex and separating sprites with the restart index.
 */
void MetalSpriteRenderer::distort_setup() {
  m_distort_tri_count = 0;
  m_distort_vertices.clear();
  m_distort_indices.clear();

  int sprite_idx = 0;
  int sprites_left = m_distort_sprite_count;

  while (sprites_left != 0) {
    // flag is the 'resolution' of the circle sprite: that many pie slices
    u32 flag = m_distort_frame_data.at(sprite_idx).flag;
    u32 slices_left = flag;

    // flag has a minimum value of 3, which selects the first ientry; the
    // ientry indices carry the +352 start of the entry array in VU memory
    int entry_index = m_distort_sine_tables.ientry[flag - 3].x() - 352;

    SpriteDistortFrameData frame_data = m_distort_frame_data.at(sprite_idx);
    sprite_idx++;

    math::Vector2f vf03 = frame_data.st;
    math::Vector3f vf14 = frame_data.xyz;

    // each slice shares the center vertex
    u32 center_vert_idx = (u32)m_distort_vertices.size();
    m_distort_vertices.push_back({vf14, vf03});

    do {
      math::Vector3f vf06 = m_distort_sine_tables.entry[entry_index++].xyz();
      math::Vector2f vf07 = m_distort_sine_tables.entry[entry_index++].xy();
      math::Vector3f vf08 = m_distort_sine_tables.entry[entry_index + 0].xyz();
      math::Vector2f vf09 = m_distort_sine_tables.entry[entry_index + 1].xy();

      slices_left--;

      math::Vector2f vf11 = (vf07 * frame_data.rgba.z()) + frame_data.st;
      math::Vector2f vf13 = (vf09 * frame_data.rgba.z()) + frame_data.st;
      math::Vector3f vf06_2 = (vf06 * frame_data.rgba.x()) + frame_data.xyz;
      math::Vector2f vf07_2 = (vf07 * frame_data.rgba.x()) + frame_data.st;
      math::Vector3f vf08_2 = (vf08 * frame_data.rgba.x()) + frame_data.xyz;
      math::Vector2f vf09_2 = (vf09 * frame_data.rgba.x()) + frame_data.st;
      math::Vector3f vf10 = (vf06 * frame_data.rgba.y()) + frame_data.xyz;
      math::Vector3f vf12 = (vf08 * frame_data.rgba.y()) + frame_data.xyz;

      m_distort_indices.push_back((u32)m_distort_vertices.size());
      m_distort_vertices.push_back({vf06_2, vf07_2});

      m_distort_indices.push_back((u32)m_distort_vertices.size());
      m_distort_vertices.push_back({vf08_2, vf09_2});

      m_distort_indices.push_back((u32)m_distort_vertices.size());
      m_distort_vertices.push_back({vf10, vf11});

      m_distort_indices.push_back((u32)m_distort_vertices.size());
      m_distort_vertices.push_back({vf12, vf13});

      // the shared center vertex closes the slice
      m_distort_indices.push_back(center_vert_idx);

      m_distort_tri_count += 2;
    } while (slices_left != 0);

    // end of this sprite's strip
    m_distort_indices.push_back(UINT32_MAX);

    sprites_left--;
  }
}

/*!
 * Mirror of Sprite3::distort_draw + distort_draw_common. GL blits the frame so
 * far into a texture; here the game pass is split around a blit into
 * m_distort_snapshot, and the sprites are drawn sampling it. The GS state is
 * the one distort_dma's asserts pin: standard alpha blend, no depth write,
 * linear filtering, clamped sampling; the depth test is the sprite default
 * (GEQUAL), as in the GL mode bookkeeping.
 */
void MetalSpriteRenderer::distort_draw(MetalSharedRenderState* render_state,
                                       MetalFrameContext& ctx) {
  if (m_distort_tri_count == 0) {
    return;
  }
  if (!ctx.cmds || !ctx.game_color || !ctx.game_depth) {
    // a context that cannot split the pass (not the game's frame path)
    return;
  }

  const u32 vtx_bytes = (u32)(m_distort_vertices.size() * sizeof(SpriteDistortVertex));
  const u32 idx_bytes = (u32)(m_distort_indices.size() * sizeof(u32));
  if (vtx_bytes > 2 * 1024 * 1024 || idx_bytes > 2 * 1024 * 1024) {
    // more distort data than one stream page holds; never seen in practice
    if (!m_warned_distort_overflow) {
      lg::warn("Metal sprite {}: distort data too large ({} verts), skipping the effect",
               m_name, m_distort_vertices.size());
      m_warned_distort_overflow = true;
    }
    return;
  }

  // the snapshot must match the game target
  if (!m_distort_snapshot || m_distort_snapshot.width != ctx.game_color.width ||
      m_distort_snapshot.height != ctx.game_color.height ||
      m_distort_snapshot.pixelFormat != ctx.game_color.pixelFormat) {
    auto* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:ctx.game_color.pixelFormat
                                                           width:ctx.game_color.width
                                                          height:ctx.game_color.height
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModePrivate;
    m_distort_snapshot = [ctx.game_color.device newTextureWithDescriptor:desc];
  }

  // GL's glBlitFramebuffer into the distort fbo
  ctx.resume_pass_with_framebuffer_copy(m_distort_snapshot);

  id<MTLBuffer> vbuf;
  u32 voffset;
  memcpy(ctx.stream->alloc(vtx_bytes, &vbuf, &voffset), m_distort_vertices.data(), vtx_bytes);
  id<MTLBuffer> ibuf;
  u32 ioffset;
  memcpy(ctx.stream->alloc(idx_bytes, &ibuf, &ioffset), m_distort_indices.data(), idx_bytes);

  MetalPsoKey pso_key;
  pso_key.shader = MetalShaderId::SPRITE_DISTORT;
  pso_key.color_format = ctx.color_format;
  pso_key.depth_format = ctx.depth_format;
  pso_key.blend_enable = true;
  pso_key.blend_src_rgb = MTLBlendFactorSourceAlpha;
  pso_key.blend_dst_rgb = MTLBlendFactorOneMinusSourceAlpha;
  pso_key.blend_src_alpha = MTLBlendFactorOne;
  pso_key.blend_dst_alpha = MTLBlendFactorZero;

  MetalDepthStencilKey depth_key;
  depth_key.depth_test = true;
  depth_key.compare = MTLCompareFunctionGreaterEqual;
  depth_key.depth_write = false;

  MetalSamplerKey sampler_key;
  sampler_key.min_filter = MTLSamplerMinMagFilterLinear;
  sampler_key.mag_filter = MTLSamplerMinMagFilterLinear;
  sampler_key.wrap_s = MTLSamplerAddressModeClampToEdge;
  sampler_key.wrap_t = MTLSamplerAddressModeClampToEdge;

  SpriteDistortParams params = {};
  for (int i = 0; i < 4; i++) {
    params.color[i] = (float)m_distort_sine_tables.color[i] / 255.0f;
  }
  params.height_scale = metal_height_scale(render_state->version);
  params.fb_v_offset = (1.f - 1.f / metal_scissor_adjust(render_state->version)) / 2.f;

  id<MTLRenderCommandEncoder> enc = ctx.enc;
  id<MTLRenderPipelineState> pso = ctx.pso_cache->get_pipeline(pso_key);
  ASSERT(pso);
  [enc setRenderPipelineState:pso];
  [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(depth_key)];
  [enc setVertexBuffer:vbuf offset:voffset atIndex:0];
  [enc setVertexBytes:&params length:sizeof(params) atIndex:1];
  [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
  [enc setFragmentTexture:m_distort_snapshot atIndex:0];
  [enc setFragmentSamplerState:ctx.sampler_cache->get(sampler_key) atIndex:0];
  [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                  indexCount:(NSUInteger)m_distort_indices.size()
                   indexType:MTLIndexTypeUInt32
                 indexBuffer:ibuf
           indexBufferOffset:ioffset];

  ctx.draw_calls++;
  ctx.triangles += m_distort_tri_count;
  m_stats.draw_calls++;
  m_stats.triangles += m_distort_tri_count;
}

/*!
 * Mirror of Sprite3::handle_sprite_frame_setup.
 */
void MetalSpriteRenderer::handle_sprite_frame_setup(GameVersion version, DmaFollower& dma) {
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
  ASSERT(frame_data.vifcode0().kind == VifCode::Kind::STCYCL);
  VifCodeStcycl frame_data_stcycl(frame_data.vifcode0());
  ASSERT(frame_data_stcycl.cl == 4);
  ASSERT(frame_data_stcycl.wl == 4);
  ASSERT(frame_data.vifcode1().kind == VifCode::Kind::UNPACK_V4_32);
  VifCodeUnpack frame_data_unpack(frame_data.vifcode1());
  ASSERT(frame_data_unpack.addr_qw == SpriteDataMem::FrameData);
  ASSERT(frame_data_unpack.use_tops_flag == false);
  switch (version) {
    case GameVersion::Jak1: {
      ASSERT(frame_data.size_bytes == (int)sizeof(SpriteFrameDataJak1));
      SpriteFrameDataJak1 jak1_data;
      memcpy(&jak1_data, frame_data.data, sizeof(SpriteFrameDataJak1));
      m_frame_data.from_jak1(jak1_data);
    } break;
    case GameVersion::Jak2:
      ASSERT(frame_data.size_bytes == (int)sizeof(SpriteFrameData));
      memcpy(&m_frame_data, frame_data.data, sizeof(SpriteFrameData));
      break;
    default:
      ASSERT_NOT_REACHED();
  }

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
    switch (render_state->version) {
      case GameVersion::Jak1:
        ASSERT(run.vifcode1().immediate == SpriteProgMem::Sprites2dHud_Jak1);
        break;
      case GameVersion::Jak2:
        ASSERT(run.vifcode1().immediate == SpriteProgMem::Sprites2dHud_Jak2);
        break;
      default:
        ASSERT_NOT_REACHED();
    }

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

void MetalSpriteRenderer::handle_zbuf(GameVersion version, u64 val) {
  GsZbuf x(val);
  ASSERT(x.psm() == TextureFormat::PSMZ24);
  ASSERT(x.zbp() == kNormalZbp[version]);
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

    if (render_state->version > GameVersion::Jak1 &&
        m_vec_data_2d[sprite_idx].matrix() == -1) {
      m_stats.glow_marked_sprites++;
      continue;
    }

    auto& adgif = m_adgif[sprite_idx];
    handle_tex0(adgif.tex0_data);
    handle_tex1(adgif.tex1_data);
    if (GsRegisterAddress(adgif.clamp_addr) == GsRegisterAddress::ZBUF_1) {
      handle_zbuf(render_state->version, adgif.clamp_data);
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
    m_stats.normal_sprites_submitted++;
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
  vs_params.height_scale = metal_height_scale(render_state->version);
  vs_params.scissor_adjust = metal_scissor_adjust(render_state->version);

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
