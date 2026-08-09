#include "game/graphics/pipelines/metal/metal_jak2_raw_image_upload_plan.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "common/dma/dma_chain_read.h"
#include "common/goal_constants.h"

#include "game/graphics/pipelines/metal/metal_jak2_chain_validation.h"

namespace metal_renderer {
namespace {

constexpr u32 vif(VifCode::Kind kind, u16 immediate = 0) {
  return (static_cast<u32>(kind) << 24) | immediate;
}

template <typename T>
T read_unaligned(const u8* data) {
  T result;
  std::memcpy(&result, data, sizeof(result));
  return result;
}

bool range_is_valid(u64 offset, u64 size, std::size_t memory_size) {
  return offset <= memory_size && size <= memory_size - offset;
}

struct CheckedTransfer {
  DmaTag tag{0};
  DmaTransfer data;
};

bool read_transfer(DmaFollower* dma, CheckedTransfer* out) {
  if (!dma || !out || dma->ended()) {
    return false;
  }
  out->tag = dma->current_tag();
  out->data = dma->read_and_advance();
  return !out->tag.spr;
}

bool is_cnt(const CheckedTransfer& transfer, u16 qwc, u32 vif0, u32 vif1) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.tag.addr == 0 &&
         transfer.tag.qwc == qwc && transfer.data.size_bytes == static_cast<u32>(qwc) * 16 &&
         transfer.data.vif0() == vif0 && transfer.data.vif1() == vif1;
}

bool contains_pc_port(const CheckedTransfer& transfer) {
  return transfer.data.vifcode0().kind == VifCode::Kind::PC_PORT ||
         transfer.data.vifcode1().kind == VifCode::Kind::PC_PORT;
}

}  // namespace

std::optional<Jak2RawImageUploadPlan> plan_jak2_raw_image_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size) {
  const std::size_t packet_size =
      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  const std::size_t live_size = std::min<std::size_t>(live_ee_memory_size, EE_MAIN_MEM_SIZE);
  const u64 bucket_offset64 =
      static_cast<u64>(chain_offset) + static_cast<u64>(kJak2RawImageUploadBucket) * 16;
  const u64 bucket_end64 = bucket_offset64 + 16;
  if (!dma_packet_snapshot || bucket_end64 > packet_size ||
      bucket_end64 > std::numeric_limits<u32>::max() ||
      !validate_jak2_metal_dma_chain(dma_packet_snapshot, packet_size, chain_offset)) {
    return std::nullopt;
  }

  try {
    const u32 bucket_offset = static_cast<u32>(bucket_offset64);
    const u32 bucket_end = static_cast<u32>(bucket_end64);
    DmaFollower dma(dma_packet_snapshot, bucket_offset, packet_size);
    Jak2RawImageUploadPlan plan;
    std::size_t transfer_count = 0;
    while (dma.current_tag_offset() != bucket_end) {
      if (++transfer_count > 4096) {
        return std::nullopt;
      }
      CheckedTransfer start;
      if (!read_transfer(&dma, &start)) {
        return std::nullopt;
      }
      if (!contains_pc_port(start)) {
        continue;
      }
      if (plan.present ||
          !is_cnt(start, 0, vif(VifCode::Kind::PC_PORT, 12), 0)) {
        return std::nullopt;
      }

      CheckedTransfer upload;
      CheckedTransfer finish;
      if (!read_transfer(&dma, &upload) ||
          !is_cnt(upload, 1, vif(VifCode::Kind::PC_PORT, 16), 0) ||
          !read_transfer(&dma, &finish) ||
          !is_cnt(finish, 0, vif(VifCode::Kind::PC_PORT, 13), 0)) {
        return std::nullopt;
      }
      transfer_count += 2;

      plan.source_offset = read_unaligned<u32>(upload.data.data);
      plan.width = read_unaligned<u16>(upload.data.data + 4);
      plan.height = read_unaligned<u16>(upload.data.data + 6);
      plan.destination = read_unaligned<u32>(upload.data.data + 8);
      plan.format = upload.data.data[12];
      plan.force_to_gpu = upload.data.data[13];
      const u64 pixel_count = static_cast<u64>(plan.width) * plan.height;
      const u64 pixel_bytes = pixel_count * sizeof(u32);
      if (!live_ee_memory || plan.width != kJak2RawImageWidth ||
          plan.height != kJak2RawImageHeight ||
          plan.destination != kJak2RawImageDestination ||
          plan.format != kJak2RawImagePsmct32 || plan.force_to_gpu != 1 ||
          pixel_count > std::numeric_limits<std::size_t>::max() ||
          !range_is_valid(plan.source_offset, pixel_bytes, live_size)) {
        return std::nullopt;
      }

      plan.rgba.resize(static_cast<std::size_t>(pixel_count));
      std::memcpy(plan.rgba.data(), live_ee_memory + plan.source_offset,
                  static_cast<std::size_t>(pixel_bytes));
      plan.present = true;
    }
    return plan;
  } catch (...) {
    return std::nullopt;
  }
}

static std::optional<u32> count_jak2_raw_image_upload_markers(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset) {
  const std::size_t packet_size =
      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  const u64 bucket_offset64 =
      static_cast<u64>(chain_offset) + static_cast<u64>(kJak2RawImageUploadBucket) * 16;
  const u64 bucket_end64 = bucket_offset64 + 16;
  if (!dma_packet_snapshot || bucket_end64 > packet_size ||
      bucket_end64 > std::numeric_limits<u32>::max()) {
    return std::nullopt;
  }

  try {
    const u32 bucket_offset = static_cast<u32>(bucket_offset64);
    const u32 bucket_end = static_cast<u32>(bucket_end64);
    DmaFollower dma(dma_packet_snapshot, bucket_offset, packet_size);
    u32 marker_count = 0;
    std::size_t transfer_count = 0;
    while (dma.current_tag_offset() != bucket_end) {
      if (++transfer_count > 4096) {
        return std::nullopt;
      }
      CheckedTransfer transfer;
      if (!read_transfer(&dma, &transfer)) {
        return std::nullopt;
      }
      const auto vif0 = transfer.data.vifcode0();
      if (vif0.kind == VifCode::Kind::PC_PORT && vif0.immediate == 12) {
        ++marker_count;
      }
    }
    return marker_count;
  } catch (...) {
    return std::nullopt;
  }
}

bool copied_jak2_raw_image_upload_markers_match_plan(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    bool plan_present) {
  const auto marker_count = count_jak2_raw_image_upload_markers(
      dma_packet_snapshot, dma_packet_snapshot_size, chain_offset);
  return marker_count && *marker_count == (plan_present ? 1u : 0u);
}

}  // namespace metal_renderer
