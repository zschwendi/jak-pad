#pragma once

#include <array>
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
constexpr std::size_t kMenuOptionListSize = 0x14;
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
  uint32_t save_options_title = 0;
  uint32_t insufficient_space_options = 0;
  uint32_t create_game_options = 0;
  uint32_t already_exists_options = 0;
  uint32_t icon_info_options = 0;
  uint32_t loading_options = 0;

  TypeIdentity progress_type;
  TypeIdentity progress_global_state_type;
  TypeIdentity menu_option_list_type;
  TypeIdentity state_type;

  uint32_t progress_symbol = 0;
  uint32_t title_symbol = 0;
  uint32_t none_symbol = 0;
  uint32_t idle_symbol = 0;
  uint32_t select_save_title_symbol = 0;
  uint32_t no_memory_card_symbol = 0;
  uint32_t create_game_symbol = 0;
  uint32_t already_exists_symbol = 0;
  uint32_t icon_info_symbol = 0;
  uint32_t creating_symbol = 0;
  uint32_t saving_symbol = 0;
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

enum class SemanticPhase : int32_t {
  unavailable = 0,
  select_save_title = 1,
  no_memory_card = 2,
  create_game = 3,
  creating = 4,
  saving = 5,
  already_exists = 6,
  icon_info = 7,
};

enum SemanticAction : uint32_t {
  action_none = 0,
  action_up = 1u << 0,
  action_down = 1u << 1,
  action_left = 1u << 2,
  action_right = 1u << 3,
  action_confirm = 1u << 4,
};

struct SemanticSnapshot {
  bool available = false;
  SemanticPhase phase = SemanticPhase::unavailable;
  int32_t option_index = -1;
  uint32_t action_mask = action_none;
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

struct StableFields {
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

inline bool read_stable_fields(const MemoryView& memory,
                               const Inputs& inputs,
                               StableFields* fields,
                               Diagnostics* diagnostics) {
  if (!fields) {
    return false;
  }
  Diagnostics local_diagnostics;
  Diagnostics& detail = diagnostics ? *diagnostics : local_diagnostics;
  detail = {};
  if (!memory.false_object || !inputs.true_object ||
      inputs.true_object == memory.false_object || !inputs.progress_symbol ||
      !inputs.title_symbol || !inputs.none_symbol || !inputs.idle_symbol ||
      inputs.progress_symbol == inputs.title_symbol || inputs.title_symbol == inputs.none_symbol ||
      inputs.none_symbol == inputs.idle_symbol || inputs.master_mode != inputs.progress_symbol) {
    detail.rejection = Rejection::invalid_inputs;
    return false;
  }

  if (!memory.read(inputs.progress_pointer, 0, &fields->progress) ||
      !valid_basic_object(memory, fields->progress, inputs.progress_type) ||
      !valid_basic_object(memory, inputs.progress_state, inputs.progress_global_state_type)) {
    detail.rejection = Rejection::invalid_objects;
    return false;
  }

  if (!memory.read(fields->progress, layout::kProcessState, &fields->process_state) ||
      !memory.read(fields->progress, layout::kProcessNextState, &fields->process_next_state) ||
      !memory.read(fields->progress, layout::kProgressCurrentOptions,
                   &fields->current_options) ||
      !memory.read(fields->progress, layout::kProgressMenuTransition,
                   &fields->menu_transition) ||
      !memory.read(fields->progress, layout::kProgressOptionIndex, &fields->option_index) ||
      !memory.read(fields->progress, layout::kProgressSelectedOption,
                   &fields->selected_option) ||
      !memory.read(fields->progress, layout::kProgressCurrent, &fields->current) ||
      !memory.read(fields->progress, layout::kProgressNext, &fields->next) ||
      !memory.read(inputs.progress_state, layout::kProgressStartingState,
                   &fields->starting_state) ||
      !valid_basic_object(memory, fields->process_state, inputs.state_type)) {
    detail.rejection = Rejection::unreadable_fields;
    return false;
  }

  detail.progress = fields->progress;
  detail.process_state = fields->process_state;
  detail.process_next_state = fields->process_next_state;
  detail.current_options = fields->current_options;
  detail.current = fields->current;
  detail.next = fields->next;
  detail.starting_state = fields->starting_state;
  detail.option_index = fields->option_index;
  detail.selected_option = fields->selected_option;
  detail.menu_transition = fields->menu_transition;

  if (!valid_basic_object(memory, fields->current_options, inputs.menu_option_list_type)) {
    detail.rejection = Rejection::invalid_objects;
    return false;
  }
  if (!memory.read(fields->process_state, layout::kStateName, &fields->process_state_name) ||
      fields->process_state_name != inputs.idle_symbol) {
    detail.process_state_name = fields->process_state_name;
    detail.rejection = Rejection::wrong_process_state;
    return false;
  }
  detail.process_state_name = fields->process_state_name;
  // enter-state leaves next-state pointing at the state it just installed. A different
  // pointer means another transition has been scheduled but has not entered yet.
  if (fields->process_next_state != fields->process_state) {
    detail.rejection = Rejection::scheduled_state;
    return false;
  }
  if (fields->selected_option != memory.false_object &&
      fields->selected_option != inputs.true_object) {
    detail.rejection = Rejection::invalid_selected_option;
    return false;
  }
  if (!std::isfinite(fields->menu_transition)) {
    detail.rejection = Rejection::invalid_transition;
    return false;
  }
  return true;
}

inline Snapshot read(const MemoryView& memory,
                     const Inputs& inputs,
                     Diagnostics* diagnostics = nullptr) {
  Snapshot out;
  Diagnostics local_diagnostics;
  Diagnostics& detail = diagnostics ? *diagnostics : local_diagnostics;
  StableFields fields;
  if (!read_stable_fields(memory, inputs, &fields, &detail)) {
    return out;
  }
  if (fields.current_options != inputs.title_pc_options) {
    detail.rejection = Rejection::wrong_options;
    return out;
  }
  if (fields.current != inputs.title_symbol) {
    detail.rejection = Rejection::wrong_current;
    return out;
  }
  if (fields.next != inputs.none_symbol) {
    detail.rejection = Rejection::wrong_next;
    return out;
  }
  if (fields.starting_state != inputs.title_symbol) {
    detail.rejection = Rejection::wrong_starting_state;
    return out;
  }
  if (fields.option_index < kTitlePCRawOptionMin ||
      fields.option_index > kTitlePCRawOptionMax) {
    detail.rejection = Rejection::invalid_option;
    return out;
  }
  if (fields.menu_transition < 0.f || fields.menu_transition > 1.f) {
    detail.rejection = Rejection::invalid_transition;
    return out;
  }

  out.available = true;
  out.screen = 27;
  out.option_index = fields.option_index;
  out.selected_option = fields.selected_option == inputs.true_object;
  out.in_transition = fields.menu_transition != 0.f;
  out.navigation_available = !out.in_transition;
  out.starting_screen = 27;
  return out;
}

template <std::size_t Size>
inline bool all_nonzero_unique(const std::array<uint32_t, Size>& values) {
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (!values[i]) {
      return false;
    }
    for (std::size_t j = i + 1; j < values.size(); ++j) {
      if (values[i] == values[j]) {
        return false;
      }
    }
  }
  return true;
}

inline SemanticSnapshot read_semantic(const MemoryView& memory, const Inputs& inputs) {
  SemanticSnapshot out;
  const std::array state_symbols = {
      inputs.progress_symbol, inputs.title_symbol, inputs.none_symbol, inputs.idle_symbol,
      inputs.select_save_title_symbol, inputs.no_memory_card_symbol,
      inputs.create_game_symbol, inputs.already_exists_symbol, inputs.creating_symbol,
      inputs.saving_symbol, inputs.icon_info_symbol,
  };
  const std::array option_lists = {
      inputs.title_pc_options, inputs.save_options_title, inputs.insufficient_space_options,
      inputs.create_game_options, inputs.already_exists_options, inputs.loading_options,
      inputs.icon_info_options,
  };
  if (!all_nonzero_unique(state_symbols) || !all_nonzero_unique(option_lists)) {
    return out;
  }

  StableFields fields;
  if (!read_stable_fields(memory, inputs, &fields, nullptr) ||
      fields.next != inputs.none_symbol || fields.selected_option != memory.false_object ||
      fields.menu_transition != 0.f) {
    return out;
  }

  const bool title_origin = fields.starting_state == inputs.title_symbol;
  if (title_origin && fields.current == inputs.select_save_title_symbol &&
      fields.current_options == inputs.save_options_title &&
      fields.option_index >= 0 && fields.option_index <= 4) {
    out.phase = SemanticPhase::select_save_title;
    out.action_mask = action_up | action_down | action_confirm;
  } else if (title_origin && fields.current == inputs.no_memory_card_symbol &&
             fields.current_options == inputs.insufficient_space_options &&
             fields.option_index == 0) {
    out.phase = SemanticPhase::no_memory_card;
    out.action_mask = action_confirm;
  } else if (title_origin && fields.current == inputs.create_game_symbol &&
             fields.current_options == inputs.create_game_options &&
             fields.option_index == 0) {
    out.phase = SemanticPhase::create_game;
    out.action_mask = action_left | action_right | action_confirm;
  } else if (title_origin && fields.current == inputs.already_exists_symbol &&
             fields.current_options == inputs.already_exists_options &&
             fields.option_index == 0) {
    out.phase = SemanticPhase::already_exists;
    out.action_mask = action_left | action_right | action_confirm;
  } else if (fields.starting_state == inputs.icon_info_symbol &&
             fields.current == inputs.icon_info_symbol &&
             fields.current_options == inputs.icon_info_options && fields.option_index == 0) {
    out.phase = SemanticPhase::icon_info;
    out.action_mask = action_confirm;
  } else if (title_origin && fields.current == inputs.creating_symbol &&
             fields.current_options == inputs.loading_options && fields.option_index == 0) {
    out.phase = SemanticPhase::creating;
  } else if (title_origin && fields.current == inputs.saving_symbol &&
             fields.current_options == inputs.loading_options && fields.option_index == 0) {
    out.phase = SemanticPhase::saving;
  } else {
    return out;
  }

  out.available = true;
  out.option_index = fields.option_index;
  return out;
}

}  // namespace jak2_progress_menu_reader
