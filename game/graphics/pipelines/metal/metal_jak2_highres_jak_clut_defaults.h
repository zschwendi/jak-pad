#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"
#include "game/graphics/pipelines/metal/metal_jak2_prison_clut_cpu.h"
#include "game/graphics/texture/TextureID.h"
#import <Metal/Metal.h>

struct GpuTexture;
class TexturePool;

namespace tfrag3 {
struct Level;
}

namespace metal_renderer {

constexpr std::array<std::size_t, kJak2HighresJakClutBlendSlotCount>
    kJak2HighresJakDefaultAnimatedTextureSlots = {13, 11, 8, 9, 10};

class Jak2HighresJakClutDefaults {
 public:
  struct PreparedOutput {
    u32 destination_tbp = 0;
    u16 width = 0;
    u16 height = 0;
    std::vector<u8> rgba;
  };

  using Prepared = std::array<PreparedOutput, kJak2HighresJakClutBlendSlotCount>;

  struct Stats {
    u64 preparations = 0;
    u64 publications = 0;
    u16 last_opcode = 0;
    std::array<u32, kJak2HighresJakClutBlendSlotCount> destination_tbps = {};
    std::array<u64, kJak2HighresJakClutBlendSlotCount> texture_handles = {};
  };

  Jak2HighresJakClutDefaults(id<MTLDevice> device,
                             id<MTLCommandQueue> queue,
                             TexturePool* texture_pool);
  ~Jak2HighresJakClutDefaults();

  Jak2HighresJakClutDefaults(const Jak2HighresJakClutDefaults&) = delete;
  Jak2HighresJakClutDefaults& operator=(const Jak2HighresJakClutDefaults&) = delete;

  bool initialize(const tfrag3::Level& common_level);
  bool prepare(const Jak2PrisPrisonJakAnimatorPlan& plan,
               const tfrag3::Level& common_level,
               Prepared* out);
  bool publish(const Prepared& prepared, u16 opcode);
  void merge_animated_texture_slots(std::span<u64> slots) const;
  void detach_pool();

  const std::vector<u64>& animated_texture_slots() const { return m_animated_texture_slots; }
  const Stats& stats() const { return m_stats; }
  const char* last_error() const { return m_error.c_str(); }

 private:
  struct GroupState {
    std::array<u64, kJak2HighresJakClutBlendSlotCount> texture_handles = {};
    std::array<GpuTexture*, kJak2HighresJakClutBlendSlotCount> pool_textures = {};
    std::array<PcTextureId, kJak2HighresJakClutBlendSlotCount> pool_texture_ids = {};
  };

  bool fail(const char* message);
  GroupState* group_for_opcode(u16 opcode);
  void release_textures();

  id<MTLDevice> m_device = nil;
  id<MTLCommandQueue> m_queue = nil;
  TexturePool* m_texture_pool = nullptr;
  GroupState m_oracle_group;
  GroupState m_nest_group;
  std::vector<u64> m_animated_texture_slots;
  bool m_slot_contract_valid = false;
  Stats m_stats;
  std::string m_error;
};

}  // namespace metal_renderer
