#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"
#include "common/dma/dma_chain_read.h"

namespace metal_renderer {

inline constexpr std::size_t kMetalVisibilityBytes = 128 * 16;
inline constexpr std::size_t kMetalInactiveVisibilityBytes = 16;
inline constexpr std::size_t kMetalBackgroundFallbackBytes = 25 * 16;
inline constexpr std::size_t kMetalMaxVisibilityLevels = 32;
inline constexpr std::size_t kMetalMaxVisDataTransfers = kMetalMaxVisibilityLevels * 2 + 2;

struct MetalLevelVisibility {
  bool valid = false;
  std::array<u8, kMetalVisibilityBytes> data = {};
};

// Owns every byte copied from one frame's visibility bucket. The raw fallback
// block deliberately stays title-neutral here; background renderers interpret
// it as their version's PC-port camera structure.
struct MetalVisibilityFrame {
  std::array<MetalLevelVisibility, kMetalMaxVisibilityLevels> levels = {};
  std::size_t level_count = 0;
  u32 fog_vif0 = 0;
  bool has_fallback = false;
  std::array<u8, kMetalBackgroundFallbackBytes> fallback = {};

  void reset();
};

// Decodes level payload/boundary pairs plus an optional fallback/boundary pair.
// The destination is unchanged on failure, and it never retains DMA pointers.
bool decode_metal_visibility_frame(const DmaTransfer* transfers,
                                   std::size_t transfer_count,
                                   std::size_t level_count,
                                   MetalVisibilityFrame* out);

}  // namespace metal_renderer
