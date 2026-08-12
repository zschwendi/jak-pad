#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"
#include "game/graphics/pipelines/metal/metal_jak2_prison_clut_cpu.h"

#import <Metal/Metal.h>

namespace tfrag3 {
struct Level;
}

namespace metal_renderer {

constexpr std::array<std::size_t, kJak2DarkJakClutBlendSlotCount>
    kJak2DarkJakClutAnimatedTextureSlots = {0, 1, 2, 3};

class Jak2DarkJakClutExecutor {
 public:
  struct PreparedOutput {
    u32 destination_tbp = 0;
    u16 width = 0;
    u16 height = 0;
    std::vector<u8> rgba;
  };

  using Prepared = std::array<PreparedOutput, kJak2DarkJakClutBlendSlotCount>;

  struct Stats {
    u64 preparations = 0;
    u64 publications = 0;
    std::array<u32, kJak2DarkJakClutBlendSlotCount> destination_tbps = {};
    std::array<u64, kJak2DarkJakClutBlendSlotCount> texture_handles = {};
  };

  Jak2DarkJakClutExecutor(id<MTLDevice> device, id<MTLCommandQueue> queue);
  ~Jak2DarkJakClutExecutor();

  Jak2DarkJakClutExecutor(const Jak2DarkJakClutExecutor&) = delete;
  Jak2DarkJakClutExecutor& operator=(const Jak2DarkJakClutExecutor&) = delete;

  bool prepare(const Jak2CommonPrisDarkJakAnimatorPlan& plan,
               const tfrag3::Level& common_level,
               Prepared* out);
  bool publish(const Prepared& prepared);
  void merge_animated_texture_slots(std::span<u64> slots) const;

  const std::vector<u64>& animated_texture_slots() const { return m_animated_texture_slots; }
  const Stats& stats() const { return m_stats; }
  const char* last_error() const { return m_error.c_str(); }

 private:
  bool fail(const char* message);
  void release_textures();

  id<MTLDevice> m_device = nil;
  id<MTLCommandQueue> m_queue = nil;
  std::array<u64, kJak2DarkJakClutBlendSlotCount> m_texture_handles = {};
  std::vector<u64> m_animated_texture_slots;
  bool m_slot_contract_valid = false;
  Stats m_stats;
  std::string m_error;
};

}  // namespace metal_renderer
