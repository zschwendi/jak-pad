#include "game/graphics/pipelines/metal/metal_jak2_opcode27_skull_gem_cpu.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

namespace {

using metal_renderer::Jak2Opcode27LayerTransition;
using metal_renderer::Jak2Opcode27LayerValues;
using metal_renderer::Jak2Opcode27RgbaSource;
using metal_renderer::Jak2Opcode27SkullGemPlan;
using metal_renderer::Jak2Opcode27SkullGemRgba;
using metal_renderer::kJak2Opcode27SkullGemLayerCount;
using metal_renderer::kJak2Opcode27SkullGemSize;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

Jak2Opcode27LayerValues identity_values() {
  Jak2Opcode27LayerValues values;
  values.color = {1.f, 1.f, 1.f, 1.f};
  values.scale = {1.f, 1.f};
  values.offset = {0.5f, 0.5f};
  values.st_scale = {1.f, 1.f};
  values.st_offset = {0.5f, 0.5f};
  return values;
}

Jak2Opcode27LayerTransition constant_transition(const Jak2Opcode27LayerValues& values) {
  return {values, values};
}

Jak2Opcode27SkullGemPlan single_layer_plan(const Jak2Opcode27LayerTransition& transition,
                                           float time = 0.f) {
  Jak2Opcode27SkullGemPlan plan;
  plan.time = time;
  plan.layers[0] = transition;
  const Jak2Opcode27LayerValues degenerate;
  for (int layer = 1; layer < kJak2Opcode27SkullGemLayerCount; ++layer) {
    plan.layers[layer] = constant_transition(degenerate);
  }
  return plan;
}

Jak2Opcode27RgbaSource solid_source(u8 r, u8 g, u8 b, u8 a) {
  return {1, 1, {r, g, b, a}};
}

std::array<Jak2Opcode27RgbaSource, kJak2Opcode27SkullGemLayerCount> sources_with_first(
    Jak2Opcode27RgbaSource source) {
  return {std::move(source), solid_source(0, 0, 0, 0), solid_source(0, 0, 0, 0)};
}

std::array<u8, 4> pixel_at(const Jak2Opcode27SkullGemRgba& output, int x, int y) {
  const std::size_t offset = (y * kJak2Opcode27SkullGemSize + x) * 4;
  return {output[offset + 0], output[offset + 1], output[offset + 2], output[offset + 3]};
}

Jak2Opcode27RgbaSource coordinate_source() {
  Jak2Opcode27RgbaSource source;
  source.width = kJak2Opcode27SkullGemSize;
  source.height = kJak2Opcode27SkullGemSize;
  source.rgba.resize(source.width * source.height * 4);
  for (std::size_t y = 0; y < source.height; ++y) {
    for (std::size_t x = 0; x < source.width; ++x) {
      const std::size_t offset = (y * source.width + x) * 4;
      source.rgba[offset + 0] = static_cast<u8>(x * 7);
      source.rgba[offset + 1] = static_cast<u8>(y * 7);
      source.rgba[offset + 2] = static_cast<u8>(x + y);
      source.rgba[offset + 3] = 128;
    }
  }
  return source;
}

void test_fixed_clear() {
  Jak2Opcode27SkullGemPlan plan;
  const std::array<Jak2Opcode27RgbaSource, kJak2Opcode27SkullGemLayerCount> sources = {
      solid_source(255, 0, 0, 255), solid_source(0, 255, 0, 255),
      solid_source(0, 0, 255, 255)};
  Jak2Opcode27SkullGemRgba output;
  check(metal_renderer::compose_jak2_opcode27_skull_gem_cpu(plan, sources, &output),
        "degenerate layers leave the fixed clear valid");
  for (int y = 0; y < kJak2Opcode27SkullGemSize; ++y) {
    for (int x = 0; x < kJak2Opcode27SkullGemSize; ++x) {
      check(pixel_at(output, x, y) == std::array<u8, 4>{0, 0, 0, 255},
            "the 32x32 fixed destination clears to opaque black like tex_anim.frag");
    }
  }
}

void test_identity_transform() {
  const auto plan = single_layer_plan(constant_transition(identity_values()));
  const auto sources = sources_with_first(solid_source(25, 50, 100, 64));
  Jak2Opcode27SkullGemRgba output;
  check(metal_renderer::compose_jak2_opcode27_skull_gem_cpu(plan, sources, &output),
        "identity plan is valid");
  for (int y = 0; y < kJak2Opcode27SkullGemSize; ++y) {
    for (int x = 0; x < kJak2Opcode27SkullGemSize; ++x) {
      check(pixel_at(output, x, y) == std::array<u8, 4>{13, 25, 50, 64},
            "identity transform preserves coverage and fixed blend semantics");
    }
  }
}

void test_half_interpolation() {
  Jak2Opcode27LayerValues start;
  start.color = {0.5f, 0.5f, 0.5f, 0.5f};
  start.scale = {0.5f, 0.5f};
  start.offset = {0.25f, 0.25f};
  start.st_scale = {0.f, 0.f};
  start.st_offset = {0.f, 0.f};
  start.qs = {0.f, 0.f, 0.f, 0.f};
  start.rot = -100.f;
  start.st_rot = -200.f;

  Jak2Opcode27LayerValues end;
  end.color = {1.5f, 1.5f, 1.5f, 1.5f};
  end.scale = {1.5f, 1.5f};
  end.offset = {0.75f, 0.75f};
  end.st_scale = {2.f, 2.f};
  end.st_offset = {1.f, 1.f};
  end.qs = {2.f, 2.f, 2.f, 2.f};
  end.rot = 100.f;
  end.st_rot = 200.f;

  const auto plan = single_layer_plan({start, end}, 150.f);
  const auto source = coordinate_source();
  const auto sources = sources_with_first(source);
  Jak2Opcode27SkullGemRgba output;
  check(metal_renderer::compose_jak2_opcode27_skull_gem_cpu(plan, sources, &output),
        "half-interpolated plan is valid");
  for (int y = 0; y < kJak2Opcode27SkullGemSize; ++y) {
    for (int x = 0; x < kJak2Opcode27SkullGemSize; ++x) {
      check(pixel_at(output, x, y) ==
                std::array<u8, 4>{static_cast<u8>(x * 7), static_cast<u8>(y * 7),
                                  static_cast<u8>(x + y), 128},
            "half interpolation reaches identity color, position, ST, and rotations");
    }
  }
}

void test_repeat_bilinear_sampling() {
  Jak2Opcode27LayerValues values = identity_values();
  values.st_scale = {0.f, 0.f};
  values.st_offset = {0.f, 0.f};
  Jak2Opcode27RgbaSource source{2,
                                2,
                                {0, 0, 0, 128, 64, 0, 0, 128, 128, 0, 0, 128, 255, 0, 0, 128}};
  const auto sources = sources_with_first(std::move(source));
  Jak2Opcode27SkullGemRgba output;
  check(metal_renderer::compose_jak2_opcode27_skull_gem_cpu(
            single_layer_plan(constant_transition(values)), sources, &output),
        "repeat-bilinear plan is valid");
  check(pixel_at(output, 0, 0) == std::array<u8, 4>{112, 0, 0, 128} &&
            pixel_at(output, 31, 31) == std::array<u8, 4>{112, 0, 0, 128},
        "sampling at the repeat seam bilinearly averages all four source texels");
}

void test_full_rgba_modulation() {
  Jak2Opcode27LayerValues values = identity_values();
  values.color = {0.5f, 0.25f, 0.75f, 0.5f};
  const auto sources = sources_with_first(solid_source(200, 160, 120, 128));
  Jak2Opcode27SkullGemRgba output;
  check(metal_renderer::compose_jak2_opcode27_skull_gem_cpu(
            single_layer_plan(constant_transition(values)), sources, &output),
        "per-channel modulation plan is valid");
  check(pixel_at(output, 7, 9) == std::array<u8, 4>{50, 20, 45, 64},
        "all four interpolated color channels modulate the sampled RGBA texel");
}

void test_layer_order_blend_and_alpha_replacement() {
  Jak2Opcode27SkullGemPlan plan;
  plan.layers.fill(constant_transition(identity_values()));
  const std::array<Jak2Opcode27RgbaSource, kJak2Opcode27SkullGemLayerCount> sources = {
      solid_source(100, 0, 0, 64), solid_source(0, 120, 0, 128), solid_source(0, 0, 80, 32)};
  Jak2Opcode27SkullGemRgba output;
  check(metal_renderer::compose_jak2_opcode27_skull_gem_cpu(plan, sources, &output),
        "three-layer plan is valid");
  check(pixel_at(output, 12, 19) == std::array<u8, 4>{50, 120, 20, 32},
        "ordered layers add B2/D1 RGB and the final covered layer replaces alpha");
}

void test_position_and_st_rotations() {
  const auto source = coordinate_source();

  Jak2Opcode27LayerValues st_rotation = identity_values();
  st_rotation.st_rot = 16384.f;
  Jak2Opcode27SkullGemRgba st_output;
  check(metal_renderer::compose_jak2_opcode27_skull_gem_cpu(
            single_layer_plan(constant_transition(st_rotation)), sources_with_first(source),
            &st_output),
        "ST rotation plan is valid");
  check(pixel_at(st_output, 5, 0) == std::array<u8, 4>{35, 217, 36, 128} &&
            pixel_at(st_output, 5, 31) == std::array<u8, 4>{35, 0, 5, 128},
        "a quarter-turn ST rotation follows TextureAnimator's vertical reflection");

  Jak2Opcode27LayerValues position_rotation = identity_values();
  position_rotation.rot = 16384.f;
  Jak2Opcode27SkullGemRgba position_output;
  check(metal_renderer::compose_jak2_opcode27_skull_gem_cpu(
            single_layer_plan(constant_transition(position_rotation)), sources_with_first(source),
            &position_output),
        "position rotation plan is valid");
  check(pixel_at(position_output, 5, 0) == std::array<u8, 4>{35, 217, 36, 128} &&
            pixel_at(position_output, 5, 31) == std::array<u8, 4>{35, 0, 5, 128},
        "a quarter-turn position rotation changes texture orientation through the fixed quad");
}

void test_invalid_inputs_fail_without_output_mutation() {
  const auto valid_plan = single_layer_plan(constant_transition(identity_values()));
  const auto valid_sources = sources_with_first(solid_source(1, 2, 3, 4));
  Jak2Opcode27SkullGemRgba output;
  output.fill(0xa5);
  const auto sentinel = output;

  auto sources = valid_sources;
  sources[0].width = 0;
  check(!metal_renderer::compose_jak2_opcode27_skull_gem_cpu(valid_plan, sources, &output) &&
            output == sentinel,
        "zero source dimensions fail without output mutation");
  sources = valid_sources;
  sources[0].rgba.pop_back();
  check(!metal_renderer::compose_jak2_opcode27_skull_gem_cpu(valid_plan, sources, &output) &&
            output == sentinel,
        "a malformed RGBA byte count fails without output mutation");
  sources = valid_sources;
  sources[0].width = std::numeric_limits<std::size_t>::max();
  sources[0].height = 2;
  check(!metal_renderer::compose_jak2_opcode27_skull_gem_cpu(valid_plan, sources, &output) &&
            output == sentinel,
        "overflowing source dimensions fail without output mutation");

  auto plan = valid_plan;
  plan.time = -1.f;
  check(!metal_renderer::compose_jak2_opcode27_skull_gem_cpu(plan, valid_sources, &output) &&
            output == sentinel,
        "time below the fixed interval fails without output mutation");
  plan.time = 301.f;
  check(!metal_renderer::compose_jak2_opcode27_skull_gem_cpu(plan, valid_sources, &output) &&
            output == sentinel,
        "time above the fixed interval fails without output mutation");
  plan = valid_plan;
  plan.layers[0].start.color[0] = std::numeric_limits<float>::quiet_NaN();
  check(!metal_renderer::compose_jak2_opcode27_skull_gem_cpu(plan, valid_sources, &output) &&
            output == sentinel,
        "non-finite color fails without output mutation");
  plan = valid_plan;
  plan.layers[0].end.qs[3] = std::numeric_limits<float>::infinity();
  check(!metal_renderer::compose_jak2_opcode27_skull_gem_cpu(plan, valid_sources, &output) &&
            output == sentinel,
        "non-finite unused LayerVals fields still fail validation");
  plan = valid_plan;
  plan.layers[0].start.st_rot = std::numeric_limits<float>::infinity();
  check(!metal_renderer::compose_jak2_opcode27_skull_gem_cpu(plan, valid_sources, &output) &&
            output == sentinel,
        "non-finite rotation fails without output mutation");
}

void test_stable_repeated_output() {
  Jak2Opcode27SkullGemPlan plan;
  plan.time = 123.f;
  plan.layers.fill(constant_transition(identity_values()));
  const std::array<Jak2Opcode27RgbaSource, kJak2Opcode27SkullGemLayerCount> sources = {
      coordinate_source(), solid_source(17, 93, 41, 72), solid_source(5, 11, 19, 33)};
  Jak2Opcode27SkullGemRgba first;
  Jak2Opcode27SkullGemRgba second;
  check(metal_renderer::compose_jak2_opcode27_skull_gem_cpu(plan, sources, &first) &&
            metal_renderer::compose_jak2_opcode27_skull_gem_cpu(plan, sources, &second) &&
            first == second,
        "identical plans and owned sources produce stable repeated RGBA output");
}

}  // namespace

int main() {
  test_fixed_clear();
  test_identity_transform();
  test_half_interpolation();
  test_repeat_bilinear_sampling();
  test_full_rgba_modulation();
  test_layer_order_blend_and_alpha_replacement();
  test_position_and_st_rotations();
  test_invalid_inputs_fail_without_output_mutation();
  test_stable_repeated_output();
  std::puts("metal_jak2_opcode27_skull_gem_cpu_test: PASS");
  return 0;
}
