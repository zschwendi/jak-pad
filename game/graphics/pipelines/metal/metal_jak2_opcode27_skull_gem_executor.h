#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "game/graphics/pipelines/metal/metal_jak2_opcode27_skull_gem_cpu.h"

#import <Metal/Metal.h>

class MetalPoolTexture;
class TexturePool;
namespace tfrag3 {
struct Level;
}

namespace metal_renderer {

constexpr std::size_t kJak2SkullGemAnimatedTextureSlot = 14;

class Jak2Opcode27SkullGemExecutor {
 public:
  struct Prepared {
    u32 destination_tbp = 0;
    Jak2Opcode27SkullGemRgba rgba = {};
  };

  struct Stats {
    u64 preparations = 0;
    u64 publications = 0;
    u32 destination_tbp = 0;
    u64 texture_handle = 0;
  };

  Jak2Opcode27SkullGemExecutor(id<MTLDevice> device,
                               id<MTLCommandQueue> queue,
                               TexturePool* pool);
  ~Jak2Opcode27SkullGemExecutor();

  Jak2Opcode27SkullGemExecutor(const Jak2Opcode27SkullGemExecutor&) = delete;
  Jak2Opcode27SkullGemExecutor& operator=(const Jak2Opcode27SkullGemExecutor&) = delete;

  bool prepare(const Jak2Opcode27SkullGemPlan& plan,
               const tfrag3::Level& common_level,
               Prepared* out);
  bool publish(const Prepared& prepared);
  void detach_pool();

  const std::vector<u64>& animated_texture_slots() const { return m_animated_texture_slots; }
  const Stats& stats() const { return m_stats; }
  const char* last_error() const { return m_error.c_str(); }

 private:
  bool fail(const char* message);

  id<MTLDevice> m_device = nil;
  id<MTLCommandQueue> m_queue = nil;
  TexturePool* m_pool = nullptr;
  std::unique_ptr<MetalPoolTexture> m_publication;
  std::vector<u64> m_animated_texture_slots;
  bool m_slot_contract_valid = false;
  Stats m_stats;
  std::string m_error;
};

}  // namespace metal_renderer
