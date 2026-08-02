#include "game/graphics/pipelines/metal/metal_camera_trace.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  if (condition) {
    std::printf("ok   %s\n", what);
  } else {
    std::printf("FAIL %s\n", what);
    failures++;
  }
}

metal_camera_trace::Snapshot scalar_snapshot(float value) {
  std::array<float, metal_camera_trace::kSnapshotBytes / sizeof(float)> components;
  components.fill(value);
  metal_camera_trace::Snapshot out;
  std::memcpy(out.bytes.data(), components.data(), out.bytes.size());
  return out;
}

}  // namespace

int main() {
  std::array<u8, metal_camera_trace::kCameraMatrixBytes> camera = {};
  std::array<u8, metal_camera_trace::kTranslationBytes> translation = {};
  for (std::size_t i = 0; i < camera.size(); i++) {
    camera[i] = static_cast<u8>(i + 1);
  }
  for (std::size_t i = 0; i < translation.size(); i++) {
    translation[i] = static_cast<u8>(0x80 + i);
  }

  const auto expected = metal_camera_trace::make_snapshot(camera.data(), translation.data());
  auto changed_camera = camera;
  changed_camera[2 * 16 + 3] ^= 0x5a;
  auto changed_translation = translation;
  changed_translation[7] ^= 0xa5;
  const auto changed =
      metal_camera_trace::make_snapshot(changed_camera.data(), changed_translation.data());

  check(expected.bytes[0] == 1 && expected.bytes[63] == 64 && expected.bytes[64] == 0x80,
        "the snapshot concatenates camera-temp and trans without padding");
  check(metal_camera_trace::fingerprint(expected) == metal_camera_trace::fingerprint(expected),
        "equal camera snapshots have equal fingerprints");
  check(metal_camera_trace::fingerprint(expected) != metal_camera_trace::fingerprint(changed),
        "a changed camera snapshot has a different fingerprint");
  check(metal_camera_trace::mismatched_qwords(expected, changed) == ((1u << 2) | (1u << 4)),
        "camera and translation differences name their exact qwords");

  metal_camera_trace::FrameTrace trace;
  trace.reset(&expected);
  auto first = trace.observe(expected);
  auto second = trace.observe(expected);
  auto third = trace.observe(changed);
  check(first.expected_mismatch_qwords == 0 && first.packet_mismatch_qwords == 0,
        "the first matching packet establishes the chain baseline");
  check(second.expected_mismatch_qwords == 0 && second.packet_mismatch_qwords == 0,
        "equal packets agree with the live camera and each other");
  check(third.expected_mismatch_qwords == ((1u << 2) | (1u << 4)) &&
            third.packet_mismatch_qwords == ((1u << 2) | (1u << 4)),
        "a bad packet is attributed against both live and first-packet state");
  check(trace.packet_count() == 3 && trace.expected_mismatches() == 1 &&
            trace.packet_mismatches() == 1,
        "the frame trace counts packets and both mismatch classes");

  trace.reset(nullptr);
  trace.observe(expected);
  trace.observe(changed);
  check(trace.expected_mismatches() == 0 && trace.packet_mismatches() == 1,
        "capture replay still checks packet agreement without a live camera");

  check(metal_camera_trace::normalized_snapshot_distance(scalar_snapshot(2.f),
                                                          scalar_snapshot(2.f)) == 0.0,
        "equal producer snapshots have zero normalized distance");

  metal_camera_trace::ProducerAlternationTrace smooth_producer;
  const auto smooth_first = smooth_producer.observe(100, scalar_snapshot(0.f));
  const auto smooth_second = smooth_producer.observe(101, scalar_snapshot(0.5f));
  const auto smooth_third = smooth_producer.observe(102, scalar_snapshot(1.f));
  check(!smooth_first.has_three_frame_window && !smooth_second.has_three_frame_window &&
            smooth_third.has_three_frame_window && !smooth_third.alternation,
        "smooth consecutive producer motion does not trigger an alternation");
  check(smooth_third.older_to_current_distance > smooth_third.older_to_previous_distance &&
            smooth_third.older_to_current_distance > smooth_third.previous_to_current_distance,
        "smooth-motion distances attribute continued travel away from the older snapshot");

  metal_camera_trace::ProducerAlternationTrace alternating_producer;
  alternating_producer.observe(200, scalar_snapshot(0.f));
  alternating_producer.observe(201, scalar_snapshot(1.f));
  const auto returned = alternating_producer.observe(202, scalar_snapshot(0.1f));
  check(returned.has_three_frame_window && returned.alternation &&
            returned.older_fingerprint != returned.current_fingerprint,
        "a non-identical A/B/A-style producer return triggers the diagnostic");
  check(returned.older_to_current_distance < returned.older_to_previous_distance &&
            returned.older_to_current_distance < returned.previous_to_current_distance,
        "the trigger reports the return distance and both adjacent distances");
  check(alternating_producer.alternations() == 1,
        "producer alternations are counted deterministically");

  auto durable_event = returned;
  const auto later_non_event = alternating_producer.observe(203, scalar_snapshot(0.2f));
  if (later_non_event.alternation) {
    durable_event = later_non_event;
  }
  check(later_non_event.has_three_frame_window && !later_non_event.alternation &&
            durable_event.older_frame_id == returned.older_frame_id &&
            durable_event.previous_frame_id == returned.previous_frame_id &&
            durable_event.current_frame_id == returned.current_frame_id &&
            durable_event.older_fingerprint == returned.older_fingerprint &&
            durable_event.previous_fingerprint == returned.previous_fingerprint &&
            durable_event.current_fingerprint == returned.current_fingerprint,
        "a later ordinary window does not erase the last producer alternation evidence");

  alternating_producer.observe(203, scalar_snapshot(1.f));
  alternating_producer.observe(205, scalar_snapshot(1.f));
  const auto after_gap = alternating_producer.observe(206, scalar_snapshot(0.f));
  check(!after_gap.has_three_frame_window && alternating_producer.alternations() == 1,
        "duplicate frames are ignored and frame gaps reset bounded producer history");

  metal_camera_trace::PresentationOrderTrace presentation;
  check(presentation.observe(1, 10.0) && presentation.observe(2, 11.0),
        "monotonic submission and presentation identities are accepted");
  check(presentation.observe(4, 13.0) && presentation.observe(3, 12.0),
        "out-of-order callbacks with consistent submission and presentation order are accepted");
  check(!presentation.observe(5, 12.5),
        "an actual submission and presentation-time inversion is detected");
  check(presentation.observe(6, 0.0),
        "a zero presented time is recorded as a drop rather than an order reversal");
  check(presentation.presentations() == 5 && presentation.mismatches() == 1 &&
            presentation.drops() == 1 && presentation.last_dropped_submission_id() == 6,
        "presentation provenance separates completions, inversions, and drops");
  check(presentation.last_submission_id() == 4 && presentation.last_presented_time() == 13.0,
        "last presented identity follows maximum actual presentation time");

  return failures ? 1 : 0;
}
