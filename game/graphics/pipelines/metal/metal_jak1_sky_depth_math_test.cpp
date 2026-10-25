// Characterizes why the Jak 1 sky leaves the physical-immersive eye frustum. It pins the
// mechanism with the production camera derivation and passes on the unfixed renderer by design;
// the failing regression is the GPU fixture in metal_jak1_sky_immersive_depth_test.mm.
//
// sky-tng emits its camera-centred dome and clouds at GS z = 0 and its below-horizon base at
// GS z = 256. Inverting the finite packet projection places those depths on planes roughly
// 13.1 km and 7.3 km in front of the game camera: beyond a 5,000 m reverse-Z eye far plane.

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

#include "game/graphics/pipelines/metal/metal_immersive_camera.h"

namespace {

using Matrix4 = std::array<float, 16>;
using Vector4 = std::array<float, 4>;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kGsDepthScale = 16777215.f;  // MetalDirectRenderer: z / 0xffffff

float& element(Matrix4& matrix, int column, int row) {
  return matrix[column * 4 + row];
}

int fail(const std::string& message) {
  std::fprintf(stderr, "metal-jak1-sky-depth-math-test: FAIL: %s\n", message.c_str());
  return 1;
}

// Jak 1 update-math-camera constants (goal_src/jak1/engine/gfx/math-camera.gc). Fog range only
// scales w and cancels from every quantity checked here.
metal_renderer::ImmersivePacketCamera make_jak1_packet() {
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
  packet.rot = {
      {{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}, {0.f, 0.f, 0.f, 1.f}}};
  packet.perspective[0][0] = -256.f / (x_ratio * kNear);
  packet.perspective[1][1] = -112.f / (y_ratio * kNear);
  packet.perspective[2][2] = (kFar + kNear) * half_depth_slope;
  packet.perspective[2][3] = fog_slope / kNear;
  packet.perspective[3][2] = -2.f * half_depth_slope * kFar * kNear;
  return packet;
}

// Reverse-Z right/up/back projection with the host's 5,000 m far plane
// (GOALPad/Vision/Immersive/VisionImmersiveProof.swift defaultDepthRange).
Matrix4 reverse_z_eye_projection() {
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

// eye_from_game_camera_meters for an eye at `x` meters along game-camera right.
Matrix4 eye_translated_right(float x) {
  Matrix4 out = metal_renderer::kIdentityMatrix4;
  element(out, 3, 0) = -x;
  return out;
}

bool derive(const metal_renderer::ImmersivePacketCamera& packet,
            const Matrix4& clip_from_eye,
            const Matrix4& eye_from_game_camera_meters,
            metal_renderer::ViewTransform* out) {
  metal_renderer::PhysicalViewCamera view;
  view.enabled = true;
  view.clip_from_eye = clip_from_eye;
  view.eye_from_game_camera_meters = eye_from_game_camera_meters;
  auto error = metal_renderer::ImmersiveCameraError::None;
  return metal_renderer::derive_immersive_view_transform(
             packet, metal_renderer::kJak1MetalScissorAdjust, view, out, &error) &&
         error == metal_renderer::ImmersiveCameraError::None;
}

// Direct-shader game clip of a centred sky vertex with the given GS depth.
Vector4 sky_game_clip(float gs_z) {
  return {0.f, 0.f, gs_z / kGsDepthScale, 1.f};
}

}  // namespace

int main() {
  const auto packet = make_jak1_packet();

  // 1. With an identity eye projection the derived transform yields right/up/back eye meters, so
  //    the reconstructed position of each sky depth can be read directly.
  metal_renderer::ViewTransform eye_meters;
  if (!derive(packet, metal_renderer::kIdentityMatrix4, metal_renderer::kIdentityMatrix4,
              &eye_meters)) {
    return fail("identity-eye derivation failed");
  }
  const struct {
    float gs_z;
    float expected_meters;
    const char* label;
  } planes[] = {{0.f, 13135.f, "dome/clouds GS z 0"}, {256.f, 7287.f, "base GS z 256"}};
  for (const auto& plane : planes) {
    const auto eye = metal_renderer::apply(eye_meters, sky_game_clip(plane.gs_z));
    if (!(eye[3] > 0.f)) {
      return fail(std::string(plane.label) + " is not in front of the game camera");
    }
    const float ahead = -eye[2] / eye[3];
    if (std::abs(ahead - plane.expected_meters) > 0.01f * plane.expected_meters) {
      return fail(std::string(plane.label) + " reconstructed at " + std::to_string(ahead) + " m");
    }
    std::printf("%s reconstructs %.0f m ahead\n", plane.label, ahead);
  }

  // 2. Through the host's reverse-Z eye, both planes land outside Metal's 0 <= z <= w volume.
  metal_renderer::ViewTransform eye;
  if (!derive(packet, reverse_z_eye_projection(), metal_renderer::kIdentityMatrix4, &eye)) {
    return fail("reverse-Z eye derivation failed");
  }
  for (const auto& plane : planes) {
    const auto clip = metal_renderer::apply(eye, sky_game_clip(plane.gs_z));
    if (!(clip[3] > 0.f) || !(clip[2] < 0.f)) {
      return fail(std::string(plane.label) + " unexpectedly inside the eye depth range");
    }
    std::printf("%s eye z/w = %.3e (clipped without depth clamp)\n", plane.label,
                clip[2] / clip[3]);
  }
  const auto dome = metal_renderer::apply(eye, sky_game_clip(0.f));
  if (std::abs(dome[2] / dome[3] - -1.239e-5f) > 0.05e-5f) {
    return fail("dome eye depth drifted from the audited -1.24e-5");
  }

  // 3. Head translation barely moves the reprojected sky: it behaves as a rotational sky.
  metal_renderer::ViewTransform shifted;
  if (!derive(packet, reverse_z_eye_projection(), eye_translated_right(0.065f), &shifted)) {
    return fail("translated-eye derivation failed");
  }
  const auto moved = metal_renderer::apply(shifted, sky_game_clip(0.f));
  const float ndc_shift = std::abs(moved[0] / moved[3] - dome[0] / dome[3]);
  if (ndc_shift > 1e-5f) {
    return fail("0.065 m head translation moved the sky by " + std::to_string(ndc_shift) + " NDC");
  }

  // 4. Comfort/windowed identity keeps the vertex on the inclusive far boundary.
  const auto comfort = metal_renderer::apply(metal_renderer::ViewTransform{}, sky_game_clip(0.f));
  if (comfort[2] != 0.f || comfort[3] != 1.f) {
    return fail("identity sky vertex left the clip-volume boundary");
  }

  std::puts("metal-jak1-sky-depth-math-test: PASS (characterization only)");
  return 0;
}
