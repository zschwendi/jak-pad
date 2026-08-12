#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace metal_renderer {

constexpr std::size_t kJak2ClutBlendSlotCount = 6;
constexpr std::size_t kJak2DarkJakClutBlendSlotCount = 4;
constexpr std::size_t kJak2ClutBlendPaletteEntryCount = 256;

using Jak2ClutBlendRgba = std::array<u8, 4>;
using Jak2ClutBlendPalette =
    std::array<Jak2ClutBlendRgba, kJak2ClutBlendPaletteEntryCount>;

struct Jak2ClutBlendIndexedTexture {
  std::size_t width = 0;
  std::size_t height = 0;
  std::span<const u8> indices;
};

struct Jak2ClutBlendInput {
  Jak2ClutBlendIndexedTexture destination;
  Jak2ClutBlendPalette start_palette = {};
  Jak2ClutBlendPalette end_palette = {};
};

/*! Blends six indexed RGBA textures into owned row-major RGBA byte vectors.
 *
 * The two palettes are accumulated in float with weights `{1 - morph, morph}`
 * and each channel is converted to u8 by truncation, matching ClutBlender.
 * Returns false without changing outputs when morph, slot counts, dimensions,
 * or index counts are invalid.
 */
bool blend_jak2_clut_group_cpu(float morph,
                               std::span<const Jak2ClutBlendInput> inputs,
                               std::span<std::vector<u8>> outputs);

/*! Four-output common Dark Jak variant of the same indexed-palette blend. */
bool blend_jak2_dark_jak_clut_group_cpu(float morph,
                                        std::span<const Jak2ClutBlendInput> inputs,
                                        std::span<std::vector<u8>> outputs);

}  // namespace metal_renderer
