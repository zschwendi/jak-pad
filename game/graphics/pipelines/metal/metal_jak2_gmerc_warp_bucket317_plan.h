#pragma once

#include <cstddef>
#include <optional>

#include "common/common_types.h"

namespace metal_renderer {

constexpr u32 kJak2GmercWarpBucket = 317;
constexpr u32 kJak2GmercWarpMaximumTransfers = 1024;
constexpr u64 kJak2GmercWarpMaximumPayloadBytes = 16 * 1024 * 1024;
constexpr u32 kJak2GmercWarpMaximumFragments = 10000;
constexpr u32 kJak2GmercWarpMaximumVertices = 500000 - 1;
constexpr u32 kJak2GmercWarpMaximumAdgifs = 10000;
constexpr u32 kJak2GmercWarpNoTransferIndex = 0xffffffff;

enum class Jak2GmercWarpBucket317Variant : u8 {
  Absent,
  SetupOnly,
  Fragments,
};

enum class Jak2GmercWarpBucket317RejectReason : u8 {
  None,
  InvalidInput,
  DmaChain,
  TransferLimit,
  PayloadLimit,
  SetupGrammar,
  SetupOnlyMarker,
  FragmentGrammar,
  ContinuedPositionGrammar,
  ContinuedMscalGrammar,
  TerminatorGrammar,
  BoundaryGrammar,
  TrailingTransfer,
};

struct Jak2GmercWarpBucket317Plan {
  u32 bucket_id = kJak2GmercWarpBucket;
  Jak2GmercWarpBucket317Variant variant = Jak2GmercWarpBucket317Variant::Absent;
  u32 transfer_count = 0;
  u32 fragment_count = 0;
  u32 continued_fragment_count = 0;
  u32 vertex_count = 0;
  u32 adgif_count = 0;
  u64 payload_bytes = 0;
  u64 semantic_fingerprint = 0;
};

/*
 * Passively preflight Jak II's GMERC_WARP bucket without retaining DMA, EE, renderer, or Metal
 * pointers. The accepted forms are the exact empty bucket plus the bounded Jak II Generic2
 * envelope consumed by Generic2::process_dma_jak2: fixed setup, optional source-shaped
 * fragments (including its continued-fragment form), and the fixed FLUSHA/DIRECT terminator.
 * A setup that reaches the bucket boundary before fragments is also classified explicitly.
 * Unknown tags, VIF shapes, trailing transfers, source-limit overflows, and malformed chains fail
 * closed. A returned plan classifies Absent, SetupOnly, or Fragments; std::nullopt is the
 * Malformed classification. Rejection outputs report the first typed failure and offending
 * transfer index without retaining packet payloads.
 */
std::optional<Jak2GmercWarpBucket317Plan> plan_jak2_gmerc_warp_bucket317(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    Jak2GmercWarpBucket317RejectReason* out_rejection = nullptr,
    u32* out_rejection_transfer_index = nullptr);

const char* jak2_gmerc_warp_bucket317_reject_reason_name(
    Jak2GmercWarpBucket317RejectReason reason);

/* Compare independent live and copied plans by owned semantics, ignoring DMA relocation. */
bool jak2_gmerc_warp_bucket317_plans_match(const Jak2GmercWarpBucket317Plan& live,
                                           const Jak2GmercWarpBucket317Plan& copied);

}  // namespace metal_renderer
