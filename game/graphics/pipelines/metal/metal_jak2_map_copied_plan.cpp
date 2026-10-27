#include "game/graphics/pipelines/metal/metal_jak2_map_copied_plan.h"

#include <cstring>
#include <vector>

namespace metal_renderer {
namespace {

constexpr u32 kCnt = 1;
constexpr u32 kNext = 2;
constexpr u32 kAddrMask = 0x7fffffffu;  // tag bits 32..62; bit 63 (SPR) is compared

u32 rd32(const u8* p) {
  u32 v;
  std::memcpy(&v, p, 4);
  return v;
}

// Tag offsets of each transfer from the bucket-319 entry up to (excluding) the bucket-320 entry.
bool walk(const u8* mem, std::size_t size, u32 chain_offset, std::vector<u32>* tags) {
  tags->clear();
  if (!mem || chain_offset % 16) {
    return false;
  }
  const u64 start = u64(chain_offset) + 319 * 16;
  const u64 end = u64(chain_offset) + 320 * 16;
  if (end + 16 > size) {
    return false;
  }
  u64 at = start;
  while (at != end) {
    if (tags->size() >= kJak2MapCopiedParityMaximumTransfers || at % 16 || at + 16 > size) {
      return false;
    }
    for (u32 seen : *tags) {
      if (seen == at) {
        return false;
      }
    }
    const u32 w0 = rd32(mem + at);
    const u32 addr = rd32(mem + at + 4) & kAddrMask;
    const u32 id = (w0 >> 28) & 7;
    const u64 payload_end = at + 16 + u64(w0 & 0xffff) * 16;
    if (payload_end > size) {
      return false;
    }
    tags->push_back(u32(at));
    if (id == kCnt && addr == 0) {
      at = payload_end;
    } else if (id == kNext) {
      at = addr;
    } else {
      return false;
    }
  }
  return true;
}

bool same_layer(const Jak2MapIndexedLayerUpload& a, const Jak2MapIndexedLayerUpload& b) {
  return a.indices == b.indices && a.clut == b.clut && a.width == b.width &&
         a.height == b.height && a.index_source_offset == b.index_source_offset &&
         a.clut_source_offset == b.clut_source_offset;
}

bool same_plan_except_marker_offsets(const Jak2MapTextureUploadTypedPlan& a,
                                     const Jak2MapTextureUploadTypedPlan& b) {
  if (a.present != b.present || a.mixed != b.mixed || a.layer_count != b.layer_count ||
      a.ordinary_count != b.ordinary_count || a.op_count != b.op_count ||
      a.transfer_count != b.transfer_count || a.pc_port_marker_count != b.pc_port_marker_count ||
      a.direct_replay_count != b.direct_replay_count ||
      a.layer_count > kJak2MapIndexedLayerMaximum ||
      a.ordinary_count > kJak2MapTextureUploadMaximumGroups ||
      a.op_count > kJak2MapMixedUploadMaximumOps) {
    return false;
  }
  for (std::size_t i = 0; i < kJak2MapIndexedLayerMaximum; i++) {
    const auto& sa = a.layer_sprites[i];
    const auto& sb = b.layer_sprites[i];
    if (!same_layer(a.layers[i], b.layers[i]) || sa.x0 != sb.x0 || sa.y0 != sb.y0 ||
        sa.x1 != sb.x1 || sa.y1 != sb.y1) {
      return false;
    }
  }
  for (std::size_t i = 0; i < kJak2MapTextureUploadMaximumGroups; i++) {
    const auto& oa = a.ordinary[i];
    const auto& ob = b.ordinary[i];
    if (oa.page_offset != ob.page_offset || oa.page_header != ob.page_header ||
        oa.mode != ob.mode) {
      return false;
    }
  }
  for (std::size_t i = 0; i < kJak2MapMixedUploadMaximumOps; i++) {
    const auto& pa = a.ops[i];
    const auto& pb = b.ops[i];
    if (pa.kind != pb.kind || pa.index != pb.index || pa.marker_count != pb.marker_count ||
        pa.marker_transfer_ordinals != pb.marker_transfer_ordinals) {
      return false;
    }
  }
  return true;
}

// Every marker offset must be the tag of its ordinal in that snapshot's walk.
bool markers_match_walk(const Jak2MapTextureUploadTypedPlan& p, const std::vector<u32>& tags) {
  for (std::size_t i = 0; i < p.op_count; i++) {
    const auto& op = p.ops[i];
    if (op.marker_count > 4) {
      return false;
    }
    for (std::size_t m = 0; m < op.marker_count; m++) {
      const u32 ord = op.marker_transfer_ordinals[m];
      if (ord >= tags.size() || op.marker_tag_offsets[m] != tags[ord]) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

const char* jak2_map_copied_parity_rejection_name(Jak2MapCopiedParityRejection rejection) {
  switch (rejection) {
    case Jak2MapCopiedParityRejection::None: return "none";
    case Jak2MapCopiedParityRejection::LivePlanNotMixed: return "live_plan_not_mixed";
    case Jak2MapCopiedParityRejection::OwnedCount: return "owned_count";
    case Jak2MapCopiedParityRejection::LiveWalk: return "live_walk";
    case Jak2MapCopiedParityRejection::CopiedWalk: return "copied_walk";
    case Jak2MapCopiedParityRejection::TransferCount: return "transfer_count";
    case Jak2MapCopiedParityRejection::TagMismatch: return "tag_mismatch";
    case Jak2MapCopiedParityRejection::PayloadMismatch: return "payload_mismatch";
    case Jak2MapCopiedParityRejection::CopiedReplan: return "copied_replan";
    case Jak2MapCopiedParityRejection::PlanMismatch: return "plan_mismatch";
    case Jak2MapCopiedParityRejection::MarkerOffset: return "marker_offset";
  }
  return "unknown";
}

Jak2MapCopiedParityResult validate_jak2_map_copied_plan(
    const Jak2MapTextureUploadTypedPlan& live_plan,
    const u8* live_dma_snapshot,
    std::size_t live_dma_snapshot_size,
    u32 live_chain_offset,
    const u8* copied_dma_snapshot,
    std::size_t copied_dma_snapshot_size,
    u32 copied_chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size) {
  Jak2MapCopiedParityResult result;
  auto fail = [&](Jak2MapCopiedParityRejection why, u32 ordinal = kJak2MapMixedNoTransfer) {
    result.rejection = why;
    result.transfer_ordinal = ordinal;
    result.plan.reset();
    return result;
  };
  if (!live_plan.present || !live_plan.mixed) {
    return fail(Jak2MapCopiedParityRejection::LivePlanNotMixed);
  }
  // Bound every owned count before any count-driven loop or array access.
  if (live_plan.layer_count > kJak2MapIndexedLayerMaximum ||
      live_plan.ordinary_count > kJak2MapTextureUploadMaximumGroups ||
      live_plan.op_count > kJak2MapMixedUploadMaximumOps ||
      live_plan.transfer_count > kJak2MapCopiedParityMaximumTransfers ||
      live_plan.pc_port_marker_count > kJak2MapCopiedParityMaximumTransfers ||
      live_plan.direct_replay_count > kJak2MapCopiedParityMaximumTransfers) {
    return fail(Jak2MapCopiedParityRejection::OwnedCount);
  }
  std::vector<u32> live_tags, copied_tags;
  if (!walk(live_dma_snapshot, live_dma_snapshot_size, live_chain_offset, &live_tags)) {
    return fail(Jak2MapCopiedParityRejection::LiveWalk, u32(live_tags.size()));
  }
  if (!walk(copied_dma_snapshot, copied_dma_snapshot_size, copied_chain_offset, &copied_tags)) {
    return fail(Jak2MapCopiedParityRejection::CopiedWalk, u32(copied_tags.size()));
  }
  if (live_tags.size() != live_plan.transfer_count || copied_tags.size() != live_tags.size()) {
    return fail(Jak2MapCopiedParityRejection::TransferCount);
  }
  for (std::size_t i = 0; i < live_tags.size(); i++) {
    const u8* lt = live_dma_snapshot + live_tags[i];
    const u8* ct = copied_dma_snapshot + copied_tags[i];
    const u32 lw1 = rd32(lt + 4), cw1 = rd32(ct + 4);
    const bool next = ((rd32(lt) >> 28) & 7) == kNext;
    const u32 ignore = next ? kAddrMask : 0;
    if (std::memcmp(lt, ct, 4) != 0 || (lw1 & ~ignore) != (cw1 & ~ignore) ||
        std::memcmp(lt + 8, ct + 8, 8) != 0) {
      return fail(Jak2MapCopiedParityRejection::TagMismatch, u32(i));
    }
    const std::size_t bytes = std::size_t(rd32(lt) & 0xffff) * 16;
    if (std::memcmp(lt + 16, ct + 16, bytes) != 0) {
      return fail(Jak2MapCopiedParityRejection::PayloadMismatch, u32(i));
    }
  }
  if (!markers_match_walk(live_plan, live_tags)) {
    return fail(Jak2MapCopiedParityRejection::MarkerOffset);
  }
  auto copied = plan_jak2_map_texture_upload_typed(copied_dma_snapshot, copied_dma_snapshot_size,
                                                   copied_chain_offset, live_ee_memory,
                                                   live_ee_memory_size, &result.replan_diagnostic);
  if (!copied || !copied->present || !copied->mixed) {
    return fail(Jak2MapCopiedParityRejection::CopiedReplan);
  }
  if (!same_plan_except_marker_offsets(live_plan, *copied)) {
    return fail(Jak2MapCopiedParityRejection::PlanMismatch);
  }
  if (!markers_match_walk(*copied, copied_tags)) {
    return fail(Jak2MapCopiedParityRejection::MarkerOffset);
  }
  result.plan = std::move(copied);
  return result;
}

}  // namespace metal_renderer
