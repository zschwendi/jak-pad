#include "metal_bucket_renderer.h"

#include <array>
#include <cstring>

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/pipelines/metal/metal_bucket_chain_semantics.h"
#include "game/graphics/pipelines/metal/metal_eye_renderer.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/pipelines/metal/metal_vis_data.h"
#include "game/graphics/texture/TexturePool.h"

void* MetalStreamBuffer::alloc(u32 size, id<MTLBuffer>* out_buffer, u32* out_offset) {
  ASSERT(size <= kPageSize);
  m_offset = (m_offset + 15) & ~15u;
  if (m_pages.empty() || m_offset + size > kPageSize) {
    if (m_page + 1 < m_pages.size() && !m_pages.empty()) {
      m_page++;
    } else {
      m_pages.push_back([m_device newBufferWithLength:kPageSize
                                              options:MTLResourceStorageModeShared]);
      m_page = m_pages.size() - 1;
    }
    m_offset = 0;
  }
  *out_buffer = m_pages[m_page];
  *out_offset = m_offset;
  void* ptr = (u8*)m_pages[m_page].contents + m_offset;
  m_offset += size;
  return ptr;
}

void MetalFrameContext::resume_pass_with_framebuffer_copy(id<MTLTexture> snapshot) {
  ASSERT(cmds && game_color && game_depth);
  [enc endEncoding];

  id<MTLBlitCommandEncoder> blit = [cmds blitCommandEncoder];
  [blit copyFromTexture:game_color
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(game_color.width, game_color.height, 1)
              toTexture:snapshot
       destinationSlice:0
       destinationLevel:0
      destinationOrigin:MTLOriginMake(0, 0, 0)];
  [blit endEncoding];

  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = game_color;
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.depthAttachment.texture = game_depth;
  pass.depthAttachment.loadAction = MTLLoadActionLoad;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.stencilAttachment.texture = game_depth;
  pass.stencilAttachment.loadAction = MTLLoadActionLoad;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;
  enc = [cmds renderCommandEncoderWithDescriptor:pass];
  [enc setCullMode:MTLCullModeNone];
}

/*!
 * Same walk as the GL EmptyBucketRenderer's Jak 1 branch: NEXT into the
 * bucket, CALL to the default-registers chain, CNT + RET inside it, NEXT to
 * the next bucket.
 */
void MetalEmptyBucketRenderer::render(DmaFollower& dma,
                                      MetalSharedRenderState* render_state,
                                      MetalFrameContext& /*ctx*/) {
  const auto layout = metal_renderer::bucket_chain_layout(render_state->version);
  ASSERT_MSG(layout != metal_renderer::MetalBucketChainLayout::Unsupported,
             fmt::format("Metal bucket renderer {} ({}) does not support game version {}", m_my_id,
                         m_name, (int)render_state->version));
  if (layout == metal_renderer::MetalBucketChainLayout::Jak1DefaultRegs) {
    auto first_tag = dma.current_tag();
    dma.read_and_advance();
    ASSERT(first_tag.kind == DmaTag::Kind::NEXT && first_tag.qwc == 0);

    auto call_tag = dma.current_tag();
    dma.read_and_advance();
    ASSERT_MSG(call_tag.kind == DmaTag::Kind::CALL && call_tag.qwc == 0,
               fmt::format("Metal bucket renderer {} ({}) was supposed to be empty, but wasn't",
                           m_my_id, m_name));

    ASSERT(dma.current_tag_offset() == render_state->default_regs_buffer);
    dma.read_and_advance();
    ASSERT(dma.current_tag().kind == DmaTag::Kind::RET);
    dma.read_and_advance();

    auto to_next_buffer = dma.current_tag();
    ASSERT(to_next_buffer.kind == DmaTag::Kind::NEXT);
    ASSERT(to_next_buffer.qwc == 0);
    dma.read_and_advance();

    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
  } else {
    const auto first_tag = dma.current_tag();
    dma.read_and_advance();
    ASSERT_MSG(metal_renderer::is_strict_empty_bucket_tag(layout, first_tag),
               fmt::format("Metal bucket renderer {} ({}) was supposed to be empty, but wasn't",
                           m_my_id, m_name));
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
  }
}

void MetalSkipRenderer::render(DmaFollower& dma,
                               MetalSharedRenderState* render_state,
                               MetalFrameContext& /*ctx*/) {
  const auto layout = metal_renderer::bucket_chain_layout(render_state->version);
  ASSERT(layout != metal_renderer::MetalBucketChainLayout::Unsupported);
  u64 bytes = 0;
  while (dma.current_tag_offset() != render_state->next_bucket) {
    bytes += dma.read_and_advance().size_bytes;
    if (layout == metal_renderer::MetalBucketChainLayout::Jak1DefaultRegs &&
        dma.current_tag_offset() == render_state->default_regs_buffer) {
      // the bucket-ending bounce through the default-registers chain is
      // structure, not content; don't count it
      dma.read_and_advance();  // cnt
      dma.read_and_advance();  // ret
    }
  }
  if (bytes > 0) {
    m_skipped_bytes += bytes;
    if (!m_warned) {
      lg::warn("Metal: bucket [{}] {} is not ported yet; skipped {} bytes of DMA (logged once)",
               m_my_id, m_name, bytes);
      m_warned = true;
    }
  }
}

void MetalHostHandledRenderer::render(DmaFollower& dma,
                                      MetalSharedRenderState* render_state,
                                      MetalFrameContext& /*ctx*/) {
  ASSERT(metal_renderer::bucket_chain_layout(render_state->version) ==
         metal_renderer::MetalBucketChainLayout::Jak2Direct);
  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }
}

void MetalVisibilityBucketRenderer::render(DmaFollower& dma,
                                           MetalSharedRenderState* render_state,
                                           MetalFrameContext& /*ctx*/) {
  auto* background = render_state->background;
  ASSERT(background);
  auto reject = [&](const char* reason) {
    metal_background_expect(false, m_name, reason, background);
    metal_finish_bucket(dma, *render_state);
  };

  if (metal_renderer::bucket_chain_layout(render_state->version) !=
      metal_renderer::MetalBucketChainLayout::Jak2Direct) {
    reject("the Jak 2 direct bucket layout");
    return;
  }
  if (m_level_count > metal_renderer::kMetalMaxVisibilityLevels) {
    reject("a visibility level count within owned-state capacity");
    return;
  }

  const auto start_tag = dma.current_tag();
  if (metal_renderer::is_strict_empty_bucket_tag(
          metal_renderer::MetalBucketChainLayout::Jak2Direct, start_tag)) {
    dma.read_and_advance();
    if (dma.current_tag_offset() != render_state->next_bucket) {
      reject("one strict Jak 2 empty-bucket tag");
    }
    return;
  }
  if (start_tag.kind != DmaTag::Kind::NEXT || start_tag.qwc != 0) {
    reject("a zero-byte NEXT before visibility payloads");
    return;
  }
  dma.read_and_advance();
  if (dma.current_tag_offset() == render_state->next_bucket) {
    metal_background_expect(false, m_name, "visibility payloads after the opening NEXT",
                            background);
    return;
  }

  std::array<DmaTransfer, metal_renderer::kMetalMaxVisDataTransfers> transfers;
  std::size_t transfer_count = 0;
  for (std::size_t level = 0; level < m_level_count; level++) {
    if (dma.current_tag_offset() == render_state->next_bucket) {
      reject("one CNT visibility payload per level");
      return;
    }
    const auto payload_tag = dma.current_tag();
    if (payload_tag.kind != DmaTag::Kind::CNT ||
        (payload_tag.qwc != metal_renderer::kMetalVisibilityBytes / 16 &&
         payload_tag.qwc != metal_renderer::kMetalInactiveVisibilityBytes / 16)) {
      reject("one exact CNT visibility payload per level");
      return;
    }
    transfers[transfer_count++] = dma.read_and_advance();
    if (dma.current_tag_offset() == render_state->next_bucket ||
        dma.current_tag().kind != DmaTag::Kind::NEXT || dma.current_tag().qwc != 0) {
      reject("one zero-byte NEXT boundary per visibility payload");
      return;
    }
    transfers[transfer_count++] = dma.read_and_advance();
  }

  if (dma.current_tag_offset() != render_state->next_bucket) {
    const auto fallback_tag = dma.current_tag();
    if (fallback_tag.kind != DmaTag::Kind::CNT ||
        fallback_tag.qwc != metal_renderer::kMetalBackgroundFallbackBytes / 16) {
      reject("an optional CNT background fallback payload");
      return;
    }
    transfers[transfer_count++] = dma.read_and_advance();
    if (dma.current_tag_offset() == render_state->next_bucket ||
        dma.current_tag().kind != DmaTag::Kind::NEXT || dma.current_tag().qwc != 0) {
      reject("a zero-byte NEXT boundary after the background fallback");
      return;
    }
    transfers[transfer_count++] = dma.read_and_advance();
  }
  if (dma.current_tag_offset() != render_state->next_bucket) {
    reject("only visibility pairs and one optional background fallback pair");
    return;
  }

  if (!metal_renderer::decode_metal_visibility_frame(
          transfers.data(), transfer_count, m_level_count, &background->visibility)) {
    metal_background_expect(false, m_name, "an exact owned visibility frame", background);
    return;
  }
  if (!background->use_occlusion_culling) {
    for (std::size_t level = 0; level < background->visibility.level_count; level++) {
      background->visibility.levels[level].valid = false;
    }
  }
  std::memcpy(render_state->fog_color.data(), &background->visibility.fog_vif0,
              sizeof(background->visibility.fog_vif0));
}

void MetalTextureBucketRenderer::render(DmaFollower& dma,
                                        MetalSharedRenderState* render_state,
                                        MetalFrameContext& ctx) {
  std::function<void(DmaFollower&)> eye_dma_handler;
  if (render_state->eye_renderer) {
    eye_dma_handler = [&](DmaFollower& d) {
      render_state->eye_renderer->render_from_texture_bucket(d, render_state, ctx);
    };
  }
  m_last_stats = m_handler.process(dma, render_state->next_bucket, *render_state->texture_pool,
                                   render_state->ee_memory, render_state->offset_of_s7,
                                   eye_dma_handler);
}
