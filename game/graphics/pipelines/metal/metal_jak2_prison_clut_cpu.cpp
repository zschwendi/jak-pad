#include "game/graphics/pipelines/metal/metal_jak2_prison_clut_cpu.h"

#include <cmath>
#include <limits>
#include <utility>

namespace metal_renderer {
namespace {

bool valid_dimensions(const Jak2ClutBlendIndexedTexture& texture,
                      std::size_t* pixel_count) {
  if (texture.width == 0 || texture.height == 0 ||
      texture.width > std::numeric_limits<std::size_t>::max() / texture.height) {
    return false;
  }

  *pixel_count = texture.width * texture.height;
  return texture.indices.size() == *pixel_count &&
         *pixel_count <= std::numeric_limits<std::size_t>::max() / 4;
}

bool blend_palette(const Jak2ClutBlendPalette& start,
                   const Jak2ClutBlendPalette& end,
                   float morph,
                   Jak2ClutBlendPalette* result) {
  const float start_weight = 1.f - morph;
  for (std::size_t entry = 0; entry < kJak2ClutBlendPaletteEntryCount; ++entry) {
    for (std::size_t channel = 0; channel < 4; ++channel) {
      float blended = 0.f;
      blended += static_cast<float>(start[entry][channel]) * start_weight;
      blended += static_cast<float>(end[entry][channel]) * morph;
      if (!std::isfinite(blended) || blended < 0.f || blended > 255.f) {
        return false;
      }
      (*result)[entry][channel] = static_cast<u8>(blended);
    }
  }
  return true;
}

}  // namespace

bool blend_jak2_clut_group_cpu(float morph,
                               std::span<const Jak2ClutBlendInput> inputs,
                               std::span<std::vector<u8>> outputs) {
  if (!std::isfinite(morph) || morph < 0.f || morph > 1.f ||
      inputs.size() != kJak2ClutBlendSlotCount ||
      outputs.size() != kJak2ClutBlendSlotCount) {
    return false;
  }

  std::array<std::size_t, kJak2ClutBlendSlotCount> pixel_counts = {};
  for (std::size_t slot = 0; slot < kJak2ClutBlendSlotCount; ++slot) {
    if (!valid_dimensions(inputs[slot].destination, &pixel_counts[slot])) {
      return false;
    }
  }

  std::array<Jak2ClutBlendPalette, kJak2ClutBlendSlotCount> palettes = {};
  for (std::size_t slot = 0; slot < kJak2ClutBlendSlotCount; ++slot) {
    if (!blend_palette(inputs[slot].start_palette, inputs[slot].end_palette, morph,
                       &palettes[slot])) {
      return false;
    }
  }

  std::array<std::vector<u8>, kJak2ClutBlendSlotCount> next_outputs;
  for (std::size_t slot = 0; slot < kJak2ClutBlendSlotCount; ++slot) {
    next_outputs[slot].resize(pixel_counts[slot] * 4);
    for (std::size_t pixel = 0; pixel < pixel_counts[slot]; ++pixel) {
      const Jak2ClutBlendRgba& color = palettes[slot][inputs[slot].destination.indices[pixel]];
      for (std::size_t channel = 0; channel < color.size(); ++channel) {
        next_outputs[slot][pixel * color.size() + channel] = color[channel];
      }
    }
  }

  for (std::size_t slot = 0; slot < kJak2ClutBlendSlotCount; ++slot) {
    outputs[slot] = std::move(next_outputs[slot]);
  }
  return true;
}

}  // namespace metal_renderer
