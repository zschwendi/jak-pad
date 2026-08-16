#include "game/graphics/pipelines/metal/metal_jak2_opcode41_cloud_cpu.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

metal_renderer::Jak2Opcode41CloudInput synthetic_input() {
  metal_renderer::Jak2Opcode41CloudInput input;
  input.cloud_min = 0.2f;
  input.cloud_max = 0.75f;
  input.times = {1200.f, 600.f, 300.f, 150.f};
  input.max_times = {4800.f, 2400.f, 1200.f, 600.f};
  input.scales = {0.49f, 0.19f, 0.145f, 0.015f};
  return input;
}

u64 rgba_hash(const std::array<u32, metal_renderer::kJak2Opcode41CloudPixelCount>& rgba) {
  u64 hash = 14695981039346656037ull;
  for (const u32 pixel : rgba) {
    for (int byte = 0; byte < 4; ++byte) {
      hash ^= (pixel >> (byte * 8)) & 0xff;
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

void test_threshold_endpoints() {
  using metal_renderer::jak2_opcode41_cloud_lookup;
  check(jak2_opcode41_cloud_lookup(0.25f, 0.25f, 0.75f) == 0.f,
        "cloud lookup is zero at its minimum");
  check(jak2_opcode41_cloud_lookup(0.75f, 0.25f, 0.75f) == 1.f,
        "cloud lookup is one at its maximum");
  check(std::fabs(jak2_opcode41_cloud_lookup(0.5f, 0.25f, 0.75f) - 0.5f) < 0.000001f,
        "cloud lookup uses the shader's sin-squared midpoint");
  check(jak2_opcode41_cloud_lookup(0.6f, 0.6f, 0.2f) == 0.f &&
            jak2_opcode41_cloud_lookup(0.6001f, 0.6f, 0.2f) == 1.f,
        "a reversed range collapses maximum to minimum like the shader");

  auto input = synthetic_input();
  input.cloud_min = 1.f;
  input.cloud_max = 2.f;
  metal_renderer::Jak2Opcode41CloudCpu transparent;
  check(transparent.generate(input), "transparent endpoint input is valid");
  for (const u32 pixel : transparent.rgba()) {
    check(pixel == 0x00808080u, "values below minimum pack gray RGB and zero alpha");
  }

  input.cloud_min = -1.f;
  input.cloud_max = -0.5f;
  metal_renderer::Jak2Opcode41CloudCpu opaque;
  check(opaque.generate(input), "opaque endpoint input is valid");
  for (const u32 pixel : opaque.rgba()) {
    check(pixel == 0x80808080u, "values above maximum pack gray RGB and half-range alpha");
  }
}

void test_deterministic_hash_and_repeatability() {
  const auto input = synthetic_input();
  metal_renderer::Jak2Opcode41CloudCpu first;
  metal_renderer::Jak2Opcode41CloudCpu second;
  check(first.generate(input) && second.generate(input), "synthetic cloud inputs are valid");
  const u64 first_hash = rgba_hash(first.rgba());
  check(first_hash == 3189851289004097446ull,
        "synthetic pre-rollover input has its deterministic RGBA hash");
  check(first.rgba() == second.rgba(), "fresh generators produce identical pixels");

  check(first.generate(input), "repeated identical input remains valid");
  check(rgba_hash(first.rgba()) == first_hash,
        "repeated identical input produces the same owned output");
}

void test_rollover_hash() {
  auto before_wrap = synthetic_input();
  before_wrap.times = {4700.f, 2300.f, 1100.f, 500.f};
  auto after_wrap = synthetic_input();
  after_wrap.times = {100.f, 100.f, 100.f, 100.f};

  metal_renderer::Jak2Opcode41CloudCpu generator;
  check(generator.generate(before_wrap), "pre-rollover input is valid");
  check(generator.generate(after_wrap), "rollover input is valid");
  const u64 rollover_hash = rgba_hash(generator.rgba());
  check(rollover_hash == 1926388136240243494ull,
        "post-rollover input has its deterministic RGBA hash");

  generator.reset();
  check(generator.generate(synthetic_input()) &&
            rgba_hash(generator.rgba()) == 3189851289004097446ull,
        "reset restores the deterministic initial noise and output state");
}

void test_invalid_inputs_fail_without_mutation() {
  metal_renderer::Jak2Opcode41CloudCpu generator;
  auto input = synthetic_input();
  check(generator.generate(input), "baseline input is valid");
  const u64 baseline = rgba_hash(generator.rgba());

  input.cloud_min = std::numeric_limits<float>::quiet_NaN();
  check(!generator.generate(input) && rgba_hash(generator.rgba()) == baseline,
        "non-finite thresholds are rejected without changing output");
  input = synthetic_input();
  input.times[1] = std::numeric_limits<float>::infinity();
  check(!generator.generate(input) && rgba_hash(generator.rgba()) == baseline,
        "non-finite times are rejected without changing output");
  input = synthetic_input();
  input.max_times[2] = 0.f;
  check(!generator.generate(input) && rgba_hash(generator.rgba()) == baseline,
        "zero max time is rejected without changing output");
  input.max_times[2] = -1.f;
  check(!generator.generate(input) && rgba_hash(generator.rgba()) == baseline,
        "negative max time is rejected without changing output");
  input.max_times[2] = std::numeric_limits<float>::infinity();
  check(!generator.generate(input) && rgba_hash(generator.rgba()) == baseline,
        "non-finite max time is rejected without changing output");
  input = synthetic_input();
  input.scales[3] = std::numeric_limits<float>::infinity();
  check(!generator.generate(input) && rgba_hash(generator.rgba()) == baseline,
        "non-finite scales are rejected without changing output");
}

}  // namespace

int main() {
  test_threshold_endpoints();
  test_deterministic_hash_and_repeatability();
  test_rollover_hash();
  test_invalid_inputs_fail_without_mutation();
  std::puts("metal_jak2_opcode41_cloud_cpu_test: PASS");
  return 0;
}
