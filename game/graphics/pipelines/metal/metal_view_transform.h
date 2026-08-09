#pragma once

#include <array>
#include <cmath>

namespace metal_renderer {

// Column-major transform from the game's final Metal homogeneous clip coordinates into one
// host-owned view's homogeneous clip coordinates. Identity preserves the conventional one-view
// clip coordinates. A visionOS host can supply projection(view) * inverse(game projection and
// view) once it has resolved the eye/head pose relative to the game window.
struct ViewTransform {
  std::array<float, 16> clip_from_game_clip = {
      1.f, 0.f, 0.f, 0.f,
      0.f, 1.f, 0.f, 0.f,
      0.f, 0.f, 1.f, 0.f,
      0.f, 0.f, 0.f, 1.f,
  };
};

inline bool is_finite(const ViewTransform& transform) {
  for (float value : transform.clip_from_game_clip) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  return true;
}

inline std::array<float, 4> apply(const ViewTransform& transform,
                                  const std::array<float, 4>& clip) {
  std::array<float, 4> out = {};
  for (int column = 0; column < 4; column++) {
    for (int row = 0; row < 4; row++) {
      out[row] += transform.clip_from_game_clip[column * 4 + row] * clip[column];
    }
  }
  return out;
}

}  // namespace metal_renderer
