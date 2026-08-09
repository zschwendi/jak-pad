#include "game/graphics/pipelines/metal/metal_jak2_blit_display_renderer.h"

#include <limits>

#include "common/util/Assert.h"

#include "game/graphics/texture/TexturePool.h"

#include "fmt/format.h"

namespace metal_renderer {
namespace {

void begin_game_pass(MetalFrameContext& ctx, MTLLoadAction color_load_action) {
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = ctx.game_color;
  pass.colorAttachments[0].loadAction = color_load_action;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  pass.depthAttachment.texture = ctx.game_depth;
  pass.depthAttachment.loadAction =
      color_load_action == MTLLoadActionClear ? MTLLoadActionClear : MTLLoadActionLoad;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = 0.0;
  pass.stencilAttachment.texture = ctx.game_depth;
  pass.stencilAttachment.loadAction =
      color_load_action == MTLLoadActionClear ? MTLLoadActionClear : MTLLoadActionLoad;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;
  pass.stencilAttachment.clearStencil = 0;
  ctx.enc = [ctx.cmds renderCommandEncoderWithDescriptor:pass];
  [ctx.enc setCullMode:MTLCullModeNone];
}

}  // namespace

Jak2BlitDisplayExecutor::Jak2BlitDisplayExecutor(TexturePool* texture_pool)
    : m_texture_pool(texture_pool) {}

Jak2BlitDisplayExecutor::~Jak2BlitDisplayExecutor() {
  // MetalRenderer can outlive its TexturePool in the desktop shell. Releasing
  // the backend handle is safe in either destruction order; the pool's CPU
  // references disappear with the pool itself.
  if (m_texture_handle) {
    metal_texture_release(m_texture_handle);
  }
}

bool Jak2BlitDisplayExecutor::ensure_snapshot(MetalSharedRenderState* render_state,
                                              MetalFrameContext& ctx) {
  ASSERT(render_state && render_state->texture_pool == m_texture_pool);
  ASSERT(ctx.game_color && ctx.game_depth && ctx.cmds);
  const u32 width = static_cast<u32>(ctx.game_color.width);
  const u32 height = static_cast<u32>(ctx.game_color.height);
  if (!width || !height || width > std::numeric_limits<u16>::max() ||
      height > std::numeric_limits<u16>::max()) {
    return false;
  }
  if (m_snapshot && m_snapshot.width == width && m_snapshot.height == height) {
    std::lock_guard<std::mutex> pool_lock(m_texture_pool->mutex());
    m_texture_pool->move_existing_to_vram(m_pool_texture, kJak2BlitDisplayTbp);
    return true;
  }

  if (m_texture_handle) {
    {
      std::lock_guard<std::mutex> pool_lock(m_texture_pool->mutex());
      m_texture_pool->unload_texture(m_texture_id, m_texture_handle);
    }
    metal_texture_release(m_texture_handle);
    m_texture_handle = 0;
    m_pool_texture = nullptr;
  }

  auto* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:ctx.game_color.pixelFormat
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModePrivate;
  m_snapshot = [ctx.game_color.device newTextureWithDescriptor:descriptor];
  if (!m_snapshot) {
    return false;
  }
  m_texture_handle = metal_texture_register(m_snapshot);
  if (!m_texture_handle) {
    m_snapshot = nil;
    return false;
  }

  TextureInput input;
  input.gpu_texture = m_texture_handle;
  input.w = static_cast<u16>(width);
  input.h = static_cast<u16>(height);
  input.debug_page_name = "PC-BLIT";
  input.debug_name = "jak2-blit-display";
  {
    std::lock_guard<std::mutex> pool_lock(m_texture_pool->mutex());
    if (!m_texture_id_allocated) {
      m_texture_id = m_texture_pool->allocate_pc_port_texture(GameVersion::Jak2);
      m_texture_id_allocated = true;
    }
    input.id = m_texture_id;
    m_pool_texture = m_texture_pool->give_texture_and_load_to_vram(input, kJak2BlitDisplayTbp);
  }
  return true;
}

void Jak2BlitDisplayExecutor::restart_with_clear(MetalFrameContext& ctx, bool copy_snapshot) {
  ASSERT(ctx.enc && ctx.cmds && ctx.game_color && ctx.game_depth);
  [ctx.enc endEncoding];
  if (copy_snapshot) {
    ASSERT(m_snapshot && m_snapshot.width == ctx.game_color.width &&
           m_snapshot.height == ctx.game_color.height);
    id<MTLBlitCommandEncoder> blit = [ctx.cmds blitCommandEncoder];
    [blit copyFromTexture:ctx.game_color
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(ctx.game_color.width, ctx.game_color.height, 1)
                toTexture:m_snapshot
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
  }
  begin_game_pass(ctx, MTLLoadActionClear);
}

void Jak2BlitDisplayExecutor::restore_snapshot(MetalFrameContext& ctx) {
  ASSERT(ctx.enc && ctx.cmds && ctx.game_color && ctx.game_depth && m_snapshot);
  [ctx.enc endEncoding];
  id<MTLBlitCommandEncoder> blit = [ctx.cmds blitCommandEncoder];
  [blit copyFromTexture:m_snapshot
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(m_snapshot.width, m_snapshot.height, 1)
              toTexture:ctx.game_color
       destinationSlice:0
       destinationLevel:0
      destinationOrigin:MTLOriginMake(0, 0, 0)];
  [blit endEncoding];
  begin_game_pass(ctx, MTLLoadActionLoad);
}

bool Jak2BlitDisplayExecutor::execute(const Jak2BlitDisplayPlan& plan,
                                      MetalSharedRenderState* render_state,
                                      MetalFrameContext& ctx) {
  m_stats = {};
  m_stats.plan_valid = static_cast<bool>(plan);
  m_stats.transfer_count = plan.transfer_count;
  m_stats.unsupported_pc_port_count = plan.unsupported_pc_port_count;
  if (!plan || !render_state || !m_texture_pool || !ctx.enc || !ctx.cmds || !ctx.game_color ||
      !ctx.game_depth) {
    return false;
  }

  const bool snapshot = plan.command == Jak2BlitDisplayCommand::Snapshot ||
                        plan.command == Jak2BlitDisplayCommand::SnapshotThenCopyBack;
  m_stats.snapshot_requested = snapshot;
  m_stats.copy_back_requested = plan.command == Jak2BlitDisplayCommand::CopyBack ||
                                plan.command == Jak2BlitDisplayCommand::SnapshotThenCopyBack;
  if (snapshot && !ensure_snapshot(render_state, ctx)) {
    return false;
  }
  if (m_stats.copy_back_requested) {
    m_copy_back_pending = true;
  }

  // OpenGL BlitDisplays copies first and then clears color/depth/stencil. The
  // upcoming SKY_DRAW and PROGRESS buckets therefore see a clean target while
  // their texture lookup sees the ordered snapshot.
  restart_with_clear(ctx, snapshot);

  m_stats.texture_handle = m_texture_handle;
  m_stats.texture_tbp = m_texture_handle ? kJak2BlitDisplayTbp : 0;
  if (m_texture_handle) {
    const auto lookup = m_texture_pool->lookup(kJak2BlitDisplayTbp);
    m_stats.texture_lookup_hit = lookup && *lookup == m_texture_handle;
    m_stats.used_placeholder = lookup && *lookup == m_texture_pool->get_placeholder_texture();
  }
  return true;
}

void Jak2BlitDisplayExecutor::finish_frame(MetalFrameContext& ctx) {
  if (!m_copy_back_pending) {
    return;
  }
  m_copy_back_pending = false;
  if (!m_snapshot || !ctx.game_color || m_snapshot.width != ctx.game_color.width ||
      m_snapshot.height != ctx.game_color.height) {
    return;
  }
  restore_snapshot(ctx);
  m_stats.copy_back_performed = true;
}

}  // namespace metal_renderer

MetalJak2BlitDisplayRenderer::MetalJak2BlitDisplayRenderer(const std::string& name,
                                                           int my_id,
                                                           TexturePool* texture_pool)
    : MetalBucketRenderer(name, my_id), m_executor(texture_pool) {}

void MetalJak2BlitDisplayRenderer::render(DmaFollower& dma,
                                          MetalSharedRenderState* render_state,
                                          MetalFrameContext& ctx) {
  ASSERT(render_state->version == GameVersion::Jak2);
  metal_renderer::Jak2BlitDisplayPlanner planner;
  while (dma.current_tag_offset() != render_state->next_bucket) {
    const auto transfer = dma.read_and_advance();
    planner.observe(transfer.vif0(), transfer.vif1(), transfer.data, transfer.size_bytes);
  }
  ASSERT_MSG(planner.plan(),
             fmt::format("Metal Jak II BlitDisplays rejected bucket 3: {}",
                         metal_renderer::jak2_blit_display_plan_error_name(planner.plan().error)));
  ASSERT_MSG(m_executor.execute(planner.plan(), render_state, ctx),
             "Metal Jak II BlitDisplays execution failed");
}
