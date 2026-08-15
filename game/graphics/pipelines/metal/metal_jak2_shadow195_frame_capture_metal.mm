#include "game/graphics/pipelines/metal/metal_jak2_shadow195_frame_capture_metal.h"

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_shadow2_renderer.h"

namespace metal_renderer {
namespace {

using Plane = Jak2Shadow195ReadbackPlaneLayout;
using ReadbackLayout = Jak2Shadow195ReadbackLayout;

void copy_plane(id<MTLBlitCommandEncoder> blit,
                id<MTLTexture> texture,
                NSUInteger slice,
                id<MTLBuffer> buffer,
                const Plane& plane,
                MTLBlitOption option) {
  [blit copyFromTexture:texture
            sourceSlice:slice
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(texture.width, texture.height, 1)
               toBuffer:buffer
      destinationOffset:static_cast<NSUInteger>(plane.offset)
 destinationBytesPerRow:static_cast<NSUInteger>(plane.row_bytes)
destinationBytesPerImage:static_cast<NSUInteger>(plane.row_bytes * texture.height)
                options:option];
}

void resume_game_pass(MetalFrameContext& context) {
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = context.game_color;
  pass.colorAttachments[0].slice = context.game_color_slice;
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.depthAttachment.texture = context.game_depth;
  pass.depthAttachment.slice = context.game_depth_slice;
  pass.depthAttachment.loadAction = MTLLoadActionLoad;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.stencilAttachment.texture = context.game_depth;
  pass.stencilAttachment.slice = context.game_depth_slice;
  pass.stencilAttachment.loadAction = MTLLoadActionLoad;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;
  context.enc = [context.cmds renderCommandEncoderWithDescriptor:pass];
  [context.enc setCullMode:MTLCullModeNone];
  [context.enc setViewport:context.game_viewport];
  if (context.game_scissor_valid) {
    [context.enc setScissorRect:context.game_scissor];
  }
  context.color_load_action = static_cast<u32>(MTLLoadActionLoad);
  context.color_store_action = static_cast<u32>(MTLStoreActionStore);
  context.depth_load_action = static_cast<u32>(MTLLoadActionLoad);
  context.depth_store_action = static_cast<u32>(MTLStoreActionStore);
  context.stencil_load_action = static_cast<u32>(MTLLoadActionLoad);
  context.stencil_store_action = static_cast<u32>(MTLStoreActionStore);
}

}  // namespace

Jak2Shadow195CaptureDispatchRoute route_jak2_shadow195_frame_capture(
    MetalBucketRenderer* configured,
    MetalJak2Shadow2Renderer* private_renderer,
    MetalBucketRenderer** selected) {
  if (!selected) {
    return Jak2Shadow195CaptureDispatchRoute::Unsupported;
  }
  *selected = configured;
  if (auto* deferred = dynamic_cast<MetalSkipRenderer*>(configured)) {
    if (!private_renderer) {
      return Jak2Shadow195CaptureDispatchRoute::Unsupported;
    }
    deferred->reset_last_skipped_bytes();
    *selected = private_renderer;
    return Jak2Shadow195CaptureDispatchRoute::DeferredPrivate;
  }
  if (dynamic_cast<MetalJak2Shadow2Renderer*>(configured)) {
    return Jak2Shadow195CaptureDispatchRoute::ProductionDirect;
  }
  return Jak2Shadow195CaptureDispatchRoute::Unsupported;
}

struct Jak2Shadow195AttachmentCapture::Impl {
  std::shared_ptr<Jak2Shadow195FrameCapture> capture;
  id<MTLBuffer> readback = nil;
  ReadbackLayout layout;
  u32 width = 0;
  u32 height = 0;
  bool volume_recorded = false;
  bool finished = false;
};

Jak2Shadow195AttachmentCapture::Jak2Shadow195AttachmentCapture(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {}

std::unique_ptr<Jak2Shadow195AttachmentCapture> Jak2Shadow195AttachmentCapture::begin(
    Jak2Shadow195FrameCapture* capture,
    MetalFrameContext& context,
    u64 view_id,
    bool external_target,
    double render_scale_x,
    double render_scale_y) {
  if (!capture || !context.cmds || !context.enc || !context.game_color ||
      !context.game_depth) {
    if (capture) {
      capture->fail(static_cast<u32>(Jak2Shadow195FrameCaptureFailure::InvalidContext));
    }
    return nullptr;
  }
  auto capture_owner = capture->weak_from_this().lock();
  if (!capture_owner) {
    capture->fail(static_cast<u32>(Jak2Shadow195FrameCaptureFailure::InvalidContext));
    return nullptr;
  }
  id<MTLTexture> color = context.game_color;
  id<MTLTexture> depth = context.game_depth;
  ReadbackLayout layout;
  if (!color.device || color.device != depth.device || color.pixelFormat != MTLPixelFormatBGRA8Unorm ||
      depth.pixelFormat != MTLPixelFormatDepth32Float_Stencil8 || color.sampleCount != 1 ||
      depth.sampleCount != 1 || !color.width || !color.height || color.width != depth.width ||
      color.height != depth.height || color.width > kJak2Shadow195FrameCaptureMaxDimension ||
      color.height > kJak2Shadow195FrameCaptureMaxDimension ||
      context.game_color_slice >= color.arrayLength ||
      context.game_depth_slice >= depth.arrayLength ||
      !plan_jak2_shadow195_readback_layout(static_cast<u32>(color.width),
                                          static_cast<u32>(color.height), &layout)) {
    capture->fail(static_cast<u32>(Jak2Shadow195FrameCaptureFailure::UnsupportedTarget));
    return nullptr;
  }

  id<MTLBuffer> readback =
      [color.device newBufferWithLength:static_cast<NSUInteger>(layout.total_bytes)
                                options:MTLResourceStorageModeShared];
  if (!readback) {
    capture->fail(static_cast<u32>(Jak2Shadow195FrameCaptureFailure::AllocationFailed));
    return nullptr;
  }

  const MTLScissorRect scissor =
      context.game_scissor_valid
          ? context.game_scissor
          : MTLScissorRect{0, 0, static_cast<NSUInteger>(color.width),
                           static_cast<NSUInteger>(color.height)};
  Jak2Shadow195RenderTargetMetadata metadata;
  metadata.view_id = view_id;
  metadata.external_target = external_target ? 1 : 0;
  metadata.width = static_cast<u32>(color.width);
  metadata.height = static_cast<u32>(color.height);
  metadata.pixel_format = static_cast<u32>(color.pixelFormat);
  metadata.color_slice = static_cast<u32>(context.game_color_slice);
  metadata.depth_pixel_format = static_cast<u32>(depth.pixelFormat);
  metadata.depth_slice = static_cast<u32>(context.game_depth_slice);
  metadata.stencil_pixel_format = static_cast<u32>(depth.pixelFormat);
  metadata.stencil_slice = static_cast<u32>(context.game_depth_slice);
  metadata.texture_type = static_cast<u32>(color.textureType);
  metadata.storage_mode = static_cast<u32>(color.storageMode);
  metadata.sample_count = static_cast<u32>(color.sampleCount);
  metadata.array_length = static_cast<u32>(color.arrayLength);
  metadata.mipmap_level_count = static_cast<u32>(color.mipmapLevelCount);
  metadata.viewport_origin_x = context.game_viewport.originX;
  metadata.viewport_origin_y = context.game_viewport.originY;
  metadata.viewport_width = context.game_viewport.width;
  metadata.viewport_height = context.game_viewport.height;
  metadata.viewport_znear = context.game_viewport.znear;
  metadata.viewport_zfar = context.game_viewport.zfar;
  metadata.scissor_x = static_cast<u32>(scissor.x);
  metadata.scissor_y = static_cast<u32>(scissor.y);
  metadata.scissor_width = static_cast<u32>(scissor.width);
  metadata.scissor_height = static_cast<u32>(scissor.height);
  metadata.scissor_explicit = context.game_scissor_explicit ? 1 : 0;
  metadata.color_load_action = context.color_load_action;
  metadata.color_store_action = context.color_store_action;
  metadata.depth_load_action = context.depth_load_action;
  metadata.depth_store_action = context.depth_store_action;
  metadata.stencil_load_action = context.stencil_load_action;
  metadata.stencil_store_action = context.stencil_store_action;
  metadata.render_scale_x = render_scale_x;
  metadata.render_scale_y = render_scale_y;
  capture->record_target(metadata);

  [context.enc endEncoding];
  id<MTLBlitCommandEncoder> blit = [context.cmds blitCommandEncoder];
  copy_plane(blit, color, context.game_color_slice, readback, layout.before_color,
             MTLBlitOptionNone);
  copy_plane(blit, depth, context.game_depth_slice, readback, layout.before_depth,
             MTLBlitOptionDepthFromDepthStencil);
  copy_plane(blit, depth, context.game_depth_slice, readback, layout.before_stencil,
             MTLBlitOptionStencilFromDepthStencil);
  [blit endEncoding];
  resume_game_pass(context);

  auto impl = std::make_unique<Impl>();
  impl->capture = std::move(capture_owner);
  impl->readback = readback;
  impl->layout = layout;
  impl->width = metadata.width;
  impl->height = metadata.height;
  return std::unique_ptr<Jak2Shadow195AttachmentCapture>(
      new Jak2Shadow195AttachmentCapture(std::move(impl)));
}

Jak2Shadow195AttachmentCapture::~Jak2Shadow195AttachmentCapture() {
  if (m_impl && !m_impl->finished) {
    m_impl->capture->fail(static_cast<u32>(Jak2Shadow195FrameCaptureFailure::RendererFailed));
  }
}

void Jak2Shadow195AttachmentCapture::record_post_volume(MetalFrameContext& context) {
  if (!m_impl || m_impl->finished || m_impl->volume_recorded) {
    return;
  }
  [context.enc endEncoding];
  id<MTLBlitCommandEncoder> blit = [context.cmds blitCommandEncoder];
  copy_plane(blit, context.game_depth, context.game_depth_slice, m_impl->readback,
             m_impl->layout.volume_stencil, MTLBlitOptionStencilFromDepthStencil);
  [blit endEncoding];
  resume_game_pass(context);
  m_impl->volume_recorded = true;
}

void Jak2Shadow195AttachmentCapture::finish(
    MetalFrameContext& context,
    const Jak2Shadow195CapturedRendererStats& renderer_stats) {
  if (!m_impl || m_impl->finished) {
    return;
  }
  if (!m_impl->volume_recorded) {
    m_impl->capture->fail(static_cast<u32>(Jak2Shadow195FrameCaptureFailure::RendererFailed));
    m_impl->finished = true;
    return;
  }
  m_impl->capture->record_renderer(renderer_stats);
  [context.enc endEncoding];
  id<MTLBlitCommandEncoder> blit = [context.cmds blitCommandEncoder];
  copy_plane(blit, context.game_color, context.game_color_slice, m_impl->readback,
             m_impl->layout.after_color, MTLBlitOptionNone);
  copy_plane(blit, context.game_depth, context.game_depth_slice, m_impl->readback,
             m_impl->layout.after_depth, MTLBlitOptionDepthFromDepthStencil);
  copy_plane(blit, context.game_depth, context.game_depth_slice, m_impl->readback,
             m_impl->layout.after_stencil, MTLBlitOptionStencilFromDepthStencil);
  [blit endEncoding];
  resume_game_pass(context);

  const auto capture = m_impl->capture;
  id<MTLBuffer> readback = m_impl->readback;
  const ReadbackLayout layout = m_impl->layout;
  const u32 width = m_impl->width;
  const u32 height = m_impl->height;
  [context.cmds addCompletedHandler:^(id<MTLCommandBuffer> commands) {
    if (commands.status != MTLCommandBufferStatusCompleted) {
      capture->fail(
          static_cast<u32>(Jak2Shadow195FrameCaptureFailure::CommandBufferFailed));
      return;
    }
    const u8* bytes = static_cast<const u8*>(readback.contents);
    if (!bytes) {
      capture->fail(static_cast<u32>(Jak2Shadow195FrameCaptureFailure::ReadbackFailed));
      return;
    }
    Jak2Shadow195AttachmentReadback result;
    result.before_color = bytes + layout.before_color.offset;
    result.before_color_row_bytes = layout.before_color.row_bytes;
    result.before_depth = bytes + layout.before_depth.offset;
    result.before_depth_row_bytes = layout.before_depth.row_bytes;
    result.before_stencil = bytes + layout.before_stencil.offset;
    result.before_stencil_row_bytes = layout.before_stencil.row_bytes;
    result.volume_stencil = bytes + layout.volume_stencil.offset;
    result.volume_stencil_row_bytes = layout.volume_stencil.row_bytes;
    result.after_color = bytes + layout.after_color.offset;
    result.after_color_row_bytes = layout.after_color.row_bytes;
    result.after_depth = bytes + layout.after_depth.offset;
    result.after_depth_row_bytes = layout.after_depth.row_bytes;
    result.after_stencil = bytes + layout.after_stencil.offset;
    result.after_stencil_row_bytes = layout.after_stencil.row_bytes;
    capture->complete(result, width, height);
  }];
  m_impl->finished = true;
}

}  // namespace metal_renderer
