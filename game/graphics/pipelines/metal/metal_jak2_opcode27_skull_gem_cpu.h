#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "common/common_types.h"

namespace metal_renderer {

constexpr int kJak2Opcode27SkullGemSize = 32;
constexpr int kJak2Opcode27SkullGemLayerCount = 3;
constexpr std::size_t kJak2Opcode27SkullGemRgbaBytes =
    kJak2Opcode27SkullGemSize * kJak2Opcode27SkullGemSize * 4;

struct Jak2Opcode27RgbaSource {
  std::size_t width = 0;
  std::size_t height = 0;
  std::vector<u8> rgba;
};

struct Jak2Opcode27LayerValues {
  std::array<float, 4> color = {};
  std::array<float, 2> scale = {};
  std::array<float, 2> offset = {};
  std::array<float, 2> st_scale = {};
  std::array<float, 2> st_offset = {};
  std::array<float, 4> qs = {1.f, 1.f, 1.f, 1.f};
  float rot = 0.f;
  float st_rot = 0.f;
};

struct Jak2Opcode27LayerTransition {
  Jak2Opcode27LayerValues start = {};
  Jak2Opcode27LayerValues end = {};
};

struct Jak2Opcode27SkullGemPlan {
  float time = 0.f;
  std::array<Jak2Opcode27LayerTransition, kJak2Opcode27SkullGemLayerCount> layers = {};
};

using Jak2Opcode27SkullGemRgba = std::array<u8, kJak2Opcode27SkullGemRgbaBytes>;

/*!
 * Composites the fixed Jak II opcode-27 skull-gem animation into owned RGBA8 output.
 * Source row zero and output row zero both correspond to normalized texture coordinate v = 0.
 * Returns false and leaves output unchanged when any input is invalid.
 */
bool compose_jak2_opcode27_skull_gem_cpu(
    const Jak2Opcode27SkullGemPlan& plan,
    const std::array<Jak2Opcode27RgbaSource, kJak2Opcode27SkullGemLayerCount>& sources,
    Jak2Opcode27SkullGemRgba* output);

}  // namespace metal_renderer
