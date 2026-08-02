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

struct Snapshot {
  std::array<u8, kSnapshotBytes> bytes = {};

  bool operator==(const Snapshot& other) const { return bytes == other.bytes; }
  bool operator!=(const Snapshot& other) const { return !(*this == other); }
};

inline Snapshot make_snapshot(const void* camera_matrix, const void* translation) {
  Snapshot out;
  std::memcpy(out.bytes.data(), camera_matrix, kCameraMatrixBytes);
  std::memcpy(out.bytes.data() + kCameraMatrixBytes, translation, kTranslationBytes);
  return out;
}

inline u64 fingerprint(const Snapshot& snapshot) {
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

/*!
 * A scale-independent distance over the 20 float components in the producer snapshot. This is a
 * diagnostic comparison only: it deliberately does not interpret the matrix or translation as a
 * world-space pose. Each component is normalized against the larger input magnitude so the large
 * translation terms cannot drown out changes elsewhere in the snapshot.
 */
inline double normalized_snapshot_distance(const Snapshot& a, const Snapshot& b) {
  constexpr std::size_t kFloatBytes = sizeof(float);
  constexpr std::size_t kFloatCount = kSnapshotBytes / kFloatBytes;
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

/*!
 * Retains exactly two prior producer snapshots and identifies a strong A/B/A-style return: the
 * current snapshot is at most half as far from the two-frames-ago snapshot as either adjacent leg.
 * A small minimum adjacent distance keeps float noise in an otherwise still camera from firing the
 * one-time device diagnostic. Duplicate chains for one engine frame are ignored, and a frame gap
 * resets the bounded history rather than comparing non-consecutive producer states.
 */
class ProducerAlternationTrace {
 public:
  static constexpr double kReturnDistanceRatio = 0.5;
  static constexpr double kMinimumLegDistance = 1e-4;

  ProducerAlternationObservation observe(u64 frame_id, const Snapshot& snapshot) {
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
  std::array<Snapshot, 2> m_snapshots;
  std::array<u64, 2> m_frame_ids = {};
  std::size_t m_history_size = 0;
  u64 m_alternations = 0;
};

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
