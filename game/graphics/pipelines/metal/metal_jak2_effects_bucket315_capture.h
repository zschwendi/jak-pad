#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"

#include "game/graphics/pipelines/metal/metal_jak2_effects_bucket315_plan.h"

namespace metal_renderer {

// Source Lightning maximum: lightning-draw-all emits at most 128 live controls (engines.gc), each
// drawn in exactly 2 passes, and points-to-draw <= 96 splits each pass into at most 3 chunks of
// <= 40 segments (lightning.gc). Each chunk is 3 transfers after the 8-transfer fixed envelope.
constexpr std::size_t kJak2EffectsBucket315SourceMaximumControls = 128;
constexpr std::size_t kJak2EffectsBucket315SourcePassesPerControl = 2;
constexpr std::size_t kJak2EffectsBucket315SourceMaximumChunksPerPass = 3;
constexpr std::size_t kJak2EffectsBucket315SourceMaximumFragments =
    kJak2EffectsBucket315SourceMaximumControls * kJak2EffectsBucket315SourcePassesPerControl *
    kJak2EffectsBucket315SourceMaximumChunksPerPass;
constexpr std::size_t kJak2EffectsBucket315MaximumTransfers =
    8 + 3 * kJak2EffectsBucket315SourceMaximumFragments;
static_assert(kJak2EffectsBucket315MaximumTransfers == 2312);

enum class Jak2EffectsBucket315CaptureClass : u8 {
  Malformed,
  Absent,
  Lightning,
  Other,
};

// Diagnostic-only cause of a Malformed capture. It never changes acceptance.
enum class Jak2EffectsBucket315MalformedReason : u8 {
  None,
  InvalidBucket,
  TransferBudget,
  DmaRead,
  TagBeforeBucket,
  AdgifMismatch,
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
 * `Other` means that traversal was well-formed but did not satisfy the exact source Lightning
 * grammar and state. The host accepts only Absent or Lightning before copying the chain.
 */
struct Jak2EffectsBucket315Capture {
  bool valid = false;
  bool present = false;
  Jak2EffectsBucket315CaptureClass classification = Jak2EffectsBucket315CaptureClass::Malformed;
  Jak2EffectsBucket315MalformedReason malformed_reason = Jak2EffectsBucket315MalformedReason::None;
  // Offset of the rejected tag relative to the bucket, valid for DmaRead and TagBeforeBucket.
  u32 malformed_relative_tag_offset = 0;
  u32 bucket_id = kJak2EffectsBucket;
  std::array<Jak2EffectsBucket315TransferMetadata, kJak2EffectsBucket315MaximumTransfers>
      transfers = {};
  u32 transfer_count = 0;
  u32 fragment_count = 0;
  u32 vertex_count = 0;
  u32 adgif_count = 0;
  u32 draw_count = 0;
  u64 total_payload_bytes = 0;
  u64 semantic_fingerprint = 0;
};

/*
 * Caller-owned walk scratch for one capture. It holds live source pointers, so it is kept apart
 * from the payload-free capture and is only meaningful during a capture call. Rows beyond the
 * walked transfer count are never read, so it needs no per-call clearing.
 */
struct Jak2EffectsBucket315CaptureScratch {
  std::array<const u8*, kJak2EffectsBucket315MaximumTransfers> payloads = {};
  std::array<u32, kJak2EffectsBucket315MaximumTransfers> visited_tag_offsets = {};
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
  u8 last_malformed_reason = static_cast<u8>(Jak2EffectsBucket315MalformedReason::None);
  u32 last_malformed_transfer_count = 0;
};

/*!
 * Read and fingerprint the live DMA chain before any rendering or host mutation. The capture and
 * scratch are caller-owned (about 74 KB and 28 KB at the source bound), so callers keep them off
 * the stack. A null scratch leaves the capture Malformed; a null capture is ignored.
 */
void capture_jak2_effects_bucket315(const u8* dma_packet_snapshot,
                                    std::size_t dma_packet_snapshot_size,
                                    u32 chain_offset,
                                    u32 bucket_id,
                                    Jak2EffectsBucket315Capture* out_capture,
                                    Jak2EffectsBucket315CaptureScratch* scratch);

/*! Compare independent observations without retaining either packet or their relocatable offsets. */
bool jak2_effects_bucket315_captures_match(const Jak2EffectsBucket315Capture& live,
                                           const Jak2EffectsBucket315Capture& copied);

/*! Aggregate only numeric, bounded runtime telemetry from a passive capture. */
void observe_jak2_effects_bucket315_capture(Jak2EffectsBucket315Telemetry* telemetry,
                                            const Jak2EffectsBucket315Capture& capture);

}  // namespace metal_renderer
