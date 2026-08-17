#include "game/kernel/core/jak2_face_prompt_touch_reader.h"
#include "game/kernel/core/jak2_runtime.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace {

using namespace jak2_face_prompt_touch_reader;

constexpr uint32_t kFalse = 4;
constexpr uint32_t kTypeSymbol = 0x20;
constexpr uint32_t kSnapshotType = 0x200;
constexpr uint32_t kWrongType = 0x240;
constexpr uint32_t kSnapshot = 0x404;

static_assert(sizeof(goal_jak2_face_prompt_touch_snapshot) == 12);
static_assert(offsetof(goal_jak2_face_prompt_touch_snapshot, available) == 0);
static_assert(offsetof(goal_jak2_face_prompt_touch_snapshot, requested_buttons) == 4);
static_assert(offsetof(goal_jak2_face_prompt_touch_snapshot, sequence) == 8);

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
  std::array<uint8_t, 0x1000> bytes = {};
  Inputs inputs = {};
  Reader reader;

  Fixture() {
    inputs.snapshot = kSnapshot;
    inputs.snapshot_type_symbol = kTypeSymbol;
    inputs.snapshot_type = kSnapshotType;
    write(bytes, kSnapshotType + layout::kTypeSymbol, kTypeSymbol);
    write(bytes, kSnapshotType + layout::kTypeAllocatedSize,
          static_cast<uint16_t>(layout::kObjectSize));
    write(bytes, kSnapshot - BASIC_OFFSET, kSnapshotType);
    set_field(layout::kRevision, layout::kRevisionValue);
    set_field(layout::kSequence, int32_t{2});
    set_field(layout::kRequestedButtons, layout::kTriangle);
    set_field(layout::kOwner, uint64_t{0x1234000056780000});
    set_field(layout::kHeartbeat, int32_t{10});
  }

  template <typename T>
  void set_field(std::size_t offset, T value) {
    write(bytes, inputs.snapshot + static_cast<uint32_t>(offset), value);
  }

  Snapshot read(uint64_t frame) {
    return reader.read({bytes.data(), bytes.size(), kFalse}, inputs, frame);
  }
};

Snapshot confirm(Fixture& fixture, uint64_t frame) {
  expect(!fixture.read(frame).available, "the first sample fails closed");
  fixture.set_field(layout::kHeartbeat, int32_t{11});
  return fixture.read(frame + 1);
}

}  // namespace

int main() {
  {
    Fixture fixture;
    const Snapshot snapshot = confirm(fixture, 100);
    expect(snapshot.available && snapshot.requested_buttons == layout::kTriangle &&
               snapshot.sequence == 2,
           "a later-frame heartbeat publishes the typed Triangle snapshot");
    expect(fixture.read(103).available, "the heartbeat grace includes two frames");
    expect(!fixture.read(104).available, "the snapshot fails closed after two stale frames");
    fixture.set_field(layout::kHeartbeat, int32_t{12});
    expect(fixture.read(105).available, "a changed heartbeat restores freshness");
  }
  {
    Fixture fixture;
    expect(!fixture.read(200).available, "the initial sample is unavailable");
    fixture.set_field(layout::kHeartbeat, int32_t{11});
    expect(!fixture.read(200).available, "a same-frame heartbeat cannot establish freshness");
    expect(fixture.read(201).available, "that heartbeat is accepted on a later frame");
  }
  {
    Fixture fixture;
    expect(confirm(fixture, 300).available, "the first owner becomes current");
    fixture.set_field(layout::kSequence, int32_t{4});
    fixture.set_field(layout::kOwner, uint64_t{0x2234000056780000});
    fixture.set_field(layout::kHeartbeat, int32_t{1});
    expect(!fixture.read(302).available, "an owner change starts a new sample");
    fixture.set_field(layout::kHeartbeat, int32_t{2});
    expect(fixture.read(303).available, "the new owner requires its own heartbeat");
  }
  {
    Fixture fixture;
    fixture.set_field(layout::kRequestedButtons, layout::kCross | layout::kTriangle);
    const Snapshot snapshot = confirm(fixture, 400);
    expect(snapshot.available &&
               snapshot.requested_buttons == (layout::kCross | layout::kTriangle),
           "only ordinary face-button chords cross the reader boundary");
  }
  {
    Fixture fixture;
    fixture.set_field(layout::kRevision, int32_t{2});
    expect(!fixture.read(500).available, "an unknown revision fails closed");
  }
  {
    Fixture fixture;
    fixture.set_field(layout::kSequence, int32_t{3});
    expect(!fixture.read(510).available, "an odd seqlock fails closed");
    expect(!validate(2, layout::kRevisionValue, layout::kTriangle,
                     uint64_t{0x1234000056780000}, 1, 4)
                .available,
           "a sequence change during the copied read fails closed");
  }
  {
    Fixture fixture;
    fixture.set_field(layout::kOwner, uint64_t{0});
    expect(!fixture.read(520).available, "a cleared owner fails closed");
  }
  {
    Fixture fixture;
    fixture.set_field(layout::kRequestedButtons, uint32_t{1u << 11});
    expect(!fixture.read(530).available, "a non-face button bit fails closed");
  }
  {
    Fixture fixture;
    write(fixture.bytes, kSnapshot - BASIC_OFFSET, kWrongType);
    expect(!fixture.read(540).available, "a wrong object type fails closed");
  }
  {
    Fixture fixture;
    write(fixture.bytes, kSnapshotType + layout::kTypeAllocatedSize,
          static_cast<uint16_t>(layout::kObjectSize + 4));
    expect(!fixture.read(550).available, "a wrong allocated size fails closed");
  }
  {
    Fixture fixture;
    fixture.inputs.snapshot_type_symbol = 0;
    expect(!fixture.read(560).available, "a missing type symbol fails closed");
  }
  {
    Fixture fixture;
    expect(confirm(fixture, 600).available, "the snapshot is current before clear");
    fixture.set_field(layout::kSequence, int32_t{4});
    fixture.set_field(layout::kRequestedButtons, uint32_t{0});
    fixture.set_field(layout::kOwner, uint64_t{0});
    fixture.set_field(layout::kHeartbeat, int32_t{0});
    expect(!fixture.read(602).available, "a cleared publication resets the reader");
  }

  if (g_failures) {
    std::printf("jak2-face-prompt-touch-reader-test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("jak2-face-prompt-touch-reader-test: PASS");
  return 0;
}
