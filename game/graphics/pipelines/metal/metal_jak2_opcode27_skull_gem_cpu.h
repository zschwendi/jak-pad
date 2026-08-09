#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"

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

bool compose_jak2_fixed_animation_cpu(
    float time,
    std::span<const float> layer_end_times,
    std::span<const Jak2Opcode27LayerTransition> layers,
    std::span<const Jak2Opcode27RgbaSource> sources,
    std::size_t output_width,
    std::size_t output_height,
    std::vector<u8>* output);

}  // namespace metal_renderer
