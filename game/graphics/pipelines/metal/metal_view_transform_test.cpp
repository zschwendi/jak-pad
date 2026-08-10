#include "game/graphics/pipelines/metal/metal_view_transform.h"

#include <cmath>
#include <limits>

namespace {

bool close(float a, float b) {
  return std::abs(a - b) < 1e-6f;
}

metal_renderer::ViewTransform post_clip_eye(float eye_slope, float convergence_depth) {
  metal_renderer::ViewTransform out;
  // x' = x + eye_slope * (z - convergence_depth * w).
  out.clip_from_game_clip[2 * 4 + 0] = eye_slope;
  out.clip_from_game_clip[3 * 4 + 0] = -eye_slope * convergence_depth;
  return out;
}

float ndc_disparity(const metal_renderer::ViewTransform& left,
                    const metal_renderer::ViewTransform& right,
                    float depth) {
  const std::array<float, 4> clip = {0.25f, -0.5f, depth, 1.f};
  const auto left_clip = metal_renderer::apply(left, clip);
  const auto right_clip = metal_renderer::apply(right, clip);
  return right_clip[0] / right_clip[3] - left_clip[0] / left_clip[3];
}

float jak1_default_metal_depth(float camera_depth) {
  constexpr float kNear = 1024.f;
  constexpr float kFar = 40960000.f;
  constexpr float kDepthMinimum = 100.f;
  constexpr float kDepthMaximum = 16760631.f;
  constexpr float kMetalDepthScale = 16777216.f;
  const float game_depth =
      kDepthMinimum + (kDepthMaximum - kDepthMinimum) * kNear * (kFar / camera_depth - 1.f) /
                          (kFar - kNear);
  return game_depth / kMetalDepthScale;
}

}  // namespace

int main() {
  using metal_renderer::StereoDrawPath;
  using metal_renderer::StereoSpace;

  static_assert(metal_renderer::stereo_space_for(StereoDrawPath::Direct) == StereoSpace::Screen);
  static_assert(metal_renderer::stereo_space_for(StereoDrawPath::Sprite3d) == StereoSpace::World);
  static_assert(metal_renderer::stereo_space_for(StereoDrawPath::Sprite2d) == StereoSpace::Screen);
  static_assert(metal_renderer::stereo_space_for(StereoDrawPath::SpriteHud) == StereoSpace::Screen);
  static_assert(metal_renderer::stereo_space_for(StereoDrawPath::SpriteDistortion) ==
                StereoSpace::Screen);
  static_assert(metal_renderer::stereo_space_for(StereoDrawPath::Sky) == StereoSpace::World);
  static_assert(metal_renderer::stereo_space_for(StereoDrawPath::Ocean) == StereoSpace::World);
  static_assert(metal_renderer::stereo_space_for(StereoDrawPath::GenericWorld) ==
                StereoSpace::World);
  static_assert(metal_renderer::stereo_space_for(StereoDrawPath::GenericHud) ==
                StereoSpace::Screen);
  static_assert(metal_renderer::stereo_space_for(StereoDrawPath::ShadowVolume) ==
                StereoSpace::World);
  static_assert(metal_renderer::stereo_space_for(StereoDrawPath::ShadowOverlay) ==
                StereoSpace::Screen);

  const std::array<float, 4> clip = {0.25f, -0.5f, 0.75f, 1.f};
  const auto identity = metal_renderer::apply({}, clip);
  if (identity != clip) {
    return 1;
  }

  const auto left_transform = post_clip_eye(-0.08f, 0.f);
  const auto right_transform = post_clip_eye(0.08f, 0.f);
  const auto left = metal_renderer::apply(left_transform, clip);
  const auto right = metal_renderer::apply(right_transform, clip);
  if (!close(left[0], 0.19f) || !close(right[0], 0.31f) || !close(left[1], clip[1]) ||
      !close(left[2], clip[2]) || !close(left[3], clip[3]) || !close(right[1], clip[1]) ||
      !close(right[2], clip[2]) || !close(right[3], clip[3])) {
    return 1;
  }

  const std::array<float, 4> farther_clip = {clip[0], clip[1], 0.25f, clip[3]};
  const auto farther_left = metal_renderer::apply(left_transform, farther_clip);
  if (std::abs(left[0] - clip[0]) <= std::abs(farther_left[0] - clip[0])) {
    return 1;
  }

  constexpr float kSlope = 0.04f;
  constexpr float kConvergenceDepth = 0.25f;
  const auto converged_left = post_clip_eye(-kSlope * 0.5f, kConvergenceDepth);
  const auto converged_right = post_clip_eye(kSlope * 0.5f, kConvergenceDepth);
  if (!close(ndc_disparity(converged_left, converged_right, 0.f),
             -kSlope * kConvergenceDepth) ||
      !close(ndc_disparity(converged_left, converged_right, kConvergenceDepth), 0.f) ||
      !close(ndc_disparity(converged_left, converged_right, 1.f),
             kSlope * (1.f - kConvergenceDepth))) {
    return 1;
  }

  constexpr float kProductSlope = 0.025f;
  const auto product_left = post_clip_eye(-kProductSlope * 0.5f, 0.f);
  const auto product_right = post_clip_eye(kProductSlope * 0.5f, 0.f);
  const float one_meter_depth = jak1_default_metal_depth(4096.f);
  const float one_meter_pixels_at_640 =
      ndc_disparity(product_left, product_right, one_meter_depth) * 640.f * 0.5f;
  if (!close(one_meter_depth, 0.2497386f) || !close(one_meter_pixels_at_640, 1.9979088f)) {
    return 1;
  }

  const auto world_left = metal_renderer::apply(left_transform, StereoSpace::World, clip);
  const auto world_right = metal_renderer::apply(right_transform, StereoSpace::World, clip);
  if (world_left == world_right ||
      metal_renderer::apply(left_transform, StereoSpace::Screen, clip) != clip ||
      metal_renderer::apply(right_transform, StereoSpace::Screen, clip) != clip) {
    return 1;
  }

  auto invalid = metal_renderer::ViewTransform{};
  invalid.clip_from_game_clip[0] = std::numeric_limits<float>::quiet_NaN();
  if (!metal_renderer::is_finite({}) || metal_renderer::is_finite(invalid)) {
    return 1;
  }
  return 0;
}
