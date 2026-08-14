#include "game/graphics/pipelines/metal/metal_jak2_warp_renderer.h"

#include <limits>
#include <mutex>
#include <stdexcept>

#include "game/graphics/pipelines/metal/metal_jak2_gmerc_warp_bucket317_plan.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

namespace metal_renderer {

Jak2WarpSnapshotPublisher::Jak2WarpSnapshotPublisher(TexturePool* texture_pool)
    : m_texture_pool(texture_pool) {}

Jak2WarpSnapshotPublisher::~Jak2WarpSnapshotPublisher() {
  reset();
}

id<MTLTexture> Jak2WarpSnapshotPublisher::make_candidate(MetalFrameContext& ctx,
                                                         bool* replacement) const {
  if (!replacement || !m_texture_pool || !ctx.cmds || !ctx.enc || !ctx.game_color ||
      !ctx.game_depth) {
    return nil;
  }
  const NSUInteger width = ctx.game_color.width;
  const NSUInteger height = ctx.game_color.height;
  if (!width || !height || width > std::numeric_limits<u16>::max() ||
      height > std::numeric_limits<u16>::max()) {
    return nil;
  }
  *replacement = !m_snapshot || m_snapshot.device != ctx.game_color.device ||
                 m_snapshot.width != width || m_snapshot.height != height ||
                 m_snapshot.pixelFormat != ctx.game_color.pixelFormat;
  if (!*replacement) {
    return m_snapshot;
  }
  auto* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:ctx.game_color.pixelFormat
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModePrivate;
  return [ctx.game_color.device newTextureWithDescriptor:descriptor];
}

bool Jak2WarpSnapshotPublisher::publish_candidate(id<MTLTexture> candidate, bool replacement) {
  if (!candidate) {
    return false;
  }
  if (!replacement) {
    std::lock_guard<std::mutex> pool_lock(m_texture_pool->mutex());
    m_texture_pool->move_existing_to_vram(m_pool_texture, kJak2WarpTextureTbp);
    return true;
  }

  if (!m_texture_handle) {
    const u64 handle = metal_texture_register(candidate);
    if (!handle) {
      return false;
    }
    TextureInput input;
    input.debug_page_name = "PC-WARP";
    input.debug_name = "jak2-game-color-snapshot";
    input.w = static_cast<u16>(candidate.width);
    input.h = static_cast<u16>(candidate.height);
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
    if (!m_pool_texture) {
      metal_texture_release(handle);
      return false;
    }
    m_texture_handle = handle;
    m_snapshot = candidate;
    m_stats.allocations++;
    return true;
  }

  if (!m_pool_texture || !metal_texture_replace(m_texture_handle, candidate)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> pool_lock(m_texture_pool->mutex());
    m_texture_pool->update_gl_texture(m_pool_texture, static_cast<u32>(candidate.width),
                                      static_cast<u32>(candidate.height), m_texture_handle);
    m_texture_pool->move_existing_to_vram(m_pool_texture, kJak2WarpTextureTbp);
  }
  m_snapshot = candidate;
  m_stats.replacements++;
  return true;
}

bool Jak2WarpSnapshotPublisher::capture_and_publish(MetalSharedRenderState* render_state,
                                                    MetalFrameContext& ctx) {
  if (!render_state || render_state->version != GameVersion::Jak2 ||
      render_state->texture_pool != m_texture_pool) {
    m_stats.failures++;
    return false;
  }
  bool replacement = false;
  id<MTLTexture> candidate = make_candidate(ctx, &replacement);
  if (!candidate) {
    m_stats.failures++;
    return false;
  }

  // OpenGL publishes the completed framebuffer copy. Encode that copy before
  // exposing a new or resized texture through the shared TexturePool.
  ctx.resume_pass_with_framebuffer_copy(candidate);
  m_stats.copies++;
  if (!publish_candidate(candidate, replacement)) {
    m_stats.failures++;
    return false;
  }
  m_stats.publications++;
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

MetalJak2WarpBucketRenderer::MetalJak2WarpBucketRenderer(
    const std::string& name,
    int my_id,
    std::shared_ptr<MetalGeneric2> generic,
    TexturePool* texture_pool)
    : MetalBucketRenderer(name, my_id),
      m_generic(std::move(generic)),
      m_snapshot(texture_pool) {}

void MetalJak2WarpBucketRenderer::render(DmaFollower& dma,
                                         MetalSharedRenderState* render_state,
                                         MetalFrameContext& ctx) {
  if (!render_state || !render_state->jak2_gmerc_warp_bucket317_plan ||
      render_state->version != GameVersion::Jak2 ||
      render_state->jak2_gmerc_warp_bucket317_plan->bucket_id != static_cast<u32>(m_my_id)) {
    throw std::runtime_error("Jak 2 GMERC_WARP renderer has no matching copied plan");
  }
  if (render_state->host_bucket_callback) {
    render_state->host_bucket_callback(render_state->host_bucket_context,
                                       static_cast<u32>(m_my_id));
  }
  const auto& plan = *render_state->jak2_gmerc_warp_bucket317_plan;
  if (plan.variant == Jak2GmercWarpBucket317Variant::Fragments &&
      !m_snapshot.capture_and_publish(render_state, ctx)) {
    throw std::runtime_error("Jak 2 GMERC_WARP framebuffer snapshot failed");
  }
  m_generic_stats = MetalGeneric2::Stats();
  m_generic->render_in_mode(dma, render_state, ctx, MetalGeneric2::Mode::WARP,
                            &m_generic_stats);
}

}  // namespace metal_renderer
