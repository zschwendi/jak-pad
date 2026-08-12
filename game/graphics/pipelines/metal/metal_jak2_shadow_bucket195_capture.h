#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"

namespace metal_renderer {

constexpr u32 kJak2ShadowBucket195 = 195;
constexpr std::size_t kJak2ShadowBucket195MaximumTransfers = 256;
constexpr u64 kJak2ShadowBucket195MaximumPayloadBytes = 1 << 20;
constexpr u32 kJak2ShadowBucket195MaximumTransferPayloadBytes = 64 << 10;
constexpr u32 kJak2ShadowBucket195MaximumUnpackCount = 32 << 10;

enum class Jak2ShadowBucket195CaptureStatus : u8 {
  Absent,
  Observed,
  Malformed,
  LimitExceeded,
};

enum class Jak2ShadowBucket195TransferRole : u8 {
  Other,
  Constants,
  Mystery,
  Matrix,
  Mscalf10,
  Direct35,
  TopVertices,
  BottomVertices,
  CapIndices,
  WallIndices,
  FlushaDirect,
};

/*
 * Ordered scalar metadata for one DMA transfer. This owns no packet, pointer, string, or dynamic
 * geometry. For V4_8 index uploads, the capture reads only the four bounded trailing marker words
 * at `num * 4`; it never reads the index bytes before them.
 */
struct Jak2ShadowBucket195TransferMetadata {
  u32 payload_bytes = 0;
  u32 vif0 = 0;
  u32 vif1 = 0;
  u32 trailing_marker = 0;
  u16 qwc = 0;
  u16 vif0_immediate = 0;
  u16 vif1_immediate = 0;
  u16 unpack_address = 0;
  u16 trailing_mscalf_immediate = 0;
  u8 tag_kind = 0;
  u8 vif0_kind = 0;
  u8 vif1_kind = 0;
  u8 vif0_num = 0;
  u8 vif1_num = 0;
  u8 trailing_zero_words = 0;
  bool trailing_marker_in_bounds = false;
  bool boundary_after = false;
  Jak2ShadowBucket195TransferRole role = Jak2ShadowBucket195TransferRole::Other;
};

/*
 * Passive observation only. `Observed` deliberately says nothing about renderability or execution
 * eligibility, including for source-reachable top-only MSCALF 6 sequences.
 */
struct Jak2ShadowBucket195Capture {
  Jak2ShadowBucket195CaptureStatus status = Jak2ShadowBucket195CaptureStatus::Malformed;
  u32 bucket_id = kJak2ShadowBucket195;
  std::array<Jak2ShadowBucket195TransferMetadata, kJak2ShadowBucket195MaximumTransfers>
      transfers = {};
  u32 transfer_count = 0;
  u32 v4_32_transfer_count = 0;
  u32 v4_8_transfer_count = 0;
  u32 v4_32_unpack_count = 0;
  u32 v4_8_unpack_count = 0;
  u32 direct_transfer_count = 0;
  u64 total_payload_bytes = 0;
  u64 direct_payload_bytes = 0;
  u64 flusha_direct_payload_bytes = 0;
  u64 semantic_fingerprint = 0;
  u32 boundary_transfer_index = 0;
  u32 terminal_payload_bytes = 0;
  u16 terminal_qwc = 0;
  u8 terminal_tag_kind = 0;
  u8 terminal_vif0_kind = 0;
  u8 terminal_vif1_kind = 0;
  bool reached_boundary = false;
};

Jak2ShadowBucket195Capture capture_jak2_shadow_bucket195(const u8* dma_packet_snapshot,
                                                          std::size_t dma_packet_snapshot_size,
                                                          u32 chain_offset,
                                                          u32 bucket_id);

}  // namespace metal_renderer
