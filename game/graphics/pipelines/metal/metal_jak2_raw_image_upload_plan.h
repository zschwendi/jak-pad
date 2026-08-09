#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "common/common_types.h"

namespace metal_renderer {

constexpr u32 kJak2RawImageUploadBucket = 318;
constexpr u16 kJak2RawImageWidth = 512;
constexpr u16 kJak2RawImageHeight = 416;
constexpr u32 kJak2RawImageDestination = 0;
constexpr u8 kJak2RawImagePsmct32 = 0;

struct Jak2RawImageUploadPlan {
  std::vector<u32> rgba;
  u32 source_offset = 0;
  u16 width = 0;
  u16 height = 0;
  u32 destination = 0;
  u8 format = 0;
  u8 force_to_gpu = 0;
  bool present = false;
};

/*!
 * Parse the exact public Jak II `draw-raw-image` bucket grammar. The returned
 * plan owns the source pixels because PC_PORT metadata points outside the DMA
 * graph copied by FixedChunkDmaCopier. A canonical strict-empty bucket returns
 * an absent plan. Any other transfer order, image shape, format, destination,
 * or Direct packet shape is rejected before host texture mutation.
 */
std::optional<Jak2RawImageUploadPlan> plan_jak2_raw_image_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size);

}  // namespace metal_renderer
