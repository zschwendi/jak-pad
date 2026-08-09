#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common/goal_constants.h"

namespace jak2_progress_menu_reader {

namespace layout {

constexpr std::size_t live_basic_offset(std::size_t asserted_offset) {
  return asserted_offset - BASIC_OFFSET;
}

constexpr std::size_t kProgressSize = 0x164;
constexpr std::size_t kProgressGlobalStateSize = 0xe8;
constexpr std::size_t kStateSize = 0x24;

constexpr std::size_t kProcessState = live_basic_offset(64);
constexpr std::size_t kProcessNextState = live_basic_offset(68);
constexpr std::size_t kProgressCurrentOptions = live_basic_offset(200);
constexpr std::size_t kProgressMenuTransition = live_basic_offset(204);
constexpr std::size_t kProgressOptionIndex = live_basic_offset(208);
constexpr std::size_t kProgressSelectedOption = live_basic_offset(224);
constexpr std::size_t kProgressCurrent = live_basic_offset(228);
constexpr std::size_t kProgressNext = live_basic_offset(232);
constexpr std::size_t kProgressStartingState = live_basic_offset(24);
constexpr std::size_t kStateName = live_basic_offset(4);

constexpr std::size_t kTypeSymbol = 0;
constexpr std::size_t kTypeAllocatedSize = 8;

static_assert(kProcessState == 60);
static_assert(kProcessNextState == 64);
static_assert(kProgressCurrentOptions == 196);
static_assert(kProgressMenuTransition == 200);
static_assert(kProgressOptionIndex == 204);
static_assert(kProgressSelectedOption == 220);
static_assert(kProgressCurrent == 224);
static_assert(kProgressNext == 228);
static_assert(kProgressStartingState == 20);
static_assert(kStateName == 0);

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

struct TypeIdentity {
  uint32_t symbol = 0;
  uint32_t type = 0;
  uint16_t exact_size = 0;
};

struct Inputs {
  uint32_t master_mode = 0;
  uint32_t progress_pointer = 0;
  uint32_t progress_state = 0;
  uint32_t title_options = 0;

  TypeIdentity progress_type;
  TypeIdentity progress_global_state_type;
  TypeIdentity menu_option_list_type;
  TypeIdentity state_type;

  uint32_t progress_symbol = 0;
  uint32_t title_symbol = 0;
  uint32_t none_symbol = 0;
  uint32_t idle_symbol = 0;
  uint32_t true_object = 0;
};

struct Snapshot {
  bool available = false;
  int32_t screen = -1;
  int32_t option_index = -1;
  bool selected_option = false;
  bool in_transition = false;
  bool navigation_available = false;
  int32_t starting_screen = -1;
  bool can_exit_with_start = false;
  bool can_go_back = false;
};

inline bool valid_type_identity(const MemoryView& memory,
                                const TypeIdentity& identity,
                                uint16_t* allocated_size) {
  uint32_t symbol = 0;
  uint16_t size = 0;
  if (!identity.symbol || !memory.is_object(identity.type) ||
      !memory.read(identity.type, layout::kTypeSymbol, &symbol) || symbol != identity.symbol ||
      !memory.read(identity.type, layout::kTypeAllocatedSize, &size) || size < BASIC_OFFSET ||
      (identity.exact_size && size != identity.exact_size)) {
    return false;
  }
  if (allocated_size) {
    *allocated_size = size;
  }
  return true;
}

inline bool valid_basic_object(const MemoryView& memory,
                               uint32_t object,
                               const TypeIdentity& identity) {
  uint16_t allocated_size = 0;
  uint32_t type = 0;
  return valid_type_identity(memory, identity, &allocated_size) && object >= BASIC_OFFSET &&
         memory.read_absolute(object - BASIC_OFFSET, &type) && type == identity.type &&
         memory.span_fits(object - BASIC_OFFSET, allocated_size);
}

inline Snapshot read(const MemoryView& memory, const Inputs& inputs) {
  Snapshot out;
  if (!memory.false_object || !inputs.true_object ||
      inputs.true_object == memory.false_object || !inputs.progress_symbol ||
      !inputs.title_symbol || !inputs.none_symbol || !inputs.idle_symbol ||
      inputs.progress_symbol == inputs.title_symbol || inputs.title_symbol == inputs.none_symbol ||
      inputs.none_symbol == inputs.idle_symbol || inputs.master_mode != inputs.progress_symbol) {
    return out;
  }

  uint32_t progress = 0;
  if (!memory.read(inputs.progress_pointer, 0, &progress) ||
      !valid_basic_object(memory, progress, inputs.progress_type) ||
      !valid_basic_object(memory, inputs.progress_state, inputs.progress_global_state_type) ||
      !valid_basic_object(memory, inputs.title_options, inputs.menu_option_list_type)) {
    return out;
  }

  uint32_t process_state = 0;
  uint32_t process_next_state = 0;
  uint32_t current_options = 0;
  float menu_transition = 0.f;
  int32_t option_index = -1;
  uint32_t selected_option = 0;
  uint32_t current = 0;
  uint32_t next = 0;
  uint32_t starting_state = 0;
  if (!memory.read(progress, layout::kProcessState, &process_state) ||
      !memory.read(progress, layout::kProcessNextState, &process_next_state) ||
      !memory.read(progress, layout::kProgressCurrentOptions, &current_options) ||
      !memory.read(progress, layout::kProgressMenuTransition, &menu_transition) ||
      !memory.read(progress, layout::kProgressOptionIndex, &option_index) ||
      !memory.read(progress, layout::kProgressSelectedOption, &selected_option) ||
      !memory.read(progress, layout::kProgressCurrent, &current) ||
      !memory.read(progress, layout::kProgressNext, &next) ||
      !memory.read(inputs.progress_state, layout::kProgressStartingState, &starting_state) ||
      !valid_basic_object(memory, process_state, inputs.state_type)) {
    return out;
  }

  uint32_t process_state_name = 0;
  if (!memory.read(process_state, layout::kStateName, &process_state_name) ||
      process_state_name != inputs.idle_symbol || process_next_state != memory.false_object ||
      current_options != inputs.title_options || current != inputs.title_symbol ||
      next != inputs.none_symbol || starting_state != inputs.title_symbol || option_index < 0 ||
      option_index > 3 ||
      (selected_option != memory.false_object && selected_option != inputs.true_object) ||
      !std::isfinite(menu_transition) || menu_transition < 0.f || menu_transition > 1.f) {
    return out;
  }

  out.available = true;
  out.screen = 27;
  out.option_index = option_index;
  out.selected_option = selected_option == inputs.true_object;
  out.in_transition = menu_transition != 0.f;
  out.navigation_available = !out.in_transition;
  out.starting_screen = 27;
  return out;
}

}  // namespace jak2_progress_menu_reader
