#include "game/graphics/pipelines/metal/metal_jak2_raw_image_upload_executor.h"

#include <utility>

#include "game/graphics/pipelines/metal/metal_pool_texture.h"
#include "game/graphics/texture/TexturePool.h"

namespace metal_renderer {

Jak2RawImageUploadExecutor::Jak2RawImageUploadExecutor(id<MTLDevice> device,
                                                       id<MTLCommandQueue> queue,
                                                       TexturePool* pool)
    : m_device(device), m_queue(queue), m_pool(pool) {}

Jak2RawImageUploadExecutor::~Jak2RawImageUploadExecutor() = default;

bool Jak2RawImageUploadExecutor::fail(std::string error) {
  if (!m_failed) {
    m_failed = true;
    m_last_error = std::move(error);
  }
  return false;
}

bool Jak2RawImageUploadExecutor::execute(const Jak2RawImageUploadPlan& plan) {
  const std::size_t expected_pixels =
      static_cast<std::size_t>(kJak2RawImageWidth) * kJak2RawImageHeight;
  if (m_failed) {
    return false;
  }
  if (m_detached) {
    return fail("executor is detached");
  }
  if (!m_device || !m_queue || !m_pool) {
    return fail("Metal executor dependencies are unavailable");
  }
  if (!plan.present || plan.width != kJak2RawImageWidth ||
      plan.height != kJak2RawImageHeight || plan.destination != kJak2RawImageDestination ||
      plan.format != kJak2RawImagePsmct32 || plan.force_to_gpu != 1 ||
      plan.rgba.size() != expected_pixels) {
    return fail("raw-image plan shape is invalid");
  }

  if (!m_texture) {
    m_texture = std::make_unique<MetalPoolTexture>(
        m_device, m_queue, m_pool, kJak2RawImageWidth, kJak2RawImageHeight,
        kJak2RawImageDestination, "jak2-draw-raw-image");
  }
  if (!m_texture->publish(plan.rgba.data(), plan.rgba.size())) {
    return fail("raw-image texture publication failed");
  }

  m_stats.publications++;
  m_stats.texture_handle = m_texture->handle();
  m_stats.pixel_count = plan.rgba.size();
  return true;
}

void Jak2RawImageUploadExecutor::detach_pool() {
  if (m_texture) {
    m_texture->detach_pool();
  }
  m_pool = nullptr;
  m_detached = true;
}

}  // namespace metal_renderer
