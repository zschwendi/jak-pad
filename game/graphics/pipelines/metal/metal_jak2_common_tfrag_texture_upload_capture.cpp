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
constexpr u32 kFlushaVif = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
constexpr u32 kDirectVif = static_cast<u32>(VifCode::Kind::DIRECT) << 24;

struct GoalTexturePageHeaderLayout {
  struct Segment {
    u32 block_data_ptr;
    u32 size;
    u32 destination;
  };
  u32 file_info_ptr;
  u32 name_ptr;
  u32 id;
  s32 length;
  u32 mip0_size;
  u32 size;
  Segment segments[3];
  u32 pad[16];
};
static_assert(sizeof(GoalTexturePageHeaderLayout) == kJak2Bucket4OrdinaryPageHeaderBytes);

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

bool is_exact_direct_setup(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.tag.qwc == 10 &&
         transfer.size_bytes == 160 && transfer.vif0 == kFlushaVif &&
         transfer.vif1 == (kDirectVif | 10);
}

bool is_exact_gs_setup(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.tag.qwc == 2 &&
         transfer.size_bytes == 32 && transfer.vif0 == 0 &&
         transfer.vif1 == (kDirectVif | 2);
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
  if (capture.gs_setup_transfers != 0) {
    if (capture.gs_setup_transfers == 1 && capture.direct_setup_transfers == 1 &&
        capture.ordinary_descriptors == 0 && capture.animator_arrays == 0) {
      return Jak2CommonTfragTextureUploadClass::GsSetupOnly;
    }
    return Jak2CommonTfragTextureUploadClass::EyeOrOther;
  }
  if (capture.direct_setup_transfers != 0 && capture.ordinary_descriptors == 0 &&
      capture.animator_arrays == 0) {
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

bool is_audited_tfrag_texture_upload_bucket(u32 bucket_id) {
  return bucket_id == kJak2CommonTfragTextureUploadBucket ||
         std::find(kJak2NormalTfragTextureUploadBuckets.begin(),
                   kJak2NormalTfragTextureUploadBuckets.end(), bucket_id) !=
             kJak2NormalTfragTextureUploadBuckets.end() ||
         std::find(kJak2NormalShrubTextureUploadBuckets.begin(),
                   kJak2NormalShrubTextureUploadBuckets.end(), bucket_id) !=
             kJak2NormalShrubTextureUploadBuckets.end();
}

bool is_normal_tfrag_texture_upload_bucket(u32 bucket_id) {
  return std::find(kJak2NormalTfragTextureUploadBuckets.begin(),
                   kJak2NormalTfragTextureUploadBuckets.end(), bucket_id) !=
         kJak2NormalTfragTextureUploadBuckets.end();
}

bool is_normal_shrub_texture_upload_bucket(u32 bucket_id) {
  return std::find(kJak2NormalShrubTextureUploadBuckets.begin(),
                   kJak2NormalShrubTextureUploadBuckets.end(), bucket_id) !=
         kJak2NormalShrubTextureUploadBuckets.end();
}

bool metadata_is_inert_next(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::NEXT) && transfer.qwc == 0 &&
         transfer.payload_bytes == 0 &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif0_immediate == 0 &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif1_immediate == 0;
}

bool metadata_is_strict_empty(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) && transfer.qwc == 0 &&
         transfer.payload_bytes == 0 &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif0_immediate == 0 &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif1_immediate == 0;
}

bool metadata_is_ordinary_descriptor(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) && transfer.qwc == 1 &&
         transfer.payload_bytes == 16 &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::PC_PORT) &&
         transfer.vif0_immediate == 0 &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif1_immediate == 3;
}

bool metadata_is_direct_setup(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) && transfer.qwc == 10 &&
         transfer.payload_bytes == 160 &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::FLUSHA) &&
         transfer.vif0_immediate == 0 &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::DIRECT) &&
         transfer.vif1_immediate == 10;
}

bool metadata_is_gs_setup(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) && transfer.qwc == 2 &&
         transfer.payload_bytes == 32 &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif0_immediate == 0 &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::DIRECT) &&
         transfer.vif1_immediate == 2;
}

bool page_header_is_valid(const u8* live_ee_memory,
                          std::size_t live_ee_memory_size,
                          u64 page_offset) {
  const std::size_t checked_size =
      std::min<std::size_t>(live_ee_memory_size, EE_MAIN_MEM_SIZE);
  if (!live_ee_memory || page_offset == 0 ||
      !range_is_valid(page_offset, sizeof(GoalTexturePageHeaderLayout), checked_size)) {
    return false;
  }
  const auto header =
      read_unaligned<GoalTexturePageHeaderLayout>(live_ee_memory + page_offset);
  if (header.length < 0) {
    return false;
  }
  const u64 texture_pointer_bytes = static_cast<u64>(header.length) * sizeof(u32);
  return range_is_valid(page_offset + sizeof(header), texture_pointer_bytes, checked_size);
}

}  // namespace

Jak2CommonTfragTextureUploadCapture capture_jak2_tfrag_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id) {
  Jak2CommonTfragTextureUploadCapture out;
  const u64 bucket_offset64 = static_cast<u64>(chain_offset) + bucket_id * 16;
  const u64 end_offset64 = bucket_offset64 + 16;
  const std::size_t checked_packet_size =
      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  if (!dma_packet_snapshot || !is_audited_tfrag_texture_upload_bucket(bucket_id) ||
      (chain_offset & 15) != 0 ||
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
    if (is_exact_direct_setup(transfer)) {
      out.present = true;
      out.direct_setup_transfers++;
      continue;
    }
    if (is_exact_gs_setup(transfer)) {
      out.present = true;
      out.gs_setup_transfers++;
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

std::optional<Jak2NormalTfragTextureUploadPlan> plan_jak2_normal_tfrag_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture) {
  const auto capture = capture_jak2_tfrag_texture_upload(
      dma_packet_snapshot, dma_packet_snapshot_size, chain_offset, bucket_id);
  if (out_capture) {
    *out_capture = capture;
  }
  if (!is_normal_tfrag_texture_upload_bucket(bucket_id) || !capture.valid) {
    return std::nullopt;
  }

  Jak2NormalTfragTextureUploadPlan plan;
  plan.bucket_id = bucket_id;
  if (!capture.present) {
    if (capture.classification != Jak2CommonTfragTextureUploadClass::Absent ||
        capture.transfer_count != 1 || capture.total_payload_bytes != 0 ||
        capture.inert_transfers != 1 || !metadata_is_strict_empty(capture.transfers[0])) {
      return std::nullopt;
    }
    return plan;
  }

  const bool exact_counts =
      capture.classification == Jak2CommonTfragTextureUploadClass::OrdinaryOnly &&
      capture.transfer_count == 5 && capture.total_payload_bytes == 176 &&
      capture.inert_transfers == 3 && capture.ordinary_descriptors == 1 &&
      capture.direct_setup_transfers == 1 && capture.animator_arrays == 0 &&
      capture.eye_markers == 0 && capture.other_transfers == 0 &&
      capture.malformed_transfers == 0;
  if (!exact_counts || !metadata_is_inert_next(capture.transfers[0]) ||
      !metadata_is_inert_next(capture.transfers[2]) ||
      !metadata_is_inert_next(capture.transfers[4])) {
    return std::nullopt;
  }

  const bool descriptor_first = metadata_is_ordinary_descriptor(capture.transfers[1]) &&
                                metadata_is_direct_setup(capture.transfers[3]);
  if (!descriptor_first) {
    return std::nullopt;
  }
  const auto& descriptor = capture.transfers[1];
  const u64 descriptor_tag_offset = static_cast<u64>(chain_offset) + bucket_id * 16 +
                                    descriptor.relative_tag_offset;
  const u64 descriptor_data_offset = descriptor_tag_offset + 16;
  const std::size_t checked_snapshot_size =
      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  if (!range_is_valid(descriptor_data_offset, 16, checked_snapshot_size)) {
    return std::nullopt;
  }
  const u64 page_offset =
      read_unaligned<u64>(dma_packet_snapshot + descriptor_data_offset);
  const s64 mode =
      read_unaligned<s64>(dma_packet_snapshot + descriptor_data_offset + sizeof(u64));
  if (mode != -1 || !page_header_is_valid(live_ee_memory, live_ee_memory_size, page_offset)) {
    return std::nullopt;
  }

  plan.present = true;
  plan.ordinary.page_offset = page_offset;
  plan.ordinary.mode = mode;
  std::memcpy(plan.ordinary.page_header.data(), live_ee_memory + page_offset,
              plan.ordinary.page_header.size());
  return plan;
}

std::optional<Jak2NormalShrubTextureUploadPlan> plan_jak2_normal_shrub_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    Jak2CommonTfragTextureUploadCapture* out_capture) {
  const auto capture = capture_jak2_tfrag_texture_upload(
      dma_packet_snapshot, dma_packet_snapshot_size, chain_offset, bucket_id);
  if (out_capture) {
    *out_capture = capture;
  }
  if (!is_normal_shrub_texture_upload_bucket(bucket_id) || !capture.valid) {
    return std::nullopt;
  }

  Jak2NormalShrubTextureUploadPlan plan;
  plan.bucket_id = bucket_id;
  if (!capture.present) {
    if (capture.classification != Jak2CommonTfragTextureUploadClass::Absent ||
        capture.transfer_count != 1 || capture.total_payload_bytes != 0 ||
        capture.inert_transfers != 1 || !metadata_is_strict_empty(capture.transfers[0])) {
      return std::nullopt;
    }
    return plan;
  }

  const bool exact_counts =
      capture.classification == Jak2CommonTfragTextureUploadClass::GsSetupOnly &&
      capture.transfer_count == 5 && capture.total_payload_bytes == 192 &&
      capture.inert_transfers == 3 && capture.ordinary_descriptors == 0 &&
      capture.direct_setup_transfers == 1 && capture.gs_setup_transfers == 1 &&
      capture.animator_arrays == 0 && capture.eye_markers == 0 &&
      capture.other_transfers == 0 && capture.malformed_transfers == 0;
  if (!exact_counts || !metadata_is_inert_next(capture.transfers[0]) ||
      !metadata_is_gs_setup(capture.transfers[1]) ||
      !metadata_is_inert_next(capture.transfers[2]) ||
      !metadata_is_direct_setup(capture.transfers[3]) ||
      !metadata_is_inert_next(capture.transfers[4])) {
    return std::nullopt;
  }

  plan.present = true;
  return plan;
}

Jak2CommonTfragTextureUploadCapture capture_jak2_common_tfrag_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset) {
  return capture_jak2_tfrag_texture_upload(dma_packet_snapshot, dma_packet_snapshot_size,
                                           chain_offset, kJak2CommonTfragTextureUploadBucket);
}

}  // namespace metal_renderer
