#include "game/graphics/pipelines/metal/metal_jak2_warp_snapshot.h"

#include <limits>
#include <mutex>

#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

namespace metal_renderer {

Jak2WarpSnapshotPublisher::Jak2WarpSnapshotPublisher(TexturePool* texture_pool)
    : m_texture_pool(texture_pool) {}

Jak2WarpSnapshotPublisher::~Jak2WarpSnapshotPublisher() {
  reset();
}

bool Jak2WarpSnapshotPublisher::ensure_snapshot(MetalFrameContext& ctx) {
  if (!m_texture_pool || !ctx.cmds || !ctx.enc || !ctx.game_color || !ctx.game_depth) {
    return false;
  }

  const NSUInteger width = ctx.game_color.width;
  const NSUInteger height = ctx.game_color.height;
  if (!width || !height || width > std::numeric_limits<u16>::max() ||
      height > std::numeric_limits<u16>::max()) {
    return false;
  }

  const bool needs_texture =
      !m_snapshot || m_snapshot.device != ctx.game_color.device || m_snapshot.width != width ||
      m_snapshot.height != height || m_snapshot.pixelFormat != ctx.game_color.pixelFormat;
  if (!needs_texture) {
    std::lock_guard<std::mutex> pool_lock(m_texture_pool->mutex());
    m_texture_pool->move_existing_to_vram(m_pool_texture, kJak2WarpTextureTbp);
    return true;
  }

  auto* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:ctx.game_color.pixelFormat
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModePrivate;
  id<MTLTexture> replacement = [ctx.game_color.device newTextureWithDescriptor:descriptor];
  if (!replacement) {
    return false;
  }

  if (!m_texture_handle) {
    const u64 handle = metal_texture_register(replacement);
    if (!handle) {
      return false;
    }

    TextureInput input;
    input.debug_page_name = "PC-WARP";
    input.debug_name = "jak2-game-color-snapshot";
    input.w = static_cast<u16>(width);
    input.h = static_cast<u16>(height);
    input.gpu_texture = handle;
    {
      std::lock_guard<std::mutex> pool_lock(m_texture_pool->mutex());
      if (!m_texture_id_allocated) {
        m_texture_id = m_texture_pool->allocate_pc_port_texture(GameVersion::Jak2);
        m_texture_id_allocated = true;
      }
      input.id = m_texture_id;
      m_pool_texture =
          m_texture_pool->give_texture_and_load_to_vram(input, kJak2WarpTextureTbp);
    }
    m_texture_handle = handle;
    m_snapshot = replacement;
    m_stats.allocations++;
    return true;
  }

  if (!m_pool_texture || !metal_texture_replace(m_texture_handle, replacement)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> pool_lock(m_texture_pool->mutex());
    m_texture_pool->update_gl_texture(m_pool_texture, static_cast<u32>(width),
                                      static_cast<u32>(height), m_texture_handle);
    m_texture_pool->move_existing_to_vram(m_pool_texture, kJak2WarpTextureTbp);
  }
  m_snapshot = replacement;
  m_stats.replacements++;
  return true;
}

bool Jak2WarpSnapshotPublisher::publish(MetalSharedRenderState* render_state,
                                        MetalFrameContext& ctx) {
  if (!render_state || render_state->version != GameVersion::Jak2 ||
      render_state->texture_pool != m_texture_pool || !ensure_snapshot(ctx)) {
    m_stats.failures++;
    return false;
  }

  ctx.resume_pass_with_framebuffer_copy(m_snapshot);
  m_stats.publications++;
  m_stats.copies++;
  return true;
}

void Jak2WarpSnapshotPublisher::reset() {
  if (m_texture_handle) {
    if (m_texture_pool && m_pool_texture) {
      std::lock_guard<std::mutex> pool_lock(m_texture_pool->mutex());
      m_texture_pool->unload_texture(m_texture_id, m_texture_handle);
    }
    metal_texture_release(m_texture_handle);
  }
  m_snapshot = nil;
  m_texture_handle = 0;
  m_pool_texture = nullptr;
}

}  // namespace metal_renderer
