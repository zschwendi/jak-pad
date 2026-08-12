#pragma once

#include <cstddef>
#include <optional>

#include "common/common_types.h"

#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"

namespace metal_renderer {

constexpr u32 kJak2Pris2TextureUploadBucket = 228;

enum class Jak2Pris2Bucket228Variant : u8 {
  OrdinaryOnly,
  OneEyeChunk,
};

struct Jak2Pris2Bucket228Plan {
  u32 bucket_id = kJak2Pris2TextureUploadBucket;
  Jak2Pris2Bucket228Variant variant = Jak2Pris2Bucket228Variant::OrdinaryOnly;
  Jak2Bucket4OrdinaryUploadPlan ordinary;
  Jak2PrisEyeChunkPlan eye_chunk;
  u32 direct_reset_transfer_index = 0;
  u32 direct_reset_relative_tag_offset = 0;
  u32 terminal_transfer_index = 0;
  u32 terminal_relative_tag_offset = 0;
  u64 eye_slot_mask = 0;
  u64 semantic_fingerprint = 0;
};

/*!
 * Preflight the two observed PRIS2 bucket-228 envelopes without mutating renderer state: the
 * ordinary descriptor/reset form and the single different-eyes chunk form. The returned plan owns
 * the ordinary page header and the bounded eye metadata. Animator, two-chunk, absent, malformed,
 * and otherwise extended forms are rejected.
 */
std::optional<Jak2Pris2Bucket228Plan> plan_jak2_pris2_bucket228(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture = nullptr,
    Jak2PrisEyeTextureUploadRejection* out_rejection = nullptr);

/*!
 * Compare independently preflighted live and copied plans by owned semantics. DMA relocation is
 * deliberately ignored; transfer ordering and all source-written semantics must still match.
 */
bool jak2_pris2_bucket228_plans_match(const Jak2Pris2Bucket228Plan& live,
                                      const Jak2Pris2Bucket228Plan& copied);

}  // namespace metal_renderer
