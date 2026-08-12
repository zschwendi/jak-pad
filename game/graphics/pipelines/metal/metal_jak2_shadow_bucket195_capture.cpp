#include "game/graphics/pipelines/metal/metal_jak2_shadow_bucket195_capture.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "common/dma/dma.h"

namespace metal_renderer {
namespace {

constexpr u64 kOffsetBasis = 14695981039346656037ull;
constexpr u64 kPrime = 1099511628211ull;
constexpr std::size_t kMaximumEeSnapshotBytes = 128 * (1 << 20);
constexpr u16 kNoTrailingMscalf = std::numeric_limits<u16>::max();
constexpr u16 kTopVertexAddress = 4;
constexpr u16 kBottomVertexAddress = 174;
constexpr u16 kCapIndexAddress = 344;
constexpr u16 kWallIndexAddress = 600;

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

void hash_u16(u64* hash, u16 value) {
  hash_byte(hash, static_cast<u8>(value));
  hash_byte(hash, static_cast<u8>(value >> 8));
}

void hash_u32(u64* hash, u32 value) {
  for (int i = 0; i < 4; ++i) {
    hash_byte(hash, static_cast<u8>(value >> (i * 8)));
  }
}

struct CheckedTransfer {
  DmaTag tag{0};
  u32 tag_offset = 0;
  u32 payload_offset = 0;
  u32 payload_bytes = 0;
  u32 next_offset = 0;
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
  const u8* memory() const { return m_memory; }

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
    if ((payload_offset & 15) != 0 ||
        !range_is_valid(payload_offset, payload_bytes, m_memory_size) ||
        next_offset > std::numeric_limits<u32>::max() || (next_offset & 15) != 0 ||
        !range_is_valid(next_offset, 0, m_memory_size)) {
      return false;
    }

    m_visited[m_visited_count++] = m_offset;
    out->tag = tag;
    out->tag_offset = m_offset;
    out->payload_offset = static_cast<u32>(payload_offset);
    out->payload_bytes = static_cast<u32>(payload_bytes);
    out->next_offset = static_cast<u32>(next_offset);
    out->vif0 = read_unaligned<u32>(m_memory + m_offset + 8);
    out->vif1 = read_unaligned<u32>(m_memory + m_offset + 12);
    m_offset = out->next_offset;
    return true;
  }

 private:
  const u8* m_memory = nullptr;
  std::size_t m_memory_size = 0;
  u32 m_offset = 0;
  std::array<u32, kJak2ShadowBucket195MaximumTransfers> m_visited = {};
  std::size_t m_visited_count = 0;
};

bool is_kind(const VifCode& code, VifCode::Kind kind) {
  return code.kind == kind;
}

Jak2ShadowBucket195TransferRole transfer_role(const Jak2ShadowBucket195TransferMetadata& metadata) {
  const auto vif0 = VifCode(metadata.vif0);
  const auto vif1 = VifCode(metadata.vif1);
  if (metadata.payload_bytes == 208 && is_kind(vif0, VifCode::Kind::STCYCL) &&
      vif0.immediate == 0x404 && is_kind(vif1, VifCode::Kind::UNPACK_V4_32) &&
      vif1.num == 13 && VifCodeUnpack(vif1).addr_qw == 0x370) {
    return Jak2ShadowBucket195TransferRole::Constants;
  }
  if (metadata.payload_bytes == 64 && is_kind(vif0, VifCode::Kind::STCYCL) &&
      vif0.immediate == 0x404 && is_kind(vif1, VifCode::Kind::UNPACK_V4_32) &&
      vif1.num == 4 && VifCodeUnpack(vif1).addr_qw == 0x3ac) {
    return Jak2ShadowBucket195TransferRole::Mystery;
  }
  if (metadata.payload_bytes == 64 && is_kind(vif0, VifCode::Kind::STCYCL) &&
      vif0.immediate == 0x404 && is_kind(vif1, VifCode::Kind::UNPACK_V4_32) &&
      vif1.num == 4 && VifCodeUnpack(vif1).addr_qw == 0) {
    return Jak2ShadowBucket195TransferRole::Matrix;
  }
  if (metadata.payload_bytes == 0 && is_kind(vif0, VifCode::Kind::MSCALF) &&
      vif0.immediate == 10 && is_kind(vif1, VifCode::Kind::FLUSHE)) {
    return Jak2ShadowBucket195TransferRole::Mscalf10;
  }
  if (is_kind(vif0, VifCode::Kind::NOP) && is_kind(vif1, VifCode::Kind::DIRECT) &&
      vif1.immediate == 35) {
    return Jak2ShadowBucket195TransferRole::Direct35;
  }
  if (is_kind(vif1, VifCode::Kind::UNPACK_V4_32)) {
    const auto address = VifCodeUnpack(vif1).addr_qw;
    if (address == kTopVertexAddress) {
      return Jak2ShadowBucket195TransferRole::TopVertices;
    }
    if (address == kBottomVertexAddress) {
      return Jak2ShadowBucket195TransferRole::BottomVertices;
    }
  }
  if (is_kind(vif1, VifCode::Kind::UNPACK_V4_8)) {
    const auto address = VifCodeUnpack(vif1).addr_qw;
    if (address == kCapIndexAddress) {
      return Jak2ShadowBucket195TransferRole::CapIndices;
    }
    if (address == kWallIndexAddress) {
      return Jak2ShadowBucket195TransferRole::WallIndices;
    }
  }
  if (is_kind(vif0, VifCode::Kind::FLUSHA) && is_kind(vif1, VifCode::Kind::DIRECT)) {
    return Jak2ShadowBucket195TransferRole::FlushaDirect;
  }
  return Jak2ShadowBucket195TransferRole::Other;
}

bool is_exact_absent(const Jak2ShadowBucket195Capture& capture) {
  const auto& transfer = capture.transfers[0];
  return capture.reached_boundary && capture.transfer_count == 1 &&
         transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) && transfer.qwc == 0 &&
         transfer.payload_bytes == 0 && transfer.vif0 == 0 && transfer.vif1 == 0;
}

void hash_metadata(u64* fingerprint, const Jak2ShadowBucket195TransferMetadata& metadata) {
  hash_u32(fingerprint, metadata.payload_bytes);
  hash_u32(fingerprint, metadata.vif0);
  hash_u32(fingerprint, metadata.vif1);
  hash_u32(fingerprint, metadata.trailing_marker);
  hash_u16(fingerprint, metadata.qwc);
  hash_u16(fingerprint, metadata.trailing_mscalf_immediate);
  hash_byte(fingerprint, metadata.tag_kind);
  hash_byte(fingerprint, metadata.trailing_zero_words);
  hash_byte(fingerprint, metadata.trailing_marker_in_bounds ? 1 : 0);
  hash_byte(fingerprint, metadata.boundary_after ? 1 : 0);
  hash_byte(fingerprint, static_cast<u8>(metadata.role));
}

}  // namespace

Jak2ShadowBucket195Capture capture_jak2_shadow_bucket195(const u8* dma_packet_snapshot,
                                                          std::size_t dma_packet_snapshot_size,
                                                          u32 chain_offset,
                                                          u32 bucket_id) {
  Jak2ShadowBucket195Capture result;
  result.bucket_id = bucket_id;
  if (bucket_id != kJak2ShadowBucket195 ||
      chain_offset > std::numeric_limits<u32>::max() - (kJak2ShadowBucket195 + 1) * 16) {
    return result;
  }

  const u32 bucket_offset = chain_offset + kJak2ShadowBucket195 * 16;
  const u32 next_bucket = chain_offset + (kJak2ShadowBucket195 + 1) * 16;
  CheckedDmaFollower dma(dma_packet_snapshot, dma_packet_snapshot_size, bucket_offset);
  u64 fingerprint = kOffsetBasis;
  while (dma.offset() != next_bucket) {
    if (result.transfer_count == result.transfers.size()) {
      result.status = Jak2ShadowBucket195CaptureStatus::LimitExceeded;
      return result;
    }

    CheckedTransfer transfer;
    if (!dma.read(&transfer) || transfer.tag_offset < bucket_offset) {
      return result;
    }
    if (transfer.payload_bytes > kJak2ShadowBucket195MaximumTransferPayloadBytes ||
        result.total_payload_bytes >
            kJak2ShadowBucket195MaximumPayloadBytes - transfer.payload_bytes) {
      result.status = Jak2ShadowBucket195CaptureStatus::LimitExceeded;
      return result;
    }

    auto& metadata = result.transfers[result.transfer_count++];
    const VifCode vif0(transfer.vif0);
    const VifCode vif1(transfer.vif1);
    metadata.payload_bytes = transfer.payload_bytes;
    metadata.vif0 = transfer.vif0;
    metadata.vif1 = transfer.vif1;
    metadata.qwc = transfer.tag.qwc;
    metadata.vif0_immediate = vif0.immediate;
    metadata.vif1_immediate = vif1.immediate;
    metadata.tag_kind = static_cast<u8>(transfer.tag.kind);
    metadata.vif0_kind = static_cast<u8>(vif0.kind);
    metadata.vif1_kind = static_cast<u8>(vif1.kind);
    metadata.vif0_num = static_cast<u8>(vif0.num);
    metadata.vif1_num = static_cast<u8>(vif1.num);
    metadata.boundary_after = transfer.next_offset == next_bucket;
    metadata.trailing_mscalf_immediate = kNoTrailingMscalf;

    result.total_payload_bytes += transfer.payload_bytes;
    if (vif1.kind == VifCode::Kind::UNPACK_V4_32 ||
        vif1.kind == VifCode::Kind::UNPACK_V4_8) {
      metadata.unpack_address = VifCodeUnpack(vif1).addr_qw;
      const u32 unpack_count = vif1.num;
      const u32 current_count = vif1.kind == VifCode::Kind::UNPACK_V4_32
                                    ? result.v4_32_unpack_count
                                    : result.v4_8_unpack_count;
      if (current_count > kJak2ShadowBucket195MaximumUnpackCount - unpack_count) {
        result.status = Jak2ShadowBucket195CaptureStatus::LimitExceeded;
        return result;
      }
      if (vif1.kind == VifCode::Kind::UNPACK_V4_32) {
        ++result.v4_32_transfer_count;
        result.v4_32_unpack_count += unpack_count;
      } else {
        ++result.v4_8_transfer_count;
        result.v4_8_unpack_count += unpack_count;
        const u64 marker_offset = static_cast<u64>(transfer.payload_offset) + unpack_count * 4;
        const u64 payload_end = static_cast<u64>(transfer.payload_offset) + transfer.payload_bytes;
        if (marker_offset <= payload_end && payload_end - marker_offset >= 16) {
          metadata.trailing_marker_in_bounds = true;
          for (u32 word = 0; word < 3; ++word) {
            if (read_unaligned<u32>(dma.memory() + marker_offset + word * 4) == 0) {
              ++metadata.trailing_zero_words;
            }
          }
          metadata.trailing_marker = read_unaligned<u32>(dma.memory() + marker_offset + 12);
          const VifCode marker(metadata.trailing_marker);
          if (marker.kind == VifCode::Kind::MSCALF) {
            metadata.trailing_mscalf_immediate = marker.immediate;
          }
        }
      }
    }
    if (vif1.kind == VifCode::Kind::DIRECT || vif1.kind == VifCode::Kind::DIRECTHL) {
      ++result.direct_transfer_count;
      result.direct_payload_bytes += transfer.payload_bytes;
      if (vif0.kind == VifCode::Kind::FLUSHA) {
        result.flusha_direct_payload_bytes += transfer.payload_bytes;
      }
    }
    metadata.role = transfer_role(metadata);
    hash_metadata(&fingerprint, metadata);
  }

  result.reached_boundary = true;
  result.boundary_transfer_index = result.transfer_count - 1;
  const auto& terminal = result.transfers[result.boundary_transfer_index];
  result.terminal_payload_bytes = terminal.payload_bytes;
  result.terminal_qwc = terminal.qwc;
  result.terminal_tag_kind = terminal.tag_kind;
  result.terminal_vif0_kind = terminal.vif0_kind;
  result.terminal_vif1_kind = terminal.vif1_kind;
  result.semantic_fingerprint = fingerprint;
  result.status = is_exact_absent(result) ? Jak2ShadowBucket195CaptureStatus::Absent
                                          : Jak2ShadowBucket195CaptureStatus::Observed;
  return result;
}

}  // namespace metal_renderer
