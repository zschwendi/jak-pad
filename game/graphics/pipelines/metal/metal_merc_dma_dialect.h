#pragma once

#include <cstddef>

#include "common/versions/versions.h"

namespace metal_merc_dma {

struct Dialect {
  std::size_t gs_setup_bytes;
  int model_patch_count;
  std::size_t water_slot_bytes;

  constexpr std::size_t matrix_slots_offset() const {
    return 128 + 7 * 16 + water_slot_bytes;
  }
};

constexpr Dialect dialect(GameVersion version) {
  return version == GameVersion::Jak1 ? Dialect{32, 2, 16} : Dialect{48, 1, 0};
}

}  // namespace metal_merc_dma
