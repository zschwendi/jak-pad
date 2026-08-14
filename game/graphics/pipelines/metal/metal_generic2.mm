#include "metal_generic2.h"

#include <algorithm>

#include "common/log/log.h"

#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/pipelines/metal/metal_jak2_warp_renderer.h"
#include "game/graphics/texture/TexturePool.h"

namespace {

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

bool is_nop_zero(const DmaTransfer& transfer) {
  return transfer.size_bytes == 0 && transfer.vifcode0().kind == VifCode::Kind::NOP &&
         transfer.vifcode1().kind == VifCode::Kind::NOP;
}

bool is_jak2_end(const DmaTransfer& transfer) {
  return transfer.size_bytes == 160 && transfer.vifcode0().kind == VifCode::Kind::FLUSHA &&
         transfer.vifcode1().kind == VifCode::Kind::DIRECT;
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

void unpack_lightning_vertices(MetalGeneric2::Vertex* out, const u8* in, u32 count) {
  for (u32 i = 0; i < count; i++) {
    s32 s, t;
    memcpy(&s, in, sizeof(s));
    memcpy(&t, in + 4, sizeof(t));
    const s32 s_masked = s & ~1;
    out->st[0] = s_masked;
    out->st[1] = t;
    out->adc = s_masked == s;

    u32 rgba[4];
    memcpy(rgba, in + 16, sizeof(rgba));
    for (int component = 0; component < 4; component++) {
      out->rgba[component] = rgba[component];
    }

    memcpy(out->xyz.data(), in + 32, 3 * sizeof(float));
    out++;
    in += 48;
  }
}

u32 read_u32(const u8* data, u32 offset) {
  u32 value;
  memcpy(&value, data + offset, sizeof(value));
  return value;
}

constexpr u32 lightning_prim_control(GsPrim::Kind kind) {
  const u32 prim = static_cast<u32>(kind) | (1u << 3) | (1u << 4) | (1u << 6);
  return (1u << 14) | (prim << 15) | (3u << 28);
}

bool is_source_lightning_gcf_header(const u8* data, u32 vertex_count) {
  constexpr u32 kRegs = static_cast<u32>(GifTag::RegisterDescriptor::ST) |
                        (static_cast<u32>(GifTag::RegisterDescriptor::RGBAQ) << 4) |
                        (static_cast<u32>(GifTag::RegisterDescriptor::XYZF2) << 8);
  return read_u32(data, 64) == lightning_prim_control(GsPrim::Kind::TRI_FAN) &&
         read_u32(data, 68) == lightning_prim_control(GsPrim::Kind::TRI_STRIP) &&
         read_u32(data, 72) == kRegs && read_u32(data, 76) == 1 && read_u32(data, 80) == 0 &&
         read_u32(data, 84) == 0 && read_u32(data, 88) == 0x7f &&
         read_u32(data, 92) == vertex_count && read_u32(data, 96) == 0 &&
         read_u32(data, 100) == 0 && read_u32(data, 104) == 0x7f && read_u32(data, 108) == 0;
}

bool is_source_lightning_tex1(const GsTex1& tex1) {
  const u32 mmin = tex1.mmin();
  return tex1.mmag() && (mmin == 1 || (mmin == 4 && tex1.mxl() == 0));
}

bool is_source_lightning_adgif(const AdGifData& adgif, u32 vertex_count) {
  constexpr u64 kAlpha = (2ull << 2) | (1ull << 6) | (0x80ull << 32);
  const GsTex0 tex0(adgif.tex0_data);
  const GsTex1 tex1(adgif.tex1_data);
  return adgif.tex0_addr == static_cast<u64>(GsRegisterAddress::TEX0_1) && tex0.tcc() &&
         tex0.tfx() == GsTex0::TextureFunction::MODULATE &&
         adgif.tex1_addr == (static_cast<u64>(GsRegisterAddress::TEX1_1) |
                             (static_cast<u64>(0x8000u | vertex_count) << 32)) &&
         is_source_lightning_tex1(tex1) &&
         adgif.mip_addr == static_cast<u64>(GsRegisterAddress::MIPTBP1_1) &&
         adgif.clamp_data == 0b0101 &&
         adgif.clamp_addr == static_cast<u64>(GsRegisterAddress::CLAMP_1) &&
         adgif.alpha_data == kAlpha &&
         adgif.alpha_addr == static_cast<u64>(GsRegisterAddress::ALPHA_1);
}

bool is_source_lightning_direct(const DmaTransfer& transfer) {
  if (transfer.size_bytes != 32 || transfer.vif0() != 0) {
    return false;
  }
  const auto direct = transfer.vifcode1();
  if (direct.kind != VifCode::Kind::DIRECT || direct.immediate != 2) {
    return false;
  }
  const GifTag tag(transfer.data);
  u64 address;
  memcpy(&address, transfer.data + 24, sizeof(address));
  return tag.nloop() == 1 && tag.eop() && !tag.pre() && tag.flg() == GifTag::Format::PACKED &&
         tag.nreg() == 1 && tag.reg(0) == GifTag::RegisterDescriptor::AD &&
         address == static_cast<u64>(GsRegisterAddress::ZBUF_1);
}

bool is_source_lightning_unpack(const DmaTransfer& transfer,
                                u32 payload_bytes,
                                u16 address,
                                u16 count) {
  const auto unpack = transfer.vifcode1();
  const VifCodeUnpack unpack_fields(unpack);
  return transfer.size_bytes == payload_bytes && transfer.vif0() == 0 &&
         unpack.kind == VifCode::Kind::UNPACK_V4_32 && !unpack.interrupt && unpack.num == count &&
         unpack_fields.addr_qw == address && !unpack_fields.is_unsigned &&
         !unpack_fields.use_tops_flag;
}

}  // namespace

void MetalGeneric2::Stats::add(const Stats& o) {
  fragments += o.fragments;
  continued_fragments += o.continued_fragments;
  vertices += o.vertices;
  adgifs += o.adgifs;
  draw_buckets += o.draw_buckets;
  draw_calls += o.draw_calls;
  triangles += o.triangles;
  missing_textures += o.missing_textures;
  placeholder_draws += o.placeholder_draws;
  missing_warp_publications += o.missing_warp_publications;
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

void MetalGeneric2::process_dma_jak2(DmaFollower& dma, u32 next_bucket) {
  reset_buffers();

  auto first_data = dma.read_and_advance();
  if (is_nop_zero(first_data) && dma.current_tag_offset() == next_bucket) {
    return;
  }

  const auto first_kind = first_data.vifcode0().kind;
  if (!expect((first_kind == VifCode::Kind::MARK || first_kind == VifCode::Kind::NOP) &&
                  first_data.vifcode1().kind == VifCode::Kind::NOP,
              "the Jak 2 generic bucket marker")) {
    return;
  }

  auto direct_setup = dma.read_and_advance();
  if (!expect(direct_setup.size_bytes == 32 &&
                  direct_setup.vifcode0().kind == VifCode::Kind::NOP &&
                  direct_setup.vifcode1().kind == VifCode::Kind::DIRECT,
              "the Jak 2 32-byte zbuf DIRECT setup")) {
    return;
  }
  u64 zbuf_val;
  memcpy(&zbuf_val, direct_setup.data + 16, sizeof(zbuf_val));
  m_drawing_config.zmsk = GsZbuf(zbuf_val).zmsk();

  auto constants = dma.read_and_advance();
  if (!expect(constants.size_bytes == 128 &&
                  constants.vifcode0().kind == VifCode::Kind::STCYCL &&
                  constants.vifcode1().kind == VifCode::Kind::UNPACK_V4_32,
              "the Jak 2 128-byte VU constants unpack")) {
    return;
  }
  memcpy(&m_drawing_config.pfog0, constants.data + 0, 4);
  memcpy(&m_drawing_config.fog_min, constants.data + 4, 4);
  memcpy(&m_drawing_config.fog_max, constants.data + 8, 4);
  memcpy(m_drawing_config.hvdf_offset.data(), constants.data + 32, 16);

  auto vu_setup = dma.read_and_advance();
  if (!expect(vu_setup.size_bytes == 32, "the Jak 2 32-byte VU register setup")) {
    return;
  }

  if (is_nop_zero(first_data) && dma.current_tag_offset() == next_bucket) {
    return;
  }

  Fragment* continued_fragment = nullptr;
  if (!expect(dma.current_tag_offset() != next_bucket,
              "the Jak 2 generic end marker after setup")) {
    return;
  }
  auto vif_transfer = dma.read_and_advance();
  while (is_nop_zero(vif_transfer)) {
    if (!expect(dma.current_tag_offset() != next_bucket,
                "the Jak 2 generic end marker after setup NOPs")) {
      return;
    }
    vif_transfer = dma.read_and_advance();
  }

  while (!is_jak2_end(vif_transfer)) {
    if (continued_fragment) {
      auto up = vif_transfer.vifcode1();
      if (!expect(vif_transfer.vifcode0().kind == VifCode::Kind::NOP &&
                      up.kind == VifCode::Kind::UNPACK_V3_32 &&
                      vif_transfer.size_bytes * 4 / 48 == up.num &&
                      up.num == continued_fragment->vtx_count,
                  "the Jak 2 continued fragment's V3_32 position unpack")) {
        return;
      }
      unpack_vtx_positions(&m_verts[continued_fragment->vtx_idx], vif_transfer.data,
                           continued_fragment->vtx_count);
      continued_fragment = nullptr;
      auto call = dma.read_and_advance();
      if (!expect(call.size_bytes == 0 && call.vifcode1().kind == VifCode::Kind::MSCAL,
                  "the Jak 2 MSCAL after a continued fragment")) {
        return;
      }
      if (check_for_end_of_generic_data(dma, next_bucket)) {
        return;
      }
    } else {
      auto header_unpack = vif_transfer.vifcode1();
      if (!expect(vif_transfer.vifcode0().kind == VifCode::Kind::STCYCL &&
                      header_unpack.kind == VifCode::Kind::UNPACK_V4_32,
                  "a Jak 2 fragment's STCYCL + V4_32 header unpack")) {
        return;
      }
      auto* frag = next_frag();
      if (!frag) {
        return;
      }
      u32 off = handle_fragments_after_unpack_v4_32(
          vif_transfer.data, 0, header_unpack.num * 16, vif_transfer.size_bytes, frag, false);
      if (m_failed) {
        return;
      }

      if (check_for_end_of_generic_data(dma, next_bucket)) {
        return;
      }

      if (off < vif_transfer.size_bytes) {
        if (!expect(off + 8 <= vif_transfer.size_bytes,
                    "the Jak 2 second-fragment unpack tags to fit")) {
          return;
        }
        u32 stcycl_reset;
        memcpy(&stcycl_reset, vif_transfer.data + off, 4);
        if (!expect(VifCode(stcycl_reset).kind == VifCode::Kind::STCYCL,
                    "an STCYCL before a second Jak 2 fragment")) {
          return;
        }
        off += 4;
        u32 next;
        memcpy(&next, vif_transfer.data + off, 4);
        VifCode next_unpack(next);
        if (!expect(next_unpack.kind == VifCode::Kind::UNPACK_V4_32,
                    "a V4_32 header unpack for the second Jak 2 fragment")) {
          return;
        }
        auto* continue_frag = next_frag();
        if (!continue_frag) {
          return;
        }
        off = handle_fragments_after_unpack_v4_32(vif_transfer.data, off,
                                                  next_unpack.num * 16,
                                                  vif_transfer.size_bytes, continue_frag, true);
        continued_fragment = continue_frag;
        if (m_stats) {
          m_stats->continued_fragments++;
        }
        if (!expect(off == vif_transfer.size_bytes,
                    "the second Jak 2 fragment to end the transfer")) {
          return;
        }
      }
    }

    if (!expect(dma.current_tag_offset() != next_bucket,
                "the Jak 2 generic FLUSHA/DIRECT end marker")) {
      return;
    }
    vif_transfer = dma.read_and_advance();
    while (is_nop_zero(vif_transfer)) {
      if (!expect(dma.current_tag_offset() != next_bucket,
                  "the Jak 2 generic end marker after fragment NOPs")) {
        return;
      }
      vif_transfer = dma.read_and_advance();
    }
  }

  if (!expect(continued_fragment == nullptr,
              "a completed Jak 2 fragment before the end marker")) {
    return;
  }
  if (!expect(dma.current_tag_offset() != next_bucket,
              "the final Jak 2 generic NOP transfer")) {
    return;
  }
  const auto end = dma.read_and_advance();
  expect(is_nop_zero(end) && dma.current_tag_offset() == next_bucket,
         "the final Jak 2 generic NOP and bucket boundary");
}

void MetalGeneric2::process_dma_lightning(DmaFollower& dma, u32 next_bucket) {
  reset_buffers();

  const auto read_before_boundary = [&](DmaTransfer* transfer, const char* description) {
    if (!expect(dma.current_tag_offset() != next_bucket, description)) {
      return false;
    }
    *transfer = dma.read_and_advance();
    return true;
  };

  DmaTransfer first;
  if (!read_before_boundary(&first, "the Jak 2 Lightning bucket marker before the boundary")) {
    return;
  }
  if (is_nop_zero(first) && dma.current_tag_offset() == next_bucket) {
    return;
  }
  const auto first_kind = first.vifcode0().kind;
  if (!expect(first.size_bytes == 0 &&
                  (first_kind == VifCode::Kind::MARK || first_kind == VifCode::Kind::NOP) &&
                  first.vifcode1().kind == VifCode::Kind::NOP,
              "the Jak 2 Lightning bucket marker")) {
    return;
  }

  DmaTransfer direct;
  if (!read_before_boundary(&direct, "the Jak 2 Lightning DIRECT setup before the boundary") ||
      !expect(is_source_lightning_direct(direct),
              "the Jak 2 Lightning 32-byte ZBUF_1 DIRECT setup")) {
    return;
  }
  u64 zbuf_value;
  memcpy(&zbuf_value, direct.data + 16, sizeof(zbuf_value));
  if (!expect(GsZbuf(zbuf_value).zmsk(), "masked Jak 2 Lightning depth writes")) {
    return;
  }
  m_drawing_config.zmsk = true;

  DmaTransfer constants;
  if (!read_before_boundary(&constants, "the Jak 2 Lightning constants before the boundary")) {
    return;
  }
  const auto constants_stcycl = constants.vifcode0();
  const auto constants_unpack = constants.vifcode1();
  const VifCodeUnpack constants_unpack_fields(constants_unpack);
  if (!expect(constants.size_bytes == 128 && constants_stcycl.kind == VifCode::Kind::STCYCL &&
                  constants_stcycl.immediate == 0x404 && !constants_stcycl.interrupt &&
                  constants_unpack.kind == VifCode::Kind::UNPACK_V4_32 &&
                  constants_unpack.num == 8 && !constants_unpack.interrupt &&
                  constants_unpack_fields.addr_qw == 897 && !constants_unpack_fields.is_unsigned &&
                  !constants_unpack_fields.use_tops_flag,
              "the Jak 2 Lightning 128-byte constants unpack at VU address 897")) {
    return;
  }
  memcpy(&m_drawing_config.pfog0, constants.data, sizeof(float));
  memcpy(&m_drawing_config.fog_min, constants.data + 4, sizeof(float));
  memcpy(&m_drawing_config.fog_max, constants.data + 8, sizeof(float));
  memcpy(m_drawing_config.hvdf_offset.data(), constants.data + 32, 16);

  DmaTransfer vu_setup;
  if (!read_before_boundary(&vu_setup, "the Jak 2 Lightning VU setup before the boundary") ||
      !expect(vu_setup.size_bytes == 32 && vu_setup.vifcode0().kind == VifCode::Kind::MSCALF &&
                  vu_setup.vifcode0().immediate == 0 &&
                  vu_setup.vifcode1().kind == VifCode::Kind::STMOD &&
                  vu_setup.vifcode1().immediate == 0,
              "the Jak 2 Lightning 32-byte MSCALF/STMOD setup")) {
    return;
  }

  DmaTransfer setup_end;
  if (!read_before_boundary(&setup_end, "the Jak 2 Lightning setup NOP before the boundary") ||
      !expect(is_nop_zero(setup_end), "the Jak 2 Lightning setup NOP")) {
    return;
  }

  u16 expected_header_address = 837;
  u16 expected_vertex_address = 9;
  DmaTransfer transfer;
  if (!read_before_boundary(&transfer,
                            "the Jak 2 Lightning header or linker before the boundary")) {
    return;
  }
  while (transfer.vifcode1().kind == VifCode::Kind::UNPACK_V4_32) {
    if (!expect(is_source_lightning_unpack(transfer, FRAG_HEADER_SIZE + sizeof(AdGifData),
                                           expected_header_address, 12),
                "a Jak 2 Lightning 192-byte GCF header/adgif unpack")) {
      return;
    }

    DmaTransfer vertices;
    if (!read_before_boundary(&vertices, "Jak 2 Lightning vertices after the GCF header")) {
      return;
    }
    const u32 vertex_count = vertices.size_bytes / 48;
    if (!expect(
            vertices.size_bytes % 48 == 0 && vertex_count >= 4 && vertex_count <= 82 &&
                (vertex_count & 1) == 0 &&
                is_source_lightning_unpack(vertices, vertices.size_bytes, expected_vertex_address,
                                           static_cast<u16>(vertices.size_bytes / 16)),
            "Jak 2 Lightning 48-byte packed vertices")) {
      return;
    }

    AdGifData source_adgif;
    memcpy(&source_adgif, transfer.data + FRAG_HEADER_SIZE, sizeof(source_adgif));
    if (!expect(is_source_lightning_gcf_header(transfer.data, vertex_count),
                "the exact 112-byte Jak 2 Lightning GCF header") ||
        !expect(is_source_lightning_adgif(source_adgif, vertex_count),
                "the source Jak 2 Lightning ALPHA/TEX0/TEX1/MIP/CLAMP state")) {
      return;
    }

    DmaTransfer mscal;
    if (!read_before_boundary(&mscal, "Jak 2 Lightning MSCAL 6 after the vertices") ||
        !expect(mscal.size_bytes == 0 && mscal.vifcode0().kind == VifCode::Kind::NOP &&
                    mscal.vifcode1().kind == VifCode::Kind::MSCAL &&
                    mscal.vifcode1().immediate == 6,
                "Jak 2 Lightning MSCAL 6")) {
      return;
    }

    auto* fragment = next_frag();
    auto* adgif = next_adgif();
    if (!fragment || !adgif || !alloc_vtx(vertex_count)) {
      return;
    }
    memcpy(fragment->header, transfer.data, FRAG_HEADER_SIZE);
    fragment->adgif_idx = m_next_free_adgif - 1;
    fragment->adgif_count = 1;
    fragment->vtx_idx = m_next_free_vert - vertex_count;
    fragment->vtx_count = vertex_count;
    fragment->mscal_addr = 6;
    fragment->uses_hud = false;
    adgif->data = source_adgif;
    unpack_lightning_vertices(&m_verts[fragment->vtx_idx], vertices.data, vertex_count);

    expected_header_address = 1704 - expected_header_address;
    expected_vertex_address += 279;
    if (expected_vertex_address > 567) {
      expected_vertex_address = 9;
    }
    if (!read_before_boundary(&transfer,
                              "the Jak 2 Lightning next header or linker before the boundary")) {
      return;
    }
  }

  if (!expect(is_nop_zero(transfer),
              "the required Jak 2 Lightning NOP linker before the trailer")) {
    return;
  }

  DmaTransfer trailer;
  if (!read_before_boundary(&trailer,
                            "the Jak 2 Lightning FLUSHA/DIRECT trailer before the boundary")) {
    return;
  }
  const auto flusha = trailer.vifcode0();
  const auto final_direct = trailer.vifcode1();
  if (!expect(trailer.size_bytes == 160 && flusha.kind == VifCode::Kind::FLUSHA &&
                  final_direct.kind == VifCode::Kind::DIRECT && final_direct.immediate == 10,
              "the Jak 2 Lightning 160-byte FLUSHA/DIRECT trailer")) {
    return;
  }

  DmaTransfer end;
  if (!read_before_boundary(&end, "the final Jak 2 Lightning NOP before the boundary")) {
    return;
  }
  expect(is_nop_zero(end) && dma.current_tag_offset() == next_bucket,
         "the final Jak 2 Lightning NOP and bucket boundary");
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
  const bool warp_sample = m_current_mode == Mode::WARP &&
                           render_state->version == GameVersion::Jak2 &&
                           tbp_to_lookup == metal_renderer::kJak2WarpTextureTbp;
  if (warp_sample) {
    settings.sampler.wrap_s = MTLSamplerAddressModeClampToEdge;
    settings.sampler.wrap_t = MTLSamplerAddressModeClampToEdge;
  }
  bool uses_placeholder = false;
  auto tex_handle = use_mt4hh ? render_state->texture_pool->lookup_mt4hh(tbp_to_lookup)
                              : render_state->texture_pool->lookup(tbp_to_lookup);
  if (warp_sample &&
      (!tex_handle || *tex_handle == render_state->texture_pool->get_placeholder_texture())) {
    if (m_stats) {
      m_stats->missing_textures++;
      m_stats->missing_warp_publications++;
    }
    if (!m_logged["missing warp publication"]) {
      m_logged["missing warp publication"] = true;
      lg::warn("Metal generic2: no framebuffer snapshot at VRAM slot {}; skipping the warp draw "
               "(logged once)",
               tbp_to_lookup);
    }
    return;
  }
  if (!tex_handle) {
    if (m_stats) {
      m_stats->missing_textures++;
    }
    if (m_current_mode == Mode::LIGHTNING) {
      if (!m_logged["missing Lightning texture"]) {
        m_logged["missing Lightning texture"] = true;
        lg::warn("Metal generic2: no Lightning texture at VRAM slot {}; skipping the draw "
                 "(logged once)",
                 tbp_to_lookup);
      }
      return;
    }
    if (!m_logged["missing texture"]) {
      m_logged["missing texture"] = true;
      lg::warn("Metal generic2: no texture at VRAM slot {}, using the placeholder (logged once)",
               tbp_to_lookup);
    }
    uses_placeholder = true;
    tex_handle = render_state->texture_pool->get_placeholder_texture();
  }
  id<MTLTexture> tex = metal_texture_lookup(*tex_handle);
  if (warp_sample && !tex) {
    if (m_stats) {
      m_stats->missing_textures++;
      m_stats->missing_warp_publications++;
    }
    return;
  }
  if (!tex) {
    if (m_current_mode == Mode::LIGHTNING) {
      if (m_stats) {
        m_stats->missing_textures++;
      }
      return;
    }
    uses_placeholder = true;
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

  GenericVsParams vs = {};
  const bool uses_hud = first.uses_hud;
  const auto& scale = uses_hud ? m_drawing_config.hud_scale : m_drawing_config.proj_scale;
  vs.scale[0] = scale[0];
  vs.scale[1] = scale[1];
  vs.scale[2] = scale[2];
  vs.mat_23 = uses_hud ? m_drawing_config.hud_mat_23 : m_drawing_config.proj_mat_23;
  vs.mat_32 = uses_hud ? m_drawing_config.hud_mat_32 : m_drawing_config.proj_mat_32;
  vs.mat_33 = uses_hud ? m_drawing_config.hud_mat_33 : 0.f;
  vs.fog_constants[0] = m_drawing_config.pfog0;
  vs.fog_constants[1] = m_drawing_config.fog_min;
  vs.fog_constants[2] = m_drawing_config.fog_max;
  memcpy(vs.hvdf_offset, m_drawing_config.hvdf_offset.data(), sizeof(vs.hvdf_offset));
  vs.use_full_matrix = 0;
  vs.warp_sample_mode = warp_sample;
  vs.height_scale = metal_height_scale(render_state->version);
  vs.scissor_adjust = metal_scissor_adjust(render_state->version);
  vs.warp_off = warp_sample ? (1.f - 416.f / 512.f) : 0.f;
  [enc setVertexBytes:&vs length:sizeof(vs) atIndex:1];

  GenericFsParams fs = {};
  fs.fog_color[0] = render_state->fog_color[0] / 255.f;
  fs.fog_color[1] = render_state->fog_color[1] / 255.f;
  fs.fog_color[2] = render_state->fog_color[2] / 255.f;
  fs.fog_color[3] = render_state->fog_intensity / 255.f;
  fs.alpha_reject = settings.alpha_reject;
  fs.color_mult = settings.color_mult;
  fs.gfx_hack_no_tex = 0;
  fs.warp_sample_mode = warp_sample;
  [enc setFragmentBytes:&fs length:sizeof(fs) atIndex:0];

  [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                  indexCount:bucket.idx_count
                   indexType:MTLIndexTypeUInt32
                 indexBuffer:index_buffer
           indexBufferOffset:(index_base + bucket.idx_idx) * sizeof(u32)];
  if (uses_placeholder && m_stats) {
    m_stats->placeholder_draws++;
  }
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
  render_in_mode(dma, render_state, ctx, Mode::NORMAL, stats);
}

void MetalGeneric2::render_in_mode(DmaFollower& dma,
                                   MetalSharedRenderState* render_state,
                                   MetalFrameContext& ctx,
                                   Mode mode,
                                   Stats* stats) {
  m_stats = stats;
  m_current_mode = mode;
  m_failed = false;

  switch (mode) {
    case Mode::NORMAL:
    case Mode::WARP:
      if (render_state->version == GameVersion::Jak1) {
        if (mode == Mode::WARP) {
          expect(false, "Jak 2 for the Generic2 WARP mode");
        } else {
          process_dma_jak1(dma, render_state->next_bucket);
        }
      } else if (render_state->version == GameVersion::Jak2) {
        process_dma_jak2(dma, render_state->next_bucket);
      } else {
        expect(false, "a supported Generic2 game version");
      }
      break;
    case Mode::LIGHTNING:
      if (render_state->version == GameVersion::Jak2) {
        process_dma_lightning(dma, render_state->next_bucket);
      } else {
        expect(false, "Jak 2 for the Generic2 Lightning mode");
      }
      break;
  }

  if (!m_failed) {
    setup_draws(mode != Mode::LIGHTNING, mode != Mode::WARP);
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
  m_current_mode = Mode::NORMAL;

  // whatever happened, leave the follower at the next bucket
  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }
}

void MetalGeneric2BucketRenderer::render(DmaFollower& dma,
                                         MetalSharedRenderState* render_state,
                                         MetalFrameContext& ctx) {
  m_stats = MetalGeneric2::Stats();
  if (m_mode == MetalGeneric2::Mode::LIGHTNING && render_state->host_bucket_callback) {
    render_state->host_bucket_callback(render_state->host_bucket_context,
                                       static_cast<u32>(m_my_id));
  }
  m_generic->render_in_mode(dma, render_state, ctx, m_mode, &m_stats);
}
