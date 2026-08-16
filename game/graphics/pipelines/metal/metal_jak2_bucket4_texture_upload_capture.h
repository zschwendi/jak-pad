#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"

namespace metal_renderer {

constexpr u32 kJak2TextureUploadBucket = 4;
constexpr std::size_t kJak2TextureAnimatorOpcodeCount = 44;

struct Jak2Bucket4TextureUploadCapture {
  bool valid = false;
  bool present = false;
  u32 total_payload_bytes = 0;
  u32 dma_transfers = 0;
  u32 payload_transfers = 0;
  u32 inert_transfers = 0;
  u32 inert_cnt_transfers = 0;
  u32 inert_next_transfers = 0;
  u32 inert_state_mask = 0;

  u32 ordinary_descriptors = 0;
  u64 ordinary_page = 0;
  s64 ordinary_mode = 0;

  u32 animator_arrays = 0;
  u32 animator_bytes = 0;
  std::array<u32, kJak2TextureAnimatorOpcodeCount> opcode_counts = {};
  s32 cloud_destination = 0;

  u32 erase_width = 0;
  u32 erase_height = 0;
  u32 erase_destination = 0;
  u64 erase_test = 0;
  u64 erase_alpha = 0;
  u64 erase_clamp = 0;
  std::array<u32, 4> erase_clear = {};

  u32 generic_source = 0;
  u16 generic_width = 0;
  u16 generic_height = 0;
  u32 generic_destination = 0;
  u8 generic_format = 0;
  u8 generic_force_to_gpu = 0;

  u32 clut_source = 0;
  u32 clut_destination = 0;
  u32 finishes = 0;

  u32 malformed_transfers = 0;
  u32 malformed_bytes = 0;
  u32 unsupported_transfers = 0;
  u32 unsupported_bytes = 0;
};

/*!
 * Inspect the Jak II TEX_LCOM_SKY_PRE (bucket 4) packet in original EE memory.
 * This is read-only diagnostics: it does not retain packet storage, dereference
 * embedded texture sources, upload textures, or run animator GPU work.
 */
Jak2Bucket4TextureUploadCapture capture_jak2_bucket4_texture_upload(
    const u8* ee_memory,
    std::size_t ee_memory_size,
    u32 chain_offset);

}  // namespace metal_renderer
