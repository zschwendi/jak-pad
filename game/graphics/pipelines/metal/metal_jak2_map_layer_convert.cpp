#include "game/graphics/pipelines/metal/metal_jak2_map_layer_convert.h"

namespace metal_renderer {
namespace {

std::size_t psmt8_clut_address(u8 index) {
  const std::size_t clut_chunk = index / 16;
  std::size_t offset_in_chunk = index % 16;
  std::size_t x = (clut_chunk & 1) ? 8 : 0;
  std::size_t y = (clut_chunk >> 1) * 2;
  if (offset_in_chunk >= 8) {
    offset_in_chunk -= 8;
    y++;
  }
  x += offset_in_chunk;
  return x + y * 16;
}

Jak2MapLayerRgba reject(Jak2MapLayerRejection rejection) {
  Jak2MapLayerRgba result;
  result.rejection = rejection;
  return result;
}

}  // namespace

Jak2MapLayerRgba convert_jak2_map_psmt8_to_rgba(std::span<const u8> indices,
                                                std::span<const u32> psmct32_clut,
                                                std::size_t width,
                                                std::size_t height) {
  // Exact-shape compare first, so width * height is never computed for untrusted values.
  if (width != kJak2MapLayerWidth || height != kJak2MapLayerHeight) {
    return reject(Jak2MapLayerRejection::BadDimensions);
  }
  if (indices.size() != kJak2MapLayerPixelCount) {
    return reject(Jak2MapLayerRejection::BadIndexCount);
  }
  if (psmct32_clut.size() != kJak2MapLayerPsmct32ClutEntryCount) {
    return reject(Jak2MapLayerRejection::BadClutEntryCount);
  }

  Jak2MapLayerRgba result;
  result.rgba.resize(kJak2MapLayerPixelCount);
  for (std::size_t r = 0; r < kJak2MapLayerHeight; r++) {
    for (std::size_t c = 0; c < kJak2MapLayerWidth; c++) {
      const std::size_t i = c + r * kJak2MapLayerWidth;
      result.rgba[i] = psmct32_clut[psmt8_clut_address(indices[i])];
    }
  }
  result.valid = true;
  return result;
}

}  // namespace metal_renderer
