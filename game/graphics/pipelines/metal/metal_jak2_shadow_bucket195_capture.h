#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"
#include "game/graphics/pipelines/metal/metal_jak2_shadow_bucket195_limits.h"

namespace metal_renderer {

constexpr u32 kJak2ShadowBucket195 = 195;
constexpr std::size_t kJak2ShadowBucket195MaximumTransfers =
    kJak2ShadowBucket195SharedMaximumTransfers;
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

// Production form: resets and fills caller-owned storage in place every call. Null `out` returns.
void capture_jak2_shadow_bucket195(const u8* dma_packet_snapshot,
                                   std::size_t dma_packet_snapshot_size,
                                   u32 chain_offset,
                                   u32 bucket_id,
                                   Jak2ShadowBucket195Capture* out);

/*
 * DIAGNOSTIC ONLY. Never supplies render/acceptance plans and never changes the normal-path
 * 781-transfer capture or plan limits. Walks raw DMA (existing CheckedDmaFollower semantics) up to
 * a hard bound to classify why the strict capture stopped. Raw traversal validity here does NOT
 * imply strict shadow grammar validity.
 */
constexpr u32 kJak2ShadowBucket195DiagnosticMaximumTransfers = 1024;
constexpr u32 kJak2ShadowBucket195DiagnosticTailRows = 16;

enum class Jak2ShadowBucket195FollowerReject : u8 {
  None,
  InvalidArguments,
  VisitedCapacity,           // follower's fixed visited table is full
  TagOutOfSpanOrMisaligned,  // tag offset outside snapshot or not 16-byte aligned
  Cycle,                     // tag offset already visited
  InvalidTagOrPayloadSpan,   // SPR/unsupported kind/bad addr, or payload/next out of span
};

enum class Jak2ShadowBucket195DiagnosticLimit : u8 {
  None,
  TransferTable,     // transfer kJak2ShadowBucket195MaximumTransfers + 1 would be needed
  TransferPayload,   // one payload > 64 KiB
  TotalPayload,      // cumulative payload > 1 MiB
  UnpackCount,       // cumulative V4_32 or V4_8 unpack count > 32 Ki
};

enum class Jak2ShadowBucket195DiagnosticTermination : u8 {
  Boundary,          // raw walk reached next bucket tag
  DmaReadFailed,     // follower rejected tag: out-of-span/misaligned/unsupported kind/cycle
  BeforeBucket,      // tag offset below bucket start
  WalkBound,         // hard diagnostic bound reached without boundary
  InvalidArguments,
};

struct Jak2ShadowBucket195DiagnosticRow {
  u32 index = 0;
  u32 tag_offset = 0;
  u32 vif0 = 0;
  u32 vif1 = 0;
  u16 qwc = 0;
  u8 tag_kind = 0;
  bool boundary_after = false;
};

struct Jak2ShadowBucket195Diagnostic {
  Jak2ShadowBucket195DiagnosticTermination termination =
      Jak2ShadowBucket195DiagnosticTermination::InvalidArguments;
  u32 termination_index = 0;  // transfer index at which the walk terminated
  Jak2ShadowBucket195FollowerReject follower_reject = Jak2ShadowBucket195FollowerReject::None;
  Jak2ShadowBucket195DiagnosticLimit first_limit = Jak2ShadowBucket195DiagnosticLimit::None;
  u32 first_limit_transfer_index = 0;
  u32 transfers_walked = 0;
  u32 v4_32_unpack_count = 0;
  u32 v4_8_unpack_count = 0;
  u32 max_transfer_payload_bytes = 0;
  u64 total_payload_bytes = 0;
  u32 tail_row_count = 0;  // rows starting at index kJak2ShadowBucket195MaximumTransfers - 1
  std::array<Jak2ShadowBucket195DiagnosticRow, kJak2ShadowBucket195DiagnosticTailRows> tail = {};
};

Jak2ShadowBucket195Diagnostic diagnose_jak2_shadow_bucket195(const u8* dma_packet_snapshot,
                                                             std::size_t dma_packet_snapshot_size,
                                                             u32 chain_offset);

}  // namespace metal_renderer
