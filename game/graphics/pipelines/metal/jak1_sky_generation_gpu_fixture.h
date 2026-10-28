#pragma once

#include <cstdint>
#include <vector>

#include "game/graphics/pipelines/metal/metal_immersive_camera.h"

// Keep the translated GOAL headers in a C++ translation unit: their Ptr template conflicts
// with the unrelated Ptr typedef imported by Apple's Objective-C Metal headers.
namespace jak1_sky_gpu_fixture {
inline constexpr std::uint32_t kMemSize = 0x80000;
inline constexpr std::uint32_t kGiftag = 0x4000;
struct Generated {
  bool ok = false;
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
};
metal_renderer::ImmersivePacketCamera packet();
void setup_memory();
void setup_caches();
void configure_camera(const metal_renderer::ImmersivePacketCamera& camera);
void init_registers();
Generated emit_bipyramid(std::uint32_t out_addr);
std::vector<std::uint8_t> camera_snapshot();
}  // namespace jak1_sky_gpu_fixture
