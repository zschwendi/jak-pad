#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "common/common_types.h"

namespace metal_renderer {

constexpr std::size_t kJak2FogIndexedPixelCount = 256;
constexpr std::size_t kJak2FogPsmct32ClutEntryCount = 16 * 16;

using Jak2FogRgbaPixels = std::array<u32, kJak2FogIndexedPixelCount>;

/*!
 * Convert the Jak II 256x1 PSMT8 fog texture through its linear 16x16 PSMCT32 CLUT.
 * Returned words retain OpenGOAL's raw 0xAABBGGRR representation.
 */
std::optional<Jak2FogRgbaPixels> convert_jak2_fog_psmt8_to_rgba(const u8* indices,
                                                                std::size_t index_count,
                                                                const u32* psmct32_clut,
                                                                std::size_t clut_entry_count);

}  // namespace metal_renderer
