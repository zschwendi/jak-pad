#include "game/graphics/pipelines/metal/metal_jak2_prison_clut_cpu.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>
#include <vector>

namespace {

using metal_renderer::Jak2ClutBlendInput;
using metal_renderer::Jak2ClutBlendPalette;
using metal_renderer::Jak2ClutBlendRgba;
using metal_renderer::kJak2ClutBlendSlotCount;

struct Fixture {
  std::array<std::vector<u8>, kJak2ClutBlendSlotCount> index_storage;
  std::array<Jak2ClutBlendInput, kJak2ClutBlendSlotCount> inputs = {};

  Fixture() {
    for (std::size_t slot = 0; slot < kJak2ClutBlendSlotCount; ++slot) {
      index_storage[slot] = {static_cast<u8>(slot), 255, static_cast<u8>(128 + slot), 0};
      inputs[slot].destination = {2, 2, index_storage[slot]};
      for (std::size_t entry = 0; entry < 256; ++entry) {
        inputs[slot].start_palette[entry] = {
            static_cast<u8>((entry + slot + 1) & 0xff),
            static_cast<u8>((entry * 3 + slot + 2) & 0xff),
            static_cast<u8>((entry * 5 + slot + 3) & 0xff),
            static_cast<u8>((entry * 7 + slot + 4) & 0xff),
        };
        inputs[slot].end_palette[entry] = {
            static_cast<u8>((255 - entry + slot + 5) & 0xff),
            static_cast<u8>((entry * 11 + slot + 6) & 0xff),
            static_cast<u8>((entry * 13 + slot + 7) & 0xff),
            static_cast<u8>((entry * 17 + slot + 8) & 0xff),
        };
      }
    }
  }
};

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

std::array<std::vector<u8>, kJak2ClutBlendSlotCount> sentinel_outputs() {
  std::array<std::vector<u8>, kJak2ClutBlendSlotCount> outputs;
  for (auto& output : outputs) {
    output = {0xde, 0xad, 0xbe, 0xef};
  }
  return outputs;
}

std::vector<u8> expected_output(const Jak2ClutBlendInput& input,
                                const Jak2ClutBlendPalette& palette) {
  std::vector<u8> output;
  output.reserve(input.destination.indices.size() * 4);
  for (const u8 index : input.destination.indices) {
    const Jak2ClutBlendRgba& color = palette[index];
    output.insert(output.end(), color.begin(), color.end());
  }
  return output;
}

void test_endpoints_and_index_mapping() {
  Fixture fixture;
  std::array<std::vector<u8>, kJak2ClutBlendSlotCount> outputs;
  check(metal_renderer::blend_jak2_clut_group_cpu(0.f, fixture.inputs, outputs),
        "start endpoint is valid");
  for (std::size_t slot = 0; slot < kJak2ClutBlendSlotCount; ++slot) {
    check(outputs[slot] == expected_output(fixture.inputs[slot], fixture.inputs[slot].start_palette),
          "start palette endpoint and index mapping are exact");
  }

  check(metal_renderer::blend_jak2_clut_group_cpu(1.f, fixture.inputs, outputs),
        "end endpoint is valid");
  for (std::size_t slot = 0; slot < kJak2ClutBlendSlotCount; ++slot) {
    check(outputs[slot] == expected_output(fixture.inputs[slot], fixture.inputs[slot].end_palette),
          "end palette endpoint and index mapping are exact");
  }
}

void test_midpoint_truncates_channels() {
  Fixture fixture;
  fixture.inputs[0].start_palette[0] = {0, 1, 2, 3};
  fixture.inputs[0].end_palette[0] = {1, 2, 3, 4};
  std::array<std::vector<u8>, kJak2ClutBlendSlotCount> outputs;
  check(metal_renderer::blend_jak2_clut_group_cpu(0.5f, fixture.inputs, outputs),
        "midpoint input is valid");
  check(std::vector<u8>(outputs[0].begin(), outputs[0].begin() + 4) ==
            std::vector<u8>{0, 1, 2, 3},
        "midpoint palette channels truncate instead of round");
}

void test_invalid_shapes_and_counts_leave_outputs_unchanged() {
  Fixture fixture;
  auto outputs = sentinel_outputs();
  const auto baseline = outputs;

  fixture.inputs[0].destination.width = 0;
  check(!metal_renderer::blend_jak2_clut_group_cpu(0.5f, fixture.inputs, outputs) &&
            outputs == baseline,
        "zero width is rejected without mutating outputs");
  fixture.inputs[0].destination.width = 2;
  fixture.inputs[0].destination.height = 0;
  check(!metal_renderer::blend_jak2_clut_group_cpu(0.5f, fixture.inputs, outputs) &&
            outputs == baseline,
        "zero height is rejected without mutating outputs");
  fixture.inputs[0].destination.height = 2;
  fixture.inputs[0].destination.indices =
      std::span<const u8>(fixture.index_storage[0]).first(3);
  check(!metal_renderer::blend_jak2_clut_group_cpu(0.5f, fixture.inputs, outputs) &&
            outputs == baseline,
        "index count mismatch is rejected without mutating outputs");

  fixture.inputs[0].destination.indices = fixture.index_storage[0];
  fixture.inputs[0].destination.width = std::numeric_limits<std::size_t>::max();
  fixture.inputs[0].destination.height = 2;
  check(!metal_renderer::blend_jak2_clut_group_cpu(0.5f, fixture.inputs, outputs) &&
            outputs == baseline,
        "dimension multiplication overflow is rejected without mutating outputs");

  fixture.inputs[0].destination = {2, 2, fixture.index_storage[0]};
  check(!metal_renderer::blend_jak2_clut_group_cpu(
              0.5f, std::span<const Jak2ClutBlendInput>(fixture.inputs).first(5), outputs) &&
            outputs == baseline,
        "fewer than six inputs are rejected without mutating outputs");
  check(!metal_renderer::blend_jak2_clut_group_cpu(
              0.5f, fixture.inputs, std::span<std::vector<u8>>(outputs).first(5)) &&
            outputs == baseline,
        "fewer than six outputs are rejected without mutating outputs");
}

void test_invalid_morph_leaves_outputs_unchanged() {
  Fixture fixture;
  for (const float morph : {-0.01f,
                            1.01f,
                            std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity()}) {
    auto outputs = sentinel_outputs();
    const auto baseline = outputs;
    check(!metal_renderer::blend_jak2_clut_group_cpu(morph, fixture.inputs, outputs) &&
              outputs == baseline,
          "nonfinite or out-of-range morph is rejected without mutating outputs");
  }
}

}  // namespace

int main() {
  test_endpoints_and_index_mapping();
  test_midpoint_truncates_channels();
  test_invalid_shapes_and_counts_leave_outputs_unchanged();
  test_invalid_morph_leaves_outputs_unchanged();
  std::puts("metal_jak2_prison_clut_cpu_test: PASS");
  return 0;
}
