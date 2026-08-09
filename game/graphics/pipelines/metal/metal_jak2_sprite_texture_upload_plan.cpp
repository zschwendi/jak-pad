#include "game/graphics/pipelines/metal/metal_jak2_sprite_texture_upload_plan.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "common/dma/dma.h"
#include "common/goal_constants.h"

namespace metal_renderer {
namespace {

constexpr std::size_t kMaximumTransfers = kJak2MapTextureUploadDiagnosticMaximumTransfers;
constexpr u32 kPcPortVif = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
constexpr u32 kFlushaVif = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
constexpr u32 kDirectVif = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
constexpr s64 kObservedUploadMode = -1;

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
  const u8* data = nullptr;
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
  CheckedDmaFollower(const u8* memory,
                     std::size_t memory_size,
                     u32 start_offset,
                     Jak2MapTextureUploadDiagnostic* diagnostic)
      : m_memory(memory),
        m_memory_size(std::min<std::size_t>(memory_size, EE_MAIN_MEM_SIZE)),
        m_offset(start_offset),
        m_diagnostic(diagnostic) {}

  u32 offset() const { return m_offset; }

  bool read(CheckedTransfer* out) {
    if (!out || !m_memory || m_visited_count == m_visited.size() ||
        !range_is_valid(m_offset, 16, m_memory_size) || (m_offset & 15) != 0 ||
        already_visited(m_offset)) {
      return false;
    }
    m_visited[m_visited_count++] = m_offset;

    const u32 tag_offset = m_offset;
    const DmaTag tag(read_unaligned<u64>(m_memory + tag_offset));
    if (m_diagnostic) {
      if (m_diagnostic->transfer_count == m_diagnostic->transfers.size()) {
        return false;
      }
      auto& transfer = m_diagnostic->transfers[m_diagnostic->transfer_count++];
      transfer.tag_offset = tag_offset;
      transfer.payload_bytes = static_cast<u32>(tag.qwc) * 16;
      transfer.vif0 = read_unaligned<u32>(m_memory + tag_offset + 8);
      transfer.vif1 = read_unaligned<u32>(m_memory + tag_offset + 12);
      transfer.qwc = tag.qwc;
      transfer.tag_kind = static_cast<u8>(tag.kind);
      transfer.spr = tag.spr;
    }
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
      default:
        return false;
    }
    if (!range_is_valid(data_offset, payload_bytes, m_memory_size) ||
        next_offset > std::numeric_limits<u32>::max() || (next_offset & 15) != 0 ||
        !range_is_valid(next_offset, 0, m_memory_size)) {
      return false;
    }

    out->tag = tag;
    out->tag_offset = tag_offset;
    out->data = m_memory + data_offset;
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
  std::array<u32, kMaximumTransfers> m_visited = {};
  std::size_t m_visited_count = 0;
  Jak2MapTextureUploadDiagnostic* m_diagnostic = nullptr;
};

void reject(Jak2MapTextureUploadDiagnostic* diagnostic,
            Jak2MapTextureUploadRejectionStage stage,
            u32 failure_offset,
            std::size_t transfer_count_before_read) {
  if (!diagnostic) {
    return;
  }
  diagnostic->rejection_stage = stage;
  diagnostic->failure_offset = failure_offset;
  if (diagnostic->transfer_count > transfer_count_before_read) {
    diagnostic->rejected_transfer = static_cast<u32>(diagnostic->transfer_count - 1);
  }
}

bool read_transfer(CheckedDmaFollower* dma,
                   CheckedTransfer* transfer,
                   Jak2MapTextureUploadDiagnostic* diagnostic,
                   Jak2MapTextureUploadRejectionStage failure_stage) {
  if (!dma) {
    reject(diagnostic, failure_stage, 0, diagnostic ? diagnostic->transfer_count : 0);
    return false;
  }
  const u32 read_offset = dma->offset();
  const std::size_t transfer_count = diagnostic ? diagnostic->transfer_count : 0;
  if (!dma->read(transfer)) {
    reject(diagnostic, failure_stage, read_offset, transfer_count);
    return false;
  }
  return true;
}

void reject_transfer(Jak2MapTextureUploadDiagnostic* diagnostic,
                     Jak2MapTextureUploadRejectionStage stage,
                     const CheckedTransfer& transfer) {
  const std::size_t transfer_count = diagnostic && diagnostic->transfer_count
                                         ? diagnostic->transfer_count - 1
                                         : 0;
  reject(diagnostic, stage, transfer.tag_offset, transfer_count);
}

bool is_inert_next(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::NEXT && transfer.size_bytes == 0 &&
         transfer.vif0 == 0 && transfer.vif1 == 0;
}

bool is_strict_empty(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.size_bytes == 0 && transfer.vif0 == 0 &&
         transfer.vif1 == 0;
}

bool is_direct(const CheckedTransfer& transfer, u32 expected_vif0, u16 expected_qwc) {
  return transfer.tag.kind == DmaTag::Kind::CNT &&
         transfer.size_bytes == static_cast<u32>(expected_qwc) * 16 &&
         transfer.vif0 == expected_vif0 && transfer.vif1 == (kDirectVif | expected_qwc);
}

bool is_ordinary_descriptor(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.size_bytes == 16 &&
         transfer.vif0 == kPcPortVif && transfer.vif1 == 3;
}

bool page_header_is_valid(const u8* live_ee_memory,
                          std::size_t live_ee_memory_size,
                          u64 page_offset) {
  const std::size_t checked_size = std::min<std::size_t>(live_ee_memory_size, EE_MAIN_MEM_SIZE);
  if (!live_ee_memory || page_offset == 0 ||
      !range_is_valid(page_offset, sizeof(GoalTexturePageHeaderLayout), checked_size)) {
    return false;
  }
  const auto header = read_unaligned<GoalTexturePageHeaderLayout>(live_ee_memory + page_offset);
  if (header.length < 0) {
    return false;
  }
  const u64 texture_pointer_bytes = static_cast<u64>(header.length) * sizeof(u32);
  return range_is_valid(page_offset + sizeof(header), texture_pointer_bytes, checked_size);
}

bool read_boundary(CheckedDmaFollower* dma, Jak2MapTextureUploadDiagnostic* diagnostic) {
  CheckedTransfer transfer;
  if (!read_transfer(dma, &transfer, diagnostic,
                     Jak2MapTextureUploadRejectionStage::GroupBoundary)) {
    return false;
  }
  if (!is_inert_next(transfer)) {
    reject_transfer(diagnostic, Jak2MapTextureUploadRejectionStage::GroupBoundary, transfer);
    return false;
  }
  return true;
}

bool accept_descriptor_and_boundary(CheckedDmaFollower* dma,
                                    const CheckedTransfer& descriptor,
                                    const u8* live_ee_memory,
                                    std::size_t live_ee_memory_size,
                                    Jak2Bucket4OrdinaryUploadPlan* out,
                                    Jak2MapTextureUploadDiagnostic* diagnostic) {
  if (!out) {
    reject_transfer(diagnostic, Jak2MapTextureUploadRejectionStage::OrdinaryDescriptor,
                    descriptor);
    return false;
  }
  if (!is_ordinary_descriptor(descriptor)) {
    reject_transfer(diagnostic, Jak2MapTextureUploadRejectionStage::OrdinaryDescriptor,
                    descriptor);
    return false;
  }

  const u64 page_offset = read_unaligned<u64>(descriptor.data);
  const s64 mode = read_unaligned<s64>(descriptor.data + 8);
  if (mode != kObservedUploadMode ||
      !page_header_is_valid(live_ee_memory, live_ee_memory_size, page_offset)) {
    reject_transfer(diagnostic, Jak2MapTextureUploadRejectionStage::OrdinaryContents,
                    descriptor);
    return false;
  }

  out->page_offset = page_offset;
  out->mode = mode;
  std::memcpy(out->page_header.data(), live_ee_memory + page_offset, out->page_header.size());
  return read_boundary(dma, diagnostic);
}

bool read_descriptor_and_boundary(CheckedDmaFollower* dma,
                                  const u8* live_ee_memory,
                                  std::size_t live_ee_memory_size,
                                  Jak2Bucket4OrdinaryUploadPlan* out,
                                  Jak2MapTextureUploadDiagnostic* diagnostic) {
  CheckedTransfer descriptor;
  if (!read_transfer(dma, &descriptor, diagnostic,
                     Jak2MapTextureUploadRejectionStage::OrdinaryDescriptor)) {
    return false;
  }
  return accept_descriptor_and_boundary(dma, descriptor, live_ee_memory, live_ee_memory_size, out,
                                        diagnostic);
}

}  // namespace

static std::optional<Jak2GroupedTextureUploadPlan> plan_grouped_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    u32 bucket_id,
    std::size_t maximum_groups,
    bool allow_map_descriptor_first_groups,
    Jak2MapTextureUploadDiagnostic* diagnostic) {
  if (diagnostic) {
    *diagnostic = {};
  }
  const u64 bucket_offset64 = static_cast<u64>(chain_offset) + bucket_id * 16;
  const u64 end_offset64 = bucket_offset64 + 16;
  const std::size_t checked_packet_size =
      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  if (!dma_packet_snapshot || bucket_offset64 > std::numeric_limits<u32>::max() ||
      end_offset64 > std::numeric_limits<u32>::max() ||
      !range_is_valid(bucket_offset64, 16, checked_packet_size)) {
    reject(diagnostic, Jak2MapTextureUploadRejectionStage::BucketRange,
           bucket_offset64 <= std::numeric_limits<u32>::max() ? static_cast<u32>(bucket_offset64)
                                                               : 0,
           0);
    return std::nullopt;
  }

  CheckedDmaFollower dma(dma_packet_snapshot, dma_packet_snapshot_size,
                         static_cast<u32>(bucket_offset64), diagnostic);
  CheckedTransfer first;
  if (!read_transfer(&dma, &first, diagnostic,
                     Jak2MapTextureUploadRejectionStage::BucketEntry)) {
    return std::nullopt;
  }
  if (is_strict_empty(first)) {
    if (dma.offset() != static_cast<u32>(end_offset64)) {
      reject_transfer(diagnostic, Jak2MapTextureUploadRejectionStage::BucketEnd, first);
      return std::nullopt;
    }
    return Jak2GroupedTextureUploadPlan{};
  }
  if (!is_inert_next(first)) {
    reject_transfer(diagnostic, Jak2MapTextureUploadRejectionStage::InitialBoundary, first);
    return std::nullopt;
  }

  Jak2GroupedTextureUploadPlan plan;
  CheckedTransfer tail_or_group;
  if (!read_transfer(&dma, &tail_or_group, diagnostic,
                     Jak2MapTextureUploadRejectionStage::GroupOrTail)) {
    return std::nullopt;
  }

  if (allow_map_descriptor_first_groups && is_ordinary_descriptor(tail_or_group)) {
    while (is_ordinary_descriptor(tail_or_group)) {
      if (plan.upload_count == maximum_groups) {
        reject_transfer(diagnostic, Jak2MapTextureUploadRejectionStage::GroupLimit,
                        tail_or_group);
        return std::nullopt;
      }
      if (!accept_descriptor_and_boundary(&dma, tail_or_group, live_ee_memory,
                                          live_ee_memory_size,
                                          &plan.uploads[plan.upload_count], diagnostic)) {
        return std::nullopt;
      }
      plan.upload_count++;
      if (dma.offset() == static_cast<u32>(end_offset64)) {
        plan.present = true;
        return plan;
      }
      if (!read_transfer(&dma, &tail_or_group, diagnostic,
                         Jak2MapTextureUploadRejectionStage::GroupOrTail)) {
        return std::nullopt;
      }
    }
    if (!is_direct(tail_or_group, 0, 2)) {
      reject_transfer(diagnostic, Jak2MapTextureUploadRejectionStage::GroupOrTail,
                      tail_or_group);
      return std::nullopt;
    }
  }

  while (is_direct(tail_or_group, 0, 2)) {
    if (plan.upload_count == maximum_groups) {
      reject_transfer(diagnostic, Jak2MapTextureUploadRejectionStage::GroupLimit, tail_or_group);
      return std::nullopt;
    }
    if (!read_descriptor_and_boundary(&dma, live_ee_memory, live_ee_memory_size,
                                      &plan.uploads[plan.upload_count], diagnostic)) {
      return std::nullopt;
    }
    plan.upload_count++;
    if (!read_transfer(&dma, &tail_or_group, diagnostic,
                       Jak2MapTextureUploadRejectionStage::GroupOrTail)) {
      return std::nullopt;
    }
  }

  if (plan.upload_count == 0) {
    reject_transfer(diagnostic, Jak2MapTextureUploadRejectionStage::GroupOrTail, tail_or_group);
    return std::nullopt;
  }
  if (!is_direct(tail_or_group, kFlushaVif, 10)) {
    reject_transfer(diagnostic, Jak2MapTextureUploadRejectionStage::Tail, tail_or_group);
    return std::nullopt;
  }
  CheckedTransfer final_boundary;
  if (!read_transfer(&dma, &final_boundary, diagnostic,
                     Jak2MapTextureUploadRejectionStage::FinalBoundary)) {
    return std::nullopt;
  }
  if (!is_inert_next(final_boundary)) {
    reject_transfer(diagnostic, Jak2MapTextureUploadRejectionStage::FinalBoundary,
                    final_boundary);
    return std::nullopt;
  }
  if (dma.offset() != static_cast<u32>(end_offset64)) {
    reject(diagnostic, Jak2MapTextureUploadRejectionStage::BucketEnd, dma.offset(),
           diagnostic ? diagnostic->transfer_count : 0);
    return std::nullopt;
  }
  plan.present = true;
  return plan;
}

std::optional<Jak2SpriteTextureUploadPlan> plan_jak2_sprite_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size) {
  return plan_grouped_texture_upload(
      dma_packet_snapshot, dma_packet_snapshot_size, chain_offset, live_ee_memory,
      live_ee_memory_size, kJak2SpriteTextureUploadBucket,
      kJak2SpriteTextureUploadMaximumGroups, false, nullptr);
}

std::optional<Jak2MapTextureUploadPlan> plan_jak2_map_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size) {
  return plan_jak2_map_texture_upload(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                                      live_ee_memory, live_ee_memory_size, nullptr);
}

std::optional<Jak2MapTextureUploadPlan> plan_jak2_map_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2MapTextureUploadDiagnostic* diagnostic) {
  return plan_grouped_texture_upload(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                                     live_ee_memory, live_ee_memory_size,
                                     kJak2MapTextureUploadBucket,
                                     kJak2MapTextureUploadMaximumGroups, true, diagnostic);
}

const char* jak2_map_texture_upload_rejection_stage_name(
    Jak2MapTextureUploadRejectionStage stage) {
  switch (stage) {
    case Jak2MapTextureUploadRejectionStage::None:
      return "none";
    case Jak2MapTextureUploadRejectionStage::BucketRange:
      return "bucket-range";
    case Jak2MapTextureUploadRejectionStage::BucketEntry:
      return "bucket-entry";
    case Jak2MapTextureUploadRejectionStage::InitialBoundary:
      return "initial-boundary";
    case Jak2MapTextureUploadRejectionStage::GroupOrTail:
      return "group-or-tail";
    case Jak2MapTextureUploadRejectionStage::GroupLimit:
      return "group-limit";
    case Jak2MapTextureUploadRejectionStage::OrdinaryDescriptor:
      return "ordinary-descriptor";
    case Jak2MapTextureUploadRejectionStage::OrdinaryContents:
      return "ordinary-contents";
    case Jak2MapTextureUploadRejectionStage::GroupBoundary:
      return "group-boundary";
    case Jak2MapTextureUploadRejectionStage::Tail:
      return "tail";
    case Jak2MapTextureUploadRejectionStage::FinalBoundary:
      return "final-boundary";
    case Jak2MapTextureUploadRejectionStage::BucketEnd:
      return "bucket-end";
  }
  return "unknown";
}

}  // namespace metal_renderer
