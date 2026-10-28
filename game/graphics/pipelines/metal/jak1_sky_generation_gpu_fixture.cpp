#include "game/graphics/pipelines/metal/jak1_sky_generation_fixture.h"
#include "game/graphics/pipelines/metal/jak1_sky_generation_gpu_fixture.h"

namespace jak1 {
Ptr<Symbol> intern_from_c(const char*) {
  return {};
}
}  // namespace jak1

namespace jak1_sky_gpu_fixture {
metal_renderer::ImmersivePacketCamera packet() { return jak1_sky_fixture::packet(); }
void setup_memory() { jak1_sky_fixture::setup_memory(); }
void setup_caches() { jak1_sky_fixture::setup_caches(); }
void configure_camera(const metal_renderer::ImmersivePacketCamera& camera) {
  jak1_sky_fixture::configure_camera(camera);
}
void init_registers() { jak1_sky_fixture::init_registers(); }
Generated emit_bipyramid(std::uint32_t out_addr) {
  const auto generated = jak1_sky_fixture::emit_bipyramid(out_addr);
  return {generated.ok, generated.begin, generated.end};
}
std::vector<std::uint8_t> camera_snapshot() { return jak1_sky_fixture::camera_snapshot(); }
}  // namespace jak1_sky_gpu_fixture
