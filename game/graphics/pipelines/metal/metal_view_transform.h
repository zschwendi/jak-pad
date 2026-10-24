#pragma once

#include <array>
#include <cmath>

namespace metal_renderer {

inline constexpr std::array<float, 16> kIdentityMatrix4 = {
    1.f, 0.f, 0.f, 0.f,
    0.f, 1.f, 0.f, 0.f,
    0.f, 0.f, 1.f, 0.f,
    0.f, 0.f, 0.f, 1.f,
};

enum class StereoSpace {
  World,
  Screen,
};

// The renderer receives a mixture of game-camera geometry and authored screen overlays. Keep the
// split explicit so new two-view paths cannot accidentally give HUD/subtitle content parallax or
// leave world geometry monoscopic.
enum class StereoDrawPath {
  Direct,
  Sprite3d,
  Sprite2d,
  SpriteHud,
  SpriteDistortion,
  Sky,
  Ocean,
  GenericWorld,
  GenericHud,
  ShadowVolume,
  ShadowOverlay,
};

constexpr StereoSpace stereo_space_for(StereoDrawPath path) {
  switch (path) {
    case StereoDrawPath::Sprite3d:
    case StereoDrawPath::Sky:
    case StereoDrawPath::Ocean:
    case StereoDrawPath::GenericWorld:
    case StereoDrawPath::ShadowVolume:
      return StereoSpace::World;
    case StereoDrawPath::Direct:
    case StereoDrawPath::Sprite2d:
    case StereoDrawPath::SpriteHud:
    case StereoDrawPath::SpriteDistortion:
    case StereoDrawPath::GenericHud:
    case StereoDrawPath::ShadowOverlay:
      return StereoSpace::Screen;
  }
  return StereoSpace::Screen;
}

constexpr bool receives_view_transform(StereoSpace space) {
  return space == StereoSpace::World;
}

constexpr bool receives_view_transform(StereoDrawPath path) {
  return receives_view_transform(stereo_space_for(path));
}

// Column-major transform from the game's final Metal homogeneous clip coordinates into one
// host-owned view's homogeneous clip coordinates. Identity preserves the conventional one-view
// clip coordinates. A visionOS host can supply projection(view) * inverse(game projection and
// view) once it has resolved the eye/head pose relative to the game window.
struct ViewTransform {
  std::array<float, 16> clip_from_game_clip = kIdentityMatrix4;
};

// Same-frame physical view contract for native immersive rendering. The game-camera basis is
// right/up/back. Rotation is dimensionless; translation is in meters. The renderer combines this
// with the current DMA packet's game camera and the 4096 game-units-per-meter scale.
struct PhysicalViewCamera {
  bool enabled = false;
  std::array<float, 16> clip_from_eye = kIdentityMatrix4;
  std::array<float, 16> eye_from_game_camera_meters = kIdentityMatrix4;
};

// Conservative binocular matrix from the same game-camera meter basis. It is used only to derive
// one shared culling volume for both eye replays and is never sent to a vertex shader.
struct PhysicalBinocularCull {
  bool enabled = false;
  std::array<float, 16> clip_from_game_camera_meters = kIdentityMatrix4;
};

struct ImmersiveCameraContract {
  PhysicalViewCamera view;
  PhysicalBinocularCull binocular_cull;
};

inline bool is_finite(const ViewTransform& transform) {
  for (float value : transform.clip_from_game_clip) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  return true;
}

inline bool is_finite_physical_view(const PhysicalViewCamera& camera) {
  for (float value : camera.clip_from_eye) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  for (float value : camera.eye_from_game_camera_meters) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  return true;
}

inline bool is_finite_binocular_cull(const PhysicalBinocularCull& cull) {
  for (float value : cull.clip_from_game_camera_meters) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  return true;
}

inline bool is_finite_immersive_contract(const ImmersiveCameraContract& contract) {
  return is_finite_physical_view(contract.view) &&
         is_finite_binocular_cull(contract.binocular_cull);
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

inline std::array<float, 4> apply(const ViewTransform& transform,
                                  StereoSpace space,
                                  const std::array<float, 4>& clip) {
  return receives_view_transform(space) ? apply(transform, clip) : clip;
}

}  // namespace metal_renderer
