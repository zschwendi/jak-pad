#include "game/graphics/pipelines/metal/metal_jak2_blit_display_renderer.h"

#include <cstdlib>
#include <cstring>
#include <limits>

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/texture/TexturePool.h"

#include "fmt/format.h"

namespace metal_renderer {
namespace {

void begin_game_pass(MetalFrameContext& ctx, MTLLoadAction color_load_action) {
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = ctx.game_color;
  pass.colorAttachments[0].slice = ctx.game_color_slice;
  pass.colorAttachments[0].loadAction = color_load_action;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  pass.depthAttachment.texture = ctx.game_depth;
  pass.depthAttachment.slice = ctx.game_depth_slice;
  pass.depthAttachment.loadAction =
      color_load_action == MTLLoadActionClear ? MTLLoadActionClear : MTLLoadActionLoad;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = 0.0;
  pass.stencilAttachment.texture = ctx.game_depth;
  pass.stencilAttachment.slice = ctx.game_depth_slice;
  pass.stencilAttachment.loadAction =
      color_load_action == MTLLoadActionClear ? MTLLoadActionClear : MTLLoadActionLoad;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;
  pass.stencilAttachment.clearStencil = 0;
  ctx.enc = [ctx.cmds renderCommandEncoderWithDescriptor:pass];
  [ctx.enc setCullMode:MTLCullModeNone];
  [ctx.enc setViewport:ctx.game_viewport];
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

namespace {

// DIAGNOSTIC ONLY (not a fix): bounded numeric snapshot/copy-back metadata. No pixel readback.
// copyFromTexture copies texel rows in memory order (origin 0,0 -> 0,0); it neither flips nor
// reveals the orientation of the pixels already in game_color.
bool g_jak2_blit_copy_diag = false;

// DIAGNOSTIC ONLY: default-off PC-BLIT logging. Only GOALPAD_JAK2_DEBUG_LOG_PC_BLIT=1 enables it;
// unset, empty, "0", or any other value disables it.
bool jak2_pc_blit_debug_log_value_enabled(const char* value) {
  return value && std::strcmp(value, "1") == 0;
}

bool jak2_pc_blit_debug_log_enabled() {
  static const bool enabled =
      jak2_pc_blit_debug_log_value_enabled(std::getenv("GOALPAD_JAK2_DEBUG_LOG_PC_BLIT"));
  return enabled;
}

void log_jak2_blit_copy_event(const char* event,
                              int command,
                              bool snapshot_requested,
                              bool copy_back_requested,
                              id<MTLTexture> src,
                              NSUInteger src_slice,
                              id<MTLTexture> dst,
                              NSUInteger dst_slice) {
  if (!jak2_pc_blit_debug_log_enabled() || !g_jak2_blit_copy_diag) {
    return;
  }
  // <=6 distinct signatures per process, plus 2 fixed-index heartbeats.
  static u64 s_sigs[6] = {};
  static int s_sig_count = 0;
  static u64 s_events = 0;
  const u64 event_index = ++s_events;
  const u64 sw = src ? src.width : 0, sh = src ? src.height : 0;
  const u64 dw = dst ? dst.width : 0, dh = dst ? dst.height : 0;
  u64 sig = 1469598103934665603ull;
  const auto mix = [&sig](u64 v) { sig = (sig ^ v) * 1099511628211ull; };
  for (const char* c = event; *c; ++c) {
    mix(static_cast<u8>(*c));
  }
  mix(static_cast<u64>(command) | (snapshot_requested ? 0x100u : 0u) |
      (copy_back_requested ? 0x200u : 0u));
  mix(sw | (sh << 16) | (static_cast<u64>(src_slice) << 32));
  mix(dw | (dh << 16) | (static_cast<u64>(dst_slice) << 32));
  bool seen = false;
  for (int i = 0; i < s_sig_count; ++i) {
    seen |= s_sigs[i] == sig;
  }
  const bool heartbeat = event_index == 240 || event_index == 1200;
  const bool emit_distinct = !seen && s_sig_count < 6;
  if (emit_distinct) {
    s_sigs[s_sig_count++] = sig;
  }
  if (!emit_distinct && !heartbeat) {
    return;
  }
  lg::info(
      "GOALPAD_JAK2_BLIT_COPY_DIAG reason={} event_index={} event={} command={} "
      "snapshot_requested={} copy_back_requested={} src={}x{} src_slice={} dst={}x{} "
      "dst_slice={} sourceOrigin=(0,0,0) destinationOrigin=(0,0,0) copied={}x{} "
      "(copyFromTexture preserves row order; pixel orientation not measurable from metadata)",
      emit_distinct ? "distinct" : "heartbeat", event_index, event, command,
      snapshot_requested ? 1 : 0, copy_back_requested ? 1 : 0, sw, sh, (u64)src_slice, dw, dh,
      (u64)dst_slice, sw, sh);
}

}  // namespace

void Jak2BlitDisplayExecutor::restart_with_clear(MetalFrameContext& ctx, bool copy_snapshot) {
  ASSERT(ctx.enc && ctx.cmds && ctx.game_color && ctx.game_depth);
  [ctx.enc endEncoding];
  if (copy_snapshot) {
    ASSERT(m_snapshot && m_snapshot.width == ctx.game_color.width &&
           m_snapshot.height == ctx.game_color.height);
    id<MTLBlitCommandEncoder> blit = [ctx.cmds blitCommandEncoder];
    [blit copyFromTexture:ctx.game_color
              sourceSlice:ctx.game_color_slice
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(ctx.game_color.width, ctx.game_color.height, 1)
                toTexture:m_snapshot
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    log_jak2_blit_copy_event("snapshot_copy", -1, true, false, ctx.game_color,
                             ctx.game_color_slice, m_snapshot, 0);
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
       destinationSlice:ctx.game_color_slice
       destinationLevel:0
      destinationOrigin:MTLOriginMake(0, 0, 0)];
  [blit endEncoding];
  log_jak2_blit_copy_event("copy_back_restore", -1, false, true, m_snapshot, 0, ctx.game_color,
                           ctx.game_color_slice);
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
  g_jak2_blit_copy_diag = jak2_pc_blit_debug_log_enabled() &&
                         render_state->detailed_frame_stats_enabled &&
                         !render_state->secondary_view &&
                         render_state->version == GameVersion::Jak2;
  log_jak2_blit_copy_event("execute_plan", static_cast<int>(plan.command), snapshot,
                           m_stats.copy_back_requested, ctx.game_color, ctx.game_color_slice,
                           m_snapshot, 0);
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
    log_jak2_blit_copy_event("copy_back_skipped_mismatch", -1, false, true, m_snapshot, 0,
                             ctx.game_color, ctx.game_color_slice);
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
