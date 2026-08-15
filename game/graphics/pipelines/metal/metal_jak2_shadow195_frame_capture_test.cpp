#include "game/graphics/pipelines/metal/metal_jak2_shadow195_frame_capture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <memory>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}

}  // namespace

int main() {
  using namespace metal_renderer;

  Jak2Shadow195FrameCaptureSelector selector;
  selector.match = Jak2Shadow195MatchHostTick | Jak2Shadow195MatchEngineFrame |
                   Jak2Shadow195MatchPlanFingerprint;
  selector.host_tick_id = 1950;
  selector.engine_frame_id = 650;
  selector.plan_fingerprint = 0x195195195ull;
  const Jak2Shadow195FrameIdentity selected = {1950, 1950, 650, 0x195195195ull};
  check(jak2_shadow195_frame_capture_selector_is_valid(selector) &&
            jak2_shadow195_frame_capture_selector_matches(selector, selected) &&
            !jak2_shadow195_frame_capture_selector_matches(
                selector, Jak2Shadow195FrameIdentity{1951, 1951, 650, 0x195195195ull}) &&
            !jak2_shadow195_frame_capture_selector_matches(
                selector, Jak2Shadow195FrameIdentity{1950, 1950, 651, 0x195195195ull}) &&
            !jak2_shadow195_frame_capture_selector_matches(
                selector, Jak2Shadow195FrameIdentity{1950, 1950, 650, 0x195195196ull}),
        "the live selector requires the requested tick, frame, and semantic fingerprint");
  check(!jak2_shadow195_frame_capture_selector_is_valid({}) &&
            !jak2_shadow195_frame_capture_selector_is_valid(
                {Jak2Shadow195MatchPlanFingerprint, 0, 0, 0}),
        "an empty selector and a zero requested fingerprint are rejected");

  Jak2Shadow195ReadbackLayout layout;
  check(plan_jak2_shadow195_readback_layout(2560, 1920, &layout) &&
            layout.before_color.row_bytes == 10240 &&
            layout.before_depth.row_bytes == 10240 &&
            layout.before_stencil.row_bytes == 2560 &&
            layout.volume_stencil.row_bytes == 2560 &&
            layout.after_color.row_bytes == 10240 &&
            layout.after_depth.row_bytes == 10240 &&
            layout.after_stencil.row_bytes == 2560 &&
            layout.total_bytes == 93388800 &&
            layout.total_bytes < kJak2Shadow195FrameCaptureMaxReadbackBytes,
        "the seven-plane 4x Eco layout fits in the bounded one-shot allocation");
  check(!plan_jak2_shadow195_readback_layout(
            kJak2Shadow195FrameCaptureMaxDimension + 1, 1, &layout) &&
            layout.total_bytes == 0 &&
            !plan_jak2_shadow195_readback_layout(
                kJak2Shadow195FrameCaptureMaxDimension,
                kJak2Shadow195FrameCaptureMaxDimension, &layout) &&
            layout.total_bytes == 0,
        "the dimension max plus one and a layout over the aggregate cap are rejected");

  auto capture = std::make_shared<Jak2Shadow195FrameCapture>();
  check(capture->arm(selector) && !capture->try_select({1950, 1950, 650, 1}) &&
            capture->try_select(selected) && !capture->try_select(selected) &&
            !capture->arm(selector),
        "selection is one-shot and cannot be replaced while its GPU result is pending");

  std::array<u8, 4 * 4 * 4> before = {};
  std::array<u8, 4 * 4 * 4> after = {};
  const auto set_pixel = [](auto* image, u32 x, u32 y, std::array<u8, 4> value) {
    const std::size_t offset = (y * 4 + x) * 4;
    std::copy(value.begin(), value.end(), image->begin() + offset);
  };
  set_pixel(&before, 1, 1, {10, 20, 30, 255});
  after = before;
  set_pixel(&after, 2, 1, {1, 2, 3, 255});
  set_pixel(&after, 1, 2, {4, 5, 6, 255});
  std::array<float, 16> before_depth;
  before_depth.fill(0.25f);
  auto after_depth = before_depth;
  after_depth[5] = 0.5f;
  after_depth[6] = std::numeric_limits<float>::infinity();
  std::array<u8, 16> before_stencil = {};
  before_stencil[1] = 2;
  auto volume_stencil = before_stencil;
  volume_stencil[6] = 1;
  const auto after_stencil = volume_stencil;

  Jak2Shadow195RenderTargetMetadata target;
  target.width = 4;
  target.height = 4;
  target.pixel_format = 80;
  target.color_slice = 1;
  capture->record_target(target);
  Jak2Shadow195CapturedRendererStats renderer;
  renderer.executions = 1;
  renderer.ready = 1;
  renderer.draw_calls = 4;
  renderer.reached_boundary = 1;
  capture->record_renderer(renderer);
  Jak2Shadow195AttachmentReadback readback;
  readback.before_color = before.data();
  readback.before_color_row_bytes = 16;
  readback.before_depth = reinterpret_cast<const u8*>(before_depth.data());
  readback.before_depth_row_bytes = 16;
  readback.before_stencil = before_stencil.data();
  readback.before_stencil_row_bytes = 4;
  readback.volume_stencil = volume_stencil.data();
  readback.volume_stencil_row_bytes = 4;
  readback.after_color = after.data();
  readback.after_color_row_bytes = 16;
  readback.after_depth = reinterpret_cast<const u8*>(after_depth.data());
  readback.after_depth_row_bytes = 16;
  readback.after_stencil = after_stencil.data();
  readback.after_stencil_row_bytes = 4;
  capture->complete(readback, 4, 4);
  const auto result = capture->result();
  check(result.status == Jak2Shadow195FrameCaptureStatus::Complete &&
            result.selected.host_tick_id == 1950 && result.selected.engine_frame_id == 650 &&
            result.target.width == 4 && result.target.height == 4 &&
            result.target.color_slice == 1 && result.before.hash != result.after.hash &&
            result.before.nonzero_pixels == 1 && result.before.nonzero_bounds.valid &&
            result.before.nonzero_bounds.min_x == 1 && result.before.nonzero_bounds.min_y == 1 &&
            result.before.nonzero_bounds.max_x_exclusive == 2 &&
            result.before.nonzero_bounds.max_y_exclusive == 2 &&
            result.after.nonzero_pixels == 3 && result.after.nonzero_bounds.min_x == 1 &&
            result.after.nonzero_bounds.min_y == 1 &&
            result.after.nonzero_bounds.max_x_exclusive == 3 &&
            result.after.nonzero_bounds.max_y_exclusive == 3 &&
            result.difference.changed_pixels == 2 && result.difference.changed_bounds.valid &&
            result.difference.changed_bounds.min_x == 1 &&
            result.difference.changed_bounds.min_y == 1 &&
            result.difference.changed_bounds.max_x_exclusive == 3 &&
            result.difference.changed_bounds.max_y_exclusive == 3 &&
            result.before_depth.finite_pixels == 16 &&
            result.before_depth.nonfinite_pixels == 0 &&
            result.before_depth.min_finite == 0.25f &&
            result.before_depth.max_finite == 0.25f &&
            result.after_depth.finite_pixels == 15 &&
            result.after_depth.nonfinite_pixels == 1 &&
            result.after_depth.min_finite == 0.25f &&
            result.after_depth.max_finite == 0.5f &&
            result.depth_difference.changed_pixels == 2 &&
            result.depth_difference.changed_bounds.min_x == 1 &&
            result.depth_difference.changed_bounds.max_x_exclusive == 3 &&
            result.before_stencil.nonzero_pixels == 1 &&
            result.volume_stencil.nonzero_pixels == 2 &&
            result.after_stencil.hash == result.volume_stencil.hash &&
            result.volume_stencil_difference.changed_pixels == 1 &&
            result.final_stencil_difference.changed_pixels == 0 &&
            result.renderer.executions == 1 && result.renderer.ready == 1 &&
            result.renderer.draw_calls == 4 && result.renderer.reached_boundary == 1,
        "the capture retains only fixed numeric identity, target, hashes, bounds, and stats");

  check(capture->arm({Jak2Shadow195MatchPlanFingerprint, 0, 0, 7}) &&
            capture->result().status == Jak2Shadow195FrameCaptureStatus::Armed,
        "a completed one-shot capture can be explicitly rearmed");
  capture->fail(9);
  check(capture->result().status == Jak2Shadow195FrameCaptureStatus::Armed,
        "an unrelated failure cannot disarm a selector before it matches");
  check(capture->try_select({0, 0, 0, 7}), "the rearmed selector can select its exact plan");
  capture->complete({}, 4, 4);
  check(capture->result().status == Jak2Shadow195FrameCaptureStatus::Failed &&
            capture->result().failure_reason ==
                static_cast<u32>(Jak2Shadow195FrameCaptureFailure::ReadbackFailed),
        "invalid readback input terminates the selected capture with a numeric failure");

  if (failures) {
    std::printf("FAIL: %d Jak II Shadow195 frame-capture checks failed\n", failures);
    return 1;
  }
  std::puts("Jak II Shadow195 frame-capture metadata tests passed");
  return 0;
}
