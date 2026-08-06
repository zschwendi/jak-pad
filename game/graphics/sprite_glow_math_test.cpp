#include "game/graphics/sprite_glow_math.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>

using math::Vector4f;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
  if (!condition) {
    failures++;
  }
}

struct Fixture {
  SpriteGlowConsts consts = {};
  SpriteGlowData data = {};
  alignas(16) std::array<u8, sizeof(AdGifData)> adgif = {};

  Fixture() {
    consts.camera[0] = Vector4f(1.f, 0.f, 0.f, 0.f);
    consts.camera[1] = Vector4f(0.f, 1.f, 0.f, 0.f);
    consts.camera[2] = Vector4f(0.f, 0.f, 1.f, 0.f);
    consts.camera[3] = Vector4f(0.f, 0.f, 0.f, 1.f);

    consts.perspective[0] = Vector4f(1.f, 0.f, 0.f, 0.f);
    consts.perspective[1] = Vector4f(0.f, 1.f, 0.f, 0.f);
    consts.perspective[2] = Vector4f(0.f, 0.f, 0.25f, 0.f);
    consts.perspective[3] = Vector4f(0.f, 0.f, 0.f, 1.f);
    consts.hvdf = Vector4f(128.f, 96.f, 0.f, 0.f);
    consts.hmge = Vector4f(1.f, 1.f, 1.f, 1.f);
    consts.deg_to_rad = 0.017453292519943295f;
    consts.basis_x = Vector4f(1.f, 0.f, 0.f, 0.f);
    consts.basis_y = Vector4f(0.f, 1.f, 0.f, 0.f);
    consts.xy_array[0] = Vector4f(-1.f, -1.f, 0.f, 0.f);
    consts.xy_array[1] = Vector4f(1.f, -1.f, 0.f, 0.f);
    consts.xy_array[2] = Vector4f(-1.f, 1.f, 0.f, 0.f);
    consts.xy_array[3] = Vector4f(1.f, 1.f, 0.f, 0.f);
    consts.clamp_min = Vector4f(0.f, 0.f, 0.f, 0.f);
    consts.clamp_max = Vector4f(256.f, 192.f, 64.f, 16.f);

    data.pos[0] = 0.25f;
    data.pos[1] = -0.25f;
    data.pos[2] = 2.f;
    data.size_x = 8.f;
    data.size_probe = 4.f;
    data.z_offset = 0.25f;
    data.rot_angle = 15.f;
    data.size_y = 6.f;
    data.color[0] = 64.f;
    data.color[1] = 32.f;
    data.color[2] = 16.f;
    data.color[3] = 128.f;
    data.fade_b = 1.f;

    for (std::size_t i = 0; i < adgif.size(); i++) {
      adgif[i] = static_cast<u8>((i * 37 + 11) & 0xff);
    }
  }
};

template <int Size>
bool all_finite(const math::Vector<float, Size>& value) {
  for (float component : value) {
    if (!std::isfinite(component)) {
      return false;
    }
  }
  return true;
}

bool all_math_output_finite(const SpriteGlowOutput& output) {
  for (const auto& position : output.first_clear_pos) {
    if (!all_finite(position)) {
      return false;
    }
  }
  for (const auto& position : output.second_clear_pos) {
    if (!all_finite(position)) {
      return false;
    }
  }
  for (const auto& uv : output.offscreen_uv) {
    if (!all_finite(uv)) {
      return false;
    }
  }
  for (const auto& position : output.flare_xyzw) {
    if (!all_finite(position)) {
      return false;
    }
  }
  return all_finite(output.flare_draw_color) && std::isfinite(output.perspective_q);
}

using OutputBytes = std::array<u8, sizeof(SpriteGlowOutput)>;

OutputBytes output_bytes(const SpriteGlowOutput& output) {
  OutputBytes bytes;
  std::memcpy(bytes.data(), &output, bytes.size());
  return bytes;
}

SpriteGlowOutput sentinel_output() {
  static_assert(std::is_trivially_copyable_v<SpriteGlowOutput>);
  SpriteGlowOutput output;
  std::memset(&output, 0xa5, sizeof(output));
  return output;
}

void test_accepts_visible_sprite() {
  Fixture fixture;
  SpriteGlowOutput output = {};
  check(glow_math(&fixture.consts, false, &fixture.data, fixture.adgif.data(), &output),
        "a finite in-frustum sprite is accepted");
}

void test_rejects_clipped_sprite_transactionally() {
  Fixture fixture;
  fixture.data.pos[0] = 4.f;
  SpriteGlowOutput output = sentinel_output();
  SpriteGlowRejectReason reason = SpriteGlowRejectReason::NONE;
  const auto before = output_bytes(output);
  check(!glow_math(&fixture.consts, false, &fixture.data, fixture.adgif.data(), &output, &reason),
        "an out-of-frustum sprite is rejected");
  check(reason == SpriteGlowRejectReason::CLIPPED_X,
        "an out-of-frustum sprite reports its exact clip axis");
  check(output_bytes(output) == before, "clip rejection leaves the destination unchanged");
}

void test_outputs_are_finite() {
  Fixture fixture;
  SpriteGlowOutput output = {};
  const bool accepted =
      glow_math(&fixture.consts, false, &fixture.data, fixture.adgif.data(), &output);
  check(accepted && all_math_output_finite(output),
        "accepted sprite positions, UVs, color, and perspective Q are finite");
}

void test_flare_color_is_nonzero() {
  Fixture fixture;
  SpriteGlowOutput output = {};
  const bool accepted =
      glow_math(&fixture.consts, false, &fixture.data, fixture.adgif.data(), &output);
  check(accepted && output.flare_draw_color.x() > 0.f && output.flare_draw_color.y() > 0.f &&
            output.flare_draw_color.z() > 0.f,
        "accepted sprite preserves a nonzero faded flare color");
}

void test_adgif_is_copied_exactly() {
  Fixture fixture;
  SpriteGlowOutput output = {};
  const bool accepted =
      glow_math(&fixture.consts, false, &fixture.data, fixture.adgif.data(), &output);
  check(accepted && std::memcmp(&output.adgif, fixture.adgif.data(), fixture.adgif.size()) == 0,
        "accepted sprite copies all five adgif quadwords exactly");
}

void test_rejects_zero_camera_depth_for_offset_transactionally() {
  Fixture fixture;
  fixture.data.pos[2] = 0.f;
  SpriteGlowOutput output = sentinel_output();
  const auto before = output_bytes(output);
  check(!glow_math(&fixture.consts, false, &fixture.data, fixture.adgif.data(), &output),
        "nonzero z offset at zero camera depth is rejected");
  check(output_bytes(output) == before, "camera-depth rejection leaves the destination unchanged");
}

void test_allows_zero_camera_depth_without_offset() {
  Fixture fixture;
  fixture.data.pos[2] = 0.f;
  fixture.data.z_offset = 0.f;
  SpriteGlowOutput output = {};
  const bool accepted =
      glow_math(&fixture.consts, false, &fixture.data, fixture.adgif.data(), &output);
  check(accepted && all_math_output_finite(output),
        "zero camera depth remains valid when no z-offset division is needed");
}

void test_rejects_invalid_perspective_denominators_transactionally() {
  Fixture zero_fixture;
  zero_fixture.consts.hmge.w() = 0.f;
  SpriteGlowOutput zero_output = sentinel_output();
  const auto zero_before = output_bytes(zero_output);
  check(!glow_math(&zero_fixture.consts, false, &zero_fixture.data, zero_fixture.adgif.data(),
                   &zero_output),
        "zero perspective denominator is rejected");
  check(output_bytes(zero_output) == zero_before,
        "zero-denominator rejection leaves the destination unchanged");

  Fixture nonfinite_fixture;
  nonfinite_fixture.consts.perspective[3].w() = std::numeric_limits<float>::max();
  nonfinite_fixture.consts.hmge.w() = std::numeric_limits<float>::max();
  SpriteGlowOutput nonfinite_output = sentinel_output();
  const auto nonfinite_before = output_bytes(nonfinite_output);
  check(!glow_math(&nonfinite_fixture.consts, false, &nonfinite_fixture.data,
                   nonfinite_fixture.adgif.data(), &nonfinite_output),
        "nonfinite computed perspective denominator is rejected");
  check(output_bytes(nonfinite_output) == nonfinite_before,
        "nonfinite-denominator rejection leaves the destination unchanged");
}

void test_rejects_nonfinite_input_and_output_transactionally() {
  Fixture input_fixture;
  input_fixture.data.fade_b = std::numeric_limits<float>::infinity();
  SpriteGlowOutput input_output = sentinel_output();
  const auto input_before = output_bytes(input_output);
  check(!glow_math(&input_fixture.consts, false, &input_fixture.data, input_fixture.adgif.data(),
                   &input_output),
        "nonfinite sprite input is rejected");
  check(output_bytes(input_output) == input_before,
        "nonfinite-input rejection leaves the destination unchanged");

  Fixture output_fixture;
  output_fixture.data.color[0] = std::numeric_limits<float>::max();
  output_fixture.data.color[3] = std::numeric_limits<float>::max();
  SpriteGlowOutput output = sentinel_output();
  const auto output_before = output_bytes(output);
  check(!glow_math(&output_fixture.consts, false, &output_fixture.data,
                   output_fixture.adgif.data(), &output),
        "nonfinite computed glow output is rejected");
  check(output_bytes(output) == output_before,
        "nonfinite-output rejection leaves the destination unchanged");
}

void test_ignores_unread_nonfinite_constants() {
  Fixture fixture;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  fixture.consts.pfog0 = nan;
  fixture.consts.min_scale = nan;
  fixture.consts.inv_area = nan;
  for (auto& row : fixture.consts.sincos) {
    row = math::Vector4f(nan, nan, nan, nan);
  }

  SpriteGlowOutput output = {};
  check(glow_math(&fixture.consts, false, &fixture.data, fixture.adgif.data(), &output),
        "nonfinite constants unused by the VU transform do not reject a sprite");
  check(all_math_output_finite(output),
        "unused nonfinite constants cannot contaminate the transformed output");
}

}  // namespace

int main() {
  test_accepts_visible_sprite();
  test_rejects_clipped_sprite_transactionally();
  test_outputs_are_finite();
  test_flare_color_is_nonzero();
  test_adgif_is_copied_exactly();
  test_rejects_zero_camera_depth_for_offset_transactionally();
  test_allows_zero_camera_depth_without_offset();
  test_rejects_invalid_perspective_denominators_transactionally();
  test_rejects_nonfinite_input_and_output_transactionally();
  test_ignores_unread_nonfinite_constants();

  if (failures) {
    std::printf("FAIL: %d sprite glow math checks failed\n", failures);
    return 1;
  }
  std::puts("PASS: sprite glow math is deterministic and finite");
  return 0;
}
