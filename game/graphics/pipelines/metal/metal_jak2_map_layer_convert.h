#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace metal_renderer {

constexpr std::size_t kJak2MapLayerWidth = 512;
constexpr std::size_t kJak2MapLayerHeight = 208;
constexpr std::size_t kJak2MapLayerPixelCount = kJak2MapLayerWidth * kJak2MapLayerHeight;
constexpr std::size_t kJak2MapLayerPsmct32ClutEntryCount = 16 * 16;

enum class Jak2MapLayerRejection {
  None,
  BadDimensions,
  BadIndexCount,
  BadClutEntryCount,
};

struct Jak2MapLayerRgba {
  bool valid = false;
  Jak2MapLayerRejection rejection = Jak2MapLayerRejection::None;
  // Owned row-major (row 0 first) raw 0xAABBGGRR words; empty unless valid.
  std::vector<u32> rgba;
};

/*!
 * Convert one Jak II 512x208 PSMT8 bigmap layer through its 16x16 PSMCT32 CSM1 CLUT.
 * Mirrors OpenGL TextureAnimator's GENERIC_PSMT8 path: linear row order, no VRAM unswizzle,
 * words copied unchanged (no channel swap, no alpha rescale). Inputs are only read.
 */
Jak2MapLayerRgba convert_jak2_map_psmt8_to_rgba(std::span<const u8> indices,
                                                std::span<const u32> psmct32_clut,
                                                std::size_t width,
                                                std::size_t height);

}  // namespace metal_renderer
