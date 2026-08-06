#include "game/graphics/pipelines/metal/metal_jak2_fog_texture_convert.h"

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

}  // namespace

std::optional<Jak2FogRgbaPixels> convert_jak2_fog_psmt8_to_rgba(const u8* indices,
                                                                std::size_t index_count,
                                                                const u32* psmct32_clut,
                                                                std::size_t clut_entry_count) {
  if (!indices || !psmct32_clut || index_count != kJak2FogIndexedPixelCount ||
      clut_entry_count != kJak2FogPsmct32ClutEntryCount) {
    return std::nullopt;
  }

  Jak2FogRgbaPixels rgba = {};
  for (std::size_t i = 0; i < rgba.size(); i++) {
    rgba[i] = psmct32_clut[psmt8_clut_address(indices[i])];
  }
  return rgba;
}

}  // namespace metal_renderer
