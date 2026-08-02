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

metal_camera_trace::RenderSnapshot scalar_render_snapshot(float value) {
  std::array<float, metal_camera_trace::kRenderSnapshotBytes / sizeof(float)> components;
  components.fill(value);
  metal_camera_trace::RenderSnapshot out;
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

  std::array<u8, 16> hvdf = {};
  std::array<u8, 16> fog = {};
  std::array<u8, 4 * 16> rotation = {};
  std::array<u8, 4 * 16> perspective = {};
  for (std::size_t i = 0; i < hvdf.size(); i++) {
    hvdf[i] = static_cast<u8>(0x20 + i);
    fog[i] = static_cast<u8>(0x30 + i);
  }
  for (std::size_t i = 0; i < rotation.size(); i++) {
    rotation[i] = static_cast<u8>(0x40 + i);
    perspective[i] = static_cast<u8>(0x90 + i);
  }

  const auto expected_render = metal_camera_trace::make_render_snapshot(
      camera.data(), hvdf.data(), fog.data(), translation.data(), rotation.data(),
      perspective.data());
  auto changed_rotation = rotation;
  changed_rotation[2 * 16 + 3] ^= 0x5a;
  auto changed_perspective = perspective;
  changed_perspective[3 * 16 + 7] ^= 0xa5;
  const auto changed_render = metal_camera_trace::make_render_snapshot(
      camera.data(), hvdf.data(), fog.data(), translation.data(), changed_rotation.data(),
      changed_perspective.data());
  check(expected_render.bytes[0] == 1 && expected_render.bytes[64] == 0x20 &&
            expected_render.bytes[80] == 0x30 && expected_render.bytes[84] == 0 &&
            expected_render.bytes[96] == 0x80 && expected_render.bytes[112] == 0x40 &&
            expected_render.bytes[176] == 0x90,
        "the render snapshot preserves the packet's view/projection field order");
  const auto producer_subset_before =
      metal_camera_trace::make_snapshot(camera.data(), translation.data());
  const auto producer_subset_after =
      metal_camera_trace::make_snapshot(camera.data(), translation.data());
  check(metal_camera_trace::fingerprint(producer_subset_before) ==
                metal_camera_trace::fingerprint(producer_subset_after) &&
            metal_camera_trace::fingerprint(expected_render) !=
                metal_camera_trace::fingerprint(changed_render),
        "rotation and perspective changes expose the producer subset's diagnostic blind spot");
  check(metal_camera_trace::mismatched_render_qwords(expected_render, changed_render) ==
            ((1u << 9) | (1u << 14)),
        "render-camera differences name their exact rotation and perspective qwords");

  std::array<u8, metal_camera_trace::jak1_math_camera::kDeclaredObjectBytes> math_camera = {};
  const auto copy_field = [&](std::size_t offset, const auto& field) {
    std::memcpy(math_camera.data() + offset, field.data(), field.size());
  };
  copy_field(metal_camera_trace::jak1_math_camera::kPerspectiveOffset, perspective);
  copy_field(metal_camera_trace::jak1_math_camera::kCameraRotationOffset, rotation);
  copy_field(metal_camera_trace::jak1_math_camera::kCameraMatrixOffset, camera);
  copy_field(metal_camera_trace::jak1_math_camera::kHvdfOffset, hvdf);
  copy_field(metal_camera_trace::jak1_math_camera::kFogOffset, fog);
  copy_field(metal_camera_trace::jak1_math_camera::kTranslationOffset, translation);
  metal_camera_trace::RenderSnapshot live_render;
  check(metal_camera_trace::try_make_jak1_live_render_snapshot(
            math_camera.data(), math_camera.size(), &live_render) &&
            live_render == expected_render,
        "the live Jak 1 math-camera layout reproduces add-pc-tfrag3-data serialization");
  check(!metal_camera_trace::try_make_jak1_live_render_snapshot(
            math_camera.data(), metal_camera_trace::jak1_math_camera::kRequiredObjectBytes - 1,
            &live_render) &&
            !metal_camera_trace::try_make_jak1_live_render_snapshot(
                nullptr, math_camera.size(), &live_render) &&
            !metal_camera_trace::try_make_jak1_live_render_snapshot(
                math_camera.data(), math_camera.size(), nullptr),
        "the live Jak 1 math-camera copy rejects null and truncated objects");

  metal_camera_trace::RenderFrameTrace render_trace;
  render_trace.reset(&expected_render);
  const auto render_first = render_trace.observe(expected_render);
  const auto render_second = render_trace.observe(changed_render);
  check(render_first.expected_mismatch_qwords == 0 && render_first.packet_mismatch_qwords == 0 &&
            render_second.expected_mismatch_qwords == ((1u << 9) | (1u << 14)) &&
            render_second.packet_mismatch_qwords == ((1u << 9) | (1u << 14)) &&
            render_trace.expected_mismatches() == 1 && render_trace.packet_mismatches() == 1,
        "the render trace compares every packet field with live and first-packet state");
  render_trace.reset();
  render_trace.observe(expected_render);
  render_trace.observe(changed_render);
  check(render_trace.expected_mismatches() == 0 && render_trace.packet_mismatches() == 1,
        "the full render trace remains packet-only when the host supplies no live snapshot");

  u16 last_alternation_live_mismatch_qwords = 0xffff;
  u16 last_alternation_packet_mismatch_qwords = 0xffff;
  metal_camera_trace::latch_render_mismatch_qwords_on_alternation(
      true, render_first.expected_mismatch_qwords, render_first.packet_mismatch_qwords,
      last_alternation_live_mismatch_qwords, last_alternation_packet_mismatch_qwords);
  check(last_alternation_live_mismatch_qwords == 0 &&
            last_alternation_packet_mismatch_qwords == 0,
        "a matching alternation chain latches zero full render mismatch masks");
  metal_camera_trace::latch_render_mismatch_qwords_on_alternation(
      true, render_second.expected_mismatch_qwords, render_second.packet_mismatch_qwords,
      last_alternation_live_mismatch_qwords, last_alternation_packet_mismatch_qwords);
  check(last_alternation_live_mismatch_qwords == ((1u << 9) | (1u << 14)) &&
            last_alternation_packet_mismatch_qwords == ((1u << 9) | (1u << 14)),
        "a divergent alternation chain latches its full live and packet mismatch masks");
  metal_camera_trace::latch_render_mismatch_qwords_on_alternation(
      false, 0, 0, last_alternation_live_mismatch_qwords,
      last_alternation_packet_mismatch_qwords);
  check(last_alternation_live_mismatch_qwords == ((1u << 9) | (1u << 14)) &&
            last_alternation_packet_mismatch_qwords == ((1u << 9) | (1u << 14)),
        "a later non-alternating chain preserves the last full render mismatch masks");

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

  metal_camera_trace::RenderAlternationTrace alternating_render;
  alternating_render.observe(300, scalar_render_snapshot(0.f));
  alternating_render.observe(301, scalar_render_snapshot(1.f));
  const auto render_returned = alternating_render.observe(302, scalar_render_snapshot(0.1f));
  check(render_returned.has_three_frame_window && render_returned.alternation &&
            alternating_render.alternations() == 1,
        "full view/projection A/B/A returns are counted independently of the producer subset");

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
