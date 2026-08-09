#include "game/graphics/pipelines/metal/metal_view_transform.h"

#include <cassert>
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
  const std::array<float, 4> clip = {0.25f, -0.5f, 0.75f, 1.f};
  const auto identity = metal_renderer::apply({}, clip);
  assert(identity == clip);

  const auto left = metal_renderer::apply(horizontal_parallax(-0.08f), clip);
  const auto right = metal_renderer::apply(horizontal_parallax(0.08f), clip);
  assert(close(left[0], 0.19f));
  assert(close(right[0], 0.31f));
  assert(close(left[1], clip[1]) && close(left[2], clip[2]) && close(left[3], clip[3]));
  assert(close(right[1], clip[1]) && close(right[2], clip[2]) && close(right[3], clip[3]));

  const std::array<float, 4> nearer_clip = {clip[0], clip[1], 0.25f, clip[3]};
  const auto nearer_left = metal_renderer::apply(horizontal_parallax(-0.08f), nearer_clip);
  assert(std::abs(left[0] - clip[0]) > std::abs(nearer_left[0] - clip[0]));

  auto invalid = metal_renderer::ViewTransform{};
  invalid.clip_from_game_clip[0] = std::numeric_limits<float>::quiet_NaN();
  assert(metal_renderer::is_finite({}));
  assert(!metal_renderer::is_finite(invalid));
  return 0;
}
