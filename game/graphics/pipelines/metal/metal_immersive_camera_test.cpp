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
  metal_renderer::ImmersivePacketCamera packet;
  packet.hvdf_off = {2048.f, 1984.f, 16384.f, 17.f};
  packet.fog = {-4096.f, 8.f, 240.f, 3.f};
  packet.trans = {1234.f, -567.f, 9101.f, 1.f};
  const float c = std::cos(yaw);
  const float s = std::sin(yaw);
  packet.rot = {{{c, 0.f, -s, 0.f}, {0.f, 1.f, 0.f, 0.f}, {s, 0.f, c, 0.f}, {0.f, 0.f, 0.f, 1.f}}};
  packet.perspective[0][0] = 256.f;
  packet.perspective[1][1] = 128.f;
  packet.perspective[2][3] = 4096.f;
  packet.perspective[3][2] = -1000.f;
  return packet;
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
  const auto packet = make_packet(0.23f);
  Matrix4 packet_projection = {};
  metal_renderer::ImmersiveCameraError error = {};
  if (!metal_renderer::packet_clip_from_game_camera_meters(
          packet, metal_renderer::kJak1MetalScissorAdjust, &packet_projection, &error)) {
    return fail("packet projection reconstruction");
  }

  metal_renderer::PhysicalViewCamera center;
  center.enabled = true;
  center.clip_from_eye = packet_projection;
  metal_renderer::ViewTransform center_transform;
  if (!metal_renderer::derive_immersive_view_transform(
          packet, metal_renderer::kJak1MetalScissorAdjust, center, &center_transform, &error) ||
      !matrix_close(center_transform.clip_from_game_clip, metal_renderer::kIdentityMatrix4,
                    2e-5f)) {
    return fail("zero-baseline physical view was not mono identity");
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
  const Vector4 one_meter_center_clip = multiply(packet_projection, Vector4{0.f, 0.f, -1.f, 1.f});
  const auto left_clip = multiply(left_transform.clip_from_game_clip, one_meter_center_clip);
  const auto right_clip = multiply(right_transform.clip_from_game_clip, one_meter_center_clip);
  const float disparity = right_clip[0] / right_clip[3] - left_clip[0] / left_clip[3];
  if (!close(std::abs(disparity), 0.064f, 2e-4f) ||
      left_transform.clip_from_game_clip == right_transform.clip_from_game_clip) {
    return fail("64 mm physical disparity");
  }

  auto next_packet = packet;
  next_packet.perspective[0][0] = 240.f;
  metal_renderer::ViewTransform next_packet_left;
  if (!metal_renderer::derive_immersive_view_transform(
          next_packet, metal_renderer::kJak1MetalScissorAdjust, left, &next_packet_left, &error) ||
      next_packet_left.clip_from_game_clip == left_transform.clip_from_game_clip) {
    return fail("view transform did not follow the current packet camera");
  }
  auto followed_packet = packet;
  followed_packet.trans[0] += 8192.f;
  followed_packet.trans[2] -= 4096.f;
  metal_renderer::ViewTransform followed_left;
  Matrix4 followed_projection = {};
  if (!metal_renderer::derive_immersive_view_transform(
          followed_packet, metal_renderer::kJak1MetalScissorAdjust, left, &followed_left, &error) ||
      !metal_renderer::packet_clip_from_game_camera_meters(
          followed_packet, metal_renderer::kJak1MetalScissorAdjust, &followed_projection, &error)) {
    return fail("native third-person follow-camera translation was replaced");
  }
  const Vector4 follow_point = {0.2f, 0.1f, -2.f, 1.f};
  const auto base_follow_clip =
      multiply(left_transform.clip_from_game_clip, multiply(packet_projection, follow_point));
  const auto moved_follow_clip =
      multiply(followed_left.clip_from_game_clip, multiply(followed_projection, follow_point));
  if (!projectively_close(base_follow_clip, moved_follow_clip, 2e-4f)) {
    return fail("native third-person follow-camera translation was replaced");
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

  metal_renderer::PhysicalBinocularCull binocular;
  binocular.enabled = true;
  binocular.clip_from_game_camera_meters = metal_renderer::kIdentityMatrix4;
  // A one-meter-wide right/up/back box in camera space. Side-plane extraction is independent of
  // the depth rows; this synthetic clip retains an edge sphere and rejects a farther one.
  metal_renderer::ImmersiveCullPlanes planes = {};
  if (!metal_renderer::derive_immersive_cull_planes(packet, binocular, &planes, &error)) {
    return fail("binocular cull derivation");
  }
  const float c = std::cos(0.23f);
  const float s = std::sin(0.23f);
  const auto world_from_camera = [&](float camera_x) {
    return Vector4{packet.trans[0] + c * camera_x * metal_renderer::kJakWorldUnitsPerMeter,
                   packet.trans[1],
                   packet.trans[2] + s * camera_x * metal_renderer::kJakWorldUnitsPerMeter, 1.f};
  };
  const auto edge = world_from_camera(0.95f);
  const auto outside = world_from_camera(1.2f);
  if (!sphere_visible(planes, edge[0], edge[1], edge[2], 0.1f * 4096.f) ||
      sphere_visible(planes, outside[0], outside[1], outside[2], 0.05f * 4096.f)) {
    return fail("shared conservative binocular culling");
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

  std::cout << "PASS mono identity, current-packet dual replay, native follow camera, 64 mm "
               "per-eye disparity, head yaw, shared binocular culling, invalid rejection, and "
               "HUD identity, strict same-frame cache identity, and HUD-chain continuation\n";
  return 0;
}
