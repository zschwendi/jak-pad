#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "common/common_types.h"

#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_plan.h"

namespace metal_renderer {

constexpr u32 kJak2SpriteTextureUploadBucket = 312;
constexpr std::size_t kJak2SpriteTextureUploadMaximumGroups = 2;

struct Jak2SpriteTextureUploadPlan {
  std::array<Jak2Bucket4OrdinaryUploadPlan, kJak2SpriteTextureUploadMaximumGroups> uploads = {};
  std::size_t upload_count = 0;
  bool present = false;
};

/*!
 * Parse the exact live Jak II TEX_ALL_SPRITE DMA grammar. The Direct payloads are validated only
 * as inert transfer shapes because the matching GL TextureUploadHandler has add_direct disabled.
 * The returned plan owns each ordered ordinary descriptor and page header and retains no host
 * pointers. A canonical empty bucket-table entry returns an absent plan. Malformed packets,
 * alternate transfer shapes, and any present form with a count other than one or two ordinary
 * uploads return no plan.
 */
std::optional<Jak2SpriteTextureUploadPlan> plan_jak2_sprite_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size);

}  // namespace metal_renderer
