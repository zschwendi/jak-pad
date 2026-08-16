#include "game/graphics/pipelines/metal/metal_jak2_warp_texture_upload_plan.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "common/goal_constants.h"

namespace metal_renderer {
namespace {

constexpr u8 kDmaCnt = 1;
constexpr u8 kDmaNext = 2;
constexpr u32 kPcPortVif = 8u << 24;
constexpr u32 kFlushaVif = 19u << 24;
constexpr u32 kDirectVif = 80u << 24;
constexpr s64 kObservedUploadMode = -1;
constexpr u64 kFnvOffsetBasis = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;
constexpr std::size_t kGoalTexturePageLengthOffset = 12;

struct CheckedTransfer {
  u32 payload_bytes = 0;
  u32 vif0 = 0;
  u32 vif1 = 0;
  u16 qwc = 0;
  u8 kind = 0;
  const u8* payload = nullptr;
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

void hash_bytes(u64* hash, const void* bytes, std::size_t size) {
  const auto* input = static_cast<const u8*>(bytes);
  for (std::size_t i = 0; i < size; ++i) {
    *hash ^= input[i];
    *hash *= kFnvPrime;
  }
}

template <typename T>
void hash_value(u64* hash, const T& value) {
  hash_bytes(hash, &value, sizeof(value));
}

class CheckedDmaFollower {
 public:
  CheckedDmaFollower(const u8* memory, std::size_t memory_size, u32 offset)
      : m_memory(memory), m_memory_size(memory_size), m_offset(offset) {}

  u32 offset() const { return m_offset; }

  bool read(CheckedTransfer* out) {
    if (!out || !m_memory || m_visited_count == m_visited.size() || (m_offset & 15) != 0 ||
        !range_is_valid(m_offset, 16, m_memory_size) || already_visited(m_offset)) {
      return false;
    }

    const u64 raw_tag = read_unaligned<u64>(m_memory + m_offset);
    const u16 qwc = static_cast<u16>(raw_tag);
    const u8 kind = static_cast<u8>((raw_tag >> 28) & 7);
    const u32 address = static_cast<u32>((raw_tag >> 32) & 0x7fffffff);
    const bool spr = (raw_tag >> 63) != 0;
    const bool reserved = (raw_tag & 0x8fff0000ull) != 0;
    if (spr || reserved) {
      return false;
    }

    const u64 payload_bytes = static_cast<u64>(qwc) * 16;
    const u64 data_offset = static_cast<u64>(m_offset) + 16;
    u64 next_offset = 0;
    if (kind == kDmaCnt) {
      if (address != 0) {
        return false;
      }
      next_offset = data_offset + payload_bytes;
    } else if (kind == kDmaNext) {
      if (address == 0) {
        return false;
      }
      next_offset = address;
    } else {
      return false;
    }

    if (!range_is_valid(data_offset, payload_bytes, m_memory_size) ||
        next_offset > std::numeric_limits<u32>::max() || (next_offset & 15) != 0 ||
        !range_is_valid(next_offset, 0, m_memory_size)) {
      return false;
    }

    m_visited[m_visited_count++] = m_offset;
    out->payload_bytes = static_cast<u32>(payload_bytes);
    out->vif0 = read_unaligned<u32>(m_memory + m_offset + 8);
    out->vif1 = read_unaligned<u32>(m_memory + m_offset + 12);
    out->qwc = qwc;
    out->kind = kind;
    out->payload = m_memory + data_offset;
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
  std::array<u32, kJak2WarpTextureUploadMaximumTransfers> m_visited = {};
  std::size_t m_visited_count = 0;
};

bool is_strict_empty(const CheckedTransfer& transfer) {
  return transfer.kind == kDmaCnt && transfer.qwc == 0 && transfer.payload_bytes == 0 &&
         transfer.vif0 == 0 && transfer.vif1 == 0;
}

bool is_inert_next(const CheckedTransfer& transfer) {
  return transfer.kind == kDmaNext && transfer.qwc == 0 && transfer.payload_bytes == 0 &&
         transfer.vif0 == 0 && transfer.vif1 == 0;
}

bool is_gs_texflush_setup(const CheckedTransfer& transfer) {
  return transfer.kind == kDmaCnt && transfer.qwc == 2 && transfer.payload_bytes == 32 &&
         transfer.vif0 == 0 && transfer.vif1 == (kDirectVif | 2);
}

bool is_ordinary_descriptor(const CheckedTransfer& transfer) {
  return transfer.kind == kDmaCnt && transfer.qwc == 1 && transfer.payload_bytes == 16 &&
         transfer.vif0 == kPcPortVif && transfer.vif1 == 3;
}

bool is_direct_reset(const CheckedTransfer& transfer) {
  return transfer.kind == kDmaCnt && transfer.qwc == 10 && transfer.payload_bytes == 160 &&
         transfer.vif0 == kFlushaVif && transfer.vif1 == (kDirectVif | 10);
}

Jak2WarpTextureUploadTransferSemantics semantics_for(const CheckedTransfer& transfer) {
  Jak2WarpTextureUploadTransferSemantics result;
  result.payload_bytes = transfer.payload_bytes;
  result.vif0 = transfer.vif0;
  result.vif1 = transfer.vif1;
  result.qwc = transfer.qwc;
  result.tag_kind = transfer.kind;
  if (transfer.payload_bytes != 0) {
    u64 fingerprint = kFnvOffsetBasis;
    hash_bytes(&fingerprint, transfer.payload, transfer.payload_bytes);
    result.payload_fingerprint = fingerprint;
  }
  return result;
}

bool transfer_semantics_match(const Jak2WarpTextureUploadTransferSemantics& a,
                              const Jak2WarpTextureUploadTransferSemantics& b) {
  return a.payload_bytes == b.payload_bytes && a.vif0 == b.vif0 && a.vif1 == b.vif1 &&
         a.qwc == b.qwc && a.tag_kind == b.tag_kind &&
         a.payload_fingerprint == b.payload_fingerprint;
}

bool page_header_is_valid(const u8* live_ee_memory,
                          std::size_t live_ee_memory_size,
                          u64 page_offset) {
  const std::size_t checked_size = std::min<std::size_t>(live_ee_memory_size, EE_MAIN_MEM_SIZE);
  if (!live_ee_memory || page_offset == 0 ||
      !range_is_valid(page_offset, kJak2Bucket4OrdinaryPageHeaderBytes, checked_size)) {
    return false;
  }
  const s32 length =
      read_unaligned<s32>(live_ee_memory + page_offset + kGoalTexturePageLengthOffset);
  if (length < 0) {
    return false;
  }
  const u64 pointer_bytes = static_cast<u64>(length) * sizeof(u32);
  return range_is_valid(page_offset + kJak2Bucket4OrdinaryPageHeaderBytes, pointer_bytes,
                        checked_size);
}

bool upload_semantics_match(const Jak2Bucket4OrdinaryUploadPlan& a,
                            const Jak2Bucket4OrdinaryUploadPlan& b) {
  return a.page_offset == b.page_offset && a.mode == b.mode && a.page_header == b.page_header;
}

u64 plan_fingerprint(const Jak2WarpTextureUploadPlan& plan) {
  u64 fingerprint = kFnvOffsetBasis;
  hash_value(&fingerprint, plan.bucket_id);
  hash_value(&fingerprint, plan.variant);
  hash_value(&fingerprint, plan.transfer_count);
  hash_value(&fingerprint, plan.total_payload_bytes);
  hash_value(&fingerprint, plan.upload_count);
  if (plan.variant == Jak2WarpTextureUploadVariant::Absent) {
    return fingerprint;
  }
  for (std::size_t i = 0; i < plan.transfer_count; ++i) {
    const auto& transfer = plan.transfers[i];
    hash_value(&fingerprint, transfer.payload_bytes);
    hash_value(&fingerprint, transfer.vif0);
    hash_value(&fingerprint, transfer.vif1);
    hash_value(&fingerprint, transfer.qwc);
    hash_value(&fingerprint, transfer.tag_kind);
    hash_value(&fingerprint, transfer.payload_fingerprint);
  }
  for (std::size_t i = 0; i < plan.upload_count; ++i) {
    const auto& upload = plan.uploads[i];
    hash_value(&fingerprint, upload.page_offset);
    hash_value(&fingerprint, upload.mode);
    hash_bytes(&fingerprint, upload.page_header.data(), upload.page_header.size());
  }
  return fingerprint;
}

}  // namespace

std::optional<Jak2WarpTextureUploadPlan> plan_jak2_warp_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size) {
  const u64 bucket_offset64 = static_cast<u64>(chain_offset) + static_cast<u64>(bucket_id) * 16;
  const u64 end_offset64 = bucket_offset64 + 16;
  const std::size_t checked_packet_size =
      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  if (!dma_packet_snapshot || bucket_id != kJak2WarpTextureUploadBucket ||
      (chain_offset & 15) != 0 || bucket_offset64 > std::numeric_limits<u32>::max() ||
      end_offset64 > std::numeric_limits<u32>::max() ||
      !range_is_valid(bucket_offset64, 16, checked_packet_size)) {
    return std::nullopt;
  }

  const u32 bucket_offset = static_cast<u32>(bucket_offset64);
  const u32 end_offset = static_cast<u32>(end_offset64);
  CheckedDmaFollower dma(dma_packet_snapshot, checked_packet_size, bucket_offset);
  Jak2WarpTextureUploadPlan plan;
  auto read_transfer = [&dma, &plan](CheckedTransfer* transfer) {
    if (plan.transfer_count == plan.transfers.size() || !dma.read(transfer)) {
      return false;
    }
    plan.transfers[plan.transfer_count++] = semantics_for(*transfer);
    plan.total_payload_bytes += transfer->payload_bytes;
    return true;
  };

  CheckedTransfer transfer;
  if (!read_transfer(&transfer)) {
    return std::nullopt;
  }
  if ((is_strict_empty(transfer) || is_inert_next(transfer)) && dma.offset() == end_offset) {
    plan.semantic_fingerprint = plan_fingerprint(plan);
    return plan;
  }
  if (!is_inert_next(transfer)) {
    return std::nullopt;
  }

  plan.variant = Jak2WarpTextureUploadVariant::Ordinary;
  if (!read_transfer(&transfer)) {
    return std::nullopt;
  }
  while (is_gs_texflush_setup(transfer)) {
    if (plan.upload_count == kJak2WarpTextureUploadMaximumGroups) {
      return std::nullopt;
    }

    CheckedTransfer descriptor;
    CheckedTransfer boundary;
    if (!read_transfer(&descriptor) || !is_ordinary_descriptor(descriptor) ||
        !read_transfer(&boundary) || !is_inert_next(boundary)) {
      return std::nullopt;
    }

    auto& upload = plan.uploads[plan.upload_count];
    upload.page_offset = read_unaligned<u64>(descriptor.payload);
    upload.mode = read_unaligned<s64>(descriptor.payload + sizeof(u64));
    if (upload.mode != kObservedUploadMode ||
        !page_header_is_valid(live_ee_memory, live_ee_memory_size, upload.page_offset)) {
      return std::nullopt;
    }
    std::memcpy(upload.page_header.data(), live_ee_memory + upload.page_offset,
                upload.page_header.size());
    ++plan.upload_count;

    if (!read_transfer(&transfer)) {
      return std::nullopt;
    }
  }

  if (plan.upload_count == 0 || !is_direct_reset(transfer)) {
    return std::nullopt;
  }
  CheckedTransfer final_boundary;
  if (!read_transfer(&final_boundary) || !is_inert_next(final_boundary) ||
      dma.offset() != end_offset || plan.total_payload_bytes != 160 + 48 * plan.upload_count) {
    return std::nullopt;
  }

  plan.semantic_fingerprint = plan_fingerprint(plan);
  return plan;
}

bool jak2_warp_texture_upload_plans_match(const Jak2WarpTextureUploadPlan& live,
                                          const Jak2WarpTextureUploadPlan& copied) {
  if (live.bucket_id != kJak2WarpTextureUploadBucket ||
      copied.bucket_id != kJak2WarpTextureUploadBucket || live.variant != copied.variant ||
      live.transfer_count != copied.transfer_count ||
      live.total_payload_bytes != copied.total_payload_bytes ||
      live.upload_count != copied.upload_count ||
      live.semantic_fingerprint != copied.semantic_fingerprint) {
    return false;
  }
  if (live.variant == Jak2WarpTextureUploadVariant::Absent) {
    return live.transfer_count == 1 && live.total_payload_bytes == 0 && live.upload_count == 0;
  }
  for (std::size_t i = 0; i < live.transfer_count; ++i) {
    if (!transfer_semantics_match(live.transfers[i], copied.transfers[i])) {
      return false;
    }
  }
  for (std::size_t i = 0; i < live.upload_count; ++i) {
    if (!upload_semantics_match(live.uploads[i], copied.uploads[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace metal_renderer
