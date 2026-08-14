#include "game/graphics/pipelines/metal/metal_jak2_effects_bucket315_capture.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_set>

#include "common/dma/dma.h"
#include "common/dma/gs.h"

namespace metal_renderer {
namespace {

constexpr u64 kOffsetBasis = 14695981039346656037ull;
constexpr u64 kPrime = 1099511628211ull;
constexpr std::size_t kMaximumEeSnapshotBytes = 128 * (1 << 20);

template <typename T>
T read_unaligned(const u8* source) {
  T result;
  std::memcpy(&result, source, sizeof(result));
  return result;
}

bool range_is_valid(u64 offset, u64 size, std::size_t memory_size) {
  return offset <= memory_size && size <= memory_size - offset;
}

void hash_byte(u64* hash, u8 value) {
  *hash ^= value;
  *hash *= kPrime;
}

void hash_u32(u64* hash, u32 value) {
  for (int i = 0; i < 4; ++i) {
    hash_byte(hash, static_cast<u8>(value >> (i * 8)));
  }
}

u64 fingerprint_bytes(const u8* bytes, u32 byte_count) {
  u64 result = kOffsetBasis;
  for (u32 i = 0; i < byte_count; ++i) {
    hash_byte(&result, bytes[i]);
  }
  return result;
}

struct CheckedTransfer {
  DmaTag tag{0};
  u32 tag_offset = 0;
  const u8* payload = nullptr;
  u32 payload_bytes = 0;
  u32 vif0 = 0;
  u32 vif1 = 0;
};

class CheckedDmaFollower {
 public:
  CheckedDmaFollower(const u8* memory, std::size_t memory_size, u32 start_offset)
      : m_memory(memory),
        m_memory_size(std::min<std::size_t>(memory_size, kMaximumEeSnapshotBytes)),
        m_offset(start_offset) {}

  u32 offset() const { return m_offset; }

  bool read(CheckedTransfer* out) {
    if (!out || !m_memory || m_visited_count == m_visited.size() ||
        !range_is_valid(m_offset, 16, m_memory_size) || (m_offset & 15) != 0 ||
        std::find(m_visited.begin(), m_visited.begin() + m_visited_count, m_offset) !=
            m_visited.begin() + m_visited_count) {
      return false;
    }
    const DmaTag tag(read_unaligned<u64>(m_memory + m_offset));
    if (tag.spr) {
      return false;
    }
    const u64 payload_bytes = static_cast<u64>(tag.qwc) * 16;
    u64 payload_offset = 0;
    u64 next_offset = 0;
    switch (tag.kind) {
      case DmaTag::Kind::CNT:
        if (tag.addr != 0) {
          return false;
        }
        payload_offset = static_cast<u64>(m_offset) + 16;
        next_offset = payload_offset + payload_bytes;
        break;
      case DmaTag::Kind::NEXT:
        if (tag.addr == 0) {
          return false;
        }
        payload_offset = static_cast<u64>(m_offset) + 16;
        next_offset = tag.addr;
        break;
      case DmaTag::Kind::REF:
      case DmaTag::Kind::REFS:
        if (tag.qwc != 0 && tag.addr == 0) {
          return false;
        }
        payload_offset = tag.addr;
        next_offset = static_cast<u64>(m_offset) + 16;
        break;
      default:
        return false;
    }
    if ((payload_offset & 15) != 0 || !range_is_valid(payload_offset, payload_bytes, m_memory_size) ||
        next_offset > std::numeric_limits<u32>::max() || (next_offset & 15) != 0 ||
        !range_is_valid(next_offset, 0, m_memory_size)) {
      return false;
    }
    m_visited[m_visited_count++] = m_offset;
    out->tag = tag;
    out->tag_offset = m_offset;
    out->payload = m_memory + payload_offset;
    out->payload_bytes = static_cast<u32>(payload_bytes);
    out->vif0 = read_unaligned<u32>(m_memory + m_offset + 8);
    out->vif1 = read_unaligned<u32>(m_memory + m_offset + 12);
    m_offset = static_cast<u32>(next_offset);
    return true;
  }

 private:
  const u8* m_memory = nullptr;
  std::size_t m_memory_size = 0;
  u32 m_offset = 0;
  std::array<u32, kJak2EffectsBucket315MaximumTransfers> m_visited = {};
  std::size_t m_visited_count = 0;
};

bool is_absent(const Jak2EffectsBucket315Capture& capture) {
  const auto& transfer = capture.transfers[0];
  return capture.transfer_count == 1 && transfer.payload_bytes == 0 && transfer.vif0 == 0 &&
         transfer.vif1 == 0;
}

bool is_nop_zero(const CheckedTransfer& transfer) {
  return transfer.payload_bytes == 0 && transfer.vif0 == 0 && transfer.vif1 == 0;
}

constexpr u32 lightning_prim_control(GsPrim::Kind kind) {
  const u32 prim = static_cast<u32>(kind) | (1u << 3) | (1u << 4) | (1u << 6);
  return (1u << 14) | (prim << 15) | (3u << 28);
}

bool is_source_lightning_gcf_header(const CheckedTransfer& transfer, u32 vertex_count) {
  if (transfer.payload_bytes != kJak2EffectsLightningHeaderBytes) {
    return false;
  }
  constexpr u32 kRegs = static_cast<u32>(GifTag::RegisterDescriptor::ST) |
                        (static_cast<u32>(GifTag::RegisterDescriptor::RGBAQ) << 4) |
                        (static_cast<u32>(GifTag::RegisterDescriptor::XYZF2) << 8);
  const auto* data = transfer.payload;
  return read_unaligned<u32>(data + 64) == lightning_prim_control(GsPrim::Kind::TRI_FAN) &&
         read_unaligned<u32>(data + 68) == lightning_prim_control(GsPrim::Kind::TRI_STRIP) &&
         read_unaligned<u32>(data + 72) == kRegs && read_unaligned<u32>(data + 76) == 1 &&
         read_unaligned<u32>(data + 80) == 0 && read_unaligned<u32>(data + 84) == 0 &&
         read_unaligned<u32>(data + 88) == 0x7f &&
         read_unaligned<u32>(data + 92) == vertex_count &&
         read_unaligned<u32>(data + 96) == 0 && read_unaligned<u32>(data + 100) == 0 &&
         read_unaligned<u32>(data + 104) == 0x7f && read_unaligned<u32>(data + 108) == 0;
}

bool is_source_lightning_adgif(const CheckedTransfer& transfer,
                               u32 vertex_count,
                               u32* effective_tbp) {
  if (transfer.payload_bytes != kJak2EffectsLightningHeaderBytes || !effective_tbp) {
    return false;
  }
  constexpr u64 kAlpha = (2ull << 2) | (1ull << 6) | (0x80ull << 32);
  const auto adgif = read_unaligned<AdGifData>(transfer.payload + 112);
  const GsTex0 tex0(adgif.tex0_data);
  const GsTex1 tex1(adgif.tex1_data);
  if (adgif.tex0_addr != static_cast<u64>(GsRegisterAddress::TEX0_1) || !tex0.tcc() ||
      tex0.tfx() != GsTex0::TextureFunction::MODULATE ||
      adgif.tex1_addr != (static_cast<u64>(GsRegisterAddress::TEX1_1) |
                          (static_cast<u64>(0x8000u | vertex_count) << 32)) ||
      !tex1.mmag() || tex1.mmin() != 1 ||
      adgif.mip_addr != static_cast<u64>(GsRegisterAddress::MIPTBP1_1) ||
      adgif.clamp_data != 0b0101 ||
      adgif.clamp_addr != static_cast<u64>(GsRegisterAddress::CLAMP_1) ||
      adgif.alpha_data != kAlpha ||
      adgif.alpha_addr != static_cast<u64>(GsRegisterAddress::ALPHA_1)) {
    return false;
  }
  *effective_tbp = tex0.tbp0();
  if (tex0.psm() == GsTex0::PSM::PSMT4HH) {
    *effective_tbp |= 0x8000;
  }
  return true;
}

bool is_source_lightning_direct(const CheckedTransfer& transfer) {
  if (transfer.payload_bytes != 32 || transfer.vif0 != 0) {
    return false;
  }
  const VifCode direct(transfer.vif1);
  if (direct.kind != VifCode::Kind::DIRECT || direct.immediate != 2) {
    return false;
  }
  const GifTag tag(transfer.payload);
  const u64 address = read_unaligned<u64>(transfer.payload + 24);
  const GsZbuf zbuf(read_unaligned<u64>(transfer.payload + 16));
  return tag.nloop() == 1 && tag.eop() && !tag.pre() &&
         tag.flg() == GifTag::Format::PACKED && tag.nreg() == 1 &&
         tag.reg(0) == GifTag::RegisterDescriptor::AD &&
         address == static_cast<u64>(GsRegisterAddress::ZBUF_1) && zbuf.zmsk();
}

bool is_source_lightning_unpack(const CheckedTransfer& transfer,
                                u32 payload_bytes,
                                u16 address,
                                u16 count) {
  if (transfer.payload_bytes != payload_bytes || transfer.vif0 != 0) {
    return false;
  }
  const VifCode unpack(transfer.vif1);
  const VifCodeUnpack fields(unpack);
  return unpack.kind == VifCode::Kind::UNPACK_V4_32 && !unpack.interrupt &&
         unpack.num == count && fields.addr_qw == address && !fields.is_unsigned &&
         !fields.use_tops_flag;
}

std::optional<Jak2EffectsBucket315Plan> make_exact_plan(
    const std::array<CheckedTransfer, kJak2EffectsBucket315MaximumTransfers>& transfers,
    u32 transfer_count) {
  if (transfer_count == 1 && is_nop_zero(transfers[0])) {
    return Jak2EffectsBucket315Plan{};
  }
  constexpr u32 kFixedTransferCount = 8;
  if (transfer_count < kFixedTransferCount || (transfer_count - kFixedTransferCount) % 3 != 0) {
    return std::nullopt;
  }
  const auto& marker = transfers[0];
  const VifCode marker_vif0(marker.vif0);
  if (marker.payload_bytes != 0 ||
      (marker_vif0.kind != VifCode::Kind::MARK && marker_vif0.kind != VifCode::Kind::NOP) ||
      VifCode(marker.vif1).kind != VifCode::Kind::NOP ||
      !is_source_lightning_direct(transfers[1])) {
    return std::nullopt;
  }
  const auto& constants = transfers[2];
  const VifCode stcycl(constants.vif0);
  const VifCode constants_unpack(constants.vif1);
  const VifCodeUnpack constants_fields(constants_unpack);
  if (constants.payload_bytes != 128 || stcycl.kind != VifCode::Kind::STCYCL ||
      stcycl.immediate != 0x404 || stcycl.interrupt ||
      constants_unpack.kind != VifCode::Kind::UNPACK_V4_32 || constants_unpack.num != 8 ||
      constants_unpack.interrupt || constants_fields.addr_qw != 897 ||
      constants_fields.is_unsigned || constants_fields.use_tops_flag) {
    return std::nullopt;
  }
  const auto& vu_setup = transfers[3];
  const VifCode mscalf(vu_setup.vif0);
  const VifCode stmod(vu_setup.vif1);
  if (vu_setup.payload_bytes != 32 || mscalf.kind != VifCode::Kind::MSCALF ||
      mscalf.immediate != 0 || stmod.kind != VifCode::Kind::STMOD ||
      stmod.immediate != 0 || !is_nop_zero(transfers[4])) {
    return std::nullopt;
  }

  Jak2EffectsBucket315Plan result;
  result.variant = Jak2EffectsBucket315Variant::Lightning;
  result.transfer_count = transfer_count;
  u16 header_address = 837;
  u16 vertex_address = 9;
  for (u32 i = 5; i + 3 < transfer_count; i += 3) {
    const auto& header = transfers[i];
    const auto& vertices = transfers[i + 1];
    const auto& mscal = transfers[i + 2];
    const u32 vertex_count = vertices.payload_bytes / kJak2EffectsLightningVertexBytes;
    if (!is_source_lightning_unpack(header, kJak2EffectsLightningHeaderBytes, header_address, 12) ||
        vertices.payload_bytes % kJak2EffectsLightningVertexBytes != 0 || vertex_count < 4 ||
        vertex_count > 82 || (vertex_count & 1) != 0 ||
        !is_source_lightning_unpack(vertices, vertices.payload_bytes, vertex_address,
                                    static_cast<u16>(vertices.payload_bytes / 16)) ||
        !is_source_lightning_gcf_header(header, vertex_count)) {
      return std::nullopt;
    }
    u32 unused_tbp = 0;
    if (!is_source_lightning_adgif(header, vertex_count, &unused_tbp)) {
      return std::nullopt;
    }
    const VifCode mscal_vif0(mscal.vif0);
    const VifCode mscal_vif1(mscal.vif1);
    if (mscal.payload_bytes != 0 || mscal_vif0.kind != VifCode::Kind::NOP ||
        mscal_vif1.kind != VifCode::Kind::MSCAL || mscal_vif1.immediate != 6 ||
        result.vertex_count > kJak2EffectsLightningMaxVertices - vertex_count) {
      return std::nullopt;
    }
    result.fragment_count++;
    result.vertex_count += vertex_count;
    header_address = 1704 - header_address;
    vertex_address += 279;
    if (vertex_address > 567) {
      vertex_address = 9;
    }
  }
  const auto& linker = transfers[transfer_count - 3];
  const auto& trailer = transfers[transfer_count - 2];
  const auto& final_nop = transfers[transfer_count - 1];
  const VifCode flusha(trailer.vif0);
  const VifCode direct(trailer.vif1);
  if (!is_nop_zero(linker) || trailer.payload_bytes != 160 ||
      flusha.kind != VifCode::Kind::FLUSHA || direct.kind != VifCode::Kind::DIRECT ||
      direct.immediate != 10 ||
      !is_nop_zero(final_nop) ||
      result.fragment_count > kJak2EffectsLightningMaxFragments) {
    return std::nullopt;
  }
  return result;
}

}  // namespace

Jak2EffectsBucket315Capture capture_jak2_effects_bucket315(const u8* dma_packet_snapshot,
                                                            std::size_t dma_packet_snapshot_size,
                                                            u32 chain_offset,
                                                            u32 bucket_id) {
  Jak2EffectsBucket315Capture result;
  result.bucket_id = bucket_id;
  if (bucket_id != kJak2EffectsBucket || chain_offset >
          std::numeric_limits<u32>::max() - (kJak2EffectsBucket + 1) * 16) {
    return result;
  }
  const u32 bucket_offset = chain_offset + kJak2EffectsBucket * 16;
  const u32 next_bucket = chain_offset + (kJak2EffectsBucket + 1) * 16;
  CheckedDmaFollower dma(dma_packet_snapshot, dma_packet_snapshot_size, bucket_offset);
  std::array<CheckedTransfer, kJak2EffectsBucket315MaximumTransfers> checked_transfers = {};
  u64 fingerprint = kOffsetBasis;
  while (dma.offset() != next_bucket) {
    if (result.transfer_count == result.transfers.size()) {
      return result;
    }
    CheckedTransfer transfer;
    if (!dma.read(&transfer) || transfer.tag_offset < bucket_offset) {
      return result;
    }
    checked_transfers[result.transfer_count] = transfer;
    auto& metadata = result.transfers[result.transfer_count++];
    metadata.relative_tag_offset = transfer.tag_offset - bucket_offset;
    metadata.payload_bytes = transfer.payload_bytes;
    metadata.vif0 = transfer.vif0;
    metadata.vif1 = transfer.vif1;
    metadata.payload_fingerprint = fingerprint_bytes(transfer.payload, transfer.payload_bytes);
    metadata.qwc = transfer.tag.qwc;
    metadata.tag_kind = static_cast<u8>(transfer.tag.kind);
    hash_u32(&fingerprint, metadata.payload_bytes);
    hash_u32(&fingerprint, metadata.vif0);
    hash_u32(&fingerprint, metadata.vif1);
    hash_u32(&fingerprint, metadata.qwc);
    hash_byte(&fingerprint, metadata.tag_kind);
    for (int i = 0; i < 8; ++i) {
      hash_byte(&fingerprint, static_cast<u8>(metadata.payload_fingerprint >> (i * 8)));
    }
    result.total_payload_bytes += transfer.payload_bytes;
  }
  const auto plan = make_exact_plan(checked_transfers, result.transfer_count);
  if (!plan) {
    result.valid = true;
    result.present = !is_absent(result);
    result.classification = is_absent(result) ? Jak2EffectsBucket315CaptureClass::Absent
                                               : Jak2EffectsBucket315CaptureClass::Other;
    result.semantic_fingerprint = fingerprint;
    return result;
  }
  result.valid = true;
  result.present = plan->variant != Jak2EffectsBucket315Variant::Absent;
  result.classification = result.present ? Jak2EffectsBucket315CaptureClass::Lightning
                                          : Jak2EffectsBucket315CaptureClass::Absent;
  result.fragment_count = plan->fragment_count;
  result.vertex_count = plan->vertex_count;
  result.adgif_count = plan->fragment_count;
  std::unordered_set<u32> non_hud_draws;
  for (u32 i = 5; i + 3 < result.transfer_count; i += 3) {
    u32 effective_tbp = 0;
    const bool exact_adgif = is_source_lightning_adgif(
        checked_transfers[i], checked_transfers[i + 1].payload_bytes / 48, &effective_tbp);
    if (!exact_adgif) {
      return {};
    }
    const float mat_33 = read_unaligned<float>(checked_transfers[i].payload + 60);
    if (mat_33 == 0.f) {
      non_hud_draws.insert(effective_tbp);
    } else {
      result.draw_count++;
    }
  }
  result.draw_count += static_cast<u32>(non_hud_draws.size());
  result.semantic_fingerprint = fingerprint;
  return result;
}

bool jak2_effects_bucket315_captures_match(const Jak2EffectsBucket315Capture& live,
                                           const Jak2EffectsBucket315Capture& copied) {
  if (!live.valid || !copied.valid || live.bucket_id != kJak2EffectsBucket ||
      copied.bucket_id != kJak2EffectsBucket || live.classification != copied.classification ||
      live.transfer_count != copied.transfer_count ||
      live.fragment_count != copied.fragment_count || live.vertex_count != copied.vertex_count ||
      live.adgif_count != copied.adgif_count || live.draw_count != copied.draw_count ||
      live.total_payload_bytes != copied.total_payload_bytes ||
      live.semantic_fingerprint != copied.semantic_fingerprint) {
    return false;
  }
  for (u32 i = 0; i < live.transfer_count; ++i) {
    const auto& a = live.transfers[i];
    const auto& b = copied.transfers[i];
    if (a.payload_bytes != b.payload_bytes || a.vif0 != b.vif0 || a.vif1 != b.vif1 ||
        a.payload_fingerprint != b.payload_fingerprint ||
        a.qwc != b.qwc || a.tag_kind != b.tag_kind) {
      return false;
    }
  }
  return true;
}

void observe_jak2_effects_bucket315_capture(Jak2EffectsBucket315Telemetry* telemetry,
                                            const Jak2EffectsBucket315Capture& capture) {
  if (!telemetry) {
    return;
  }
  ++telemetry->captures;
  if (!capture.valid) {
    ++telemetry->malformed_captures;
    return;
  }
  ++telemetry->valid_captures;
  telemetry->captured_payload_bytes += capture.total_payload_bytes;
  telemetry->last_transfer_count = capture.transfer_count;
  telemetry->last_fragment_count = capture.fragment_count;
  telemetry->last_vertex_count = capture.vertex_count;
  telemetry->last_payload_bytes = capture.total_payload_bytes;
  telemetry->last_semantic_fingerprint = capture.semantic_fingerprint;
  telemetry->last_classification = static_cast<u8>(capture.classification);
  switch (capture.classification) {
    case Jak2EffectsBucket315CaptureClass::Absent:
      ++telemetry->absent_captures;
      break;
    case Jak2EffectsBucket315CaptureClass::Lightning:
      ++telemetry->lightning_captures;
      break;
    case Jak2EffectsBucket315CaptureClass::Other:
      ++telemetry->other_captures;
      break;
    case Jak2EffectsBucket315CaptureClass::Malformed:
      ++telemetry->malformed_captures;
      break;
  }
}

}  // namespace metal_renderer
