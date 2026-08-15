#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "game/graphics/pipelines/metal/metal_jak2_prison_clut_cpu.h"
#import <Metal/Metal.h>

namespace tfrag3 {
struct Level;
}

namespace metal_renderer {

constexpr std::array<std::size_t, kJak2HighresJakClutBlendSlotCount>
    kJak2HighresJakDefaultAnimatedTextureSlots = {13, 11, 8, 9, 10};

class Jak2HighresJakClutDefaults {
 public:
  Jak2HighresJakClutDefaults(id<MTLDevice> device, id<MTLCommandQueue> queue);
  ~Jak2HighresJakClutDefaults();

  Jak2HighresJakClutDefaults(const Jak2HighresJakClutDefaults&) = delete;
  Jak2HighresJakClutDefaults& operator=(const Jak2HighresJakClutDefaults&) = delete;

  bool initialize(const tfrag3::Level& common_level);
  void merge_animated_texture_slots(std::span<u64> slots) const;

  const std::vector<u64>& animated_texture_slots() const { return m_animated_texture_slots; }
  const char* last_error() const { return m_error.c_str(); }

 private:
  bool fail(const char* message);
  void release_textures();

  id<MTLDevice> m_device = nil;
  id<MTLCommandQueue> m_queue = nil;
  std::array<u64, kJak2HighresJakClutBlendSlotCount> m_texture_handles = {};
  std::vector<u64> m_animated_texture_slots;
  bool m_slot_contract_valid = false;
  std::string m_error;
};

}  // namespace metal_renderer
