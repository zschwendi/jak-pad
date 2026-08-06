#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "common/dma/dma.h"
#include "common/goal_constants.h"

namespace metal_renderer {
namespace {

constexpr u16 kStartAnimatorArray = 12;
constexpr u16 kFinishAnimatorArray = 13;
constexpr u32 kPcPortVif = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;

struct CheckedTransfer {
  DmaTag tag{0};
  u32 tag_offset = 0;
  u32 size_bytes = 0;
  u32 vif0 = 0;
  u32 vif1 = 0;
};

template <typename T>
T read_unaligned(const u8* address) {
  T result;
  std::memcpy(&result, address, sizeof(result));
  return result;
}

bool range_is_valid(u64 offset, u64 size, std::size_t memory_size) {
  return offset <= memory_size && size <= memory_size - offset;
}

class CheckedDmaFollower {
 public:
  CheckedDmaFollower(const u8* memory, std::size_t memory_size, u32 start_offset)
      : m_memory(memory),
        m_memory_size(std::min<std::size_t>(memory_size, EE_MAIN_MEM_SIZE)),
        m_offset(start_offset) {}

  u32 offset() const { return m_offset; }

  bool read(CheckedTransfer* out) {
    if (!out || !m_memory || m_visited_count == m_visited.size() ||
        !range_is_valid(m_offset, 16, m_memory_size) || (m_offset & 15) != 0 ||
        already_visited(m_offset)) {
      return false;
    }

    const DmaTag tag(read_unaligned<u64>(m_memory + m_offset));
    if (tag.spr) {
      return false;
    }

    const u64 payload_bytes = static_cast<u64>(tag.qwc) * 16;
    u64 data_offset = 0;
    u64 next_offset = 0;
    switch (tag.kind) {
      case DmaTag::Kind::CNT:
        if (tag.addr != 0) {
          return false;
        }
        data_offset = static_cast<u64>(m_offset) + 16;
        next_offset = data_offset + payload_bytes;
        break;
      case DmaTag::Kind::NEXT:
        if (tag.addr == 0) {
          return false;
        }
        data_offset = static_cast<u64>(m_offset) + 16;
        next_offset = tag.addr;
        break;
      case DmaTag::Kind::REF:
      case DmaTag::Kind::REFS:
        if (tag.qwc != 0 && tag.addr == 0) {
          return false;
        }
        data_offset = tag.addr;
        next_offset = static_cast<u64>(m_offset) + 16;
        break;
      default:
        return false;
    }

    if ((data_offset & 15) != 0 || !range_is_valid(data_offset, payload_bytes, m_memory_size) ||
        next_offset > std::numeric_limits<u32>::max() || (next_offset & 15) != 0 ||
        !range_is_valid(next_offset, 0, m_memory_size)) {
      return false;
    }

    m_visited[m_visited_count++] = m_offset;
    out->tag = tag;
    out->tag_offset = m_offset;
    out->size_bytes = static_cast<u32>(payload_bytes);
    out->vif0 = read_unaligned<u32>(m_memory + m_offset + 8);
    out->vif1 = read_unaligned<u32>(m_memory + m_offset + 12);
    m_offset = static_cast<u32>(next_offset);
    return true;
  }

 private:
  bool already_visited(u32 offset) const {
    return std::find(m_visited.begin(), m_visited.begin() + m_visited_count, offset) !=
           m_visited.begin() + m_visited_count;
  }

  const u8* m_memory = nullptr;
  std::size_t m_memory_size = 0;
  u32 m_offset = 0;
  std::array<u32, kJak2CommonTfragTextureUploadMaximumTransfers> m_visited = {};
  std::size_t m_visited_count = 0;
};

bool is_inert(const CheckedTransfer& transfer) {
  return transfer.size_bytes == 0 && transfer.vif0 == 0 && transfer.vif1 == 0;
}

bool is_exact_animator_start(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.tag.qwc == 0 &&
         transfer.vif0 == (kPcPortVif | kStartAnimatorArray) && transfer.vif1 == 0;
}

bool is_exact_animator_finish(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.tag.qwc == 0 &&
         transfer.vif0 == (kPcPortVif | kFinishAnimatorArray) &&
         (transfer.vif1 == 0 || transfer.vif1 == kPcPortVif);
}

bool is_ordinary_descriptor(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.size_bytes == 16 &&
         transfer.vif0 == kPcPortVif && transfer.vif1 == 3;
}

bool record_transfer(const CheckedTransfer& transfer,
                     u32 bucket_offset,
                     Jak2CommonTfragTextureUploadCapture* out) {
  if (transfer.tag_offset < bucket_offset) {
    return false;
  }
  const VifCode vif0(transfer.vif0);
  const VifCode vif1(transfer.vif1);
  auto& metadata = out->transfers[out->transfer_count++];
  metadata.relative_tag_offset = transfer.tag_offset - bucket_offset;
  metadata.payload_bytes = transfer.size_bytes;
  metadata.qwc = transfer.tag.qwc;
  metadata.tag_kind = static_cast<u8>(transfer.tag.kind);
  metadata.vif0_kind = static_cast<u8>(vif0.kind);
  metadata.vif0_immediate = vif0.immediate;
  metadata.vif1_kind = static_cast<u8>(vif1.kind);
  metadata.vif1_immediate = vif1.immediate;
  out->total_payload_bytes += transfer.size_bytes;
  return true;
}

Jak2CommonTfragTextureUploadClass classify(
    const Jak2CommonTfragTextureUploadCapture& capture) {
  if (capture.eye_markers != 0 || capture.other_transfers != 0) {
    return Jak2CommonTfragTextureUploadClass::EyeOrOther;
  }
  if (capture.ordinary_descriptors != 0 && capture.animator_arrays != 0) {
    return Jak2CommonTfragTextureUploadClass::OrdinaryAndAnimator;
  }
  if (capture.ordinary_descriptors != 0) {
    return Jak2CommonTfragTextureUploadClass::OrdinaryOnly;
  }
  if (capture.animator_arrays != 0) {
    return Jak2CommonTfragTextureUploadClass::AnimatorOnly;
  }
  return Jak2CommonTfragTextureUploadClass::Absent;
}

}  // namespace

Jak2CommonTfragTextureUploadCapture capture_jak2_common_tfrag_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset) {
  Jak2CommonTfragTextureUploadCapture out;
  const u64 bucket_offset64 =
      static_cast<u64>(chain_offset) + kJak2CommonTfragTextureUploadBucket * 16;
  const u64 end_offset64 = bucket_offset64 + 16;
  const std::size_t checked_packet_size =
      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  if (!dma_packet_snapshot || (chain_offset & 15) != 0 ||
      bucket_offset64 > std::numeric_limits<u32>::max() ||
      end_offset64 > std::numeric_limits<u32>::max() ||
      !range_is_valid(bucket_offset64, 16, checked_packet_size)) {
    out.malformed_transfers = 1;
    return out;
  }

  const u32 bucket_offset = static_cast<u32>(bucket_offset64);
  const u32 end_offset = static_cast<u32>(end_offset64);
  CheckedDmaFollower dma(dma_packet_snapshot, dma_packet_snapshot_size,
                         bucket_offset);
  bool inside_animator = false;
  while (dma.offset() != end_offset) {
    if (out.transfer_count == out.transfers.size()) {
      out.malformed_transfers++;
      return out;
    }

    CheckedTransfer transfer;
    if (!dma.read(&transfer)) {
      out.malformed_transfers++;
      return out;
    }
    if (!record_transfer(transfer, bucket_offset, &out)) {
      out.malformed_transfers++;
      return out;
    }

    const VifCode vif0(transfer.vif0);

    if (inside_animator) {
      if (vif0.kind == VifCode::Kind::PC_PORT &&
          vif0.immediate < out.opcode_counts.size()) {
        out.opcode_counts[vif0.immediate]++;
      }
      if (vif0.kind == VifCode::Kind::PC_PORT &&
          vif0.immediate == kStartAnimatorArray) {
        out.malformed_transfers++;
        return out;
      }
      if (vif0.kind == VifCode::Kind::PC_PORT &&
          vif0.immediate == kFinishAnimatorArray) {
        if (!is_exact_animator_finish(transfer)) {
          out.malformed_transfers++;
          return out;
        }
        inside_animator = false;
        continue;
      }
      out.animator_body_transfers++;
      out.animator_payload_bytes += transfer.size_bytes;
      continue;
    }

    if (vif0.kind == VifCode::Kind::PC_PORT &&
        vif0.immediate == kStartAnimatorArray) {
      if (!is_exact_animator_start(transfer)) {
        out.malformed_transfers++;
        return out;
      }
      out.present = true;
      out.animator_arrays++;
      out.opcode_counts[kStartAnimatorArray]++;
      inside_animator = true;
      continue;
    }

    if (transfer.tag.qwc == 8) {
      out.present = true;
      out.eye_markers++;
      continue;
    }
    if (is_inert(transfer)) {
      if (transfer.tag.kind == DmaTag::Kind::CNT || transfer.tag.kind == DmaTag::Kind::NEXT) {
        out.inert_transfers++;
      } else {
        out.present = true;
        out.other_transfers++;
      }
      continue;
    }
    out.present = true;
    if (is_ordinary_descriptor(transfer)) {
      out.ordinary_descriptors++;
    } else {
      out.other_transfers++;
    }
  }

  if (inside_animator) {
    out.malformed_transfers++;
    return out;
  }

  out.valid = true;
  out.classification = classify(out);
  return out;
}

}  // namespace metal_renderer
