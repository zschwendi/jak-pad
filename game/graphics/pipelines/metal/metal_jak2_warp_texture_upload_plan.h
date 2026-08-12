#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "common/common_types.h"

#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_plan.h"

namespace metal_renderer {

constexpr u32 kJak2WarpTextureUploadBucket = 316;
constexpr std::size_t kJak2WarpTextureUploadMaximumGroups = 7;
constexpr std::size_t kJak2WarpTextureUploadMaximumTransfers =
    3 * kJak2WarpTextureUploadMaximumGroups + 3;

enum class Jak2WarpTextureUploadVariant : u8 {
  Absent,
  Ordinary,
};

struct Jak2WarpTextureUploadTransferSemantics {
  u32 payload_bytes = 0;
  u32 vif0 = 0;
  u32 vif1 = 0;
  u16 qwc = 0;
  u8 tag_kind = 0;
  u64 payload_fingerprint = 0;
};

struct Jak2WarpTextureUploadPlan {
  u32 bucket_id = kJak2WarpTextureUploadBucket;
  Jak2WarpTextureUploadVariant variant = Jak2WarpTextureUploadVariant::Absent;
  std::array<Jak2WarpTextureUploadTransferSemantics, kJak2WarpTextureUploadMaximumTransfers>
      transfers = {};
  std::size_t transfer_count = 0;
  u64 total_payload_bytes = 0;
  std::array<Jak2Bucket4OrdinaryUploadPlan, kJak2WarpTextureUploadMaximumGroups> uploads = {};
  std::size_t upload_count = 0;
  u64 semantic_fingerprint = 0;
};

/*!
 * Preflight either exact empty Jak II bucket-316 form (zero-CNT or zero-NEXT directly to the bucket
 * boundary) or the observed grouped ordinary-upload grammar. A present plan contains one through
 * seven ordered groups, owns every validated texture-page header, and has exactly
 * 160 + 48 * upload_count payload bytes. Animator, mixed, extra, and malformed forms fail closed.
 * DMA tag addresses are followed but not retained as semantics, allowing an independently
 * relocated copy to be compared with the live plan.
 */
std::optional<Jak2WarpTextureUploadPlan> plan_jak2_warp_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size);

/*!
 * Compare independently parsed live and copied plans by owned semantics. Relocatable DMA tag
 * addresses and packet offsets are ignored; transfer payloads, descriptors, and page headers must
 * match exactly. The zero-CNT and boundary-linked zero-NEXT forms are equivalent absence semantics.
 */
bool jak2_warp_texture_upload_plans_match(const Jak2WarpTextureUploadPlan& live,
                                          const Jak2WarpTextureUploadPlan& copied);

}  // namespace metal_renderer
