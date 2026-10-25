#include "game/graphics/pipelines/metal/metal_immersive_camera.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

#include "game/graphics/opengl_renderer/buckets.h"

namespace {

using Matrix4 = std::array<float, 16>;
using Vector4 = std::array<float, 4>;

constexpr float kPi = 3.14159265358979323846f;

float& element(Matrix4& matrix, int column, int row) {
  return matrix[column * 4 + row];
}

Vector4 multiply(const Matrix4& matrix, const Vector4& vector) {
  Vector4 out = {};
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      out[row] += matrix[column * 4 + row] * vector[column];
    }
  }
  return out;
}

bool close(float actual, float expected, float tolerance) {
  return std::isfinite(actual) && std::abs(actual - expected) <= tolerance;
}

bool matrix_close(const Matrix4& actual, const Matrix4& expected, float tolerance) {
  for (std::size_t i = 0; i < actual.size(); ++i) {
    if (!close(actual[i], expected[i], tolerance)) {
      return false;
    }
  }
  return true;
}

bool projectively_close(const Vector4& actual, const Vector4& expected, float tolerance) {
  if (std::abs(actual[3]) < 1e-8f || std::abs(expected[3]) < 1e-8f) {
    return false;
  }
  for (int lane = 0; lane < 3; ++lane) {
    if (!close(actual[lane] / actual[3], expected[lane] / expected[3], tolerance)) {
      return false;
    }
  }
  return true;
}

metal_renderer::ImmersivePacketCamera make_packet(float yaw) {
  constexpr float kNear = 1024.f;
  constexpr float kFar = 40960000.f;
  constexpr float kFogStart = 40960.f;
  constexpr float kFogEnd = 819200.f;
  constexpr float kFogMinimum = 150.f;
  constexpr float kFogMaximum = 255.f;
  constexpr float kDepthMinimum = 100.f;
  constexpr float kDepthMaximum = 16760631.f;
  const float x_ratio = std::tan(32.f * kPi / 180.f);
  const float y_ratio = x_ratio * 0.75f;
  const float fog_slope = kNear * (kFogMinimum - kFogMaximum) / (kFogEnd - kFogStart);
  const float depth_half_range = -0.5f * (kDepthMaximum - kDepthMinimum);
  const float half_depth_slope = depth_half_range / (kNear * (kFar - kNear));

  metal_renderer::ImmersivePacketCamera packet;
  packet.hvdf_off = {2048.f, 2048.f, 0.5f * (kDepthMaximum + kDepthMinimum), 17.f};
  packet.fog = {fog_slope, kFogMinimum, kFogMaximum, 0.f};
  packet.trans = {1234.f, -567.f, 9101.f, 1.f};
  const float c = std::cos(yaw);
  const float s = std::sin(yaw);
  packet.rot = {{{c, 0.f, -s, 0.f}, {0.f, 1.f, 0.f, 0.f}, {s, 0.f, c, 0.f}, {0.f, 0.f, 0.f, 1.f}}};
  packet.perspective[0][0] = -256.f / (x_ratio * kNear);
  packet.perspective[1][1] = -112.f / (y_ratio * kNear);
  packet.perspective[2][2] = (kFar + kNear) * half_depth_slope;
  packet.perspective[2][3] = fog_slope / kNear;
  packet.perspective[3][2] = -2.f * half_depth_slope * kFar * kNear;
  return packet;
}

Vector4 conventional_game_clip(const metal_renderer::ImmersivePacketCamera& packet,
                               const Vector4& world) {
  const float fog_scale = packet.fog[0];
  const std::array<float, 3> scale = {
      fog_scale * packet.perspective[0][0] / 256.f,
      -fog_scale * packet.perspective[1][1] / 128.f,
      fog_scale * packet.perspective[2][2] / 8388608.f +
          (packet.hvdf_off[2] / 8388608.f - 1.f) * packet.perspective[2][3],
  };
  float pc_camera[4][4] = {};
  for (int column = 0; column < 3; ++column) {
    for (int lane = 0; lane < 3; ++lane) {
      pc_camera[column][lane] = packet.rot[column][lane] * scale[lane];
    }
    pc_camera[column][3] = packet.rot[column][2] * packet.perspective[2][3];
  }
  pc_camera[3][2] = fog_scale * packet.perspective[3][2] / 8388608.f;

  const Vector4 relative = {
      world[0] - packet.trans[0] * world[3],
      world[1] - packet.trans[1] * world[3],
      world[2] - packet.trans[2] * world[3],
      0.f,
  };
  Vector4 clip = {};
  for (int lane = 0; lane < 4; ++lane) {
    clip[lane] = -pc_camera[3][lane] * world[3];
    for (int column = 0; column < 3; ++column) {
      clip[lane] -= pc_camera[column][lane] * relative[column];
    }
  }
  clip[1] *= metal_renderer::kJak1MetalScissorAdjust;
  clip[2] = (clip[2] + clip[3]) * 0.5f;
  return clip;
}

Vector4 source_camera_from_world(const metal_renderer::ImmersivePacketCamera& packet,
                                 const Vector4& world) {
  Vector4 camera = {0.f, 0.f, 0.f, world[3]};
  for (int axis = 0; axis < 3; ++axis) {
    for (int world_axis = 0; world_axis < 3; ++world_axis) {
      camera[axis] += packet.rot[world_axis][axis] *
                      (world[world_axis] - packet.trans[world_axis] * world[3]) /
                      metal_renderer::kJakWorldUnitsPerMeter;
    }
  }
  return camera;
}

Vector4 right_up_back_from_source_camera(const Vector4& source_camera) {
  return {-source_camera[0], source_camera[1], -source_camera[2], source_camera[3]};
}

Vector4 world_from_source_camera(const metal_renderer::ImmersivePacketCamera& packet,
                                 float yaw,
                                 const Vector4& source_camera) {
  const float c = std::cos(yaw);
  const float s = std::sin(yaw);
  return {
      packet.trans[0] +
          (c * source_camera[0] - s * source_camera[2]) * metal_renderer::kJakWorldUnitsPerMeter,
      packet.trans[1] + source_camera[1] * metal_renderer::kJakWorldUnitsPerMeter,
      packet.trans[2] +
          (s * source_camera[0] + c * source_camera[2]) * metal_renderer::kJakWorldUnitsPerMeter,
      source_camera[3],
  };
}

Matrix4 right_up_back_projection() {
  constexpr float kNearMeters = 0.1f;
  constexpr float kFarMeters = 5000.f;
  Matrix4 projection = {};
  element(projection, 0, 0) = 1.2f;
  element(projection, 1, 1) = 1.35f;
  element(projection, 2, 2) = kNearMeters / (kFarMeters - kNearMeters);
  element(projection, 2, 3) = -1.f;
  element(projection, 3, 2) = kNearMeters * kFarMeters / (kFarMeters - kNearMeters);
  return projection;
}

Matrix4 translation(float x, float y, float z) {
  Matrix4 out = metal_renderer::kIdentityMatrix4;
  element(out, 3, 0) = x;
  element(out, 3, 1) = y;
  element(out, 3, 2) = z;
  return out;
}

Matrix4 yaw(float radians) {
  Matrix4 out = metal_renderer::kIdentityMatrix4;
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  element(out, 0, 0) = c;
  element(out, 2, 0) = s;
  element(out, 0, 2) = -s;
  element(out, 2, 2) = c;
  return out;
}

bool sphere_visible(const metal_renderer::ImmersiveCullPlanes& planes,
                    float x,
                    float y,
                    float z,
                    float radius) {
  for (int plane = 0; plane < 4; ++plane) {
    const float distance =
        planes[0][plane] * x + planes[1][plane] * y + planes[2][plane] * z - planes[3][plane];
    if (distance <= -radius) {
      return false;
    }
  }
  return true;
}

int fail(const std::string& message) {
  std::cerr << "FAIL " << message << '\n';
  return 1;
}

}  // namespace

int main() {
  constexpr float kPacketYaw = 0.23f;
  const auto packet = make_packet(kPacketYaw);
  const Vector4 subject_source_camera = {0.45f, -0.25f, 4.f, 1.f};
  const Vector4 subject_world = world_from_source_camera(packet, kPacketYaw, subject_source_camera);
  const Vector4 decoded_source_camera = source_camera_from_world(packet, subject_world);
  if (!close(decoded_source_camera[0], subject_source_camera[0], 2e-5f) ||
      !close(decoded_source_camera[1], subject_source_camera[1], 2e-5f) ||
      !close(decoded_source_camera[2], subject_source_camera[2], 2e-5f)) {
    return fail("real packet rot/trans camera decode");
  }
  const Vector4 subject_right_up_back = right_up_back_from_source_camera(decoded_source_camera);
  const Vector4 subject_game_clip = conventional_game_clip(packet, subject_world);
  if (!(subject_game_clip[3] > 0.f)) {
    return fail("real GOAL +Z-forward subject was not conventionally visible");
  }

  Matrix4 packet_projection = {};
  metal_renderer::ImmersiveCameraError error = {};
  if (!metal_renderer::packet_clip_from_game_camera_meters(
          packet, metal_renderer::kJak1MetalScissorAdjust, &packet_projection, &error)) {
    return fail("packet projection reconstruction");
  }
  if (!projectively_close(multiply(packet_projection, subject_right_up_back), subject_game_clip,
                          2e-4f)) {
    return fail("Jak left/up/forward to host right/up/back conversion");
  }

  metal_renderer::PhysicalViewCamera center;
  center.enabled = true;
  center.clip_from_eye = right_up_back_projection();
  metal_renderer::ViewTransform center_transform;
  if (!metal_renderer::derive_immersive_view_transform(
          packet, metal_renderer::kJak1MetalScissorAdjust, center, &center_transform, &error)) {
    return fail("independent right/up/back physical projection");
  }
  const Vector4 center_clip = multiply(center_transform.clip_from_game_clip, subject_game_clip);
  const Vector4 expected_center_clip = multiply(center.clip_from_eye, subject_right_up_back);
  if (!projectively_close(center_clip, expected_center_clip, 2e-4f) || !(center_clip[3] > 0.f) ||
      !(center_clip[2] / center_clip[3] > 0.f) || !(center_clip[2] / center_clip[3] < 1.f)) {
    return fail("third-person subject did not remain in front with valid Metal depth");
  }

  metal_renderer::PhysicalViewCamera packet_view;
  packet_view.enabled = true;
  packet_view.clip_from_eye = packet_projection;
  metal_renderer::ViewTransform packet_identity;
  if (!metal_renderer::derive_immersive_view_transform(
          packet, metal_renderer::kJak1MetalScissorAdjust, packet_view, &packet_identity, &error) ||
      !matrix_close(packet_identity.clip_from_game_clip, metal_renderer::kIdentityMatrix4, 2e-5f)) {
    return fail("packet-matched physical view was not mono identity");
  }

  auto left = center;
  auto right = center;
  left.eye_from_game_camera_meters = translation(0.032f, 0.f, 0.f);
  right.eye_from_game_camera_meters = translation(-0.032f, 0.f, 0.f);
  metal_renderer::ViewTransform left_transform;
  metal_renderer::ViewTransform right_transform;
  if (!metal_renderer::derive_immersive_view_transform(
          packet, metal_renderer::kJak1MetalScissorAdjust, left, &left_transform, &error) ||
      !metal_renderer::derive_immersive_view_transform(
          packet, metal_renderer::kJak1MetalScissorAdjust, right, &right_transform, &error)) {
    return fail("64 mm eye transforms");
  }
  const auto left_clip = multiply(left_transform.clip_from_game_clip, subject_game_clip);
  const auto right_clip = multiply(right_transform.clip_from_game_clip, subject_game_clip);
  const auto expected_left_clip = multiply(
      left.clip_from_eye, multiply(left.eye_from_game_camera_meters, subject_right_up_back));
  const auto expected_right_clip = multiply(
      right.clip_from_eye, multiply(right.eye_from_game_camera_meters, subject_right_up_back));
  const float disparity = right_clip[0] / right_clip[3] - left_clip[0] / left_clip[3];
  if (!projectively_close(left_clip, expected_left_clip, 2e-4f) ||
      !projectively_close(right_clip, expected_right_clip, 2e-4f) ||
      !close(std::abs(disparity), 1.2f * 0.064f / subject_source_camera[2], 2e-4f) ||
      left_transform.clip_from_game_clip == right_transform.clip_from_game_clip) {
    return fail("64 mm physical disparity");
  }

  auto next_packet = packet;
  next_packet.perspective[0][0] *= 0.95f;
  metal_renderer::ViewTransform next_packet_left;
  if (!metal_renderer::derive_immersive_view_transform(
          next_packet, metal_renderer::kJak1MetalScissorAdjust, left, &next_packet_left, &error) ||
      next_packet_left.clip_from_game_clip == left_transform.clip_from_game_clip) {
    return fail("view transform did not follow the current packet camera");
  }
  auto followed_packet = packet;
  followed_packet.trans[0] += 1024.f;
  followed_packet.trans[2] += 512.f;
  metal_renderer::ViewTransform followed_left;
  if (!metal_renderer::derive_immersive_view_transform(
          followed_packet, metal_renderer::kJak1MetalScissorAdjust, left, &followed_left, &error)) {
    return fail("translated third-person camera derivation");
  }
  const Vector4 followed_source_camera = source_camera_from_world(followed_packet, subject_world);
  const Vector4 followed_right_up_back = right_up_back_from_source_camera(followed_source_camera);
  const Vector4 followed_game_clip = conventional_game_clip(followed_packet, subject_world);
  const Vector4 followed_clip = multiply(followed_left.clip_from_game_clip, followed_game_clip);
  const Vector4 expected_followed_clip = multiply(
      left.clip_from_eye, multiply(left.eye_from_game_camera_meters, followed_right_up_back));
  if (!projectively_close(followed_clip, expected_followed_clip, 2e-4f) ||
      projectively_close(followed_clip, left_clip, 2e-4f)) {
    return fail("packet trans was not preserved as the third-person camera origin");
  }

  auto yawed = left;
  yawed.eye_from_game_camera_meters = yaw(7.f * 3.14159265358979323846f / 180.f);
  element(yawed.eye_from_game_camera_meters, 3, 0) = 0.032f;
  metal_renderer::ViewTransform yawed_transform;
  if (!metal_renderer::derive_immersive_view_transform(
          packet, metal_renderer::kJak1MetalScissorAdjust, yawed, &yawed_transform, &error) ||
      matrix_close(yawed_transform.clip_from_game_clip, left_transform.clip_from_game_clip,
                   1e-3f)) {
    return fail("head-recenter yaw did not change the world transform");
  }
  const Vector4 yawed_clip = multiply(yawed_transform.clip_from_game_clip, subject_game_clip);
  const Vector4 expected_yawed_clip = multiply(
      yawed.clip_from_eye, multiply(yawed.eye_from_game_camera_meters, subject_right_up_back));
  if (!projectively_close(yawed_clip, expected_yawed_clip, 2e-4f)) {
    return fail("head-recenter yaw used the wrong camera basis");
  }

  metal_renderer::PhysicalBinocularCull binocular;
  binocular.enabled = true;
  binocular.clip_from_game_camera_meters = right_up_back_projection();
  metal_renderer::ImmersiveCullPlanes planes = {};
  if (!metal_renderer::derive_immersive_cull_planes(packet, binocular, &planes, &error)) {
    return fail("binocular cull derivation");
  }
  const Vector4 behind_world =
      world_from_source_camera(packet, kPacketYaw, Vector4{0.f, 0.f, -4.f, 1.f});
  if (!sphere_visible(planes, subject_world[0], subject_world[1], subject_world[2],
                      0.2f * metal_renderer::kJakWorldUnitsPerMeter) ||
      sphere_visible(planes, behind_world[0], behind_world[1], behind_world[2],
                     0.1f * metal_renderer::kJakWorldUnitsPerMeter)) {
    return fail("shared binocular culling did not retain the in-front subject");
  }

  auto singular = packet;
  singular.perspective[0][0] = 0.f;
  if (metal_renderer::derive_immersive_view_transform(
          singular, metal_renderer::kJak1MetalScissorAdjust, center, &center_transform, &error) ||
      error != metal_renderer::ImmersiveCameraError::SingularGameClip) {
    return fail("singular packet was accepted");
  }
  auto invalid = center;
  invalid.clip_from_eye[0] = std::numeric_limits<float>::quiet_NaN();
  if (metal_renderer::derive_immersive_view_transform(
          packet, metal_renderer::kJak1MetalScissorAdjust, invalid, &center_transform, &error) ||
      error != metal_renderer::ImmersiveCameraError::NonFiniteInput) {
    return fail("nonfinite view was accepted");
  }
  metal_renderer::PhysicalViewCamera disabled;
  if (metal_renderer::derive_immersive_view_transform(
          packet, metal_renderer::kJak1MetalScissorAdjust, disabled, &center_transform, &error) ||
      error != metal_renderer::ImmersiveCameraError::DisabledContract) {
    return fail("disabled physical contract was accepted");
  }

  metal_renderer::ImmersiveCameraContract left_contract;
  left_contract.view = left;
  left_contract.binocular_cull = binocular;
  metal_renderer::ImmersiveCameraContract right_contract;
  right_contract.view = right;
  right_contract.binocular_cull = binocular;
  metal_renderer::ImmersiveStereoFrameCamera cached_camera;
  cached_camera.left = left_transform;
  cached_camera.right = right_transform;
  cached_camera.binocular_cull = planes;
  cached_camera.packet_fingerprint = 0x1234;
  cached_camera.packet_count = 3;
  metal_renderer::ImmersiveStereoFrameCameraCache cache;
  cache.store(41, 9001, left_contract, right_contract, cached_camera);
  if (!cache.find(41, 9001, left_contract, right_contract) ||
      cache.find(42, 9001, left_contract, right_contract) ||
      cache.find(41, 9002, left_contract, right_contract)) {
    return fail("same-frame cache identity");
  }
  auto changed_right_contract = right_contract;
  changed_right_contract.view.eye_from_game_camera_meters[12] += 0.001f;
  if (cache.find(41, 9001, left_contract, changed_right_contract)) {
    return fail("same-frame cache accepted changed physical contract");
  }
  cache.clear();
  if (cache.find(41, 9001, left_contract, right_contract)) {
    return fail("cleared same-frame cache remained available");
  }
  if (metal_renderer::jak1_bucket_content_requires_world_camera(
          static_cast<int>(jak1::BucketId::DEBUG)) ||
      metal_renderer::jak1_bucket_content_requires_world_camera(
          static_cast<int>(jak1::BucketId::SUBTITLE)) ||
      metal_renderer::jak1_bucket_content_requires_world_camera(
          static_cast<int>(jak1::BucketId::TFRAG_TEX_LEVEL0)) ||
      !metal_renderer::jak1_bucket_content_requires_world_camera(
          static_cast<int>(jak1::BucketId::TFRAG_LEVEL0)) ||
      !metal_renderer::jak1_bucket_content_requires_world_camera(
          static_cast<int>(jak1::BucketId::MERC_AFTER_ALPHA)) ||
      !metal_renderer::jak1_bucket_content_requires_world_camera(
          static_cast<int>(jak1::BucketId::SPRITE))) {
    return fail("World versus Screen chain classification");
  }

  const std::array<float, 4> hud_clip = {0.25f, -0.5f, 0.75f, 1.f};
  if (metal_renderer::apply(left_transform, metal_renderer::StereoSpace::Screen, hud_clip) !=
          hud_clip ||
      metal_renderer::apply(left_transform, metal_renderer::StereoSpace::World, hud_clip) ==
          hud_clip) {
    return fail("HUD/world transform classification");
  }

  std::cout << "PASS real GOAL camera signs, left/up/forward to right/up/back conversion, "
               "third-person subject depth, mono identity, current-packet dual replay, 64 mm "
               "per-eye disparity, head yaw, shared binocular culling, invalid rejection, HUD "
               "identity, strict same-frame cache identity, and HUD-chain continuation\n";
  return 0;
}
