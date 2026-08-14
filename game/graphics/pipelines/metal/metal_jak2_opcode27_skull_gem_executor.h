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
constexpr std::size_t kJak2SecurityEnvironmentAnimatedTextureSlot = 20;
constexpr std::size_t kJak2SecurityDotAnimatedTextureSlot = 21;

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
    u64 security_preparations = 0;
    u64 security_publications = 0;
  };

  struct PreparedSecurityOutput {
    u32 destination_tbp = 0;
    u16 width = 0;
    u16 height = 0;
    std::vector<u8> rgba;
  };

  struct PreparedSecurity {
    PreparedSecurityOutput environment;
    PreparedSecurityOutput dot;
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
  bool prepare_security(const Jak2Opcode30SecurityPlan& plan,
                        const tfrag3::Level& common_level,
                        const tfrag3::Level& ctywide_level,
                        PreparedSecurity* out);
  bool publish_security(const PreparedSecurity& prepared);
  bool prepare_security_environment(const Jak2Opcode30SecurityEnvironmentPlan& plan,
                                    const tfrag3::Level& ctywide_level,
                                    PreparedSecurityOutput* out);
  bool publish_security_environment(const PreparedSecurityOutput& prepared);
  void detach_pool();

  const std::vector<u64>& animated_texture_slots() const { return m_animated_texture_slots; }
  const Stats& stats() const { return m_stats; }
  const char* last_error() const { return m_error.c_str(); }

 private:
  bool fail(const char* message);
  bool publish_security_output(const PreparedSecurityOutput& prepared,
                               std::size_t slot,
                               const char* label,
                               std::unique_ptr<MetalPoolTexture>* publication);

  id<MTLDevice> m_device = nil;
  id<MTLCommandQueue> m_queue = nil;
  TexturePool* m_pool = nullptr;
  std::unique_ptr<MetalPoolTexture> m_publication;
  std::unique_ptr<MetalPoolTexture> m_security_environment_publication;
  std::unique_ptr<MetalPoolTexture> m_security_dot_publication;
  std::vector<u64> m_animated_texture_slots;
  bool m_slot_contract_valid = false;
  Stats m_stats;
  std::string m_error;
};

}  // namespace metal_renderer
