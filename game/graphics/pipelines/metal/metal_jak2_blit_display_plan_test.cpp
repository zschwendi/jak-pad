#include "game/graphics/pipelines/metal/metal_jak2_blit_display_plan.h"

#include <array>
#include <cstdio>

#include "common/dma/dma.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  failures += !condition;
}

constexpr u32 vif(VifCode::Kind kind, u16 immediate = 0) {
  return (static_cast<u32>(kind) << 24) | immediate;
}

}  // namespace

int main() {
  using namespace metal_renderer;
  constexpr auto pc_port = VifCode::Kind::PC_PORT;
  const std::array<u8, 16> zero_qword = {};

  Jak2BlitDisplayPlanner snapshot;
  snapshot.observe(vif(pc_port, 0x10), vif(pc_port, kJak2BlitDisplayTbp), zero_qword.data(),
                   zero_qword.size());
  check(snapshot.plan() && snapshot.plan().command == Jak2BlitDisplayCommand::Snapshot &&
            snapshot.plan().transfer_count == 1,
        "the exact fx-copy-buf PC_PORT pair plans one TBP 0x3300 snapshot");

  Jak2BlitDisplayPlanner copy_back;
  copy_back.observe(vif(pc_port, 0x11), vif(pc_port), nullptr, 0);
  check(copy_back.plan() && copy_back.plan().command == Jak2BlitDisplayCommand::CopyBack,
        "the exact countdown PC_PORT pair plans bounded copy-back");

  auto nonzero_qword = zero_qword;
  nonzero_qword[7] = 1;
  Jak2BlitDisplayPlanner nonzero_snapshot;
  nonzero_snapshot.observe(vif(pc_port, 0x10), vif(pc_port, kJak2BlitDisplayTbp),
                           nonzero_qword.data(), nonzero_qword.size());
  check(!nonzero_snapshot.plan() &&
            nonzero_snapshot.plan().error == Jak2BlitDisplayPlanError::InvalidSnapshotShape,
        "snapshot planning rejects a nonzero source payload");

  Jak2BlitDisplayPlanner wrong_tbp;
  wrong_tbp.observe(vif(pc_port, 0x10), vif(pc_port, 0x3301), zero_qword.data(), zero_qword.size());
  check(
      !wrong_tbp.plan() && wrong_tbp.plan().error == Jak2BlitDisplayPlanError::InvalidSnapshotShape,
      "snapshot planning rejects every destination except TBP 0x3300");

  Jak2BlitDisplayPlanner duplicate;
  duplicate.observe(vif(pc_port, 0x11), vif(pc_port), nullptr, 0);
  duplicate.observe(vif(pc_port, 0x11), vif(pc_port), nullptr, 0);
  check(!duplicate.plan() && duplicate.plan().error == Jak2BlitDisplayPlanError::DuplicateCommand,
        "one bucket cannot request multiple framebuffer operations");

  Jak2BlitDisplayPlanner unsupported;
  unsupported.observe(vif(pc_port, 0x13), 0, zero_qword.data(), zero_qword.size());
  check(unsupported.plan() && unsupported.plan().command == Jak2BlitDisplayCommand::None &&
            unsupported.plan().unsupported_pc_port_count == 1,
        "unimplemented BlitDisplays effects stay explicitly counted");

  Jak2BlitDisplayPlanner bounded;
  for (std::size_t i = 0; i <= kJak2BlitDisplayMaxTransfers; ++i) {
    bounded.observe(0, 0, nullptr, 0);
  }
  check(!bounded.plan() && bounded.plan().error == Jak2BlitDisplayPlanError::TooManyTransfers,
        "the planner rejects a bucket beyond its fixed transfer bound");

  if (failures) {
    std::printf("FAIL: %d Jak II BlitDisplays planner checks failed\n", failures);
    return 1;
  }
  std::printf("PASS: Jak II BlitDisplays source grammar is bounded and exact\n");
  return 0;
}
