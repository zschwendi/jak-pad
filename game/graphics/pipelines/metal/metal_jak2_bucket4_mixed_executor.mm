#include "game/graphics/pipelines/metal/metal_jak2_bucket4_mixed_executor.h"

#include <array>
#include <cstring>
#include <utility>

#include "game/graphics/pipelines/metal/metal_jak2_fog_texture_convert.h"
#include "game/graphics/pipelines/metal/metal_pool_texture.h"
#include "game/graphics/texture/TexturePool.h"

namespace metal_renderer {
namespace {

constexpr u32 kCloudWidth = kJak2Opcode41CloudSize;
constexpr u32 kCloudHeight = kJak2Opcode41CloudSize;
constexpr u32 kFogWidth = kJak2FogIndexedPixelCount;
constexpr u32 kFogHeight = 1;

}  // namespace

Jak2Bucket4MixedExecutor::Jak2Bucket4MixedExecutor(id<MTLDevice> device,
                                                   id<MTLCommandQueue> queue,
                                                   TexturePool* pool)
    : m_device(device), m_queue(queue), m_pool(pool) {}

Jak2Bucket4MixedExecutor::~Jak2Bucket4MixedExecutor() = default;

bool Jak2Bucket4MixedExecutor::fail(std::string error) {
  if (!m_failed) {
    m_failed = true;
    m_stats.failures++;
    m_last_error = std::move(error);
  }
  return false;
}

bool Jak2Bucket4MixedExecutor::plan_shape_is_valid(const Jak2Bucket4MixedPlan& plan) const {
  if (!m_pool || plan.sky.cloud_destination < 0 || plan.fog.width != kFogWidth ||
      plan.fog.height != kFogHeight || plan.fog.format != 19 || plan.fog.force_to_gpu != 1 ||
      plan.erase.width != 16 || plan.erase.height != 16 ||
      plan.erase.destination != plan.fog.clut_destination) {
    return false;
  }

  const auto slot_count = m_pool->all_textures().size();
  const auto cloud_destination = static_cast<u32>(plan.sky.cloud_destination);
  return cloud_destination < slot_count && plan.fog.destination < slot_count &&
         plan.erase.destination < slot_count && cloud_destination != plan.fog.destination &&
         cloud_destination != plan.erase.destination &&
         plan.fog.destination != plan.erase.destination;
}

bool Jak2Bucket4MixedExecutor::execute(const Jak2Bucket4MixedPlan& plan) {
  if (m_failed) {
    return false;
  }
  if (m_detached) {
    return fail("executor is detached");
  }
  if (!m_device || !m_queue || !m_pool) {
    return fail("Metal executor dependencies are unavailable");
  }
  if (!plan_shape_is_valid(plan)) {
    return fail("mixed plan texture shape or destinations are invalid");
  }

  const auto cloud_destination = static_cast<u32>(plan.sky.cloud_destination);
  if (m_destinations_bound &&
      (cloud_destination != m_cloud_destination || plan.fog.destination != m_fog_destination)) {
    return fail("mixed plan destinations changed across frames");
  }

  Jak2Opcode41CloudInput cloud_input;
  cloud_input.cloud_min = plan.sky.cloud_min;
  cloud_input.cloud_max = plan.sky.cloud_max;
  for (std::size_t i = 0; i < kJak2Opcode41CloudLayerCount; ++i) {
    cloud_input.times[i] = plan.sky.times[i + 1];
    cloud_input.max_times[i] = plan.sky.max_times[i];
    cloud_input.scales[i] = plan.sky.scales[i];
  }
  if (!m_cloud_cpu.generate(cloud_input)) {
    return fail("opcode-41 cloud generation failed");
  }

  std::array<u32, kJak2FogPsmct32ClutEntryCount> aligned_clut = {};
  static_assert(sizeof(aligned_clut) == kJak2Bucket4ClutBytes);
  std::memcpy(aligned_clut.data(), plan.fog.clut.data(), plan.fog.clut.size());
  const auto fog_rgba = convert_jak2_fog_psmt8_to_rgba(
      plan.fog.indices.data(), plan.fog.indices.size(), aligned_clut.data(), aligned_clut.size());
  if (!fog_rgba) {
    return fail("fog CLUT conversion failed");
  }

  if (!m_destinations_bound) {
    m_cloud_destination = cloud_destination;
    m_fog_destination = plan.fog.destination;
    m_cloud_publication =
        std::make_unique<MetalPoolTexture>(m_device, m_queue, m_pool, kCloudWidth, kCloudHeight,
                                           m_cloud_destination, "jak2-opcode41-cloud");
    m_fog_publication = std::make_unique<MetalPoolTexture>(
        m_device, m_queue, m_pool, kFogWidth, kFogHeight, m_fog_destination, "jak2-opcode16-fog");
    m_destinations_bound = true;
  }

  if (!m_cloud_publication->publish(m_cloud_cpu.rgba().data(), m_cloud_cpu.rgba().size())) {
    return fail("cloud texture publication failed");
  }
  m_stats.cloud_publications++;

  // Opcode 14's validated erase is not published: opcode 15 immediately replaces
  // the same GS destination with the CLUT consumed by this fog conversion.
  if (!m_fog_publication->publish(fog_rgba->data(), fog_rgba->size())) {
    return fail("fog texture publication failed");
  }
  m_stats.fog_publications++;
  m_stats.frames++;
  return true;
}

void Jak2Bucket4MixedExecutor::detach_pool() {
  if (m_cloud_publication) {
    m_cloud_publication->detach_pool();
  }
  if (m_fog_publication) {
    m_fog_publication->detach_pool();
  }
  m_pool = nullptr;
  m_detached = true;
}

}  // namespace metal_renderer
