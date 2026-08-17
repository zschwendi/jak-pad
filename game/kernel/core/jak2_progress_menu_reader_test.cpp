#include "game/kernel/core/jak2_progress_menu_reader.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

using namespace jak2_progress_menu_reader;

constexpr uint32_t kFalse = 4;
constexpr uint32_t kTrue = 8;
constexpr uint32_t kProgressSymbol = 0x20;
constexpr uint32_t kProgressGlobalStateSymbol = 0x24;
constexpr uint32_t kMenuOptionListSymbol = 0x28;
constexpr uint32_t kStateSymbol = 0x2c;
constexpr uint32_t kTitleSymbol = 0x30;
constexpr uint32_t kNoneSymbol = 0x34;
constexpr uint32_t kIdleSymbol = 0x38;
constexpr uint32_t kSelectLoadSymbol = 0x3c;
constexpr uint32_t kSelectSaveSymbol = 0x40;
constexpr uint32_t kSelectSaveTitleSymbol = 0x44;
constexpr uint32_t kSelectSaveTitleHeroSymbol = 0x48;
constexpr uint32_t kNoMemoryCardSymbol = 0x4c;
constexpr uint32_t kCreateGameSymbol = 0x50;
constexpr uint32_t kLoadingSymbol = 0x54;
constexpr uint32_t kCreatingSymbol = 0x58;
constexpr uint32_t kSavingSymbol = 0x5c;
constexpr uint32_t kAlreadyExistsSymbol = 0x60;
constexpr uint32_t kIconInfoSymbol = 0x64;
constexpr uint32_t kProgressType = 0x200;
constexpr uint32_t kProgressGlobalStateType = 0x240;
constexpr uint32_t kMenuOptionListType = 0x280;
constexpr uint32_t kStateType = 0x2c0;
constexpr uint32_t kProgressPointer = 0x500;
constexpr uint32_t kProgress = 0x804;
constexpr uint32_t kProgressState = 0x1004;
constexpr uint32_t kTitlePCOptions = 0x1204;
constexpr uint32_t kConsoleTitleOptions = 0x1304;
constexpr uint32_t kLoadSaveOptions = 0x1404;
constexpr uint32_t kSaveOptionsTitle = 0x1504;
constexpr uint32_t kInsufficientSpaceOptions = 0x1604;
constexpr uint32_t kCreateGameOptions = 0x1704;
constexpr uint32_t kLoadingOptions = 0x1804;
constexpr uint32_t kIdleState = 0x1904;
constexpr uint32_t kAlreadyExistsOptions = 0x1a04;
constexpr uint32_t kIconInfoOptions = 0x1b04;

int g_failures = 0;

void expect(bool condition, const char* description) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
  if (!condition) {
    g_failures++;
  }
}

template <typename T, std::size_t Size>
void write(std::array<uint8_t, Size>& memory, uint32_t address, T value) {
  std::memcpy(memory.data() + address, &value, sizeof(value));
}

struct Fixture {
  std::array<uint8_t, 0x3000> bytes = {};
  Inputs inputs = {};

  Fixture() {
    inputs.master_mode = kProgressSymbol;
    inputs.progress_pointer = kProgressPointer;
    inputs.progress_state = kProgressState;
    inputs.title_pc_options = kTitlePCOptions;
    inputs.load_save_options = kLoadSaveOptions;
    inputs.save_options_title = kSaveOptionsTitle;
    inputs.insufficient_space_options = kInsufficientSpaceOptions;
    inputs.create_game_options = kCreateGameOptions;
    inputs.already_exists_options = kAlreadyExistsOptions;
    inputs.icon_info_options = kIconInfoOptions;
    inputs.loading_options = kLoadingOptions;
    inputs.progress_type = {kProgressSymbol, kProgressType,
                            static_cast<uint16_t>(layout::kProgressSize)};
    inputs.progress_global_state_type = {
        kProgressGlobalStateSymbol, kProgressGlobalStateType,
        static_cast<uint16_t>(layout::kProgressGlobalStateSize)};
    inputs.menu_option_list_type = {
        kMenuOptionListSymbol, kMenuOptionListType,
        static_cast<uint16_t>(layout::kMenuOptionListSize)};
    inputs.state_type = {kStateSymbol, kStateType, static_cast<uint16_t>(layout::kStateSize)};
    inputs.progress_symbol = kProgressSymbol;
    inputs.title_symbol = kTitleSymbol;
    inputs.none_symbol = kNoneSymbol;
    inputs.idle_symbol = kIdleSymbol;
    inputs.select_load_symbol = kSelectLoadSymbol;
    inputs.select_save_symbol = kSelectSaveSymbol;
    inputs.select_save_title_symbol = kSelectSaveTitleSymbol;
    inputs.select_save_title_hero_symbol = kSelectSaveTitleHeroSymbol;
    inputs.no_memory_card_symbol = kNoMemoryCardSymbol;
    inputs.create_game_symbol = kCreateGameSymbol;
    inputs.already_exists_symbol = kAlreadyExistsSymbol;
    inputs.icon_info_symbol = kIconInfoSymbol;
    inputs.loading_symbol = kLoadingSymbol;
    inputs.creating_symbol = kCreatingSymbol;
    inputs.saving_symbol = kSavingSymbol;
    inputs.true_object = kTrue;

    write_type(kProgressType, kProgressSymbol, layout::kProgressSize);
    write_type(kProgressGlobalStateType, kProgressGlobalStateSymbol,
               layout::kProgressGlobalStateSize);
    write_type(kMenuOptionListType, kMenuOptionListSymbol, layout::kMenuOptionListSize);
    write_type(kStateType, kStateSymbol, layout::kStateSize);

    write(bytes, kProgressPointer, kProgress);
    write(bytes, kProgress - BASIC_OFFSET, kProgressType);
    write(bytes, kProgressState - BASIC_OFFSET, kProgressGlobalStateType);
    write(bytes, kTitlePCOptions - BASIC_OFFSET, kMenuOptionListType);
    write(bytes, kConsoleTitleOptions - BASIC_OFFSET, kMenuOptionListType);
    write(bytes, kLoadSaveOptions - BASIC_OFFSET, kMenuOptionListType);
    write(bytes, kSaveOptionsTitle - BASIC_OFFSET, kMenuOptionListType);
    write(bytes, kInsufficientSpaceOptions - BASIC_OFFSET, kMenuOptionListType);
    write(bytes, kCreateGameOptions - BASIC_OFFSET, kMenuOptionListType);
    write(bytes, kAlreadyExistsOptions - BASIC_OFFSET, kMenuOptionListType);
    write(bytes, kIconInfoOptions - BASIC_OFFSET, kMenuOptionListType);
    write(bytes, kLoadingOptions - BASIC_OFFSET, kMenuOptionListType);
    write(bytes, kIdleState - BASIC_OFFSET, kStateType);

    write(bytes, kProgress + layout::kProcessState, kIdleState);
    write(bytes, kProgress + layout::kProcessNextState, kIdleState);
    write(bytes, kProgress + layout::kProgressCurrentOptions, kTitlePCOptions);
    write(bytes, kProgress + layout::kProgressMenuTransition, 0.f);
    write<int32_t>(bytes, kProgress + layout::kProgressOptionIndex, 0);
    write(bytes, kProgress + layout::kProgressSelectedOption, kFalse);
    write(bytes, kProgress + layout::kProgressCurrent, kTitleSymbol);
    write(bytes, kProgress + layout::kProgressNext, kNoneSymbol);
    write(bytes, kProgressState + layout::kProgressStartingState, kTitleSymbol);
    write(bytes, kIdleState + layout::kStateName, kIdleSymbol);
  }

  void write_type(uint32_t type, uint32_t symbol, std::size_t size) {
    write(bytes, type + layout::kTypeSymbol, symbol);
    write<uint16_t>(bytes, type + layout::kTypeAllocatedSize, static_cast<uint16_t>(size));
  }

  Snapshot read_snapshot(Diagnostics* diagnostics = nullptr) const {
    return read({bytes.data(), bytes.size(), kFalse}, inputs, diagnostics);
  }

  SemanticSnapshot read_semantic_snapshot() const {
    return read_semantic({bytes.data(), bytes.size(), kFalse}, inputs);
  }

  void set_semantic_state(uint32_t state, uint32_t options, int32_t option = 0) {
    write(bytes, kProgress + layout::kProgressCurrent, state);
    write(bytes, kProgress + layout::kProgressCurrentOptions, options);
    write(bytes, kProgress + layout::kProgressOptionIndex, option);
  }
};

void reads_stable_title_menu() {
  Fixture fixture;
  Snapshot snapshot = fixture.read_snapshot();
  expect(snapshot.available && snapshot.screen == 27 && snapshot.starting_screen == 27 &&
             snapshot.option_index == 0 && !snapshot.selected_option &&
             !snapshot.in_transition && snapshot.navigation_available &&
             !snapshot.can_exit_with_start && !snapshot.can_go_back,
         "stable Jak II title maps to the shared title screen without exit actions");

  for (int option = kTitlePCRawOptionMin; option <= kTitlePCRawOptionMax; option++) {
    write<int32_t>(fixture.bytes, kProgress + layout::kProgressOptionIndex, option);
    write(fixture.bytes, kProgress + layout::kProgressSelectedOption,
          option == 2 ? kTrue : kFalse);
    snapshot = fixture.read_snapshot();
    expect(snapshot.available && snapshot.option_index == option &&
               snapshot.selected_option == (option == 2),
           "all five raw PC title options and exact GOAL booleans are preserved");
  }
}

void malformed_and_oob_memory_fail_closed() {
  Fixture malformed;
  write(malformed.bytes, kProgress + layout::kProgressSelectedOption, uint32_t{0xdead});
  expect(!malformed.read_snapshot().available,
         "a selected-option value other than GOAL true or false is rejected");

  Fixture nonfinite;
  write(nonfinite.bytes, kProgress + layout::kProgressMenuTransition,
        std::numeric_limits<float>::quiet_NaN());
  expect(!nonfinite.read_snapshot().available, "a non-finite transition is rejected");

  Fixture oob_object;
  write(oob_object.bytes, kProgressPointer,
        static_cast<uint32_t>(oob_object.bytes.size() - BASIC_OFFSET));
  write(oob_object.bytes, static_cast<uint32_t>(oob_object.bytes.size() - 2 * BASIC_OFFSET),
        kProgressType);
  expect(!oob_object.read_snapshot().available,
         "a progress object whose declared size leaves the memory view is rejected");

  Fixture oob_pointer;
  oob_pointer.inputs.progress_pointer = static_cast<uint32_t>(oob_pointer.bytes.size() - 2);
  expect(!oob_pointer.read_snapshot().available,
         "an out-of-bounds pointer cell is rejected before dereference");
}

void wrong_types_and_symbols_fail_closed() {
  Fixture wrong_type;
  write(wrong_type.bytes, kProgress - BASIC_OFFSET, kMenuOptionListType);
  expect(!wrong_type.read_snapshot().available, "a wrong progress type tag is rejected");

  Fixture wrong_type_identity;
  write(wrong_type_identity.bytes, kProgressType + layout::kTypeSymbol, kTitleSymbol);
  expect(!wrong_type_identity.read_snapshot().available,
         "a type object that does not name progress is rejected");

  Fixture wrong_type_size;
  write<uint16_t>(wrong_type_size.bytes, kProgressType + layout::kTypeAllocatedSize,
                  static_cast<uint16_t>(layout::kProgressSize - BASIC_OFFSET));
  expect(!wrong_type_size.read_snapshot().available,
         "a progress type with a different allocated size is rejected");

  Fixture wrong_current;
  write(wrong_current.bytes, kProgress + layout::kProgressCurrent, kNoneSymbol);
  expect(!wrong_current.read_snapshot().available,
         "a current screen other than the exact title symbol is rejected");

  Fixture wrong_next;
  write(wrong_next.bytes, kProgress + layout::kProgressNext, kTitleSymbol);
  expect(!wrong_next.read_snapshot().available,
         "a title process without the stable none next symbol is rejected");

  Fixture wrong_options;
  write(wrong_options.bytes, kProgress + layout::kProgressCurrentOptions, kConsoleTitleOptions);
  Diagnostics wrong_options_diagnostics;
  expect(!wrong_options.read_snapshot(&wrong_options_diagnostics).available &&
             wrong_options_diagnostics.rejection == Rejection::wrong_options &&
             wrong_options_diagnostics.current_options == kConsoleTitleOptions,
         "the console title option list is rejected in place of the exact PC list");

  Fixture wrong_mode;
  wrong_mode.inputs.master_mode = kTitleSymbol;
  expect(!wrong_mode.read_snapshot().available,
         "a retained title object outside progress mode is rejected");

  Fixture scheduled_state;
  write(scheduled_state.bytes, kProgress + layout::kProcessNextState, kProgressState);
  expect(!scheduled_state.read_snapshot().available,
         "a process whose pending state differs from its entered idle state is rejected");

  Fixture wrong_state_name;
  write(wrong_state_name.bytes, kIdleState + layout::kStateName, kTitleSymbol);
  Diagnostics wrong_state_diagnostics;
  expect(!wrong_state_name.read_snapshot(&wrong_state_diagnostics).available &&
             wrong_state_diagnostics.rejection == Rejection::wrong_process_state &&
             wrong_state_diagnostics.process_state_name == kTitleSymbol,
         "a progress process outside the exact idle state is rejected");
}

void transition_and_option_bounds_are_explicit() {
  Fixture transition;
  write(transition.bytes, kProgress + layout::kProgressMenuTransition, 0.5f);
  Snapshot snapshot = transition.read_snapshot();
  expect(snapshot.available && snapshot.in_transition && !snapshot.navigation_available,
         "a bounded title fade is visible but cannot receive navigation");

  write(transition.bytes, kProgress + layout::kProgressMenuTransition, 1.25f);
  expect(!transition.read_snapshot().available, "a transition outside the proven range is rejected");

  Fixture negative_option;
  write<int32_t>(negative_option.bytes, kProgress + layout::kProgressOptionIndex,
                 kTitlePCRawOptionMin - 1);
  expect(!negative_option.read_snapshot().available, "a negative title option is rejected");

  Fixture large_option;
  write<int32_t>(large_option.bytes, kProgress + layout::kProgressOptionIndex,
                 kTitlePCRawOptionMax + 1);
  expect(!large_option.read_snapshot().available,
         "a raw title option beyond the five-entry PC list is rejected");
}

void reads_source_proven_save_flow_semantics() {
  Fixture fixture;
  expect(!fixture.read_semantic_snapshot().available,
         "the existing raw title snapshot remains outside the additive save-flow ABI");

  for (int option = 0; option <= 4; ++option) {
    fixture.set_semantic_state(kSelectSaveTitleSymbol, kSaveOptionsTitle, option);
    const auto snapshot = fixture.read_semantic_snapshot();
    expect(snapshot.available && snapshot.phase == SemanticPhase::select_save_title &&
               snapshot.option_index == option &&
               snapshot.action_mask == (action_up | action_down | action_confirm | action_back),
           "all five title save rows expose Up, Down, Confirm, and Triangle back");
  }

  for (int option = 0; option <= 3; ++option) {
    fixture.set_semantic_state(kSelectLoadSymbol, kLoadSaveOptions, option);
    auto snapshot = fixture.read_semantic_snapshot();
    expect(snapshot.available && snapshot.phase == SemanticPhase::select_load &&
               snapshot.option_index == option &&
               snapshot.action_mask == (action_up | action_down | action_confirm | action_back),
           "all four load slots expose Up, Down, Confirm, and Triangle back");

    fixture.set_semantic_state(kSelectSaveSymbol, kLoadSaveOptions, option);
    snapshot = fixture.read_semantic_snapshot();
    expect(snapshot.available && snapshot.phase == SemanticPhase::select_save &&
               snapshot.option_index == option &&
               snapshot.action_mask == (action_up | action_down | action_confirm | action_back),
           "all four existing-save slots expose Up, Down, Confirm, and Triangle back");
  }

  fixture.set_semantic_state(kSelectSaveTitleHeroSymbol, kSaveOptionsTitle, 0);
  auto snapshot = fixture.read_semantic_snapshot();
  expect(snapshot.available && snapshot.phase == SemanticPhase::select_save_title &&
             snapshot.action_mask == (action_up | action_down | action_confirm | action_back),
         "hero title save selection shares the title-save semantic actions");

  fixture.set_semantic_state(kNoMemoryCardSymbol, kInsufficientSpaceOptions);
  snapshot = fixture.read_semantic_snapshot();
  expect(snapshot.available && snapshot.phase == SemanticPhase::no_memory_card &&
             snapshot.option_index == 0 && snapshot.action_mask == action_confirm,
         "title-origin no-memory-card exposes only Confirm");

  fixture.set_semantic_state(kCreateGameSymbol, kCreateGameOptions);
  snapshot = fixture.read_semantic_snapshot();
  expect(snapshot.available && snapshot.phase == SemanticPhase::create_game &&
             snapshot.option_index == 0 &&
             snapshot.action_mask == (action_left | action_right | action_confirm | action_back),
         "create-game exposes its responder's Left, Right, Confirm, and Triangle back");

  fixture.set_semantic_state(kAlreadyExistsSymbol, kAlreadyExistsOptions);
  snapshot = fixture.read_semantic_snapshot();
  expect(snapshot.available && snapshot.phase == SemanticPhase::already_exists &&
             snapshot.option_index == 0 &&
             snapshot.action_mask == (action_left | action_right | action_confirm | action_back),
         "already-exists exposes its responder's Left, Right, Confirm, and Triangle back");

  fixture.set_semantic_state(kIconInfoSymbol, kIconInfoOptions);
  write(fixture.bytes, kProgressState + layout::kProgressStartingState, kIconInfoSymbol);
  snapshot = fixture.read_semantic_snapshot();
  expect(snapshot.available && snapshot.phase == SemanticPhase::icon_info &&
             snapshot.option_index == 0 && snapshot.action_mask == action_confirm,
         "the save-icon information screen exposes only Confirm");

  write(fixture.bytes, kProgressState + layout::kProgressStartingState, kTitleSymbol);
  fixture.set_semantic_state(kCreatingSymbol, kLoadingOptions);
  snapshot = fixture.read_semantic_snapshot();
  expect(snapshot.available && snapshot.phase == SemanticPhase::creating &&
             snapshot.option_index == 0 && snapshot.action_mask == action_none,
         "creating is observable but exposes no touch action");

  fixture.set_semantic_state(kSavingSymbol, kLoadingOptions);
  snapshot = fixture.read_semantic_snapshot();
  expect(snapshot.available && snapshot.phase == SemanticPhase::saving &&
             snapshot.option_index == 0 && snapshot.action_mask == action_none,
         "saving is observable but exposes no touch action");

  fixture.set_semantic_state(kLoadingSymbol, kLoadingOptions);
  snapshot = fixture.read_semantic_snapshot();
  expect(snapshot.available && snapshot.phase == SemanticPhase::loading &&
             snapshot.option_index == 0 && snapshot.action_mask == action_none,
         "loading is observable but exposes no touch action");
}

void semantic_identity_and_stability_fail_closed() {
  Fixture optional_inputs_missing;
  optional_inputs_missing.set_semantic_state(kSelectLoadSymbol, kLoadSaveOptions);
  optional_inputs_missing.inputs.icon_info_symbol = 0;
  optional_inputs_missing.inputs.icon_info_options = 0;
  optional_inputs_missing.inputs.creating_symbol = 0;
  optional_inputs_missing.inputs.loading_options = 0;
  expect(optional_inputs_missing.read_semantic_snapshot().available,
         "unrelated unavailable save phases do not hide an active load menu");

  Fixture active_state_missing;
  active_state_missing.set_semantic_state(kSelectLoadSymbol, kLoadSaveOptions);
  active_state_missing.inputs.select_load_symbol = 0;
  expect(!active_state_missing.read_semantic_snapshot().available,
         "an unavailable active state identity remains fail-closed");

  Fixture active_options_missing;
  active_options_missing.set_semantic_state(kSelectLoadSymbol, kLoadSaveOptions);
  active_options_missing.inputs.load_save_options = 0;
  expect(!active_options_missing.read_semantic_snapshot().available,
         "an unavailable active option-list identity remains fail-closed");

  Fixture wrong_options;
  wrong_options.set_semantic_state(kSelectSaveTitleSymbol, kCreateGameOptions);
  expect(!wrong_options.read_semantic_snapshot().available,
         "a proven state paired with another exact option list is rejected");

  Fixture wrong_option_type;
  wrong_option_type.set_semantic_state(kCreateGameSymbol, kCreateGameOptions);
  write(wrong_option_type.bytes, kCreateGameOptions - BASIC_OFFSET, kStateType);
  expect(!wrong_option_type.read_semantic_snapshot().available,
         "a current option list with the wrong BASIC type tag is rejected");

  Fixture wrong_overwrite_options;
  wrong_overwrite_options.set_semantic_state(kAlreadyExistsSymbol, kCreateGameOptions);
  expect(!wrong_overwrite_options.read_semantic_snapshot().available,
         "already-exists paired with create-game options is rejected");

  Fixture wrong_overwrite_index;
  wrong_overwrite_index.set_semantic_state(kAlreadyExistsSymbol, kAlreadyExistsOptions, 1);
  expect(!wrong_overwrite_index.read_semantic_snapshot().available,
         "already-exists rejects a second option row");

  Fixture wrong_icon_info_options;
  wrong_icon_info_options.set_semantic_state(kIconInfoSymbol, kLoadingOptions);
  write(wrong_icon_info_options.bytes, kProgressState + layout::kProgressStartingState,
        kIconInfoSymbol);
  expect(!wrong_icon_info_options.read_semantic_snapshot().available,
         "icon-info paired with loading options is rejected");

  Fixture wrong_icon_info_index;
  wrong_icon_info_index.set_semantic_state(kIconInfoSymbol, kIconInfoOptions, 1);
  write(wrong_icon_info_index.bytes, kProgressState + layout::kProgressStartingState,
        kIconInfoSymbol);
  expect(!wrong_icon_info_index.read_semantic_snapshot().available,
         "icon-info rejects a second option row");

  Fixture wrong_icon_info_origin;
  wrong_icon_info_origin.set_semantic_state(kIconInfoSymbol, kIconInfoOptions);
  expect(!wrong_icon_info_origin.read_semantic_snapshot().available,
         "icon-info is rejected outside its source-proven spawned origin");

  Fixture wrong_option_type_size;
  wrong_option_type_size.set_semantic_state(kCreateGameSymbol, kCreateGameOptions);
  write<uint16_t>(wrong_option_type_size.bytes,
                  kMenuOptionListType + layout::kTypeAllocatedSize,
                  static_cast<uint16_t>(layout::kMenuOptionListSize - BASIC_OFFSET));
  expect(!wrong_option_type_size.read_semantic_snapshot().available,
         "a menu-option-list type with a different exact size is rejected");

  Fixture wrong_origin;
  wrong_origin.set_semantic_state(kNoMemoryCardSymbol, kInsufficientSpaceOptions);
  write(wrong_origin.bytes, kProgressState + layout::kProgressStartingState,
        kNoMemoryCardSymbol);
  expect(!wrong_origin.read_semantic_snapshot().available,
         "no-memory-card outside the exact title origin is rejected");

  Fixture pending_menu_state;
  pending_menu_state.set_semantic_state(kCreateGameSymbol, kCreateGameOptions);
  write(pending_menu_state.bytes, kProgress + layout::kProgressNext, kCreatingSymbol);
  expect(!pending_menu_state.read_semantic_snapshot().available,
         "a non-none next menu state is rejected");

  Fixture scheduled_process_state;
  scheduled_process_state.set_semantic_state(kCreateGameSymbol, kCreateGameOptions);
  write(scheduled_process_state.bytes, kProgress + layout::kProcessNextState, kProgressState);
  expect(!scheduled_process_state.read_semantic_snapshot().available,
         "a process next-state pointer different from its entered state is rejected");

  Fixture wrong_process_state;
  wrong_process_state.set_semantic_state(kCreateGameSymbol, kCreateGameOptions);
  write(wrong_process_state.bytes, kIdleState + layout::kStateName, kCreatingSymbol);
  expect(!wrong_process_state.read_semantic_snapshot().available,
         "a progress process outside the entered idle state is rejected");

  Fixture selected;
  selected.set_semantic_state(kSelectSaveTitleSymbol, kSaveOptionsTitle);
  write(selected.bytes, kProgress + layout::kProgressSelectedOption, kTrue);
  expect(!selected.read_semantic_snapshot().available,
         "a transient selected-option publication is not a stable semantic phase");

  Fixture transitioning;
  transitioning.set_semantic_state(kSelectSaveTitleSymbol, kSaveOptionsTitle);
  write(transitioning.bytes, kProgress + layout::kProgressMenuTransition, 0.01f);
  expect(!transitioning.read_semantic_snapshot().available,
         "a finite nonzero menu transition is rejected");
  write(transitioning.bytes, kProgress + layout::kProgressMenuTransition,
        std::numeric_limits<float>::infinity());
  expect(!transitioning.read_semantic_snapshot().available,
         "a non-finite menu transition is rejected");

  Fixture duplicate_symbol;
  duplicate_symbol.set_semantic_state(kCreateGameSymbol, kCreateGameOptions);
  duplicate_symbol.inputs.creating_symbol = kCreateGameSymbol;
  expect(!duplicate_symbol.read_semantic_snapshot().available,
         "colliding state symbols are rejected instead of guessed");
}

void semantic_option_bounds_are_exact() {
  Fixture save_low;
  save_low.set_semantic_state(kSelectSaveTitleSymbol, kSaveOptionsTitle, -1);
  expect(!save_low.read_semantic_snapshot().available,
         "a negative title-save option is rejected");

  Fixture save_high;
  save_high.set_semantic_state(kSelectSaveTitleSymbol, kSaveOptionsTitle, 5);
  expect(!save_high.read_semantic_snapshot().available,
         "a sixth title-save option is rejected");

  Fixture load_high;
  load_high.set_semantic_state(kSelectLoadSymbol, kLoadSaveOptions, 4);
  expect(!load_high.read_semantic_snapshot().available,
         "a fifth load slot is rejected");

  Fixture singleton;
  singleton.set_semantic_state(kNoMemoryCardSymbol, kInsufficientSpaceOptions, 1);
  expect(!singleton.read_semantic_snapshot().available,
         "a singleton semantic phase rejects option one");
}

}  // namespace

int main() {
  reads_stable_title_menu();
  malformed_and_oob_memory_fail_closed();
  wrong_types_and_symbols_fail_closed();
  transition_and_option_bounds_are_explicit();
  reads_source_proven_save_flow_semantics();
  semantic_identity_and_stability_fail_closed();
  semantic_option_bounds_are_exact();
  std::printf("\n%s (%d failures)\n",
              g_failures ? "JAK 2 PROGRESS MENU READER TEST FAILED"
                         : "JAK 2 PROGRESS MENU READER TEST PASSED",
              g_failures);
  return g_failures ? 1 : 0;
}
