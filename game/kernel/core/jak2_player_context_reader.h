#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common/goal_constants.h"

namespace jak2_player_context_reader {

namespace layout {

constexpr std::size_t kTargetSize = 0x8b8;
constexpr std::size_t kFocusStatus = 196;
constexpr std::size_t kCamUserMode = 296;
constexpr std::size_t kTypeSymbol = 0;
constexpr std::size_t kTypeAllocatedSize = 8;

constexpr uint32_t kPilotRiding = 1u << 14;
constexpr uint32_t kBoard = 1u << 18;
constexpr uint32_t kPilot = 1u << 20;

static_assert(kFocusStatus + sizeof(uint32_t) <= kTargetSize - BASIC_OFFSET);
static_assert(kCamUserMode + sizeof(uint32_t) <= kTargetSize - BASIC_OFFSET);

}  // namespace layout

struct MemoryView {
  const uint8_t* data = nullptr;
  std::size_t size = 0;
  uint32_t false_object = 0;

  bool is_object(uint32_t address) const { return address != 0 && address != false_object; }

  bool span_fits(uint64_t address, std::size_t bytes) const {
    return data && address <= size && bytes <= size - static_cast<std::size_t>(address);
  }

  template <typename T>
  bool read_absolute(uint64_t address, T* out) const {
    if (!out || !span_fits(address, sizeof(T))) {
      return false;
    }
    std::memcpy(out, data + address, sizeof(T));
    return true;
  }

  template <typename T>
  bool read(uint32_t object, std::size_t offset, T* out) const {
    if (!is_object(object)) {
      return false;
    }
    return read_absolute(static_cast<uint64_t>(object) + offset, out);
  }
};

struct Inputs {
  uint32_t target = 0;
  uint32_t target_type_symbol = 0;
  uint32_t target_type = 0;
  uint32_t normal_symbol = 0;
  uint32_t look_around_symbol = 0;
};

enum class Traversal : int32_t {
  unknown = 0,
  on_foot = 1,
  jetboard = 2,
  vehicle_transition = 3,
  vehicle_riding = 4,
};

enum class LookState : int32_t {
  unknown = 0,
  normal = 1,
  look_around = 2,
};

struct Snapshot {
  Traversal traversal = Traversal::unknown;
  LookState look_state = LookState::unknown;
};

inline bool valid_target(const MemoryView& memory, const Inputs& inputs) {
  uint32_t type_symbol = 0;
  uint16_t allocated_size = 0;
  uint32_t object_type = 0;
  return memory.false_object && inputs.target_type_symbol && inputs.target_type &&
         memory.is_object(inputs.target) &&
         memory.read(inputs.target_type, layout::kTypeSymbol, &type_symbol) &&
         type_symbol == inputs.target_type_symbol &&
         memory.read(inputs.target_type, layout::kTypeAllocatedSize, &allocated_size) &&
         allocated_size == layout::kTargetSize && inputs.target >= BASIC_OFFSET &&
         memory.read_absolute(inputs.target - BASIC_OFFSET, &object_type) &&
         object_type == inputs.target_type &&
         memory.span_fits(inputs.target - BASIC_OFFSET, allocated_size);
}

inline Traversal classify_traversal(uint32_t focus_status) {
  const bool pilot_riding = focus_status & layout::kPilotRiding;
  const bool board = focus_status & layout::kBoard;
  const bool pilot = focus_status & layout::kPilot;

  if ((board && pilot) || (pilot_riding && !pilot)) {
    return Traversal::unknown;
  }
  if (board) {
    return Traversal::jetboard;
  }
  if (pilot) {
    return pilot_riding ? Traversal::vehicle_riding : Traversal::vehicle_transition;
  }
  return Traversal::on_foot;
}

inline LookState classify_look_state(uint32_t cam_user_mode, const Inputs& inputs) {
  if (!inputs.normal_symbol || !inputs.look_around_symbol ||
      inputs.normal_symbol == inputs.look_around_symbol) {
    return LookState::unknown;
  }
  if (cam_user_mode == inputs.normal_symbol) {
    return LookState::normal;
  }
  if (cam_user_mode == inputs.look_around_symbol) {
    return LookState::look_around;
  }
  return LookState::unknown;
}

inline Snapshot read(const MemoryView& memory, const Inputs& inputs) {
  Snapshot out;
  if (!valid_target(memory, inputs)) {
    return out;
  }

  uint32_t focus_status = 0;
  uint32_t cam_user_mode = 0;
  if (!memory.read(inputs.target, layout::kFocusStatus, &focus_status) ||
      !memory.read(inputs.target, layout::kCamUserMode, &cam_user_mode)) {
    return out;
  }

  out.traversal = classify_traversal(focus_status);
  out.look_state = classify_look_state(cam_user_mode, inputs);
  return out;
}

}  // namespace jak2_player_context_reader
