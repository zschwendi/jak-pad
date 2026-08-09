#include "game/graphics/pipelines/metal/metal_view_transform.h"

#include <cmath>
#include <limits>

namespace {

bool close(float a, float b) {
  return std::abs(a - b) < 1e-6f;
}

metal_renderer::ViewTransform horizontal_parallax(float amount) {
  metal_renderer::ViewTransform out;
  // x' = x + amount * z. The separation therefore varies with game depth instead of shifting a
  // completed flat image by one constant amount.
  out.clip_from_game_clip[2 * 4 + 0] = amount;
  return out;
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

  const auto left = metal_renderer::apply(horizontal_parallax(-0.08f), clip);
  const auto right = metal_renderer::apply(horizontal_parallax(0.08f), clip);
  if (!close(left[0], 0.19f) || !close(right[0], 0.31f) || !close(left[1], clip[1]) ||
      !close(left[2], clip[2]) || !close(left[3], clip[3]) || !close(right[1], clip[1]) ||
      !close(right[2], clip[2]) || !close(right[3], clip[3])) {
    return 1;
  }

  const std::array<float, 4> nearer_clip = {clip[0], clip[1], 0.25f, clip[3]};
  const auto nearer_left = metal_renderer::apply(horizontal_parallax(-0.08f), nearer_clip);
  if (std::abs(left[0] - clip[0]) <= std::abs(nearer_left[0] - clip[0])) {
    return 1;
  }

  const auto world_left = metal_renderer::apply(horizontal_parallax(-0.08f), StereoSpace::World,
                                                clip);
  const auto world_right = metal_renderer::apply(horizontal_parallax(0.08f), StereoSpace::World,
                                                 clip);
  if (world_left == world_right ||
      metal_renderer::apply(horizontal_parallax(-0.08f), StereoSpace::Screen, clip) != clip ||
      metal_renderer::apply(horizontal_parallax(0.08f), StereoSpace::Screen, clip) != clip) {
    return 1;
  }

  auto invalid = metal_renderer::ViewTransform{};
  invalid.clip_from_game_clip[0] = std::numeric_limits<float>::quiet_NaN();
  if (!metal_renderer::is_finite({}) || metal_renderer::is_finite(invalid)) {
    return 1;
  }
  return 0;
}
