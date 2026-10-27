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
// leave world geometry monoscopic. A draw is World when its final clip position is a positive
// per-primitive multiple of the frame's game clip of a world point, even if the VU program
// divided by w first; Sprite2d (group-0 billboards such as particles) is such a draw.
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
    case StereoDrawPath::Sprite2d:
    case StereoDrawPath::Sky:
    case StereoDrawPath::Ocean:
    case StereoDrawPath::GenericWorld:
    case StereoDrawPath::ShadowVolume:
      return StereoSpace::World;
    case StereoDrawPath::Direct:
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

// Per-vertex sprite render modes (SpriteVertexIn.info[3] in shaders/sprite.metal).
inline constexpr unsigned kSpriteRenderMode2d = 1;   // group-0 world billboard
inline constexpr unsigned kSpriteRenderModeHud = 2;  // group-1 HUD
inline constexpr unsigned kSpriteRenderMode3d = 3;   // group-0 world-oriented quad

// Bit N is set when sprite vertices of render mode N receive the view transform.
constexpr unsigned sprite_view_transform_mode_mask() {
  return (receives_view_transform(StereoDrawPath::Sprite2d) ? 1u << kSpriteRenderMode2d : 0u) |
         (receives_view_transform(StereoDrawPath::SpriteHud) ? 1u << kSpriteRenderModeHud : 0u) |
         (receives_view_transform(StereoDrawPath::Sprite3d) ? 1u << kSpriteRenderMode3d : 0u);
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

// Opt-in head-locked HUD plane contract. The host supplies, per eye,
// P_eye * inverse(deviceFromView) * planeFromScreen, where planeFromScreen maps game NDC to a
// 1.6x1.2 m plane 2 m in front of the device. It is parallel to the World view transform and is
// never written into clip_from_game_clip. Disabled/identity is the conventional one-view path.
struct HudPlaneTransform {
  bool enabled = false;
  std::array<float, 16> clip_from_screen_clip = kIdentityMatrix4;
};

inline bool is_finite_hud_plane(const HudPlaneTransform& hud) {
  for (float value : hud.clip_from_screen_clip) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  return true;
}

// Only an explicitly tagged Direct registration or group-1 sprite HUD vertices may use the HUD
// plane. Screen stereo space alone is not HUD: ShadowOverlay, GenericHud, SpriteDistortion and
// untagged Direct renderers keep their existing behavior.
constexpr bool hud_plane_eligible(StereoDrawPath path, bool direct_hud_opt_in) {
  switch (path) {
    case StereoDrawPath::Direct:
      return direct_hud_opt_in;
    case StereoDrawPath::SpriteHud:
      return true;
    default:
      return false;
  }
}

}  // namespace metal_renderer
