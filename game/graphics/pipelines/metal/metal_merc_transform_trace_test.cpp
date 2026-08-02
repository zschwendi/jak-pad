#include "game/graphics/pipelines/metal/metal_merc_transform_trace.h"

#include <array>
#include <cstdio>
#include <limits>

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

metal_merc_transform_trace::Event observe(metal_merc_transform_trace::Tracker& tracker,
                                          u64 frame_id,
                                          int slot,
                                          double x,
                                          double y,
                                          double z) {
  return tracker.observe(frame_id, slot, 0x1234, frame_id * 17, x, y, z, 0x1000);
}

}  // namespace

int main() {
  std::array<float, metal_merc_transform_trace::kMatrixLaneCount> lanes = {};
  lanes[3] = std::numeric_limits<float>::infinity();
  lanes[16] = std::numeric_limits<float>::quiet_NaN();
  lanes[27] = -std::numeric_limits<float>::infinity();
  check(metal_merc_transform_trace::nonfinite_lane_mask(lanes.data()) ==
            ((1u << 3) | (1u << 16) | (1u << 27)),
        "the non-finite mask names exact tmat and nmat lanes");

  metal_merc_transform_trace::Tracker healthy;
  observe(healthy, 10, 0, 1.0, 1.0, 1.0);
  check(!observe(healthy, 11, 0, 1.1, 1.0, 0.9).valid(),
        "ordinary consecutive-frame scale variation is quiet");

  metal_merc_transform_trace::Tracker scale;
  observe(scale, 20, 1, 1.0, 1.0, 1.0);
  const auto scaled = observe(scale, 21, 1, 2.0, 2.0, 2.0);
  check(scaled.issue_mask == metal_merc_transform_trace::SCALE_DISCONTINUITY &&
            scaled.previous_frame_id == 20 && scaled.current_frame_id == 21 &&
            scaled.previous_scale == 1.0 && scaled.current_scale == 2.0 &&
            scaled.previous_matrix_hash != scaled.current_matrix_hash,
        "an exact 2x uniform jump reports scale with consecutive-frame evidence");

  metal_merc_transform_trace::Tracker aspect;
  observe(aspect, 30, 2, 1.0, 1.0, 1.0);
  const auto flattened = observe(aspect, 31, 2, 0.25, 2.0, 2.0);
  check(flattened.issue_mask == metal_merc_transform_trace::ASPECT_DISCONTINUITY &&
            flattened.previous_aspect == 1.0 && flattened.current_aspect == 8.0 &&
            flattened.previous_scale == flattened.current_scale,
        "a volume-preserving flattening reports aspect without scale");

  const auto reshaped = observe(aspect, 32, 2, 2.0, 0.25, 2.0);
  check(reshaped.issue_mask == metal_merc_transform_trace::ASPECT_DISCONTINUITY &&
            reshaped.previous_aspect == reshaped.current_aspect,
        "a flattened axis swap reports aspect even when the max/min ratio is unchanged");

  metal_merc_transform_trace::Tracker uniform_aba;
  const auto uniform_b =
      uniform_aba.observe(60, 6, 0x6000, 0xa0, 1.0, 1.0, 1.0, 0x1600);
  check(!uniform_b.valid(), "the first uniform A sample only establishes a baseline");
  const auto uniform_ab =
      uniform_aba.observe(61, 6, 0x6000, 0xb0, 3.0, 3.0, 3.0, 0x2600);
  const auto uniform_ba =
      uniform_aba.observe(62, 6, 0x6000, 0xa0, 1.0, 1.0, 1.0, 0x1600);
  check(uniform_ab.issue_mask == metal_merc_transform_trace::SCALE_DISCONTINUITY &&
            uniform_ba.issue_mask == metal_merc_transform_trace::SCALE_DISCONTINUITY &&
            uniform_ab.previous_scale == uniform_ba.current_scale &&
            uniform_ab.current_scale == uniform_ba.previous_scale &&
            uniform_ab.previous_aspect == uniform_ab.current_aspect &&
            uniform_ba.previous_aspect == uniform_ba.current_aspect,
        "uniform A/B/A retains reversible scale-only evidence");
  check(uniform_ab.previous_matrix_hash == uniform_ba.current_matrix_hash &&
            uniform_ab.current_matrix_hash == uniform_ba.previous_matrix_hash &&
            uniform_ab.previous_source_base == uniform_ba.current_source_base &&
            uniform_ab.current_source_base == uniform_ba.previous_source_base,
        "uniform A/B/A retains reversible matrix and source identities");

  metal_merc_transform_trace::Tracker flattened_aba;
  flattened_aba.observe(70, 7, 0x7000, 0xa1, 1.0, 1.0, 1.0, 0x1700);
  const auto flattened_ab =
      flattened_aba.observe(71, 7, 0x7000, 0xb1, 0.25, 2.0, 2.0, 0x2700);
  const auto flattened_ba =
      flattened_aba.observe(72, 7, 0x7000, 0xa1, 1.0, 1.0, 1.0, 0x1700);
  check(flattened_ab.issue_mask == metal_merc_transform_trace::ASPECT_DISCONTINUITY &&
            flattened_ba.issue_mask == metal_merc_transform_trace::ASPECT_DISCONTINUITY &&
            flattened_ab.previous_scale == flattened_ab.current_scale &&
            flattened_ba.previous_scale == flattened_ba.current_scale &&
            flattened_ab.previous_axis_norm_x == flattened_ba.current_axis_norm_x &&
            flattened_ab.current_axis_norm_x == flattened_ba.previous_axis_norm_x &&
            flattened_ab.previous_aspect == flattened_ba.current_aspect &&
            flattened_ab.current_aspect == flattened_ba.previous_aspect,
        "flattened A/B/A retains reversible aspect and per-axis evidence without scale change");

  metal_merc_transform_trace::Tracker gaps;
  observe(gaps, 40, 3, 1.0, 1.0, 1.0);
  check(!observe(gaps, 42, 3, 4.0, 4.0, 4.0).valid(),
        "a frame gap resets comparison instead of inventing a jump");
  check(!observe(gaps, 42, 3, 8.0, 8.0, 8.0).valid(),
        "a duplicate frame never generates a second observation");
  check(!observe(gaps, 43, 4, 16.0, 16.0, 16.0).valid(),
        "bone slots keep independent histories");

  metal_merc_transform_trace::Tracker invalid;
  observe(invalid, 50, 5, 1.0, 1.0, 1.0);
  check(!observe(invalid, 51, 5, std::numeric_limits<double>::infinity(), 1.0, 1.0).valid(),
        "invalid axis data clears the affected slot without reporting a finite jump");
  check(!observe(invalid, 52, 5, 4.0, 4.0, 4.0).valid(),
        "tracking resumes from scratch after invalid axis data");
  check(!observe(invalid, 0, 5, 8.0, 8.0, 8.0).valid(),
        "unknown engine frame identity is ignored");

  return failures ? 1 : 0;
}
