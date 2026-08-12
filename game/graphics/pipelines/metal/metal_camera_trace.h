#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common/common_types.h"

namespace metal_camera_trace {

constexpr std::size_t kCameraMatrixBytes = 4 * 16;
constexpr std::size_t kTranslationBytes = 16;
constexpr std::size_t kSnapshotBytes = kCameraMatrixBytes + kTranslationBytes;
constexpr std::size_t kSnapshotQwords = kSnapshotBytes / 16;
constexpr std::size_t kRenderSnapshotQwords = 15;
constexpr std::size_t kRenderSnapshotBytes = kRenderSnapshotQwords * 16;

constexpr bool should_attach_presentation_handler(bool detailed_frame_stats_enabled) {
  return detailed_frame_stats_enabled;
}

struct Snapshot {
  std::array<u8, kSnapshotBytes> bytes = {};

  bool operator==(const Snapshot& other) const { return bytes == other.bytes; }
  bool operator!=(const Snapshot& other) const { return !(*this == other); }
};

// The packet fields that determine background vertex position, in serialization order:
// camera-temp[0...3], hvdf-off, fog.x, trans, camera-rot[0...3], perspective[0...3]. The
// remaining fog lanes are zeroed because they affect appearance rather than vertex position. Planes
// and itimes are excluded because they affect visibility and time-of-day color.
struct RenderSnapshot {
  std::array<u8, kRenderSnapshotBytes> bytes = {};

  bool operator==(const RenderSnapshot& other) const { return bytes == other.bytes; }
  bool operator!=(const RenderSnapshot& other) const { return !(*this == other); }
};

inline Snapshot make_snapshot(const void* camera_matrix, const void* translation) {
  Snapshot out;
  std::memcpy(out.bytes.data(), camera_matrix, kCameraMatrixBytes);
  std::memcpy(out.bytes.data() + kCameraMatrixBytes, translation, kTranslationBytes);
  return out;
}

inline RenderSnapshot make_render_snapshot(const void* camera_matrix,
                                           const void* hvdf_off,
                                           const void* fog,
                                           const void* translation,
                                           const void* rotation,
                                           const void* perspective) {
  RenderSnapshot out;
  auto* dst = out.bytes.data();
  std::memcpy(dst, camera_matrix, 4 * 16);
  std::memcpy(dst + 4 * 16, hvdf_off, 16);
  std::memcpy(dst + 5 * 16, fog, sizeof(float));
  std::memcpy(dst + 6 * 16, translation, 16);
  std::memcpy(dst + 7 * 16, rotation, 4 * 16);
  std::memcpy(dst + 11 * 16, perspective, 4 * 16);
  return out;
}

namespace jak1_math_camera {

// math-camera's asserted offsets in decompiler/config/jak1/all-types.gc include the four-byte
// basic-object type tag. A live GOAL basic pointer is four bytes past that tag (BASIC_OFFSET), so
// native reads use the asserted offsets minus four.
constexpr std::size_t kBasicObjectTypeTagBytes = 4;
constexpr std::size_t kPerspectiveOffset = 160 - kBasicObjectTypeTagBytes;
constexpr std::size_t kCameraRotationOffset = 368 - kBasicObjectTypeTagBytes;
constexpr std::size_t kCameraMatrixOffset = 576 - kBasicObjectTypeTagBytes;
constexpr std::size_t kHvdfOffset = 736 - kBasicObjectTypeTagBytes;
constexpr std::size_t kFogOffset = 832 - kBasicObjectTypeTagBytes;
constexpr std::size_t kTranslationOffset = 848 - kBasicObjectTypeTagBytes;
constexpr std::size_t kRequiredObjectBytes = kTranslationOffset + kTranslationBytes;
constexpr std::size_t kDeclaredObjectBytes = 0x424 - kBasicObjectTypeTagBytes;

static_assert(kRequiredObjectBytes <= kDeclaredObjectBytes);

}  // namespace jak1_math_camera

// Copies the Jak 1 fields serialized by add-pc-tfrag3-data from a live *math-camera* object into
// one fixed-size diagnostic value. The caller owns the object and must keep it stable for the
// duration of this synchronous copy.
inline bool try_make_jak1_live_render_snapshot(const void* object,
                                               std::size_t object_bytes,
                                               RenderSnapshot* out) {
  if (!object || !out || object_bytes < jak1_math_camera::kRequiredObjectBytes) {
    return false;
  }

  const auto* bytes = static_cast<const u8*>(object);
  *out = make_render_snapshot(bytes + jak1_math_camera::kCameraMatrixOffset,
                              bytes + jak1_math_camera::kHvdfOffset,
                              bytes + jak1_math_camera::kFogOffset,
                              bytes + jak1_math_camera::kTranslationOffset,
                              bytes + jak1_math_camera::kCameraRotationOffset,
                              bytes + jak1_math_camera::kPerspectiveOffset);
  return true;
}

template <typename SnapshotType>
inline u64 fingerprint(const SnapshotType& snapshot) {
  constexpr u64 kOffsetBasis = 14695981039346656037ull;
  constexpr u64 kPrime = 1099511628211ull;
  u64 hash = kOffsetBasis;
  for (const auto byte : snapshot.bytes) {
    hash ^= byte;
    hash *= kPrime;
  }
  return hash;
}

inline u8 mismatched_qwords(const Snapshot& expected, const Snapshot& actual) {
  u8 mask = 0;
  for (std::size_t qword = 0; qword < kSnapshotQwords; qword++) {
    if (std::memcmp(expected.bytes.data() + qword * 16,
                    actual.bytes.data() + qword * 16, 16) != 0) {
      mask |= static_cast<u8>(1u << qword);
    }
  }
  return mask;
}

inline u16 mismatched_render_qwords(const RenderSnapshot& expected,
                                    const RenderSnapshot& actual) {
  u16 mask = 0;
  for (std::size_t qword = 0; qword < kRenderSnapshotQwords; qword++) {
    if (std::memcmp(expected.bytes.data() + qword * 16,
                    actual.bytes.data() + qword * 16, 16) != 0) {
      mask |= static_cast<u16>(1u << qword);
    }
  }
  return mask;
}

inline void latch_render_mismatch_qwords_on_alternation(bool alternation,
                                                        u16 live_mismatch_qwords,
                                                        u16 packet_mismatch_qwords,
                                                        u16& last_live_mismatch_qwords,
                                                        u16& last_packet_mismatch_qwords) {
  if (alternation) {
    last_live_mismatch_qwords = live_mismatch_qwords;
    last_packet_mismatch_qwords = packet_mismatch_qwords;
  }
}

/*!
 * A scale-independent distance over every float component in a camera snapshot. This is a
 * diagnostic comparison only: it deliberately does not interpret the fields as a world-space pose.
 * Each component is normalized against the larger input magnitude so the large translation terms
 * cannot drown out changes elsewhere in the snapshot.
 */
template <typename SnapshotType>
inline double normalized_snapshot_distance(const SnapshotType& a, const SnapshotType& b) {
  constexpr std::size_t kFloatBytes = sizeof(float);
  constexpr std::size_t kFloatCount = sizeof(a.bytes) / kFloatBytes;
  double squared_distance = 0.0;
  for (std::size_t component = 0; component < kFloatCount; component++) {
    const auto offset = component * kFloatBytes;
    if (std::memcmp(a.bytes.data() + offset, b.bytes.data() + offset, kFloatBytes) == 0) {
      continue;
    }

    float a_value = 0.f;
    float b_value = 0.f;
    std::memcpy(&a_value, a.bytes.data() + offset, kFloatBytes);
    std::memcpy(&b_value, b.bytes.data() + offset, kFloatBytes);
    if (!std::isfinite(a_value) || !std::isfinite(b_value)) {
      squared_distance += 1.0;
      continue;
    }

    const double scale =
        std::max(1.0, std::max(std::abs((double)a_value), std::abs((double)b_value)));
    const double delta = ((double)b_value - (double)a_value) / scale;
    squared_distance += delta * delta;
  }
  return std::sqrt(squared_distance / kFloatCount);
}

struct ProducerAlternationObservation {
  bool has_three_frame_window = false;
  bool alternation = false;
  u64 older_frame_id = 0;
  u64 previous_frame_id = 0;
  u64 current_frame_id = 0;
  u64 older_fingerprint = 0;
  u64 previous_fingerprint = 0;
  u64 current_fingerprint = 0;
  double older_to_previous_distance = 0.0;
  double previous_to_current_distance = 0.0;
  double older_to_current_distance = 0.0;
};

using RenderAlternationObservation = ProducerAlternationObservation;

struct RetainedAlternationObservation {
  bool valid = false;
  u64 older_frame_id = 0;
  u64 previous_frame_id = 0;
  u64 current_frame_id = 0;
  u64 older_fingerprint = 0;
  u64 previous_fingerprint = 0;
  u64 current_fingerprint = 0;
  u64 host_tick_id = 0;
  u64 chain_ordinal = 0;
  u64 packet_fingerprint = 0;
  u16 live_mismatch_qwords = 0;
  u16 packet_mismatch_qwords = 0;
  double older_to_previous_distance = 0.0;
  double previous_to_current_distance = 0.0;
  double older_to_current_distance = 0.0;
};

inline void retain_alternation_observation(const RenderAlternationObservation& observation,
                                           u64 host_tick_id,
                                           u64 chain_ordinal,
                                           u64 packet_fingerprint,
                                           u16 live_mismatch_qwords,
                                           u16 packet_mismatch_qwords,
                                           RetainedAlternationObservation& retained) {
  if (!observation.alternation) {
    return;
  }

  retained.valid = true;
  retained.older_frame_id = observation.older_frame_id;
  retained.previous_frame_id = observation.previous_frame_id;
  retained.current_frame_id = observation.current_frame_id;
  retained.older_fingerprint = observation.older_fingerprint;
  retained.previous_fingerprint = observation.previous_fingerprint;
  retained.current_fingerprint = observation.current_fingerprint;
  retained.host_tick_id = host_tick_id;
  retained.chain_ordinal = chain_ordinal;
  retained.packet_fingerprint = packet_fingerprint;
  retained.live_mismatch_qwords = live_mismatch_qwords;
  retained.packet_mismatch_qwords = packet_mismatch_qwords;
  retained.older_to_previous_distance = observation.older_to_previous_distance;
  retained.previous_to_current_distance = observation.previous_to_current_distance;
  retained.older_to_current_distance = observation.older_to_current_distance;
}

/*!
 * Retains exactly two prior snapshots and identifies a strong A/B/A-style return: the
 * current snapshot is at most half as far from the two-frames-ago snapshot as either adjacent leg.
 * A small minimum adjacent distance keeps float noise in an otherwise still camera from firing the
 * diagnostic. Duplicate chains for one engine frame are ignored, and a frame gap
 * resets the bounded history rather than comparing non-consecutive producer states.
 */
template <typename SnapshotType>
class AlternationTrace {
 public:
  static constexpr double kReturnDistanceRatio = 0.5;
  static constexpr double kMinimumLegDistance = 1e-4;

  ProducerAlternationObservation observe(u64 frame_id, const SnapshotType& snapshot) {
    ProducerAlternationObservation out;
    if (m_history_size && frame_id == m_frame_ids[m_history_size - 1]) {
      return out;
    }
    if (!m_history_size || frame_id != m_frame_ids[m_history_size - 1] + 1) {
      m_snapshots[0] = snapshot;
      m_frame_ids[0] = frame_id;
      m_history_size = 1;
      return out;
    }
    if (m_history_size == 1) {
      m_snapshots[1] = snapshot;
      m_frame_ids[1] = frame_id;
      m_history_size = 2;
      return out;
    }

    out.has_three_frame_window = true;
    out.older_frame_id = m_frame_ids[0];
    out.previous_frame_id = m_frame_ids[1];
    out.current_frame_id = frame_id;
    out.older_fingerprint = fingerprint(m_snapshots[0]);
    out.previous_fingerprint = fingerprint(m_snapshots[1]);
    out.current_fingerprint = fingerprint(snapshot);
    out.older_to_previous_distance =
        normalized_snapshot_distance(m_snapshots[0], m_snapshots[1]);
    out.previous_to_current_distance = normalized_snapshot_distance(m_snapshots[1], snapshot);
    out.older_to_current_distance = normalized_snapshot_distance(m_snapshots[0], snapshot);

    const double smaller_leg =
        std::min(out.older_to_previous_distance, out.previous_to_current_distance);
    out.alternation =
        smaller_leg >= kMinimumLegDistance &&
        out.older_to_current_distance <= smaller_leg * kReturnDistanceRatio;
    if (out.alternation) {
      m_alternations++;
    }

    m_snapshots[0] = m_snapshots[1];
    m_snapshots[1] = snapshot;
    m_frame_ids[0] = m_frame_ids[1];
    m_frame_ids[1] = frame_id;
    return out;
  }

  u64 alternations() const { return m_alternations; }

 private:
  std::array<SnapshotType, 2> m_snapshots;
  std::array<u64, 2> m_frame_ids = {};
  std::size_t m_history_size = 0;
  u64 m_alternations = 0;
};

using ProducerAlternationTrace = AlternationTrace<Snapshot>;
using RenderAlternationTrace = AlternationTrace<RenderSnapshot>;

struct Observation {
  u8 expected_mismatch_qwords = 0;
  u8 packet_mismatch_qwords = 0;
};

class FrameTrace {
 public:
  void reset(const Snapshot* expected) {
    m_expected_valid = expected != nullptr;
    if (expected) {
      m_expected = *expected;
    }
    m_first_packet_valid = false;
    m_packet_count = 0;
    m_expected_mismatches = 0;
    m_packet_mismatches = 0;
    m_expected_mismatch_qwords = 0;
    m_packet_mismatch_qwords = 0;
  }

  Observation observe(const Snapshot& packet) {
    Observation out;
    m_packet_count++;
    if (m_expected_valid) {
      out.expected_mismatch_qwords = mismatched_qwords(m_expected, packet);
      if (out.expected_mismatch_qwords) {
        m_expected_mismatches++;
        m_expected_mismatch_qwords |= out.expected_mismatch_qwords;
      }
    }
    if (!m_first_packet_valid) {
      m_first_packet = packet;
      m_first_packet_valid = true;
    } else {
      out.packet_mismatch_qwords = mismatched_qwords(m_first_packet, packet);
      if (out.packet_mismatch_qwords) {
        m_packet_mismatches++;
        m_packet_mismatch_qwords |= out.packet_mismatch_qwords;
      }
    }
    return out;
  }

  bool has_packet() const { return m_first_packet_valid; }
  u64 first_packet_fingerprint() const {
    return m_first_packet_valid ? fingerprint(m_first_packet) : 0;
  }
  int packet_count() const { return m_packet_count; }
  int expected_mismatches() const { return m_expected_mismatches; }
  int packet_mismatches() const { return m_packet_mismatches; }
  u8 expected_mismatch_qwords() const { return m_expected_mismatch_qwords; }
  u8 packet_mismatch_qwords() const { return m_packet_mismatch_qwords; }

 private:
  bool m_expected_valid = false;
  Snapshot m_expected;
  bool m_first_packet_valid = false;
  Snapshot m_first_packet;
  int m_packet_count = 0;
  int m_expected_mismatches = 0;
  int m_packet_mismatches = 0;
  u8 m_expected_mismatch_qwords = 0;
  u8 m_packet_mismatch_qwords = 0;
};

struct RenderObservation {
  u16 expected_mismatch_qwords = 0;
  u16 packet_mismatch_qwords = 0;
};

class RenderFrameTrace {
 public:
  void reset(const RenderSnapshot* expected = nullptr) {
    m_expected_valid = expected != nullptr;
    if (expected) {
      m_expected = *expected;
    }
    m_first_packet_valid = false;
    m_packet_count = 0;
    m_expected_mismatches = 0;
    m_packet_mismatches = 0;
    m_expected_mismatch_qwords = 0;
    m_packet_mismatch_qwords = 0;
  }

  RenderObservation observe(const RenderSnapshot& packet) {
    RenderObservation out;
    m_packet_count++;
    if (m_expected_valid) {
      out.expected_mismatch_qwords = mismatched_render_qwords(m_expected, packet);
      if (out.expected_mismatch_qwords) {
        m_expected_mismatches++;
        m_expected_mismatch_qwords |= out.expected_mismatch_qwords;
      }
    }
    if (!m_first_packet_valid) {
      m_first_packet = packet;
      m_first_packet_valid = true;
    } else {
      out.packet_mismatch_qwords = mismatched_render_qwords(m_first_packet, packet);
      if (out.packet_mismatch_qwords) {
        m_packet_mismatches++;
        m_packet_mismatch_qwords |= out.packet_mismatch_qwords;
      }
    }
    return out;
  }

  bool has_packet() const { return m_first_packet_valid; }
  const RenderSnapshot& first_packet() const { return m_first_packet; }
  u64 first_packet_fingerprint() const {
    return m_first_packet_valid ? fingerprint(m_first_packet) : 0;
  }
  int packet_count() const { return m_packet_count; }
  int expected_mismatches() const { return m_expected_mismatches; }
  int packet_mismatches() const { return m_packet_mismatches; }
  u16 expected_mismatch_qwords() const { return m_expected_mismatch_qwords; }
  u16 packet_mismatch_qwords() const { return m_packet_mismatch_qwords; }

 private:
  bool m_expected_valid = false;
  RenderSnapshot m_expected;
  bool m_first_packet_valid = false;
  RenderSnapshot m_first_packet;
  int m_packet_count = 0;
  int m_expected_mismatches = 0;
  int m_packet_mismatches = 0;
  u16 m_expected_mismatch_qwords = 0;
  u16 m_packet_mismatch_qwords = 0;
};

class PresentationOrderTrace {
 public:
  bool observe(u64 submission_id, double presented_time) {
    if (presented_time == 0.0) {
      m_drops++;
      m_last_dropped_submission_id = submission_id;
      return true;
    }

    bool inverted = false;
    for (std::size_t i = 0; i < m_history_size; i++) {
      const auto& prior = m_history[i];
      const bool submission_before = submission_id < prior.submission_id;
      const bool submission_after = submission_id > prior.submission_id;
      const bool time_before = presented_time < prior.presented_time;
      const bool time_after = presented_time > prior.presented_time;
      inverted |= (submission_before && time_after) || (submission_after && time_before);
    }

    m_presentations++;
    if (inverted) {
      m_mismatches++;
    }

    m_history[m_next_history] = {submission_id, presented_time};
    m_next_history = (m_next_history + 1) % kHistorySize;
    m_history_size = std::min(m_history_size + 1, kHistorySize);

    if (presented_time > m_last_presented_time ||
        (presented_time == m_last_presented_time && submission_id > m_last_submission_id)) {
      m_last_submission_id = submission_id;
      m_last_presented_time = presented_time;
    }
    return !inverted;
  }

  u64 presentations() const { return m_presentations; }
  u64 mismatches() const { return m_mismatches; }
  u64 drops() const { return m_drops; }
  u64 last_submission_id() const { return m_last_submission_id; }
  u64 last_dropped_submission_id() const { return m_last_dropped_submission_id; }
  double last_presented_time() const { return m_last_presented_time; }

 private:
  struct SuccessfulPresentation {
    u64 submission_id = 0;
    double presented_time = 0.0;
  };

  static constexpr std::size_t kHistorySize = 64;
  std::array<SuccessfulPresentation, kHistorySize> m_history = {};
  std::size_t m_history_size = 0;
  std::size_t m_next_history = 0;
  u64 m_presentations = 0;
  u64 m_mismatches = 0;
  u64 m_drops = 0;
  u64 m_last_submission_id = 0;
  u64 m_last_dropped_submission_id = 0;
  double m_last_presented_time = 0.0;
};

}  // namespace metal_camera_trace
