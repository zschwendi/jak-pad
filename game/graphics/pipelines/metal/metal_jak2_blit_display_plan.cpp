#include "game/graphics/pipelines/metal/metal_jak2_blit_display_plan.h"

#include <algorithm>

#include "common/dma/dma.h"

namespace metal_renderer {
namespace {

bool is_zero_payload(const u8* payload, std::size_t size) {
  return payload && std::all_of(payload, payload + size, [](u8 byte) { return byte == 0; });
}

}  // namespace

void Jak2BlitDisplayPlanner::reject(Jak2BlitDisplayPlanError error) {
  if (m_plan.error == Jak2BlitDisplayPlanError::None) {
    m_plan.error = error;
  }
}

void Jak2BlitDisplayPlanner::observe(u32 vif0_raw,
                                     u32 vif1_raw,
                                     const u8* payload,
                                     std::size_t payload_size) {
  if (++m_plan.transfer_count > kJak2BlitDisplayMaxTransfers) {
    reject(Jak2BlitDisplayPlanError::TooManyTransfers);
    return;
  }

  const VifCode vif0(vif0_raw);
  const VifCode vif1(vif1_raw);
  if (vif0.kind != VifCode::Kind::PC_PORT) {
    if (vif1.kind == VifCode::Kind::PC_PORT) {
      m_plan.unsupported_pc_port_count++;
    }
    return;
  }

  if (vif0.immediate != 0x10 && vif0.immediate != 0x11) {
    m_plan.unsupported_pc_port_count++;
    return;
  }
  if (m_plan.command != Jak2BlitDisplayCommand::None) {
    reject(Jak2BlitDisplayPlanError::DuplicateCommand);
    return;
  }

  if (vif0.immediate == 0x10) {
    // goal_src/jak2/engine/gfx/warp.gc: fx-copy-buf emits exactly one
    // zero qword and carries the destination in the second PC_PORT VIF code.
    if (vif1.kind != VifCode::Kind::PC_PORT || vif1.immediate != kJak2BlitDisplayTbp ||
        payload_size != 16 || !is_zero_payload(payload, payload_size)) {
      reject(Jak2BlitDisplayPlanError::InvalidSnapshotShape);
      return;
    }
    m_plan.command = Jak2BlitDisplayCommand::Snapshot;
    return;
  }

  // goal_src/jak2/engine/gfx/blit-displays.gc emits this while count-down is
  // nonzero: both VIF codes are PC_PORT and the CNT has no payload.
  if (vif1.kind != VifCode::Kind::PC_PORT || vif1.immediate != 0 || payload_size != 0) {
    reject(Jak2BlitDisplayPlanError::InvalidCopyBackShape);
    return;
  }
  m_plan.command = Jak2BlitDisplayCommand::CopyBack;
}

const char* jak2_blit_display_plan_error_name(Jak2BlitDisplayPlanError error) {
  switch (error) {
    case Jak2BlitDisplayPlanError::None:
      return "none";
    case Jak2BlitDisplayPlanError::TooManyTransfers:
      return "too-many-transfers";
    case Jak2BlitDisplayPlanError::DuplicateCommand:
      return "duplicate-command";
    case Jak2BlitDisplayPlanError::InvalidSnapshotShape:
      return "invalid-snapshot-shape";
    case Jak2BlitDisplayPlanError::InvalidCopyBackShape:
      return "invalid-copy-back-shape";
  }
  return "unknown";
}

}  // namespace metal_renderer
