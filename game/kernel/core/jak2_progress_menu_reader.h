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

// The PC title list has five raw slots. Secrets may hide slot 3, but Quit remains slot 4.
constexpr int32_t kTitlePCRawOptionMin = 0;
constexpr int32_t kTitlePCRawOptionMax = 4;

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
  uint32_t title_pc_options = 0;

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

enum class Rejection : int32_t {
  none = 0,
  invalid_inputs,
  invalid_objects,
  unreadable_fields,
  wrong_process_state,
  scheduled_state,
  wrong_options,
  wrong_current,
  wrong_next,
  wrong_starting_state,
  invalid_option,
  invalid_selected_option,
  invalid_transition,
};

struct Diagnostics {
  Rejection rejection = Rejection::none;
  uint32_t progress = 0;
  uint32_t process_state = 0;
  uint32_t process_state_name = 0;
  uint32_t process_next_state = 0;
  uint32_t current_options = 0;
  uint32_t current = 0;
  uint32_t next = 0;
  uint32_t starting_state = 0;
  int32_t option_index = -1;
  uint32_t selected_option = 0;
  float menu_transition = 0.f;
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

inline Snapshot read(const MemoryView& memory,
                     const Inputs& inputs,
                     Diagnostics* diagnostics = nullptr) {
  Snapshot out;
  Diagnostics local_diagnostics;
  Diagnostics& detail = diagnostics ? *diagnostics : local_diagnostics;
  detail = {};
  if (!memory.false_object || !inputs.true_object ||
      inputs.true_object == memory.false_object || !inputs.progress_symbol ||
      !inputs.title_symbol || !inputs.none_symbol || !inputs.idle_symbol ||
      inputs.progress_symbol == inputs.title_symbol || inputs.title_symbol == inputs.none_symbol ||
      inputs.none_symbol == inputs.idle_symbol || inputs.master_mode != inputs.progress_symbol) {
    detail.rejection = Rejection::invalid_inputs;
    return out;
  }

  if (!memory.read(inputs.progress_pointer, 0, &detail.progress) ||
      !valid_basic_object(memory, detail.progress, inputs.progress_type) ||
      !valid_basic_object(memory, inputs.progress_state, inputs.progress_global_state_type) ||
      !valid_basic_object(memory, inputs.title_pc_options, inputs.menu_option_list_type)) {
    detail.rejection = Rejection::invalid_objects;
    return out;
  }

  if (!memory.read(detail.progress, layout::kProcessState, &detail.process_state) ||
      !memory.read(detail.progress, layout::kProcessNextState, &detail.process_next_state) ||
      !memory.read(detail.progress, layout::kProgressCurrentOptions, &detail.current_options) ||
      !memory.read(detail.progress, layout::kProgressMenuTransition, &detail.menu_transition) ||
      !memory.read(detail.progress, layout::kProgressOptionIndex, &detail.option_index) ||
      !memory.read(detail.progress, layout::kProgressSelectedOption, &detail.selected_option) ||
      !memory.read(detail.progress, layout::kProgressCurrent, &detail.current) ||
      !memory.read(detail.progress, layout::kProgressNext, &detail.next) ||
      !memory.read(inputs.progress_state, layout::kProgressStartingState, &detail.starting_state) ||
      !valid_basic_object(memory, detail.process_state, inputs.state_type)) {
    detail.rejection = Rejection::unreadable_fields;
    return out;
  }

  if (!memory.read(detail.process_state, layout::kStateName, &detail.process_state_name) ||
      detail.process_state_name != inputs.idle_symbol) {
    detail.rejection = Rejection::wrong_process_state;
    return out;
  }
  if (detail.process_next_state != memory.false_object) {
    detail.rejection = Rejection::scheduled_state;
    return out;
  }
  if (detail.current_options != inputs.title_pc_options) {
    detail.rejection = Rejection::wrong_options;
    return out;
  }
  if (detail.current != inputs.title_symbol) {
    detail.rejection = Rejection::wrong_current;
    return out;
  }
  if (detail.next != inputs.none_symbol) {
    detail.rejection = Rejection::wrong_next;
    return out;
  }
  if (detail.starting_state != inputs.title_symbol) {
    detail.rejection = Rejection::wrong_starting_state;
    return out;
  }
  if (detail.option_index < kTitlePCRawOptionMin ||
      detail.option_index > kTitlePCRawOptionMax) {
    detail.rejection = Rejection::invalid_option;
    return out;
  }
  if (detail.selected_option != memory.false_object &&
      detail.selected_option != inputs.true_object) {
    detail.rejection = Rejection::invalid_selected_option;
    return out;
  }
  if (!std::isfinite(detail.menu_transition) || detail.menu_transition < 0.f ||
      detail.menu_transition > 1.f) {
    detail.rejection = Rejection::invalid_transition;
    return out;
  }

  out.available = true;
  out.screen = 27;
  out.option_index = detail.option_index;
  out.selected_option = detail.selected_option == inputs.true_object;
  out.in_transition = detail.menu_transition != 0.f;
  out.navigation_available = !out.in_transition;
  out.starting_screen = 27;
  return out;
}

}  // namespace jak2_progress_menu_reader
