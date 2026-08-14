#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "common/common_types.h"

#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"

namespace metal_renderer {

inline constexpr std::array<u32, 6> kJak2Pris2TextureUploadBuckets = {
    224, 228, 232, 236, 240, 244};
inline constexpr std::array<u32, 6> kJak2Pris2MercBuckets = {
    225, 229, 233, 237, 241, 245};

// Retained for the existing passive 228/229 ABI fields and focused fixtures.
constexpr u32 kJak2Pris2TextureUploadBucket = 228;

enum class Jak2Pris2Bucket228Variant : u8 {
  Absent,
  OrdinaryOnly,
  OneEyeChunk,
  TwoEyeChunks,
};

struct Jak2Pris2Bucket228Plan {
  u32 bucket_id = kJak2Pris2TextureUploadBucket;
  Jak2Pris2Bucket228Variant variant = Jak2Pris2Bucket228Variant::Absent;
  Jak2Bucket4OrdinaryUploadPlan ordinary;
  std::array<Jak2PrisEyeChunkPlan, kJak2PrisEyeMaximumChunks> chunks = {};
  std::size_t chunk_count = 0;
  u32 direct_reset_transfer_index = 0;
  u32 direct_reset_relative_tag_offset = 0;
  u32 terminal_transfer_index = 0;
  u32 terminal_relative_tag_offset = 0;
  u64 eye_slot_mask = 0;
  u64 semantic_fingerprint = 0;
};

/*!
 * Preflight the exact source PRIS2 envelope for any dynamic level slot without mutating renderer
 * state: absence, ordinary descriptor/reset, or ordinary plus one or two different-eyes chunks.
 * The returned plan owns the ordinary page header and bounded eye metadata. Animator, malformed,
 * and otherwise extended forms are rejected.
 */
std::optional<Jak2Pris2Bucket228Plan> plan_jak2_pris2_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture = nullptr,
    Jak2PrisEyeTextureUploadRejection* out_rejection = nullptr);

// Compatibility wrapper for the original bucket-228 fixture/API.
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
bool jak2_pris2_texture_upload_plans_match(const Jak2Pris2Bucket228Plan& live,
                                           const Jak2Pris2Bucket228Plan& copied);

inline bool jak2_pris2_bucket228_plans_match(const Jak2Pris2Bucket228Plan& live,
                                             const Jak2Pris2Bucket228Plan& copied) {
  return jak2_pris2_texture_upload_plans_match(live, copied);
}

/*!
 * Adapt an independently preflighted PRIS2 plan to the existing PRIS eye renderer contract.
 * This is the only seam that broadens that renderer beyond the six per-level PRIS producers.
 */
Jak2PrisEyeTextureUploadPlan adapt_jak2_pris2_to_pris_eye_plan(
    const Jak2Pris2Bucket228Plan& source);

inline Jak2PrisEyeTextureUploadPlan adapt_jak2_pris2_bucket228_to_pris_eye_plan(
    const Jak2Pris2Bucket228Plan& source) {
  return adapt_jak2_pris2_to_pris_eye_plan(source);
}

/*! Reject overlapping eye publications across ordinary PRIS, common PRIS, and all PRIS2 plans. */
bool jak2_pris_eye_slot_masks_are_disjoint(
    const Jak2PrisEyeTextureUploadPlan* per_level_plans,
    std::size_t per_level_plan_count,
    const Jak2CommonPrisTextureUploadPlan& common_plan,
    const Jak2Pris2Bucket228Plan* pris2_plans,
    std::size_t pris2_plan_count);

}  // namespace metal_renderer
