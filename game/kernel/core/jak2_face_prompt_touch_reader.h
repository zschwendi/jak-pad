#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common/goal_constants.h"

namespace jak2_face_prompt_touch_reader {

namespace layout {

// GOAL basic-object pointers address the bytes immediately after the four-byte type tag.
constexpr std::size_t kRevision = 0;
constexpr std::size_t kSequence = 4;
constexpr std::size_t kRequestedButtons = 8;
constexpr std::size_t kOwner = 12;
constexpr std::size_t kHeartbeat = 20;
constexpr std::size_t kObjectSize = 0x1c;
constexpr std::size_t kRequiredBytes = kObjectSize - BASIC_OFFSET;
constexpr std::size_t kTypeSymbol = 0;
constexpr std::size_t kTypeAllocatedSize = 8;

constexpr int32_t kRevisionValue = 1;
constexpr uint32_t kTriangle = 1u << 12;
constexpr uint32_t kCircle = 1u << 13;
constexpr uint32_t kCross = 1u << 14;
constexpr uint32_t kSquare = 1u << 15;
constexpr uint32_t kFaceButtonMask = kTriangle | kCircle | kCross | kSquare;
constexpr uint64_t kMaximumStaleFrames = 2;

static_assert(kRevision + sizeof(int32_t) == 4);
static_assert(kSequence + sizeof(int32_t) == 8);
static_assert(kRequestedButtons + sizeof(uint32_t) == 12);
static_assert(kOwner + sizeof(uint64_t) == 20);
static_assert(kHeartbeat + sizeof(int32_t) == kRequiredBytes);

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
};

struct Inputs {
  uint32_t snapshot = 0;
  uint32_t snapshot_type_symbol = 0;
  uint32_t snapshot_type = 0;
};

struct Snapshot {
  bool available = false;
  uint32_t requested_buttons = 0;
  uint32_t sequence = 0;
};

struct RawSnapshot {
  bool available = false;
  uint32_t requested_buttons = 0;
  uint32_t sequence = 0;
  uint64_t owner = 0;
  uint32_t heartbeat = 0;
};

template <typename T>
inline T read_volatile(const volatile uint8_t* bytes, std::size_t offset) {
  T value = {};
  auto* value_bytes = reinterpret_cast<uint8_t*>(&value);
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    value_bytes[index] = bytes[offset + index];
  }
  return value;
}

inline RawSnapshot validate(int32_t first_sequence,
                            int32_t revision,
                            uint32_t requested_buttons,
                            uint64_t owner,
                            int32_t heartbeat,
                            int32_t final_sequence) {
  RawSnapshot out;
  if (revision != layout::kRevisionValue || first_sequence != final_sequence ||
      (static_cast<uint32_t>(first_sequence) & 1u) || !owner || !requested_buttons ||
      (requested_buttons & ~layout::kFaceButtonMask)) {
    return out;
  }

  out.available = true;
  out.requested_buttons = requested_buttons;
  out.sequence = static_cast<uint32_t>(first_sequence);
  out.owner = owner;
  out.heartbeat = static_cast<uint32_t>(heartbeat);
  return out;
}

inline RawSnapshot read_raw(const MemoryView& memory, const Inputs& inputs) {
  uint32_t type_symbol = 0;
  uint16_t allocated_size = 0;
  uint32_t object_type = 0;
  if (!memory.false_object || !inputs.snapshot_type_symbol ||
      !memory.is_object(inputs.snapshot_type) || !memory.is_object(inputs.snapshot) ||
      !memory.read_absolute(static_cast<uint64_t>(inputs.snapshot_type) + layout::kTypeSymbol,
                            &type_symbol) ||
      type_symbol != inputs.snapshot_type_symbol ||
      !memory.read_absolute(
          static_cast<uint64_t>(inputs.snapshot_type) + layout::kTypeAllocatedSize,
          &allocated_size) ||
      allocated_size != layout::kObjectSize || inputs.snapshot < BASIC_OFFSET ||
      !memory.read_absolute(inputs.snapshot - BASIC_OFFSET, &object_type) ||
      object_type != inputs.snapshot_type ||
      !memory.span_fits(inputs.snapshot, layout::kRequiredBytes)) {
    return {};
  }

  const auto* bytes = reinterpret_cast<const volatile uint8_t*>(memory.data + inputs.snapshot);
  const int32_t first_sequence = read_volatile<int32_t>(bytes, layout::kSequence);
  const int32_t revision = read_volatile<int32_t>(bytes, layout::kRevision);
  const uint32_t requested_buttons =
      read_volatile<uint32_t>(bytes, layout::kRequestedButtons);
  const uint64_t owner = read_volatile<uint64_t>(bytes, layout::kOwner);
  const int32_t heartbeat = read_volatile<int32_t>(bytes, layout::kHeartbeat);
  const int32_t final_sequence = read_volatile<int32_t>(bytes, layout::kSequence);
  return validate(first_sequence, revision, requested_buttons, owner, heartbeat, final_sequence);
}

class Reader {
 public:
  Snapshot read(const MemoryView& memory, const Inputs& inputs, uint64_t current_frame) {
    const RawSnapshot raw = read_raw(memory, inputs);
    if (!raw.available) {
      reset();
      return {};
    }

    if (!has_sample_ || raw.sequence != sequence_ || raw.owner != owner_ ||
        raw.requested_buttons != requested_buttons_ || current_frame < sample_frame_) {
      has_sample_ = true;
      freshness_confirmed_ = false;
      sequence_ = raw.sequence;
      owner_ = raw.owner;
      requested_buttons_ = raw.requested_buttons;
      heartbeat_ = raw.heartbeat;
      sample_frame_ = current_frame;
      last_refresh_frame_ = current_frame;
      return {};
    }

    if (current_frame == sample_frame_) {
      return {};
    }

    if (raw.heartbeat != heartbeat_) {
      freshness_confirmed_ = true;
      heartbeat_ = raw.heartbeat;
      last_refresh_frame_ = current_frame;
    } else if (!freshness_confirmed_ ||
               current_frame - last_refresh_frame_ > layout::kMaximumStaleFrames) {
      return {};
    }

    return {true, raw.requested_buttons, raw.sequence};
  }

  void reset() {
    has_sample_ = false;
    freshness_confirmed_ = false;
    sequence_ = 0;
    owner_ = 0;
    requested_buttons_ = 0;
    heartbeat_ = 0;
    sample_frame_ = 0;
    last_refresh_frame_ = 0;
  }

 private:
  bool has_sample_ = false;
  bool freshness_confirmed_ = false;
  uint32_t sequence_ = 0;
  uint64_t owner_ = 0;
  uint32_t requested_buttons_ = 0;
  uint32_t heartbeat_ = 0;
  uint64_t sample_frame_ = 0;
  uint64_t last_refresh_frame_ = 0;
};

}  // namespace jak2_face_prompt_touch_reader
