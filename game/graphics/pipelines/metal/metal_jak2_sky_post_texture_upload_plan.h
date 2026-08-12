#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "common/common_types.h"

namespace metal_renderer {

constexpr u32 kJak2SkyPostTextureUploadBucket = 309;
constexpr std::size_t kJak2SkyPostTexturePageHeaderBytes = 128;
constexpr std::size_t kJak2SkyPostTextureUploadTransferCount = 6;

enum class Jak2SkyPostTextureUploadVariant : u8 {
  Absent,
  Present,
};

struct Jak2SkyPostTransferSemantics {
  u32 payload_bytes = 0;
  u32 vif0 = 0;
  u32 vif1 = 0;
  u16 qwc = 0;
  u8 tag_kind = 0;
  u64 payload_fingerprint = 0;
};

struct Jak2SkyPostTextureUploadPlan {
  u32 bucket_id = kJak2SkyPostTextureUploadBucket;
  Jak2SkyPostTextureUploadVariant variant =
      Jak2SkyPostTextureUploadVariant::Absent;
  std::array<Jak2SkyPostTransferSemantics,
             kJak2SkyPostTextureUploadTransferCount>
      transfers = {};
  std::size_t transfer_count = 0;
  u64 total_payload_bytes = 0;
  u64 page_offset = 0;
  s64 mode = 0;
  std::array<u8, kJak2SkyPostTexturePageHeaderBytes> page_header = {};
  u64 semantic_fingerprint = 0;
};

/*!
 * Preflight an exact empty bucket tag or the source-shaped six-transfer,
 * 208-byte sky-post upload. The returned plan owns the validated 128-byte texture-page
 * header and contains no host pointers. Tag addresses and packet offsets are
 * followed for bounds checking but are not retained as semantics, so an
 * independently relocated DMA copy can be compared with the live plan.
 */
std::optional<Jak2SkyPostTextureUploadPlan> plan_jak2_sky_post_texture_upload(
    const u8 *dma_packet_snapshot, std::size_t dma_packet_snapshot_size,
    u32 chain_offset, u32 bucket_id, const u8 *live_ee_memory,
    std::size_t live_ee_memory_size);

/*!
 * Compare independently parsed live and copied plans. DMA addresses and offsets
 * are deliberately ignored; every nonrelocatable transfer payload, descriptor
 * field, and owned page-header byte must match.
 */
bool jak2_sky_post_texture_upload_plans_match(
    const Jak2SkyPostTextureUploadPlan &live,
    const Jak2SkyPostTextureUploadPlan &copied);

} // namespace metal_renderer
