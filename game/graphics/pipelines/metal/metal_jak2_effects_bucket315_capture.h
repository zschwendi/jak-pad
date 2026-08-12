#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"

#include "game/graphics/pipelines/metal/metal_jak2_effects_bucket315_plan.h"

namespace metal_renderer {

// This is a diagnostic budget, deliberately smaller than the source Lightning maximum. Hitting
// it is malformed for this passive seam; it does not claim that every source-valid packet fits.
constexpr std::size_t kJak2EffectsBucket315MaximumTransfers = 256;

enum class Jak2EffectsBucket315CaptureClass : u8 {
  Malformed,
  Absent,
  Lightning,
  Other,
};

struct Jak2EffectsBucket315TransferMetadata {
  u32 relative_tag_offset = 0;
  u32 payload_bytes = 0;
  u32 vif0 = 0;
  u32 vif1 = 0;
  u64 payload_fingerprint = 0;
  u16 qwc = 0;
  u8 tag_kind = 0;
};

/*
 * Fixed-size, payload-free observation of one live Jak II EFFECTS bucket. Every payload is hashed
 * while the source chain is still stable; no source bytes, pointers, strings, or paths survive.
 * `Other` means that traversal was well-formed but did not satisfy the source Lightning shape.
 * Every classification is diagnostic-only: a later exact typed parser must separately prove any
 * execution eligibility.
 */
struct Jak2EffectsBucket315Capture {
  bool valid = false;
  bool present = false;
  Jak2EffectsBucket315CaptureClass classification = Jak2EffectsBucket315CaptureClass::Malformed;
  u32 bucket_id = kJak2EffectsBucket;
  std::array<Jak2EffectsBucket315TransferMetadata, kJak2EffectsBucket315MaximumTransfers>
      transfers = {};
  u32 transfer_count = 0;
  u32 fragment_count = 0;
  u32 vertex_count = 0;
  u64 total_payload_bytes = 0;
  u64 semantic_fingerprint = 0;
};

struct Jak2EffectsBucket315Telemetry {
  u32 captures = 0;
  u32 valid_captures = 0;
  u32 malformed_captures = 0;
  u32 absent_captures = 0;
  u32 lightning_captures = 0;
  u32 other_captures = 0;
  u64 captured_payload_bytes = 0;
  u32 last_transfer_count = 0;
  u32 last_fragment_count = 0;
  u32 last_vertex_count = 0;
  u64 last_payload_bytes = 0;
  u64 last_semantic_fingerprint = 0;
  u8 last_classification = static_cast<u8>(Jak2EffectsBucket315CaptureClass::Malformed);
};

/*! Read and fingerprint the live DMA chain before any rendering or host mutation. */
Jak2EffectsBucket315Capture capture_jak2_effects_bucket315(const u8* dma_packet_snapshot,
                                                            std::size_t dma_packet_snapshot_size,
                                                            u32 chain_offset,
                                                            u32 bucket_id);

/*! Compare independent observations without retaining either packet or their relocatable offsets. */
bool jak2_effects_bucket315_captures_match(const Jak2EffectsBucket315Capture& live,
                                           const Jak2EffectsBucket315Capture& copied);

/*! Aggregate only numeric, bounded runtime telemetry from a passive capture. */
void observe_jak2_effects_bucket315_capture(Jak2EffectsBucket315Telemetry* telemetry,
                                            const Jak2EffectsBucket315Capture& capture);

}  // namespace metal_renderer
