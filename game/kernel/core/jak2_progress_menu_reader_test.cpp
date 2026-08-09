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
constexpr uint32_t kProgressType = 0x200;
constexpr uint32_t kProgressGlobalStateType = 0x240;
constexpr uint32_t kMenuOptionListType = 0x280;
constexpr uint32_t kStateType = 0x2c0;
constexpr uint32_t kProgressPointer = 0x500;
constexpr uint32_t kProgress = 0x804;
constexpr uint32_t kProgressState = 0x1004;
constexpr uint32_t kTitlePCOptions = 0x1204;
constexpr uint32_t kConsoleTitleOptions = 0x1304;
constexpr uint32_t kIdleState = 0x1404;

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
  std::array<uint8_t, 0x2000> bytes = {};
  Inputs inputs = {};

  Fixture() {
    inputs.master_mode = kProgressSymbol;
    inputs.progress_pointer = kProgressPointer;
    inputs.progress_state = kProgressState;
    inputs.title_pc_options = kTitlePCOptions;
    inputs.progress_type = {kProgressSymbol, kProgressType,
                            static_cast<uint16_t>(layout::kProgressSize)};
    inputs.progress_global_state_type = {
        kProgressGlobalStateSymbol, kProgressGlobalStateType,
        static_cast<uint16_t>(layout::kProgressGlobalStateSize)};
    inputs.menu_option_list_type = {kMenuOptionListSymbol, kMenuOptionListType, 0};
    inputs.state_type = {kStateSymbol, kStateType, static_cast<uint16_t>(layout::kStateSize)};
    inputs.progress_symbol = kProgressSymbol;
    inputs.title_symbol = kTitleSymbol;
    inputs.none_symbol = kNoneSymbol;
    inputs.idle_symbol = kIdleSymbol;
    inputs.true_object = kTrue;

    write_type(kProgressType, kProgressSymbol, layout::kProgressSize);
    write_type(kProgressGlobalStateType, kProgressGlobalStateSymbol,
               layout::kProgressGlobalStateSize);
    write_type(kMenuOptionListType, kMenuOptionListSymbol, 0x14);
    write_type(kStateType, kStateSymbol, layout::kStateSize);

    write(bytes, kProgressPointer, kProgress);
    write(bytes, kProgress - BASIC_OFFSET, kProgressType);
    write(bytes, kProgressState - BASIC_OFFSET, kProgressGlobalStateType);
    write(bytes, kTitlePCOptions - BASIC_OFFSET, kMenuOptionListType);
    write(bytes, kConsoleTitleOptions - BASIC_OFFSET, kMenuOptionListType);
    write(bytes, kIdleState - BASIC_OFFSET, kStateType);

    write(bytes, kProgress + layout::kProcessState, kIdleState);
    write(bytes, kProgress + layout::kProcessNextState, kFalse);
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
  write(scheduled_state.bytes, kProgress + layout::kProcessNextState, kIdleState);
  expect(!scheduled_state.read_snapshot().available,
         "a process with a scheduled state transition is rejected");

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

}  // namespace

int main() {
  reads_stable_title_menu();
  malformed_and_oob_memory_fail_closed();
  wrong_types_and_symbols_fail_closed();
  transition_and_option_bounds_are_explicit();
  std::printf("\n%s (%d failures)\n",
              g_failures ? "JAK 2 PROGRESS MENU READER TEST FAILED"
                         : "JAK 2 PROGRESS MENU READER TEST PASSED",
              g_failures);
  return g_failures ? 1 : 0;
}
