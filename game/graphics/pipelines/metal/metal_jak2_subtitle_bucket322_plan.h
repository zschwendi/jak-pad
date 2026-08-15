#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "common/common_types.h"

namespace metal_renderer {

constexpr u32 kJak2SubtitleBucket322 = 322;
constexpr std::size_t kJak2SubtitleMaximumTransfers = 4096;
constexpr std::size_t kJak2SubtitleMaximumImageUploads = 64;
constexpr u32 kJak2SubtitleNoTransferIndex = 0xffffffff;

enum class Jak2SubtitleBucket322Variant : u8 {
  Absent,
  OpaqueDirect,
  HudSpriteDirect,
  SubtitleImage,
  Mixed,
};

enum class Jak2SubtitleBucket322RejectReason : u8 {
  None,
  Chain,
  TransferLimit,
  TransferEnvelope,
  UploadGrammar,
  UploadMetadata,
  ImageDrawGrammar,
};

struct Jak2SubtitleImageUploadPlan {
  u32 clut_source_offset = 0;
  u32 image_source_offset = 0;
  u16 width = 0;
  u16 height = 0;
  u32 start_transfer_index = 0;
  u32 start_relative_tag_offset = 0;
};

struct Jak2SubtitleBucket322Plan {
  Jak2SubtitleBucket322Variant variant = Jak2SubtitleBucket322Variant::Absent;
  std::array<Jak2SubtitleImageUploadPlan, kJak2SubtitleMaximumImageUploads> image_uploads = {};
  std::size_t image_upload_count = 0;
  u32 transfer_count = 0;
  u32 linker_transfers = 0;
  u32 direct_transfers = 0;
  u32 opaque_direct_transfers = 0;
  u32 hud_sprite_pairs = 0;
  u64 direct_payload_bytes = 0;
  u64 semantic_fingerprint = 0;
};

/*!
 * Passively classify bucket 322 without mutating textures or rendering. The bounded grammar is
 * sourced from the Jak II GOAL producers:
 *
 * - arbitrary Direct packets from the dynamic MIPS2C draw-string-asm text output,
 * - the exact hud-sprite qwc-6/qwc-13 pair used by the intro, and
 * - the exact PC_PORT 12/16/16/13 subtitle-image upload followed by its qwc-7/qwc-6/qwc-6 draw.
 *
 * Multiple producer chains may be linked into the bucket in any order. Unknown PC_PORT records,
 * non-CNT payloads, malformed known packets, and inputs beyond the fixed bounds fail closed. This
 * plan is intentionally passive: opaque text grammar and live upload contents are not exhaustive,
 * so it is not an execution contract.
 */
std::optional<Jak2SubtitleBucket322Plan> plan_jak2_subtitle_bucket322(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    Jak2SubtitleBucket322RejectReason* out_rejection = nullptr,
    u32* out_rejection_transfer_index = nullptr);

bool jak2_subtitle_bucket322_plans_match(const Jak2SubtitleBucket322Plan& live,
                                          const Jak2SubtitleBucket322Plan& copied);

}  // namespace metal_renderer
