#pragma once

// Test-only analytic oracle for Jak 1 world sprites seen by physical stereo eyes.
//
// The game side is built as the EE builds it: update-math-camera (math-camera.gc) and the
// camera-temp product (cam-update.gc), in float. The expected side never inverts the game
// projection. It casts each target pixel's ray from a physical eye (IPD, head offset and yaw,
// asymmetric reverse-Z frustum) into world space and intersects authored world geometry: a
// ground plane, a ground-aligned Mode3D footprint quad, and a Mode2D billboard in the
// game-camera plane.

#include <array>
#include <cmath>

#include "game/graphics/pipelines/metal/metal_immersive_camera.h"

namespace jak1_sprite_stereo_oracle {

using Vec3 = std::array<double, 3>;

constexpr double kPi = 3.14159265358979323846;
constexpr double kUnitsPerMeter = 4096.0;
constexpr double kEyeNearMeters = 0.1;
constexpr double kEyeFarMeters = 5000.0;

inline Vec3 add(Vec3 a, Vec3 b) {
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
inline Vec3 sub(Vec3 a, Vec3 b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
inline Vec3 scale(Vec3 a, double s) {
  return {a[0] * s, a[1] * s, a[2] * s};
}
inline double dot(Vec3 a, Vec3 b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline Vec3 cross(Vec3 a, Vec3 b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline Vec3 normalize(Vec3 a) {
  return scale(a, 1.0 / std::sqrt(dot(a, a)));
}

struct Camera {
  float perspective[4][4] = {};
  float camera_rot[4][4] = {};   // transpose(inv-camera-rot); row 3 = -trans * rot
  float camera_temp[4][4] = {};  // camera_rot * perspective, the sprite VU camera
  float trans[4] = {};
  float hvdf[4] = {};
  float pfog0 = 0.f;
  float fog_min = 150.f;
  float fog_max = 255.f;
  // inv-camera-rot rows: Jak camera x projects to screen-left (perspective[0][0] < 0).
  Vec3 left = {};
  Vec3 up = {};
  Vec3 forward = {};
  Vec3 position = {};  // world units
};

// Jak 1 defaults (math-camera.gc:238-248) with the PC extended 16:9 aspect (use-vis? #f).
inline Camera make_camera(Vec3 position, double yaw, double pitch_down) {
  Camera cam;
  const float d = 1024.f, f = 40960000.f, fov = 11650.845f;
  const float x_pix = 256.f, y_pix = 112.f;
  const float fog_start = 40960.f, fog_end = 819200.f;
  const float fm = 0.00008583069f * std::fmin(11650.845f, fov);
  float x_ratio = std::tan(0.5f * (fov * static_cast<float>(2.0 * kPi / 65536.0)));
  const float y_ratio = 0.75f * x_ratio;
  x_ratio *= static_cast<float>((16.0 / 9.0) / (4.0 / 3.0));
  const float corrected_end = fog_end * fm, corrected_start = fog_start * fm;
  const float min_depth = 100.f, max_depth = 16760631.f;
  const float fog_slope = (d * (cam.fog_min - cam.fog_max)) / (corrected_end - corrected_start);
  const float half_range = -0.5f * (max_depth - min_depth);
  const float half_depth_slope = half_range / (d * (f - d));
  cam.perspective[0][0] = fm * -(x_pix / (x_ratio * d));
  cam.perspective[1][1] = fm * -(y_pix / (y_ratio * d));
  cam.perspective[2][2] = fm * (f + d) * half_depth_slope;
  cam.perspective[2][3] = (fm / d) * fog_slope;
  cam.perspective[3][2] = -2.f * half_depth_slope * f * d * fm;
  cam.hvdf[0] = 2048.f;
  cam.hvdf[1] = 2048.f;
  cam.hvdf[2] = 0.5f * (max_depth + min_depth);
  cam.hvdf[3] = (corrected_end * cam.fog_max - corrected_start * cam.fog_min) /
                (corrected_end - corrected_start);
  cam.pfog0 = fog_slope;

  cam.forward = normalize({std::sin(yaw) * std::cos(pitch_down), -std::sin(pitch_down),
                           std::cos(yaw) * std::cos(pitch_down)});
  cam.left = normalize(cross({0, 1, 0}, cam.forward));
  cam.up = cross(cam.forward, cam.left);
  cam.position = position;
  const Vec3 rows[3] = {cam.left, cam.up, cam.forward};
  for (int k = 0; k < 3; ++k) {
    for (int axis = 0; axis < 3; ++axis) {
      cam.camera_rot[k][axis] = static_cast<float>(rows[axis][k]);
    }
    cam.trans[k] = static_cast<float>(position[k]);
  }
  cam.trans[3] = 1.f;
  for (int axis = 0; axis < 3; ++axis) {
    float translated = 0.f;
    for (int k = 0; k < 3; ++k) {
      translated -= cam.trans[k] * cam.camera_rot[k][axis];
    }
    cam.camera_rot[3][axis] = translated;
  }
  cam.camera_rot[3][3] = 1.f;
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      float sum = 0.f;
      for (int k = 0; k < 4; ++k) {
        sum += cam.camera_rot[row][k] * cam.perspective[k][column];
      }
      cam.camera_temp[row][column] = sum;
    }
  }
  return cam;
}

// The background PC_PORT packet fields (tfrag.gc add-pc-tfrag3-data) for the same camera.
inline metal_renderer::ImmersivePacketCamera packet(const Camera& cam) {
  metal_renderer::ImmersivePacketCamera out;
  for (int i = 0; i < 4; ++i) {
    out.hvdf_off[i] = cam.hvdf[i];
    out.trans[i] = cam.trans[i];
    for (int j = 0; j < 4; ++j) {
      out.rot[i][j] = cam.camera_rot[i][j];
      out.perspective[i][j] = cam.perspective[i][j];
    }
  }
  out.fog = {cam.pfog0, cam.fog_min, cam.fog_max, 0.f};
  return out;
}

// A physical eye in the game-camera right/up/back meter basis.
struct Eye {
  double tan_left = 0, tan_right = 0, tan_bottom = 0, tan_top = 0;
  Vec3 origin = {};
  Vec3 right = {1, 0, 0};
  Vec3 up = {0, 1, 0};
  Vec3 back = {0, 0, 1};
};

// Head at `head_origin` (meters), yawed and pitched relative to the game camera; the eye sits
// `offset` meters along the head's right axis.
inline Eye make_eye(Vec3 head_origin,
                    double head_yaw,
                    double head_pitch,
                    double offset,
                    double tan_left,
                    double tan_right,
                    double tan_bottom,
                    double tan_top) {
  Eye eye;
  eye.back = normalize({std::sin(head_yaw) * std::cos(head_pitch), std::sin(head_pitch),
                        std::cos(head_yaw) * std::cos(head_pitch)});
  eye.right = normalize(cross({0, 1, 0}, eye.back));
  eye.up = cross(eye.back, eye.right);
  eye.origin = add(head_origin, scale(eye.right, offset));
  eye.tan_left = tan_left;
  eye.tan_right = tan_right;
  eye.tan_bottom = tan_bottom;
  eye.tan_top = tan_top;
  return eye;
}

// Column-major asymmetric reverse-Z projection for an eye looking down -Z.
inline std::array<float, 16> clip_from_eye(const Eye& eye) {
  std::array<float, 16> m = {};
  const double n = kEyeNearMeters, f = kEyeFarMeters;
  m[0 * 4 + 0] = static_cast<float>(2.0 / (eye.tan_right - eye.tan_left));
  m[2 * 4 + 0] = static_cast<float>((eye.tan_right + eye.tan_left) / (eye.tan_right - eye.tan_left));
  m[1 * 4 + 1] = static_cast<float>(2.0 / (eye.tan_top - eye.tan_bottom));
  m[2 * 4 + 1] = static_cast<float>((eye.tan_top + eye.tan_bottom) / (eye.tan_top - eye.tan_bottom));
  m[2 * 4 + 2] = static_cast<float>(n / (f - n));
  m[3 * 4 + 2] = static_cast<float>(n * f / (f - n));
  m[2 * 4 + 3] = -1.f;
  return m;
}

// Column-major inverse of the eye pose [right up back | origin].
inline std::array<float, 16> eye_from_game_camera(const Eye& eye) {
  std::array<float, 16> m = {};
  const Vec3 axes[3] = {eye.right, eye.up, eye.back};
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      m[column * 4 + row] = static_cast<float>(axes[row][column]);
    }
    m[3 * 4 + row] = static_cast<float>(-dot(axes[row], eye.origin));
  }
  m[15] = 1.f;
  return m;
}

// World-unit ray through a target position given in pixels (x right, y down). The direction is
// scaled so the ray parameter equals the eye-space distance along -Z in meters.
struct Ray {
  Vec3 origin = {};
  Vec3 direction = {};
};

inline Vec3 world_from_game_camera_direction(const Camera& cam, Vec3 rub) {
  return add(add(scale(cam.left, -rub[0]), scale(cam.up, rub[1])), scale(cam.forward, -rub[2]));
}

inline Ray pixel_ray(const Camera& cam, const Eye& eye, double x, double y, int width, int height) {
  const double ndc_x = x / width * 2.0 - 1.0;
  const double ndc_y = 1.0 - y / height * 2.0;
  const double slope_x =
      ndc_x * (eye.tan_right - eye.tan_left) * 0.5 + (eye.tan_right + eye.tan_left) * 0.5;
  const double slope_y =
      ndc_y * (eye.tan_top - eye.tan_bottom) * 0.5 + (eye.tan_top + eye.tan_bottom) * 0.5;
  const Vec3 rub = add(add(scale(eye.right, slope_x), scale(eye.up, slope_y)), scale(eye.back, -1.0));
  Ray ray;
  ray.origin =
      add(cam.position, scale(world_from_game_camera_direction(cam, eye.origin), kUnitsPerMeter));
  ray.direction = scale(world_from_game_camera_direction(cam, rub), kUnitsPerMeter);
  return ray;
}

inline double reverse_z_depth(double eye_distance_meters) {
  const double n = kEyeNearMeters, f = kEyeFarMeters;
  return (n / (f - n) * -eye_distance_meters + n * f / (f - n)) / eye_distance_meters;
}

inline bool plane_hit(const Ray& ray, Vec3 point, Vec3 normal, double* t, Vec3* hit) {
  const double denominator = dot(ray.direction, normal);
  if (std::abs(denominator) < 1e-12) {
    return false;
  }
  *t = dot(sub(point, ray.origin), normal) / denominator;
  // A surface beyond the eye far plane leaves the clear depth.
  if (!(*t > kEyeNearMeters && *t <= kEyeFarMeters)) {
    return false;
  }
  *hit = add(ray.origin, scale(ray.direction, *t));
  return true;
}

// Columns of the sprite VU quaternion rotation (sprite.metal sprite_quat_to_rot), w >= 0.
inline std::array<Vec3, 3> sprite_rotation(const float quat[3]) {
  const double x = quat[0], y = quat[1], z = quat[2];
  const double w = std::sqrt(std::abs(1.0 - (x * x + y * y + z * z)));
  return {{{1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + z * w), 2.0 * (x * z - y * w)},
           {2.0 * (x * y - z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z + x * w)},
           {2.0 * (x * z + y * w), 2.0 * (y * z - x * w), 1.0 - 2.0 * (x * x + y * y)}}};
}

// Mode3D quad: xyz_array corners (+-0.5, 0, +-0.5) scaled by (sx, 1, sy) and rotated.
struct Footprint {
  Vec3 center = {};
  double sx = 0, sy = 0;  // world units
  float quat[3] = {};
};

inline bool footprint_hit(const Footprint& sprite, const Ray& ray, double* t) {
  const auto rot = sprite_rotation(sprite.quat);
  Vec3 hit;
  if (!plane_hit(ray, sprite.center, rot[1], t, &hit)) {
    return false;
  }
  const Vec3 local = sub(hit, sprite.center);
  return std::abs(dot(local, rot[0])) <= 0.5 * sprite.sx &&
         std::abs(dot(local, rot[2])) <= 0.5 * sprite.sy;
}

// Mode2D billboard with zero rotation: xy_array corners (+-0.5, +-0.5) in world units along
// game-camera right and down, in the plane through the centre facing the game camera.
struct Billboard {
  Vec3 center = {};
  double sx = 0, sy = 0;  // world units
};

inline bool billboard_hit(const Camera& cam, const Billboard& sprite, const Ray& ray, double* t) {
  Vec3 hit;
  if (!plane_hit(ray, sprite.center, cam.forward, t, &hit)) {
    return false;
  }
  const Vec3 local = sub(hit, sprite.center);
  return std::abs(dot(local, cam.left)) <= 0.5 * sprite.sx &&
         std::abs(dot(local, cam.up)) <= 0.5 * sprite.sy;
}

inline bool ground_hit(double ground_y, const Ray& ray, double* t) {
  Vec3 hit;
  return plane_hit(ray, {0, ground_y, 0}, {0, 1, 0}, t, &hit);
}

enum class Cover { Clear, Footprint, Billboard };

struct PixelExpectation {
  Cover cover = Cover::Clear;
  double depth = 0;         // nearest visible surface depth, for probe preloads only
  double ground_depth = 0;  // preloaded analytic ground depth, 0 (far clear) without a hit
  bool billboard_behind_ground = false;
};

// The fixture has separated footprint and billboard coverage. Track the nearest surface for
// probe preloads; the GPU test separately asserts that sprites leave the attachment unchanged.
inline PixelExpectation expect_at(const Camera& cam,
                                  const Eye& eye,
                                  double ground_y,
                                  const Footprint& footprint,
                                  const Billboard& billboard,
                                  double x,
                                  double y,
                                  int width,
                                  int height) {
  const Ray ray = pixel_ray(cam, eye, x, y, width, height);
  PixelExpectation out;
  double t = 0;
  if (ground_hit(ground_y, ray, &t)) {
    out.ground_depth = reverse_z_depth(t);
  }
  out.depth = out.ground_depth;
  if (footprint_hit(footprint, ray, &t) && reverse_z_depth(t) >= out.depth) {
    out.cover = Cover::Footprint;
    out.depth = reverse_z_depth(t);
  }
  if (billboard_hit(cam, billboard, ray, &t)) {
    const double depth = reverse_z_depth(t);
    out.billboard_behind_ground = depth < out.ground_depth;
    if (depth >= out.depth) {
      out.cover = Cover::Billboard;
      out.depth = depth;
    }
  }
  return out;
}

// The pixel-centre expectation, accepted only when the classification does not change within
// half a pixel; edge pixels are left to rasterization rules.
inline bool stable_expectation(const Camera& cam,
                               const Eye& eye,
                               double ground_y,
                               const Footprint& footprint,
                               const Billboard& billboard,
                               int px,
                               int py,
                               int width,
                               int height,
                               PixelExpectation* out) {
  *out = expect_at(cam, eye, ground_y, footprint, billboard, px + 0.5, py + 0.5, width, height);
  constexpr double kOffsets[4][2] = {{-0.5, 0}, {0.5, 0}, {0, -0.5}, {0, 0.5}};
  for (const auto& offset : kOffsets) {
    const auto probe = expect_at(cam, eye, ground_y, footprint, billboard, px + 0.5 + offset[0],
                                 py + 0.5 + offset[1], width, height);
    if (probe.cover != out->cover ||
        probe.billboard_behind_ground != out->billboard_behind_ground) {
      return false;
    }
  }
  return true;
}

// One representative gameplay frame: the game camera 3 m above the ground looking 18 degrees
// down, Jak 7 m ahead. Footprint (target-part.gc sp 93: 0.6 m, yawed, ground-aligned) 1 cm
// above the ground; run-poof billboard (sp 92 class: 0.75 m) 0.2 m up and 1 m to the right, so
// its lower part is below the ground. The head is moved and turned relative to the game camera.
struct Scene {
  int width = 320;
  int height = 240;
  double ground_y = 26000.0;
  Camera cam;
  Eye eyes[2];  // left, right
  Footprint footprint;
  Billboard billboard;
};

inline Scene make_scene() {
  Scene scene;
  const Vec3 camera_position = {123456.0, scene.ground_y + 3.0 * kUnitsPerMeter, -345678.0};
  scene.cam = make_camera(camera_position, 37.0 * kPi / 180.0, 18.0 * kPi / 180.0);
  const Vec3 ahead = normalize({scene.cam.forward[0], 0.0, scene.cam.forward[2]});
  const Vec3 right = scale(scene.cam.left, -1.0);
  const Vec3 jak = {camera_position[0] + ahead[0] * 7.0 * kUnitsPerMeter, scene.ground_y,
                    camera_position[2] + ahead[2] * 7.0 * kUnitsPerMeter};

  scene.footprint.center = add(add(jak, scale(right, 0.1 * kUnitsPerMeter)),
                               {0.0, 0.01 * kUnitsPerMeter, 0.0});
  scene.footprint.sx = 0.6 * kUnitsPerMeter;
  scene.footprint.sy = 0.6 * kUnitsPerMeter;
  scene.footprint.quat[1] = static_cast<float>(std::sin(0.55));

  scene.billboard.center = add(add(jak, scale(right, 1.0 * kUnitsPerMeter)),
                               {0.0, 0.2 * kUnitsPerMeter, 0.0});
  scene.billboard.sx = 0.75 * kUnitsPerMeter;
  scene.billboard.sy = 0.75 * kUnitsPerMeter;

  const Vec3 head = {0.04, -0.02, 0.03};
  const double yaw = 4.0 * kPi / 180.0, pitch = -3.0 * kPi / 180.0;
  scene.eyes[0] = make_eye(head, yaw, pitch, -0.0315, -0.55, 0.40, -0.50, 0.35);
  scene.eyes[1] = make_eye(head, yaw, pitch, 0.0315, -0.40, 0.55, -0.50, 0.35);
  return scene;
}

}  // namespace jak1_sprite_stereo_oracle
