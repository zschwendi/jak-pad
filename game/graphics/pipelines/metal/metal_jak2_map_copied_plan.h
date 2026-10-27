#pragma once

#include <cstddef>
#include <optional>

#include "common/common_types.h"

#include "game/graphics/pipelines/metal/metal_jak2_map_texture_upload_plan.h"

namespace metal_renderer {

constexpr std::size_t kJak2MapCopiedParityMaximumTransfers = 64;

enum class Jak2MapCopiedParityRejection : u8 {
  None,
  LivePlanNotMixed,
  OwnedCount,
  LiveWalk,
  CopiedWalk,
  TransferCount,
  TagMismatch,
  PayloadMismatch,
  CopiedReplan,
  PlanMismatch,
  MarkerOffset,
};

const char* jak2_map_copied_parity_rejection_name(Jak2MapCopiedParityRejection rejection);

struct Jak2MapCopiedParityResult {
  Jak2MapCopiedParityRejection rejection = Jak2MapCopiedParityRejection::None;
  u32 transfer_ordinal = kJak2MapMixedNoTransfer;
  Jak2MapMixedDiagnostic replan_diagnostic;
  // Present only when rejection == None. Identical to the live plan except marker_tag_offsets,
  // which index the copied snapshot.
  std::optional<Jak2MapTextureUploadTypedPlan> plan;
};

/*!
 * Prove a live owned mixed TEX_ALL_MAP plan is equivalent to a FixedChunkDmaCopier relocation of
 * the same chain. Walks bucket 319 -> bucket 320 entry in both snapshots (CNT addr 0 / NEXT only,
 * <= 64 transfers) comparing every tag bit except NEXT ADDR, both VIF words and every payload
 * byte, then replans the copied snapshot against live EE and compares all owned values. Ordinary
 * (non-mixed) live plans are rejected so the caller keeps its legacy route. Cost: replan copies
 * the four 512x208 layers (~426 KiB) again.
 */
Jak2MapCopiedParityResult validate_jak2_map_copied_plan(
    const Jak2MapTextureUploadTypedPlan& live_plan,
    const u8* live_dma_snapshot,
    std::size_t live_dma_snapshot_size,
    u32 live_chain_offset,
    const u8* copied_dma_snapshot,
    std::size_t copied_dma_snapshot_size,
    u32 copied_chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size);

}  // namespace metal_renderer
