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

constexpr std::array<std::size_t, kJak2ClutBlendSlotCount>
    kJak2PrisonClutAnimatedTextureSlots = {4, 5, 7, 8, 9, 10};
constexpr std::array<std::size_t, kJak2ClutBlendSlotCount> kJak2PrisonClutPlanTbpIndices = {
    0, 1, 3, 4, 5, 6};

class Jak2PrisonClutExecutor {
 public:
  struct PreparedOutput {
    u32 destination_tbp = 0;
    u16 width = 0;
    u16 height = 0;
    std::vector<u8> rgba;
  };

  using Prepared = std::array<PreparedOutput, kJak2ClutBlendSlotCount>;

  struct Stats {
    u64 preparations = 0;
    u64 publications = 0;
    std::array<u32, kJak2ClutBlendSlotCount> destination_tbps = {};
    std::array<u64, kJak2ClutBlendSlotCount> texture_handles = {};
  };

  Jak2PrisonClutExecutor(id<MTLDevice> device, id<MTLCommandQueue> queue);
  ~Jak2PrisonClutExecutor();

  Jak2PrisonClutExecutor(const Jak2PrisonClutExecutor&) = delete;
  Jak2PrisonClutExecutor& operator=(const Jak2PrisonClutExecutor&) = delete;

  bool prepare(const Jak2PrisPrisonJakAnimatorPlan& plan,
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
  std::array<u64, kJak2ClutBlendSlotCount> m_texture_handles = {};
  std::vector<u64> m_animated_texture_slots;
  bool m_slot_contract_valid = false;
  Stats m_stats;
  std::string m_error;
};

}  // namespace metal_renderer
