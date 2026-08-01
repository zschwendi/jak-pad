#include "metal_generic2.h"

#include <algorithm>

#include "common/log/log.h"

#include "game/graphics/texture/TexturePool.h"

namespace {

constexpr float kGameHeightJak1 = 448.f;

// Must match GenericVsParams in shaders/generic.metal.
struct GenericVsParams {
  float scale[4];
  float hvdf_offset[4];
  float full_matrix[4][4];
  float fog_constants[4];
  float mat_23;
  float mat_32;
  float mat_33;
  u32 use_full_matrix;
  u32 warp_sample_mode;
  float height_scale;
  float scissor_adjust;
  float warp_off;
};
static_assert(sizeof(GenericVsParams) == 144);

// Must match GenericFsParams in shaders/generic.metal.
struct GenericFsParams {
  float fog_color[4];
  float alpha_reject;
  float color_mult;
  int gfx_hack_no_tex;
  u32 warp_sample_mode;
};
static_assert(sizeof(GenericFsParams) == 32);

bool is_nop_vif(const u8* data) {
  u32 tag0_data;
  memcpy(&tag0_data, data, 4);
  return VifCode(tag0_data).kind == VifCode::Kind::NOP;
}

bool is_nop_or_flushe_vif(const u8* data) {
  u32 tag0_data;
  memcpy(&tag0_data, data, 4);
  auto k = VifCode(tag0_data).kind;
  return k == VifCode::Kind::NOP || k == VifCode::Kind::FLUSHE;
}

u32 unpack_vtx_positions(MetalGeneric2::Vertex* vtx, const u8* data, u32 vtx_count) {
  for (u32 i = 0; i < vtx_count; i++) {
    memcpy(vtx[i].xyz.data(), data + (i * 12), 12);
  }
  return vtx_count * 12;
}

u32 unpack_vertex_colors(MetalGeneric2::Vertex* vtx, const u8* data, u32 vtx_count) {
  for (u32 i = 0; i < vtx_count; i++) {
    memcpy(vtx[i].rgba.data(), data + (i * 4), 4);
  }
  return vtx_count * 4;
}

u32 unpack_vtx_tcs(MetalGeneric2::Vertex* vtx, const u8* data, u32 vtx_count) {
  for (u32 i = 0; i < vtx_count; i++) {
    s16 s, t;
    memcpy(&s, data + (i * 4), 2);
    memcpy(&t, data + (i * 4) + 2, 2);
    s16 s_masked = s & (s16)0xfffe;
    vtx[i].st[0] = s_masked;
    vtx[i].st[1] = t;
    vtx[i].adc = s_masked == s;
  }
  return vtx_count * 4;
}

}  // namespace

void MetalGeneric2::Stats::add(const Stats& o) {
  fragments += o.fragments;
  vertices += o.vertices;
  adgifs += o.adgifs;
  draw_buckets += o.draw_buckets;
  draw_calls += o.draw_calls;
  triangles += o.triangles;
  missing_textures += o.missing_textures;
  unsupported_blends += o.unsupported_blends;
  unexpected_dma += o.unexpected_dma;
  overflow += o.overflow;
}

MetalGeneric2::MetalGeneric2(u32 num_verts, u32 num_frags, u32 num_adgif, u32 num_buckets) {
  m_verts.resize(num_verts);
  m_fragments.resize(num_frags);
  m_adgifs.resize(num_adgif);
  m_buckets.resize(num_buckets);
  m_indices.resize(num_verts * 3);
}

bool MetalGeneric2::expect(bool condition, const char* what) {
  if (condition) {
    return true;
  }
  m_failed = true;
  if (m_stats) {
    m_stats->unexpected_dma++;
  }
  if (!m_logged[what]) {
    m_logged[what] = true;
    lg::warn("Metal generic2: expected {}; the bucket is skipped (logged once)", what);
  }
  return false;
}

MetalGeneric2::Fragment* MetalGeneric2::next_frag() {
  if (m_next_free_frag >= m_fragments.size()) {
    m_failed = true;
    if (m_stats) {
      m_stats->overflow++;
    }
    return nullptr;
  }
  return &m_fragments[m_next_free_frag++];
}

MetalGeneric2::Adgif* MetalGeneric2::next_adgif() {
  if (m_next_free_adgif >= m_adgifs.size()) {
    m_failed = true;
    if (m_stats) {
      m_stats->overflow++;
    }
    return nullptr;
  }
  return &m_adgifs[m_next_free_adgif++];
}

bool MetalGeneric2::alloc_vtx(u32 count) {
  if ((u64)m_next_free_vert + count >= m_verts.size()) {
    m_failed = true;
    if (m_stats) {
      m_stats->overflow++;
    }
    return false;
  }
  m_next_free_vert += count;
  return true;
}

void MetalGeneric2::reset_buffers() {
  m_next_free_frag = 0;
  m_next_free_vert = 0;
  m_next_free_adgif = 0;
  m_next_free_bucket = 0;
  m_next_free_idx = 0;
}

// ---------------------------------------------------------------------------
// DMA - mirror of Generic2_DMA.cpp, Jak 1 path
// ---------------------------------------------------------------------------

bool MetalGeneric2::check_for_end_of_generic_data(DmaFollower& dma, u32 next_bucket) {
  while (dma.current_tag().qwc == 0 && dma.current_tag_vifcode0().kind == VifCode::Kind::NOP &&
         dma.current_tag_vifcode1().kind == VifCode::Kind::NOP) {
    // this "CALL" tag is inserted by the engine to reset the GS at the end of
    // the bucket; skipping it and its 4 tags lands on the next bucket.
    if (dma.current_tag().kind == DmaTag::Kind::CALL) {
      for (int i = 0; i < 4; i++) {
        dma.read_and_advance();
      }
      expect(dma.current_tag_offset() == next_bucket, "the GS-reset CALL to end the bucket");
      return true;
    }
    dma.read_and_advance();
  }
  return false;
}

bool MetalGeneric2::handle_bucket_setup_dma(DmaFollower& dma, u32 next_bucket) {
  // if the engine did not run the generic setup function, the bucket ends here
  if (check_for_end_of_generic_data(dma, next_bucket)) {
    return true;
  }

  // setup packet 1: GS settings. Only zmsk changes.
  auto test_and_zbuf = dma.read_and_advance();
  if (!expect(test_and_zbuf.size_bytes == 48, "a 48-byte test/zbuf setup packet")) {
    return true;
  }
  u64 zbuf_val;
  memcpy(&zbuf_val, test_and_zbuf.data + 32, 8);
  m_drawing_config.zmsk = GsZbuf(zbuf_val).zmsk();

  // setup packet 2: the constants that normally go to VU1 data memory
  auto constants = dma.read_and_advance();
  if (!expect(constants.size_bytes == 160 &&
                  constants.vifcode0().kind == VifCode::Kind::STCYCL &&
                  constants.vifcode1().kind == VifCode::Kind::UNPACK_V4_32,
              "a 160-byte VU constants unpack")) {
    return true;
  }
  memcpy(&m_drawing_config.pfog0, constants.data + 0, 4);
  memcpy(&m_drawing_config.fog_min, constants.data + 4, 4);
  memcpy(&m_drawing_config.fog_max, constants.data + 8, 4);
  memcpy(m_drawing_config.hvdf_offset.data(), constants.data + 48, 16);

  auto vu_setup = dma.read_and_advance();
  if (!expect(vu_setup.size_bytes == 32, "a 32-byte VU register setup")) {
    return true;
  }

  // if generic rendered nothing in this bucket this frame, it ends here
  return check_for_end_of_generic_data(dma, next_bucket);
}

u32 MetalGeneric2::handle_fragments_after_unpack_v4_32(const u8* data,
                                                       u32 off,
                                                       u32 first_unpack_bytes,
                                                       u32 end_of_vif,
                                                       Fragment* frag,
                                                       bool loop) {
  // note: the game relies on _something_ aligning this
  u32 off_aligned = (off + 15) & ~15;
  // each header is 7 qw plus at least 5 qw for a single adgif
  if (!expect(first_unpack_bytes >= FRAG_HEADER_SIZE + sizeof(AdGifData) &&
                  off_aligned + first_unpack_bytes <= end_of_vif,
              "a fragment header with at least one adgif")) {
    return end_of_vif;
  }
  memcpy(frag->header, data + off_aligned, FRAG_HEADER_SIZE);

  u32 adgif_bytes = (first_unpack_bytes - FRAG_HEADER_SIZE);
  u32 adgifs = adgif_bytes / sizeof(AdGifData);
  frag->adgif_idx = m_next_free_adgif;
  frag->adgif_count = adgifs;
  if (!expect(adgifs > 0 && adgif_bytes == adgifs * sizeof(AdGifData),
              "a whole number of adgifs in the fragment header")) {
    return end_of_vif;
  }
  for (u32 i = 0; i < adgifs; i++) {
    auto* add = next_adgif();
    if (!add) {
      return end_of_vif;
    }
    memcpy(&add->data, data + off_aligned + FRAG_HEADER_SIZE + (i * sizeof(AdGifData)),
           sizeof(AdGifData));
  }

  off += first_unpack_bytes;
  if (!expect(off < end_of_vif, "vertex data after the fragment header")) {
    return end_of_vif;
  }

  // the next thing is the vertex positions
  while (off + 4 <= end_of_vif && is_nop_vif(data + off)) {
    off += 4;
  }
  if (!expect(off + 4 <= end_of_vif, "the vertex STCYCL")) {
    return end_of_vif;
  }
  u32 stcycl_tag_data;
  memcpy(&stcycl_tag_data, data + off, 4);
  off += 4;
  VifCode stcycl_tag(stcycl_tag_data);
  if (!expect(stcycl_tag.kind == VifCode::Kind::STCYCL && stcycl_tag.immediate == 0x103,
              "STCYCL 0x103 before the vertex positions")) {
    return end_of_vif;
  }

  if (!expect(off + 4 <= end_of_vif, "the vertex position unpack")) {
    return end_of_vif;
  }
  u32 vtx_pos_unpack_tag_data;
  memcpy(&vtx_pos_unpack_tag_data, data + off, 4);
  VifCode vtx_pos_unpack_tag(vtx_pos_unpack_tag_data);

  if (vtx_pos_unpack_tag.kind == VifCode::Kind::UNPACK_V4_8) {
    if (!expect(loop, "a continued fragment for a V4_8 position unpack")) {
      return end_of_vif;
    }
  } else {
    if (!expect(!loop && vtx_pos_unpack_tag.kind == VifCode::Kind::UNPACK_V3_32,
                "a V3_32 vertex position unpack")) {
      return end_of_vif;
    }
    off += 4;
    frag->vtx_idx = m_next_free_vert;
    frag->vtx_count = vtx_pos_unpack_tag.num;
    if (!alloc_vtx(frag->vtx_count)) {
      return end_of_vif;
    }
    if (!expect(off + frag->vtx_count * 12 <= end_of_vif, "the vertex positions to fit")) {
      return end_of_vif;
    }
    off += unpack_vtx_positions(&m_verts[frag->vtx_idx], data + off, frag->vtx_count);

    while (off + 4 <= end_of_vif && is_nop_vif(data + off)) {
      off += 4;
    }
    if (!expect(off < end_of_vif, "data after the vertex positions")) {
      return end_of_vif;
    }
  }

  // next, vertex colors
  if (!expect(off + 4 <= end_of_vif, "the vertex color unpack")) {
    return end_of_vif;
  }
  u32 unpack_vtx_color_tag_data;
  memcpy(&unpack_vtx_color_tag_data, data + off, 4);
  off += 4;
  VifCode unpack_vtx_color_tag(unpack_vtx_color_tag_data);
  if (!expect(unpack_vtx_color_tag.kind == VifCode::Kind::UNPACK_V4_8,
              "a V4_8 vertex color unpack")) {
    return end_of_vif;
  }
  if (loop) {
    frag->vtx_idx = m_next_free_vert;
    frag->vtx_count = unpack_vtx_color_tag.num;
    if (!alloc_vtx(frag->vtx_count)) {
      return end_of_vif;
    }
  } else if (!expect(unpack_vtx_color_tag.num == frag->vtx_count,
                     "one vertex color per position")) {
    return end_of_vif;
  }
  if (!expect(off + frag->vtx_count * 4 <= end_of_vif, "the vertex colors to fit")) {
    return end_of_vif;
  }
  off += unpack_vertex_colors(&m_verts[frag->vtx_idx], data + off, frag->vtx_count);

  while (off + 4 <= end_of_vif && is_nop_vif(data + off)) {
    off += 4;
  }
  if (!expect(off < end_of_vif, "data after the vertex colors")) {
    return end_of_vif;
  }

  // next, vertex texture coordinates
  u32 unpack_vtx_tc_tag_data;
  memcpy(&unpack_vtx_tc_tag_data, data + off, 4);
  off += 4;
  VifCode unpack_vtx_tc_tag(unpack_vtx_tc_tag_data);
  if (!expect(unpack_vtx_tc_tag.kind == VifCode::Kind::UNPACK_V2_16 &&
                  unpack_vtx_tc_tag.num == frag->vtx_count,
              "one V2_16 texture coordinate per vertex")) {
    return end_of_vif;
  }
  if (!expect(off + frag->vtx_count * 4 <= end_of_vif, "the texture coordinates to fit")) {
    return end_of_vif;
  }
  off += unpack_vtx_tcs(&m_verts[frag->vtx_idx], data + off, frag->vtx_count);

  if (off == end_of_vif) {
    return off;
  }

  while (off + 4 <= end_of_vif && is_nop_vif(data + off)) {
    off += 4;
  }
  if (!expect(off + 4 <= end_of_vif, "the MSCAL that runs the fragment")) {
    return end_of_vif;
  }

  u32 stcycl_reset_data;
  memcpy(&stcycl_reset_data, data + off, 4);
  off += 4;
  VifCode stcycl_reset(stcycl_reset_data);
  if (stcycl_reset.kind == VifCode::Kind::STCYCL) {
    while (off + 4 <= end_of_vif && is_nop_vif(data + off)) {
      off += 4;
    }
    if (!expect(off + 4 <= end_of_vif, "the MSCAL after the STCYCL reset")) {
      return end_of_vif;
    }
    u32 mscal_data;
    memcpy(&mscal_data, data + off, 4);
    off += 4;
    VifCode mscal(mscal_data);
    if (!expect(mscal.kind == VifCode::Kind::MSCAL, "an MSCAL after the STCYCL reset")) {
      return end_of_vif;
    }
    frag->mscal_addr = mscal.immediate;
  } else {
    if (!expect(stcycl_reset.kind == VifCode::Kind::MSCAL, "an MSCAL or STCYCL reset")) {
      return end_of_vif;
    }
    frag->mscal_addr = stcycl_reset.immediate;

    while (off + 4 <= end_of_vif && is_nop_vif(data + off)) {
      off += 4;
    }
    if (!expect(off + 4 <= end_of_vif, "the STCYCL after the MSCAL")) {
      return end_of_vif;
    }
    u32 stcycl_data;
    memcpy(&stcycl_data, data + off, 4);
    off += 4;
    VifCode stcycl(stcycl_data);
    if (!expect(stcycl.kind == VifCode::Kind::STCYCL, "an STCYCL after the MSCAL")) {
      return end_of_vif;
    }
  }

  while (off + 4 <= end_of_vif && is_nop_or_flushe_vif(data + off)) {
    off += 4;
  }
  return off;
}

void MetalGeneric2::process_dma_jak1(DmaFollower& dma, u32 next_bucket) {
  reset_buffers();

  if (handle_bucket_setup_dma(dma, next_bucket)) {
    return;
  }

  // Loop over "fragments": a series of uploads followed by an MSCAL that runs
  // the VU program which transforms vertices and sends them to the GS.
  Fragment* continued_fragment = nullptr;

  while (dma.current_tag_offset() != next_bucket) {
    if (m_failed) {
      return;
    }
    if (continued_fragment) {
      auto continue_vif_transfer = dma.read_and_advance();
      auto up = continue_vif_transfer.vifcode1();
      if (!expect(continue_vif_transfer.vifcode0().kind == VifCode::Kind::NOP &&
                      up.kind == VifCode::Kind::UNPACK_V3_32 &&
                      continue_vif_transfer.size_bytes * 4 / 48 == up.num &&
                      up.num == continued_fragment->vtx_count,
                  "the continued fragment's V3_32 position unpack")) {
        return;
      }
      unpack_vtx_positions(&m_verts[continued_fragment->vtx_idx], continue_vif_transfer.data,
                           continued_fragment->vtx_count);
      continued_fragment = nullptr;
      auto call = dma.read_and_advance();
      if (!expect(call.size_bytes == 0 && call.vifcode1().kind == VifCode::Kind::MSCAL,
                  "the MSCAL after a continued fragment")) {
        return;
      }
      if (check_for_end_of_generic_data(dma, next_bucket)) {
        return;
      }
    } else {
      auto vif_transfer = dma.read_and_advance();
      auto v1 = vif_transfer.vifcode1();
      if (!expect(vif_transfer.vifcode0().kind == VifCode::Kind::STCYCL &&
                      v1.kind == VifCode::Kind::UNPACK_V4_32,
                  "a fragment's STCYCL + V4_32 header unpack")) {
        return;
      }
      u32 unpack_bytes = v1.num * 16;
      auto* frag = next_frag();
      if (!frag) {
        return;
      }
      u32 off = handle_fragments_after_unpack_v4_32(vif_transfer.data, 0, unpack_bytes,
                                                    vif_transfer.size_bytes, frag, false);
      if (m_failed) {
        return;
      }

      if (check_for_end_of_generic_data(dma, next_bucket)) {
        return;
      }

      if (off < vif_transfer.size_bytes) {
        u32 stcycl_reset;
        memcpy(&stcycl_reset, vif_transfer.data + off, 4);
        if (!expect(VifCode(stcycl_reset).kind == VifCode::Kind::STCYCL,
                    "an STCYCL before a second fragment in one transfer")) {
          return;
        }
        off += 4;
        u32 next;
        memcpy(&next, vif_transfer.data + off, 4);
        VifCode next_unpack(next);
        if (!expect(next_unpack.kind == VifCode::Kind::UNPACK_V4_32,
                    "a V4_32 header unpack for the second fragment")) {
          return;
        }
        auto* continue_frag = next_frag();
        if (!continue_frag) {
          return;
        }
        off = handle_fragments_after_unpack_v4_32(vif_transfer.data, off, next_unpack.num * 16,
                                                  vif_transfer.size_bytes, continue_frag, true);
        continued_fragment = continue_frag;
        if (!expect(off == vif_transfer.size_bytes,
                    "the second fragment to end the transfer")) {
          return;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Build - mirror of Generic2_Build.cpp
// ---------------------------------------------------------------------------

void MetalGeneric2::setup_draws(bool enable_at, bool default_fog) {
  if (m_next_free_frag == 0) {
    return;
  }
  m_gs = GsState();
  link_adgifs_back_to_frags();
  process_matrices();
  determine_draw_modes(enable_at, default_fog);
  draws_to_buckets();
  final_vertex_update();
  build_index_buffer();
}

void MetalGeneric2::determine_draw_modes(bool enable_at, bool default_fog) {
  DrawMode current_mode;
  current_mode.set_at(enable_at);
  current_mode.set_alpha_test(DrawMode::AlphaTest::GEQUAL);
  current_mode.set_aref(0x26);
  current_mode.set_alpha_fail(GsTest::AlphaFail::FB_ONLY);
  current_mode.set_zt(true);
  current_mode.set_depth_test(GsTest::ZTest::GEQUAL);
  current_mode.set_depth_write_enable(!m_drawing_config.zmsk);
  current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_SRC_SRC_SRC);
  m_gs.set_fog_flag(default_fog);

  u32 tbp = -1;
  GsTex0 tex0;
  tex0.data = UINT64_MAX;

  for (u32 i = 0; i < m_next_free_adgif; i++) {
    auto& ad = m_adgifs[i].data;
    auto& frag = m_fragments[m_adgifs[i].frag];
    m_adgifs[i].uses_hud = frag.uses_hud;

    // the header's giftag may set fogging through pre
    GifTag tag(frag.header + (4 * 16));
    if (tag.pre()) {
      GsPrim prim(tag.prim());
      m_gs.set_fog_flag(prim.fge());
    }

    // ADGIF 0
    if (!expect((u8)ad.tex0_addr == (u32)GsRegisterAddress::TEX0_1, "TEX0 as the first adgif")) {
      return;
    }
    if (ad.tex0_data != tex0.data) {
      tex0.data = ad.tex0_data;
      GsTex0 reg(ad.tex0_data);
      tbp = reg.tbp0();
      if (reg.psm() == GsTex0::PSM::PSMT4HH) {
        tbp |= 0x8000;
      }
      current_mode.set_tcc(reg.tcc());
      m_gs.set_tcc_flag(reg.tcc());
      bool decal = reg.tfx() == GsTex0::TextureFunction::DECAL;
      current_mode.set_decal(decal);
      m_gs.set_decal_flag(decal);
      if (!expect(reg.tfx() == GsTex0::TextureFunction::DECAL ||
                      reg.tfx() == GsTex0::TextureFunction::MODULATE,
                  "a DECAL or MODULATE texture function")) {
        return;
      }
    }

    // ADGIF 1
    if (!expect((u8)ad.tex1_addr == (u32)GsRegisterAddress::TEX1_1, "TEX1 as the second adgif")) {
      return;
    }
    {
      GsTex1 reg(ad.tex1_data);
      current_mode.set_filt_enable(reg.mmag());
    }

    // ADGIF 2 / 3
    if (!expect((u8)ad.mip_addr == (u32)GsRegisterAddress::MIPTBP1_1 &&
                    (u8)ad.clamp_addr == (u32)GsRegisterAddress::CLAMP_1,
                "MIPTBP1 and CLAMP as the third and fourth adgifs")) {
      return;
    }
    {
      bool clamp_s = ad.clamp_data & 0b001;
      bool clamp_t = ad.clamp_data & 0b100;
      current_mode.set_clamp_s_enable(clamp_s);
      current_mode.set_clamp_t_enable(clamp_t);
    }

    std::optional<u64> final_alpha;

    // ADGIF 4
    if ((u8)ad.alpha_addr == (u32)GsRegisterAddress::ALPHA_1) {
      final_alpha = ad.alpha_data;
    } else if (!expect((u8)ad.alpha_addr == (u32)GsRegisterAddress::MIPTBP2_1,
                       "ALPHA or MIPTBP2 as the fifth adgif")) {
      return;
    }

    u64 bonus_adgif_data[4];
    memcpy(bonus_adgif_data, frag.header + (5 * 16), 4 * sizeof(u64));

    std::optional<u64> final_test;
    if ((u8)bonus_adgif_data[1] == (u8)(GsRegisterAddress::ALPHA_1)) {
      final_alpha = bonus_adgif_data[0];
      if (!expect((u8)bonus_adgif_data[3] == (u8)(GsRegisterAddress::TEST_1),
                  "TEST after the bonus ALPHA")) {
        return;
      }
      final_test = bonus_adgif_data[2];
    } else {
      // ADGIF 5
      if ((u8)bonus_adgif_data[1] == (u8)(GsRegisterAddress::TEST_1)) {
        final_test = bonus_adgif_data[0];
      }
      // ADGIF 6
      if ((u8)bonus_adgif_data[3] == (u8)(GsRegisterAddress::ALPHA_1)) {
        final_alpha = bonus_adgif_data[2];
      } else if ((u8)bonus_adgif_data[3] == (u8)(GsRegisterAddress::TEST_1)) {
        final_test = bonus_adgif_data[2];
      }
    }

    if (final_alpha) {
      GsAlpha reg(*final_alpha);
      if (m_gs.gs_alpha != reg) {
        m_gs.gs_alpha = reg;
        auto a = reg.a_mode();
        auto b = reg.b_mode();
        auto c = reg.c_mode();
        auto d = reg.d_mode();
        if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::DEST &&
            c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::DEST) {
          current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
        } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::ZERO_OR_FIXED &&
                   c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::DEST) {
          current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_SRC_DST);
        } else if (a == GsAlpha::BlendMode::ZERO_OR_FIXED && b == GsAlpha::BlendMode::SOURCE &&
                   c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::DEST) {
          current_mode.set_alpha_blend(DrawMode::AlphaBlend::ZERO_SRC_SRC_DST);
        } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::DEST &&
                   c == GsAlpha::BlendMode::ZERO_OR_FIXED && d == GsAlpha::BlendMode::DEST) {
          current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_FIX_DST);
        } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::SOURCE &&
                   c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::SOURCE) {
          current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_SRC_SRC_SRC);
        } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::ZERO_OR_FIXED &&
                   c == GsAlpha::BlendMode::DEST && d == GsAlpha::BlendMode::DEST) {
          current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_DST_DST);
        } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::ZERO_OR_FIXED &&
                   c == GsAlpha::BlendMode::ZERO_OR_FIXED && d == GsAlpha::BlendMode::DEST) {
          current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_FIX_DST);
        } else {
          // the GL renderer prints and carries on with the previous blend
          if (m_stats) {
            m_stats->unsupported_blends++;
          }
          if (!m_logged["unsupported blend"]) {
            m_logged["unsupported blend"] = true;
            lg::warn("Metal generic2: unsupported blend a {} b {} c {} d {} (logged once)", (int)a,
                     (int)b, (int)c, (int)d);
          }
        }
      }
    }

    if (final_test) {
      GsTest reg(*final_test);
      current_mode.set_at(reg.alpha_test_enable());
      if (reg.alpha_test_enable()) {
        switch (reg.alpha_test()) {
          case GsTest::AlphaTest::NEVER:
            current_mode.set_alpha_test(DrawMode::AlphaTest::NEVER);
            break;
          case GsTest::AlphaTest::ALWAYS:
            current_mode.set_alpha_test(DrawMode::AlphaTest::ALWAYS);
            break;
          case GsTest::AlphaTest::GEQUAL:
            current_mode.set_alpha_test(DrawMode::AlphaTest::GEQUAL);
            break;
          default:
            if (!expect(false, "a NEVER, ALWAYS or GEQUAL alpha test")) {
              return;
            }
        }
      }

      current_mode.set_aref(reg.aref());
      current_mode.set_alpha_fail(reg.afail());
      current_mode.set_zt(reg.zte());
      current_mode.set_depth_test(reg.ztest());

      // light-trail's odd way of disabling z writes
      if (current_mode.get_alpha_fail() == GsTest::AlphaFail::FB_ONLY &&
          current_mode.get_aref() == 0x80 &&
          current_mode.get_alpha_test() == DrawMode::AlphaTest::GEQUAL) {
        current_mode.set_alpha_test(DrawMode::AlphaTest::ALWAYS);
        current_mode.disable_depth_write();
      }

      // another way to disable z-writing
      if (current_mode.get_alpha_test() == DrawMode::AlphaTest::NEVER &&
          current_mode.get_alpha_fail() == GsTest::AlphaFail::FB_ONLY) {
        current_mode.set_alpha_test(DrawMode::AlphaTest::ALWAYS);
        current_mode.disable_depth_write();
      }
    }

    m_adgifs[i].mode = current_mode;
    m_adgifs[i].vtx_flags = m_gs.vertex_flags;
    m_adgifs[i].tbp = tbp;
    m_adgifs[i].fix = m_gs.gs_alpha.fix();
  }
}

void MetalGeneric2::link_adgifs_back_to_frags() {
  for (u32 i = 0; i < m_next_free_frag; i++) {
    auto& frag = m_fragments[i];
    for (u32 j = 0; j < frag.adgif_count; j++) {
      auto& ad = m_adgifs[frag.adgif_idx + j];
      ad.vtx_count = (ad.data.tex1_addr >> 32) & 0xfff;  // drop the eop flag
      ad.vtx_idx = frag.vtx_idx + ((ad.data.tex0_addr >> 32) & 0xffff) / 3;
      if (!expect(ad.vtx_count + ad.vtx_idx <= frag.vtx_count + frag.vtx_idx,
                  "an adgif's vertices inside its fragment")) {
        return;
      }
      ad.frag = i;
    }
  }
}

void MetalGeneric2::draws_to_buckets() {
  std::unordered_map<u64, u32> draw_key_to_bucket;
  for (u32 i = 0; i < m_next_free_adgif; i++) {
    auto& ad = m_adgifs[i];
    if (ad.uses_hud) {
      // hud draws each get their own bucket
      if (m_next_free_bucket >= m_buckets.size()) {
        if (m_stats) {
          m_stats->overflow++;
        }
        return;
      }
      u32 bucket_idx = m_next_free_bucket++;
      draw_key_to_bucket[ad.key()] = bucket_idx;
      auto& bucket = m_buckets[bucket_idx];
      bucket.tbp = ad.tbp;
      bucket.mode = ad.mode;
      bucket.start = i;
      bucket.last = i;
      ad.next = UINT32_MAX;
    } else {
      u64 key = ad.key();
      const auto& bucket_it = draw_key_to_bucket.find(key);
      if (bucket_it == draw_key_to_bucket.end()) {
        if (m_next_free_bucket >= m_buckets.size()) {
          if (m_stats) {
            m_stats->overflow++;
          }
          return;
        }
        u32 bucket_idx = m_next_free_bucket++;
        draw_key_to_bucket[key] = bucket_idx;
        auto& bucket = m_buckets[bucket_idx];
        bucket.tbp = ad.tbp;
        bucket.mode = ad.mode;
        bucket.start = i;
        bucket.last = i;
        ad.next = UINT32_MAX;
      } else {
        auto& bucket = m_buckets[bucket_it->second];
        m_adgifs[bucket.last].next = i;
        ad.next = UINT32_MAX;
        bucket.last = i;
      }
    }
  }
}

void MetalGeneric2::process_matrices() {
  // The matrices are exactly a perspective projection and all the same.
  bool found_proj_matrix = false;
  std::array<math::Vector4f, 4> projection_matrix, hud_matrix;
  for (auto& row : projection_matrix) {
    row.fill(0);
  }
  for (auto& row : hud_matrix) {
    row.fill(0);
  }
  for (u32 i = 0; i < m_next_free_frag; i++) {
    float mat_33;
    memcpy(&mat_33, m_fragments[i].header + 15 * sizeof(float), sizeof(float));
    if (mat_33 == 0) {
      memcpy(&projection_matrix, m_fragments[i].header, 64);
      found_proj_matrix = true;
      break;
    }
  }
  if (!found_proj_matrix) {
    for (auto& row : projection_matrix) {
      row.fill(0);
    }
  }

  bool found_hud_matrix = false;
  for (u32 i = 0; i < m_next_free_frag; i++) {
    float mat_33;
    memcpy(&mat_33, m_fragments[i].header + 15 * sizeof(float), sizeof(float));
    if (mat_33 == 0) {
      m_fragments[i].uses_hud = false;
    } else {
      m_fragments[i].uses_hud = true;
      if (!found_hud_matrix) {
        found_hud_matrix = true;
        memcpy(&hud_matrix, m_fragments[i].header, 64);
      }
    }
  }

  m_drawing_config.proj_scale[0] = projection_matrix[0][0];
  m_drawing_config.proj_scale[1] = projection_matrix[1][1];
  m_drawing_config.proj_scale[2] = projection_matrix[2][2];
  m_drawing_config.proj_mat_23 = projection_matrix[2][3];
  m_drawing_config.proj_mat_32 = projection_matrix[3][2];

  if (found_hud_matrix) {
    m_drawing_config.hud_scale[0] = hud_matrix[0][0];
    m_drawing_config.hud_scale[1] = hud_matrix[1][1];
    m_drawing_config.hud_scale[2] = hud_matrix[2][2];
    m_drawing_config.hud_mat_23 = hud_matrix[2][3];
    m_drawing_config.hud_mat_32 = hud_matrix[3][2];
    m_drawing_config.hud_mat_33 = hud_matrix[3][3];
  }

  m_drawing_config.uses_hud = found_hud_matrix;
}

void MetalGeneric2::final_vertex_update() {
  for (u32 i = 0; i < m_next_free_adgif; i++) {
    auto& ad = m_adgifs[i];
    for (u32 j = 0; j < ad.vtx_count; j++) {
      m_verts[ad.vtx_idx + j].flags = ad.vtx_flags;
    }
  }
}

void MetalGeneric2::build_index_buffer() {
  for (u32 bucket_idx = 0; bucket_idx < m_next_free_bucket; bucket_idx++) {
    auto& bucket = m_buckets[bucket_idx];
    bucket.tri_count = 0;
    bucket.idx_idx = m_next_free_idx;

    u32 adgif_idx = bucket.start;
    while (adgif_idx != UINT32_MAX) {
      auto& adgif = m_adgifs[adgif_idx];
      // worst case: 3 indices per vertex plus the leading restart
      if ((u64)m_next_free_idx + 1 + 3 * (u64)adgif.vtx_count > m_indices.size()) {
        if (m_stats) {
          m_stats->overflow++;
        }
        bucket.idx_count = m_next_free_idx - bucket.idx_idx;
        return;
      }
      m_indices[m_next_free_idx++] = UINT32_MAX;
      for (u32 vidx = adgif.vtx_idx; vidx < adgif.vtx_idx + adgif.vtx_count; vidx++) {
        auto& vtx = m_verts[vidx];
        if (vtx.adc) {
          m_indices[m_next_free_idx++] = vidx;
          bucket.tri_count++;
        } else {
          m_indices[m_next_free_idx++] = UINT32_MAX;
          m_indices[m_next_free_idx++] = vidx - 1;
          m_indices[m_next_free_idx++] = vidx;
        }
      }
      bucket.tri_count -= 2;
      adgif_idx = adgif.next;
    }

    bucket.idx_count = m_next_free_idx - bucket.idx_idx;
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

namespace {

/*!
 * Metal equivalent of Generic2::setup_opengl_for_draw_mode: the GL function
 * mutates global state, this one fills in the state keys the encoder needs.
 * Every one of the seven blend mappings is the GL one, including the
 * SRC_DST_SRC_DST alpha channel override and SRC_DST_FIX_DST's fix/127
 * constant.
 */
struct GenericDrawSettings {
  MetalPsoKey pso;
  MetalDepthStencilKey depth;
  MetalSamplerKey sampler;
  float alpha_reject = 0.f;
  float color_mult = 1.f;
  bool needs_blend_color = false;
  float blend_color_alpha = 1.f;
  bool unsupported_blend = false;
};

GenericDrawSettings generic_settings_from_draw_mode(const DrawMode& mode,
                                                    u8 fix,
                                                    MetalFrameContext& ctx) {
  GenericDrawSettings out;
  out.pso.shader = MetalShaderId::GENERIC;
  out.pso.color_format = ctx.color_format;
  out.pso.depth_format = ctx.depth_format;

  if (mode.get_at_enable()) {
    switch (mode.get_alpha_test()) {
      case DrawMode::AlphaTest::ALWAYS:
      case DrawMode::AlphaTest::NEVER:
        break;
      case DrawMode::AlphaTest::GEQUAL:
        out.alpha_reject = mode.get_aref() / 128.f;
        break;
      default:
        break;
    }
  }

  if (!mode.get_ab_enable()) {
    out.pso.blend_enable = false;
  } else {
    out.pso.blend_enable = true;
    out.pso.blend_op_rgb = MTLBlendOperationAdd;
    out.pso.blend_op_alpha = MTLBlendOperationAdd;
    out.pso.blend_src_alpha = MTLBlendFactorOne;
    out.pso.blend_dst_alpha = MTLBlendFactorOne;
    switch (mode.get_alpha_blend()) {
      case DrawMode::AlphaBlend::SRC_DST_SRC_DST:
        // Cs * As + (1 - As) * Cd, alpha ONE/ZERO
        out.pso.blend_src_rgb = MTLBlendFactorSourceAlpha;
        out.pso.blend_dst_rgb = MTLBlendFactorOneMinusSourceAlpha;
        out.pso.blend_src_alpha = MTLBlendFactorOne;
        out.pso.blend_dst_alpha = MTLBlendFactorZero;
        break;
      case DrawMode::AlphaBlend::SRC_0_SRC_DST:
        out.pso.blend_src_rgb = MTLBlendFactorSourceAlpha;
        out.pso.blend_dst_rgb = MTLBlendFactorOne;
        out.pso.blend_src_alpha = MTLBlendFactorSourceAlpha;
        break;
      case DrawMode::AlphaBlend::ZERO_SRC_SRC_DST:
        out.pso.blend_src_rgb = MTLBlendFactorSourceAlpha;
        out.pso.blend_dst_rgb = MTLBlendFactorOne;
        out.pso.blend_src_alpha = MTLBlendFactorSourceAlpha;
        out.pso.blend_op_rgb = MTLBlendOperationReverseSubtract;
        out.pso.blend_op_alpha = MTLBlendOperationReverseSubtract;
        break;
      case DrawMode::AlphaBlend::SRC_DST_FIX_DST:
        out.pso.blend_src_rgb = MTLBlendFactorBlendAlpha;
        out.pso.blend_dst_rgb = MTLBlendFactorOneMinusBlendAlpha;
        out.pso.blend_src_alpha = MTLBlendFactorBlendAlpha;
        out.pso.blend_dst_alpha = MTLBlendFactorOneMinusBlendAlpha;
        out.needs_blend_color = true;
        out.blend_color_alpha = fix / 127.f;
        break;
      case DrawMode::AlphaBlend::SRC_SRC_SRC_SRC:
        // Cv = Cs: no blend
        out.pso.blend_src_rgb = MTLBlendFactorOne;
        out.pso.blend_dst_rgb = MTLBlendFactorZero;
        out.pso.blend_src_alpha = MTLBlendFactorOne;
        out.pso.blend_dst_alpha = MTLBlendFactorZero;
        break;
      case DrawMode::AlphaBlend::SRC_0_DST_DST:
        out.pso.blend_src_rgb = MTLBlendFactorDestinationAlpha;
        out.pso.blend_dst_rgb = MTLBlendFactorOne;
        out.pso.blend_src_alpha = MTLBlendFactorDestinationAlpha;
        break;
      case DrawMode::AlphaBlend::SRC_0_FIX_DST:
        out.pso.blend_src_rgb = MTLBlendFactorOne;
        out.pso.blend_dst_rgb = MTLBlendFactorOne;
        out.pso.blend_src_alpha = MTLBlendFactorOne;
        out.pso.blend_dst_alpha = MTLBlendFactorZero;
        break;
      default:
        out.unsupported_blend = true;
        out.pso.blend_enable = false;
        break;
    }
  }

  // The GL renderer asserts when ztest is off (the GS had bugs there); here the
  // Always compare is used and the state is otherwise unchanged.
  out.depth.depth_test = mode.get_zt_enable();
  if (mode.get_zt_enable()) {
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
        out.depth.compare = MTLCompareFunctionAlways;
        break;
    }
  } else {
    out.depth.compare = MTLCompareFunctionAlways;
  }
  out.depth.depth_write = mode.get_depth_write_enable();

  out.sampler.wrap_s =
      mode.get_clamp_s_enable() ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
  out.sampler.wrap_t =
      mode.get_clamp_t_enable() ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
  // the GL path uses GL_LINEAR (not mipmapped) when filtering is on
  out.sampler.min_filter =
      mode.get_filt_enable() ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
  out.sampler.mag_filter = out.sampler.min_filter;
  out.sampler.mip_filter = MTLSamplerMipFilterNotMipmapped;
  return out;
}

}  // namespace

void MetalGeneric2::draw_bucket(const Bucket& bucket,
                                const Adgif& first,
                                MetalSharedRenderState* render_state,
                                MetalFrameContext& ctx,
                                id<MTLBuffer> index_buffer,
                                u32 index_base) {
  if (bucket.idx_count == 0) {
    return;
  }
  auto settings = generic_settings_from_draw_mode(first.mode, (u8)first.fix, ctx);
  if (settings.unsupported_blend && m_stats) {
    m_stats->unsupported_blends++;
  }

  // mirror of setup_opengl_tex
  const u32 tbp_to_lookup = first.tbp & 0x7fff;
  const bool use_mt4hh = first.tbp & 0x8000;
  auto tex_handle = use_mt4hh ? render_state->texture_pool->lookup_mt4hh(tbp_to_lookup)
                              : render_state->texture_pool->lookup(tbp_to_lookup);
  if (!tex_handle) {
    if (m_stats) {
      m_stats->missing_textures++;
    }
    if (!m_logged["missing texture"]) {
      m_logged["missing texture"] = true;
      lg::warn("Metal generic2: no texture at VRAM slot {}, using the placeholder (logged once)",
               tbp_to_lookup);
    }
    tex_handle = render_state->texture_pool->get_placeholder_texture();
  }
  id<MTLTexture> tex = metal_texture_lookup(*tex_handle);
  if (!tex) {
    tex = metal_texture_lookup(render_state->texture_pool->get_placeholder_texture());
  }
  if (!tex) {
    return;
  }

  id<MTLRenderPipelineState> pso = ctx.pso_cache->get_pipeline(settings.pso);
  if (!pso) {
    return;
  }
  id<MTLRenderCommandEncoder> enc = ctx.enc;
  [enc setRenderPipelineState:pso];
  [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(settings.depth)];
  if (settings.needs_blend_color) {
    [enc setBlendColorRed:0 green:0 blue:0 alpha:settings.blend_color_alpha];
  }
  [enc setFragmentTexture:tex atIndex:0];
  [enc setFragmentSamplerState:ctx.sampler_cache->get(settings.sampler) atIndex:0];

  GenericFsParams fs = {};
  fs.fog_color[0] = render_state->fog_color[0] / 255.f;
  fs.fog_color[1] = render_state->fog_color[1] / 255.f;
  fs.fog_color[2] = render_state->fog_color[2] / 255.f;
  fs.fog_color[3] = render_state->fog_intensity / 255.f;
  fs.alpha_reject = settings.alpha_reject;
  fs.color_mult = settings.color_mult;
  fs.gfx_hack_no_tex = 0;
  fs.warp_sample_mode = 0;
  [enc setFragmentBytes:&fs length:sizeof(fs) atIndex:0];

  [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                  indexCount:bucket.idx_count
                   indexType:MTLIndexTypeUInt32
                 indexBuffer:index_buffer
           indexBufferOffset:(index_base + bucket.idx_idx) * sizeof(u32)];
  ctx.draw_calls++;
  ctx.triangles += bucket.tri_count;
  if (m_stats) {
    m_stats->draw_calls++;
    m_stats->triangles += bucket.tri_count;
  }
}

void MetalGeneric2::do_draws(MetalSharedRenderState* render_state, MetalFrameContext& ctx) {
  if (m_next_free_vert == 0 || m_next_free_idx == 0 || m_next_free_bucket == 0) {
    return;
  }

  // The GL renderer re-uploads its two GL_STREAM_DRAW buffers each frame; here
  // the data goes into the frame's stream buffer, which stays alive until the
  // frame's command buffer completes.
  id<MTLBuffer> vertex_buffer = nil;
  u32 vertex_offset = 0;
  void* vtx_dst = ctx.stream->alloc(m_next_free_vert * sizeof(Vertex), &vertex_buffer,
                                    &vertex_offset);
  memcpy(vtx_dst, m_verts.data(), m_next_free_vert * sizeof(Vertex));

  id<MTLBuffer> index_buffer = nil;
  u32 index_offset = 0;
  void* idx_dst =
      ctx.stream->alloc(m_next_free_idx * sizeof(u32), &index_buffer, &index_offset);
  memcpy(idx_dst, m_indices.data(), m_next_free_idx * sizeof(u32));

  id<MTLRenderCommandEncoder> enc = ctx.enc;
  [enc setVertexBuffer:vertex_buffer offset:vertex_offset atIndex:0];

  GenericVsParams vs = {};
  vs.scale[0] = m_drawing_config.proj_scale[0];
  vs.scale[1] = m_drawing_config.proj_scale[1];
  vs.scale[2] = m_drawing_config.proj_scale[2];
  vs.mat_23 = m_drawing_config.proj_mat_23;
  vs.mat_32 = m_drawing_config.proj_mat_32;
  vs.mat_33 = 0.f;
  vs.fog_constants[0] = m_drawing_config.pfog0;
  vs.fog_constants[1] = m_drawing_config.fog_min;
  vs.fog_constants[2] = m_drawing_config.fog_max;
  memcpy(vs.hvdf_offset, m_drawing_config.hvdf_offset.data(), sizeof(vs.hvdf_offset));
  vs.use_full_matrix = 0;  // Jak 1 NORMAL mode never sets one
  vs.warp_sample_mode = 0;
  vs.height_scale = 1.f;  // Jak 1
  vs.scissor_adjust = 512.f / kGameHeightJak1;
  vs.warp_off = 0.f;
  [enc setVertexBytes:&vs length:sizeof(vs) atIndex:1];

  // The GL renderer draws in a fixed alpha-mode order so translucent content
  // lands consistently; the order is copied exactly.
  constexpr DrawMode::AlphaBlend alpha_order[7] = {
      DrawMode::AlphaBlend::SRC_0_FIX_DST,    DrawMode::AlphaBlend::SRC_SRC_SRC_SRC,
      DrawMode::AlphaBlend::SRC_DST_SRC_DST,  DrawMode::AlphaBlend::SRC_0_SRC_DST,
      DrawMode::AlphaBlend::ZERO_SRC_SRC_DST, DrawMode::AlphaBlend::SRC_DST_FIX_DST,
      DrawMode::AlphaBlend::SRC_0_DST_DST,
  };
  const u32 index_base = index_offset / sizeof(u32);

  for (auto alpha : alpha_order) {
    for (u32 i = 0; i < m_next_free_bucket; i++) {
      auto& bucket = m_buckets[i];
      auto& first = m_adgifs[bucket.start];
      if (first.mode.get_alpha_blend() == alpha && !first.uses_hud) {
        draw_bucket(bucket, first, render_state, ctx, index_buffer, index_base);
      }
    }
  }

  if (m_drawing_config.uses_hud) {
    vs.scale[0] = m_drawing_config.hud_scale[0];
    vs.scale[1] = m_drawing_config.hud_scale[1];
    vs.scale[2] = m_drawing_config.hud_scale[2];
    vs.mat_23 = m_drawing_config.hud_mat_23;
    vs.mat_32 = m_drawing_config.hud_mat_32;
    vs.mat_33 = m_drawing_config.hud_mat_33;
    [enc setVertexBytes:&vs length:sizeof(vs) atIndex:1];

    for (u32 i = 0; i < m_next_free_bucket; i++) {
      auto& bucket = m_buckets[i];
      auto& first = m_adgifs[bucket.start];
      if (first.uses_hud) {
        draw_bucket(bucket, first, render_state, ctx, index_buffer, index_base);
      }
    }
  }
}

void MetalGeneric2::render(DmaFollower& dma,
                           MetalSharedRenderState* render_state,
                           MetalFrameContext& ctx,
                           Stats* stats) {
  m_stats = stats;
  m_failed = false;

  process_dma_jak1(dma, render_state->next_bucket);

  if (!m_failed) {
    // Jak 1 uses Mode::NORMAL for every generic bucket
    setup_draws(true, true);
  }
  if (!m_failed) {
    do_draws(render_state, ctx);
  }

  if (stats) {
    stats->fragments += (int)m_next_free_frag;
    stats->vertices += (int)m_next_free_vert;
    stats->adgifs += (int)m_next_free_adgif;
    stats->draw_buckets += (int)m_next_free_bucket;
  }
  m_stats = nullptr;

  // whatever happened, leave the follower at the next bucket
  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }
}

void MetalGeneric2BucketRenderer::render(DmaFollower& dma,
                                         MetalSharedRenderState* render_state,
                                         MetalFrameContext& ctx) {
  m_stats = MetalGeneric2::Stats();
  m_generic->render(dma, render_state, ctx, &m_stats);
}
