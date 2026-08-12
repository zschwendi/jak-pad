#include "game/graphics/pipelines/metal/metal_jak2_effects_bucket315_capture.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>

#include "common/dma/dma.h"

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

std::optional<Jak2EffectsBucket315VifKind> summary_vif_kind(VifCode::Kind kind) {
  switch (kind) {
    case VifCode::Kind::NOP:
      return Jak2EffectsBucket315VifKind::Nop;
    case VifCode::Kind::MARK:
      return Jak2EffectsBucket315VifKind::Mark;
    case VifCode::Kind::DIRECT:
      return Jak2EffectsBucket315VifKind::Direct;
    case VifCode::Kind::STCYCL:
      return Jak2EffectsBucket315VifKind::Stcycl;
    case VifCode::Kind::UNPACK_V4_32:
      return Jak2EffectsBucket315VifKind::UnpackV4_32;
    case VifCode::Kind::MSCALF:
      return Jak2EffectsBucket315VifKind::Mscalf;
    case VifCode::Kind::STMOD:
      return Jak2EffectsBucket315VifKind::Stmod;
    case VifCode::Kind::MSCAL:
      return Jak2EffectsBucket315VifKind::Mscal;
    case VifCode::Kind::FLUSHA:
      return Jak2EffectsBucket315VifKind::Flusha;
    default:
      return std::nullopt;
  }
}

std::optional<Jak2EffectsBucket315Plan> make_plan(const Jak2EffectsBucket315Capture& capture) {
  std::array<Jak2EffectsBucket315Transfer, kJak2EffectsBucket315MaximumTransfers> transfers = {};
  for (u32 i = 0; i < capture.transfer_count; ++i) {
    const auto& source = capture.transfers[i];
    const auto vif0_kind = summary_vif_kind(VifCode(source.vif0).kind);
    const auto vif1_kind = summary_vif_kind(VifCode(source.vif1).kind);
    if (!vif0_kind || !vif1_kind) {
      return std::nullopt;
    }
    transfers[i] = {source.payload_bytes, *vif0_kind, *vif1_kind};
  }
  return plan_jak2_effects_bucket315(transfers.data(), capture.transfer_count, capture.bucket_id);
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
  u64 fingerprint = kOffsetBasis;
  while (dma.offset() != next_bucket) {
    if (result.transfer_count == result.transfers.size()) {
      return result;
    }
    CheckedTransfer transfer;
    if (!dma.read(&transfer) || transfer.tag_offset < bucket_offset) {
      return result;
    }
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
  const auto plan = make_plan(result);
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
  result.semantic_fingerprint = fingerprint;
  return result;
}

bool jak2_effects_bucket315_captures_match(const Jak2EffectsBucket315Capture& live,
                                           const Jak2EffectsBucket315Capture& copied) {
  if (!live.valid || !copied.valid || live.bucket_id != kJak2EffectsBucket ||
      copied.bucket_id != kJak2EffectsBucket || live.classification != copied.classification ||
      live.transfer_count != copied.transfer_count ||
      live.fragment_count != copied.fragment_count || live.vertex_count != copied.vertex_count ||
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
