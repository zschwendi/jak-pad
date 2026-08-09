#pragma once

#include <cstddef>

#include "common/common_types.h"

namespace metal_renderer {

constexpr u32 kJak2BlitDisplayBucket = 3;
constexpr u32 kJak2BlitDisplayTbp = 0x3300;
constexpr std::size_t kJak2BlitDisplayMaxTransfers = 64;

enum class Jak2BlitDisplayCommand : u8 {
  None,
  Snapshot,
  CopyBack,
};

enum class Jak2BlitDisplayPlanError : u8 {
  None,
  TooManyTransfers,
  DuplicateCommand,
  InvalidSnapshotShape,
  InvalidCopyBackShape,
};

struct Jak2BlitDisplayPlan {
  Jak2BlitDisplayCommand command = Jak2BlitDisplayCommand::None;
  Jak2BlitDisplayPlanError error = Jak2BlitDisplayPlanError::None;
  std::size_t transfer_count = 0;
  std::size_t unsupported_pc_port_count = 0;

  explicit operator bool() const { return error == Jak2BlitDisplayPlanError::None; }
};

class Jak2BlitDisplayPlanner {
 public:
  void observe(u32 vif0, u32 vif1, const u8* payload, std::size_t payload_size);
  const Jak2BlitDisplayPlan& plan() const { return m_plan; }

 private:
  void reject(Jak2BlitDisplayPlanError error);

  Jak2BlitDisplayPlan m_plan;
};

const char* jak2_blit_display_plan_error_name(Jak2BlitDisplayPlanError error);

}  // namespace metal_renderer
