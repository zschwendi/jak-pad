#include "game/graphics/pipelines/metal/metal_stereo_eye_marker.h"

int main() {
  using metal_renderer::stereo_eye_marker_plan;
  using metal_renderer::StereoEyeMarkerEye;

  constexpr auto left = stereo_eye_marker_plan(640, 480, StereoEyeMarkerEye::left);
  static_assert(left.valid());
  static_assert(left.x == 14 && left.y == 14 && left.width == 3 && left.height == 16);

  constexpr auto right = stereo_eye_marker_plan(640, 480, StereoEyeMarkerEye::right);
  static_assert(right.valid());
  static_assert(right.x == 14 && right.y == 14 && right.width == 16 && right.height == 3);

  constexpr auto too_small = stereo_eye_marker_plan(14, 14, StereoEyeMarkerEye::left);
  static_assert(!too_small.valid());

  constexpr auto clipped_left = stereo_eye_marker_plan(15, 20, StereoEyeMarkerEye::left);
  static_assert(clipped_left.valid());
  static_assert(clipped_left.x + clipped_left.width <= 15);
  static_assert(clipped_left.y + clipped_left.height <= 20);

  constexpr auto clipped_right = stereo_eye_marker_plan(20, 15, StereoEyeMarkerEye::right);
  static_assert(clipped_right.valid());
  static_assert(clipped_right.x + clipped_right.width <= 20);
  static_assert(clipped_right.y + clipped_right.height <= 15);
  return 0;
}
