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
 * Inspect the general Jak II DEBUG_NO_ZBUF1 Direct bucket for zero or one exact
 * public `pc-upload-raw-texture` PC_PORT 12/16/13 sequence. Direct transfers
 * before and after that optional sequence remain renderer-owned. The returned
 * plan owns live source pixels because the metadata points outside the DMA
 * graph copied by FixedChunkDmaCopier. Unexpected, malformed, or duplicate
 * PC_PORT sequences are rejected before host texture mutation.
 */
std::optional<Jak2RawImageUploadPlan> plan_jak2_raw_image_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size);

}  // namespace metal_renderer
