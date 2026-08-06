#include "game/graphics/pipelines/metal/metal_jak2_sprite_texture_upload_plan.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "common/dma/dma.h"
#include "common/goal_constants.h"

namespace metal_renderer {
namespace {

constexpr std::size_t kMaximumTransfers = 3 * kJak2SpriteTextureUploadMaximumGroups + 3;
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
    m_visited[m_visited_count++] = m_offset;

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
      default:
        return false;
    }
    if (!range_is_valid(data_offset, payload_bytes, m_memory_size) ||
        next_offset > std::numeric_limits<u32>::max() || (next_offset & 15) != 0 ||
        !range_is_valid(next_offset, 0, m_memory_size)) {
      return false;
    }

    out->tag = tag;
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
};

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

bool read_transfer(CheckedDmaFollower* dma, CheckedTransfer* transfer) {
  return dma && dma->read(transfer);
}

bool read_boundary(CheckedDmaFollower* dma) {
  CheckedTransfer transfer;
  return read_transfer(dma, &transfer) && is_inert_next(transfer);
}

bool read_descriptor_and_boundary(CheckedDmaFollower* dma,
                                  const u8* live_ee_memory,
                                  std::size_t live_ee_memory_size,
                                  Jak2Bucket4OrdinaryUploadPlan* out) {
  CheckedTransfer descriptor;
  if (!out || !read_transfer(dma, &descriptor) || !is_ordinary_descriptor(descriptor)) {
    return false;
  }

  const u64 page_offset = read_unaligned<u64>(descriptor.data);
  const s64 mode = read_unaligned<s64>(descriptor.data + 8);
  if (mode != kObservedUploadMode ||
      !page_header_is_valid(live_ee_memory, live_ee_memory_size, page_offset)) {
    return false;
  }

  out->page_offset = page_offset;
  out->mode = mode;
  std::memcpy(out->page_header.data(), live_ee_memory + page_offset, out->page_header.size());
  return read_boundary(dma);
}

}  // namespace

std::optional<Jak2SpriteTextureUploadPlan> plan_jak2_sprite_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size) {
  const u64 bucket_offset64 = static_cast<u64>(chain_offset) + kJak2SpriteTextureUploadBucket * 16;
  const u64 end_offset64 = bucket_offset64 + 16;
  const std::size_t checked_packet_size =
      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  if (!dma_packet_snapshot || bucket_offset64 > std::numeric_limits<u32>::max() ||
      end_offset64 > std::numeric_limits<u32>::max() ||
      !range_is_valid(bucket_offset64, 16, checked_packet_size)) {
    return std::nullopt;
  }

  CheckedDmaFollower dma(dma_packet_snapshot, dma_packet_snapshot_size,
                         static_cast<u32>(bucket_offset64));
  CheckedTransfer first;
  if (!read_transfer(&dma, &first)) {
    return std::nullopt;
  }
  if (is_strict_empty(first)) {
    if (dma.offset() != static_cast<u32>(end_offset64)) {
      return std::nullopt;
    }
    return Jak2SpriteTextureUploadPlan{};
  }
  if (!is_inert_next(first)) {
    return std::nullopt;
  }

  Jak2SpriteTextureUploadPlan plan;
  CheckedTransfer tail_or_group;
  if (!read_transfer(&dma, &tail_or_group)) {
    return std::nullopt;
  }

  while (is_direct(tail_or_group, 0, 2)) {
    if (plan.upload_count == kJak2SpriteTextureUploadMaximumGroups) {
      return std::nullopt;
    }
    if (!read_descriptor_and_boundary(&dma, live_ee_memory, live_ee_memory_size,
                                      &plan.uploads[plan.upload_count])) {
      return std::nullopt;
    }
    plan.upload_count++;
    if (!read_transfer(&dma, &tail_or_group)) {
      return std::nullopt;
    }
  }

  if (plan.upload_count == 0 || !is_direct(tail_or_group, kFlushaVif, 10) ||
      !read_boundary(&dma) || dma.offset() != static_cast<u32>(end_offset64)) {
    return std::nullopt;
  }
  plan.present = true;
  return plan;
}

}  // namespace metal_renderer
