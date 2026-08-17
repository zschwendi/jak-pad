#include "game/kernel/core/jak2_player_context_reader.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace {

using namespace jak2_player_context_reader;

constexpr uint32_t kFalse = 4;
constexpr uint32_t kTargetTypeSymbol = 0x20;
constexpr uint32_t kNormalSymbol = 0x24;
constexpr uint32_t kLookAroundSymbol = 0x28;
constexpr uint32_t kUnknownModeSymbol = 0x2c;
constexpr uint32_t kTargetType = 0x200;
constexpr uint32_t kWrongType = 0x240;
constexpr uint32_t kTarget = 0x804;

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
    inputs.target = kTarget;
    inputs.target_type_symbol = kTargetTypeSymbol;
    inputs.target_type = kTargetType;
    inputs.normal_symbol = kNormalSymbol;
    inputs.look_around_symbol = kLookAroundSymbol;

    write(bytes, kTargetType + layout::kTypeSymbol, kTargetTypeSymbol);
    write(bytes, kTargetType + layout::kTypeAllocatedSize,
          static_cast<uint16_t>(layout::kTargetSize));
    write(bytes, kTarget - BASIC_OFFSET, kTargetType);
    set_context(0, kNormalSymbol);
  }

  void set_context(uint32_t focus_status, uint32_t cam_user_mode) {
    write(bytes, inputs.target + layout::kFocusStatus, focus_status);
    write(bytes, inputs.target + layout::kCamUserMode, cam_user_mode);
  }

  Snapshot read_context() const { return read({bytes.data(), bytes.size(), kFalse}, inputs); }
};

void expect_context(const Fixture& fixture,
                    Traversal traversal,
                    LookState look_state,
                    const char* description) {
  const Snapshot snapshot = fixture.read_context();
  expect(snapshot.traversal == traversal && snapshot.look_state == look_state, description);
}

}  // namespace

int main() {
  {
    Fixture fixture;
    expect_context(fixture, Traversal::on_foot, LookState::normal,
                   "no traversal bits and normal camera publish on-foot normal state");
  }
  {
    Fixture fixture;
    fixture.set_context(layout::kBoard, kNormalSymbol);
    expect_context(fixture, Traversal::jetboard, LookState::normal,
                   "the exact board bit publishes jetboard traversal");
  }
  {
    Fixture fixture;
    fixture.set_context(layout::kPilot, kNormalSymbol);
    expect_context(fixture, Traversal::vehicle_transition, LookState::normal,
                   "pilot without pilot-riding publishes vehicle transition");
  }
  {
    Fixture fixture;
    fixture.set_context(layout::kPilot | layout::kPilotRiding, kNormalSymbol);
    expect_context(fixture, Traversal::vehicle_riding, LookState::normal,
                   "pilot plus pilot-riding publishes stable vehicle traversal");
  }
  {
    Fixture fixture;
    fixture.set_context(layout::kBoard | layout::kPilot, kNormalSymbol);
    expect_context(fixture, Traversal::unknown, LookState::normal,
                   "simultaneous board and pilot bits fail traversal closed");
  }
  {
    Fixture fixture;
    fixture.set_context(layout::kPilotRiding, kNormalSymbol);
    expect_context(fixture, Traversal::unknown, LookState::normal,
                   "pilot-riding without pilot fails traversal closed");
  }
  {
    Fixture fixture;
    fixture.set_context(0, kLookAroundSymbol);
    expect_context(fixture, Traversal::on_foot, LookState::look_around,
                   "the exact look-around symbol publishes look-around state");
    fixture.set_context(0, kUnknownModeSymbol);
    expect_context(fixture, Traversal::on_foot, LookState::unknown,
                   "an unsupported camera symbol fails only look state closed");
  }
  {
    Fixture fixture;
    fixture.set_context(layout::kBoard, kNormalSymbol);
    fixture.inputs.normal_symbol = 0;
    expect_context(fixture, Traversal::jetboard, LookState::unknown,
                   "a missing normal symbol preserves valid traversal and fails look closed");
  }
  {
    Fixture fixture;
    fixture.set_context(layout::kPilot, kLookAroundSymbol);
    fixture.inputs.look_around_symbol = 0;
    expect_context(fixture, Traversal::vehicle_transition, LookState::unknown,
                   "a missing look-around symbol preserves valid traversal and fails look closed");
  }
  {
    Fixture fixture;
    write(fixture.bytes, kTarget - BASIC_OFFSET, kWrongType);
    expect_context(fixture, Traversal::unknown, LookState::unknown,
                   "a target object with the wrong exact type fails all context closed");
  }
  {
    Fixture fixture;
    write(fixture.bytes, kTargetType + layout::kTypeAllocatedSize,
          static_cast<uint16_t>(layout::kTargetSize - 4));
    expect_context(fixture, Traversal::unknown, LookState::unknown,
                   "a target type with the wrong exact size fails all context closed");
  }
  {
    Fixture fixture;
    fixture.inputs.target = 0x2f04;
    write(fixture.bytes, fixture.inputs.target - BASIC_OFFSET, kTargetType);
    expect_context(fixture, Traversal::unknown, LookState::unknown,
                   "a target allocation outside the memory view fails all context closed");
  }

  if (g_failures) {
    std::printf("jak2-player-context-reader-test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("jak2-player-context-reader-test: PASS");
  return 0;
}
