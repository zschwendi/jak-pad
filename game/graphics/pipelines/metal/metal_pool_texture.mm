#include "game/graphics/pipelines/metal/metal_pool_texture.h"

#include <limits>
#include <mutex>
#include <utility>

#include "common/util/Assert.h"

#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

namespace {

class RegistryHandleGuard {
 public:
  explicit RegistryHandleGuard(u64 handle) : m_handle(handle) {}
  ~RegistryHandleGuard() {
    if (m_handle) {
      metal_texture_release(m_handle);
    }
  }

  void disarm() { m_handle = 0; }

 private:
  u64 m_handle;
};

}  // namespace

MetalPoolTexture::MetalPoolTexture(id<MTLDevice> device,
                                   id<MTLCommandQueue> queue,
                                   TexturePool* pool,
                                   u32 width,
                                   u32 height,
                                   u32 vram_slot,
                                   std::string debug_name)
    : m_device(device),
      m_queue(queue),
      m_pool(pool),
      m_width(width),
      m_height(height),
      m_vram_slot(vram_slot),
      m_debug_name(std::move(debug_name)) {}

MetalPoolTexture::~MetalPoolTexture() {
  ASSERT_MSG(!m_pool_texture, "MetalPoolTexture must detach from its live TexturePool");
  if (m_handle) {
    metal_texture_release(m_handle);
  }
}

void MetalPoolTexture::detach_pool() {
  if (m_pool && m_pool_texture && m_handle) {
    std::lock_guard<std::mutex> pool_lock(m_pool->mutex());
    m_pool->unload_texture(m_texture_id, m_handle);
  }
  m_pool_texture = nullptr;
  m_pool = nullptr;
}

bool MetalPoolTexture::publish(const u32* rgba, std::size_t pixel_count) {
  return publish_at(rgba, pixel_count, m_vram_slot);
}

bool MetalPoolTexture::publish_at(const u32* rgba,
                                  std::size_t pixel_count,
                                  u32 vram_slot) {
  const u64 expected_pixels = static_cast<u64>(m_width) * m_height;
  if (!m_device || !m_queue || !m_pool || !rgba || m_width == 0 || m_height == 0 ||
      m_width > std::numeric_limits<u16>::max() ||
      m_height > std::numeric_limits<u16>::max() ||
      vram_slot >= m_pool->all_textures().size() ||
      expected_pixels > std::numeric_limits<std::size_t>::max() ||
      pixel_count != static_cast<std::size_t>(expected_pixels)) {
    return false;
  }

  if (!m_handle) {
    TextureInput input;
    input.debug_page_name = "PC-ANIM";
    input.debug_name = m_debug_name;
    input.common = false;
    input.src_data = nullptr;
    input.w = static_cast<u16>(m_width);
    input.h = static_cast<u16>(m_height);
    const u64 handle = metal_upload_texture_rgba8(
        m_device, m_queue, reinterpret_cast<const u8*>(rgba), m_width, m_height);
    if (!handle) {
      return false;
    }
    RegistryHandleGuard handle_guard(handle);
    std::lock_guard<std::mutex> pool_lock(m_pool->mutex());
    input.id = m_pool->allocate_pc_port_texture();
    input.gpu_texture = handle;
    GpuTexture* pool_texture = m_pool->give_texture_and_load_to_vram(input, vram_slot);
    m_texture_id = input.id;
    m_handle = handle;
    m_pool_texture = pool_texture;
    m_vram_slot = vram_slot;
    handle_guard.disarm();
  } else {
    if (!metal_update_texture_rgba8(m_handle, m_queue, reinterpret_cast<const u8*>(rgba),
                                    m_width, m_height)) {
      return false;
    }
    std::lock_guard<std::mutex> pool_lock(m_pool->mutex());
    m_pool->move_existing_to_vram(m_pool_texture, vram_slot);
    m_vram_slot = vram_slot;
  }

  m_publications++;
  return true;
}
