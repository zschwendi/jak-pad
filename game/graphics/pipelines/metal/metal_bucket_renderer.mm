#include "metal_bucket_renderer.h"

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/pipelines/metal/metal_eye_renderer.h"
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

/*!
 * Same walk as the GL EmptyBucketRenderer's Jak 1 branch: NEXT into the
 * bucket, CALL to the default-registers chain, CNT + RET inside it, NEXT to
 * the next bucket.
 */
void MetalEmptyBucketRenderer::render(DmaFollower& dma,
                                      MetalSharedRenderState* render_state,
                                      MetalFrameContext& /*ctx*/) {
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
}

void MetalSkipRenderer::render(DmaFollower& dma,
                               MetalSharedRenderState* render_state,
                               MetalFrameContext& /*ctx*/) {
  u64 bytes = 0;
  while (dma.current_tag_offset() != render_state->next_bucket) {
    bytes += dma.read_and_advance().size_bytes;
    if (dma.current_tag_offset() == render_state->default_regs_buffer) {
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
