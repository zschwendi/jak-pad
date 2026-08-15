#include "metal_bucket_renderer.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/pipelines/metal/metal_bucket_chain_semantics.h"
#include "game/graphics/pipelines/metal/metal_eye_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"
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
            sourceSlice:game_color_slice
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
  pass.colorAttachments[0].slice = game_color_slice;
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.depthAttachment.texture = game_depth;
  pass.depthAttachment.slice = game_depth_slice;
  pass.depthAttachment.loadAction = MTLLoadActionLoad;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.stencilAttachment.texture = game_depth;
  pass.stencilAttachment.slice = game_depth_slice;
  pass.stencilAttachment.loadAction = MTLLoadActionLoad;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;
  enc = [cmds renderCommandEncoderWithDescriptor:pass];
  [enc setCullMode:MTLCullModeNone];
  [enc setViewport:game_viewport];
  if (game_scissor_valid) {
    [enc setScissorRect:game_scissor];
  }
  color_load_action = static_cast<u32>(MTLLoadActionLoad);
  color_store_action = static_cast<u32>(MTLStoreActionStore);
  depth_load_action = static_cast<u32>(MTLLoadActionLoad);
  depth_store_action = static_cast<u32>(MTLStoreActionStore);
  stencil_load_action = static_cast<u32>(MTLLoadActionLoad);
  stencil_store_action = static_cast<u32>(MTLStoreActionStore);
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
  m_last_skipped_bytes = bytes;
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
  if (render_state->host_bucket_callback) {
    render_state->host_bucket_callback(render_state->host_bucket_context,
                                       static_cast<u32>(m_my_id));
  }
  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }
}

namespace {

u32 pris_expected_offset(const MetalSharedRenderState& state, u32 bucket_id, u32 relative) {
  const u64 offset = static_cast<u64>(state.buckets_base) + bucket_id * 16 + relative;
  if (offset > std::numeric_limits<u32>::max()) {
    throw std::runtime_error("Jak 2 PRIS eye plan offset overflowed");
  }
  return static_cast<u32>(offset);
}

void pris_expect_offset(const DmaFollower& dma, u32 expected, const char* what) {
  if (dma.current_tag_offset() != expected) {
    throw std::runtime_error(fmt::format("Jak 2 PRIS eye {} moved from {:#x} to {:#x}", what,
                                         expected, dma.current_tag_offset()));
  }
}

DmaTransfer pris_take(DmaFollower& dma,
                      DmaTag::Kind kind,
                      u16 qwc,
                      VifCode::Kind vif0,
                      u16 vif0_immediate,
                      VifCode::Kind vif1,
                      u16 vif1_immediate,
                      const char* what) {
  const auto tag = dma.current_tag();
  const auto code0 = dma.current_tag_vifcode0();
  const auto code1 = dma.current_tag_vifcode1();
  if (tag.kind != kind || tag.qwc != qwc || tag.spr || code0.kind != vif0 ||
      code0.immediate != vif0_immediate || code1.kind != vif1 ||
      code1.immediate != vif1_immediate) {
    throw std::runtime_error(fmt::format("Jak 2 PRIS eye {} did not match its copied plan", what));
  }
  return dma.read_and_advance();
}

const metal_renderer::Jak2PrisEyeTextureUploadPlan& pris_plan_for_bucket(
    const MetalSharedRenderState& state,
    u32 bucket_id) {
  if (!state.jak2_pris_eye_plans || state.jak2_pris_eye_plan_count == 0) {
    throw std::runtime_error("Jak 2 PRIS eye renderer has no copied execution plans");
  }
  for (std::size_t i = 0; i < state.jak2_pris_eye_plan_count; ++i) {
    if (state.jak2_pris_eye_plans[i].bucket_id == bucket_id) {
      return state.jak2_pris_eye_plans[i];
    }
  }
  throw std::runtime_error(
      fmt::format("Jak 2 PRIS eye renderer is missing bucket {} plan", bucket_id));
}

bool prison_jak_animator_body_matches(
    const DmaTransfer& transfer,
    const metal_renderer::Jak2PrisPrisonJakAnimatorPlan& plan) {
  if (!transfer.data || transfer.size_bytes != metal_renderer::kJak2PrisPrisonJakAnimatorBodyBytes) {
    return false;
  }
  float morph = 0.f;
  std::memcpy(&morph, transfer.data, sizeof(morph));
  if (!std::isfinite(morph) || morph < 0.f || morph > 1.f ||
      std::memcmp(&morph, &plan.morph, sizeof(morph)) != 0) {
    return false;
  }
  if (plan.destination_tbp_count == 0 ||
      plan.destination_tbp_count > plan.destination_tbps.size()) {
    return false;
  }
  for (std::size_t i = 0; i < plan.destination_tbp_count; ++i) {
    u32 tbp = 0;
    std::memcpy(&tbp, transfer.data + 16 + i * sizeof(tbp), sizeof(tbp));
    if (tbp != metal_renderer::kJak2PrisPrisonJakAnimatorMissingTbp &&
        tbp >= metal_renderer::kJak2PrisPrisonJakAnimatorTbpUpperBound) {
      return false;
    }
    if (tbp != plan.destination_tbps[i]) {
      return false;
    }
  }
  for (std::size_t i = plan.destination_tbp_count; i < plan.destination_tbps.size(); ++i) {
    if (plan.destination_tbps[i] != metal_renderer::kJak2PrisPrisonJakAnimatorMissingTbp) {
      return false;
    }
  }
  constexpr std::size_t kLeadingPaddingBytes = 12;
  const std::size_t destination_end =
      16 + static_cast<std::size_t>(plan.destination_tbp_count) * sizeof(u32);
  const std::size_t trailing_padding = transfer.size_bytes - destination_end;
  if (plan.source_padding_size != kLeadingPaddingBytes + trailing_padding) {
    return false;
  }
  return std::memcmp(transfer.data + 4, plan.source_padding.data(), kLeadingPaddingBytes) == 0 &&
         std::memcmp(transfer.data + destination_end,
                     plan.source_padding.data() + kLeadingPaddingBytes, trailing_padding) == 0;
}

bool dark_jak_animator_body_matches(
    const DmaTransfer& transfer,
    const metal_renderer::Jak2CommonPrisDarkJakAnimatorPlan& plan) {
  if (!transfer.data ||
      transfer.size_bytes != metal_renderer::kJak2CommonPrisDarkJakAnimatorBodyBytes) {
    return false;
  }
  float morph = 0.f;
  std::memcpy(&morph, transfer.data, sizeof(morph));
  if (!std::isfinite(morph) || morph < 0.f || morph > 1.f ||
      std::memcmp(&morph, &plan.morph, sizeof(morph)) != 0 ||
      std::memcmp(transfer.data + 4, plan.source_padding.data(), plan.source_padding.size()) != 0) {
    return false;
  }
  for (std::size_t i = 0; i < plan.destination_tbps.size(); ++i) {
    u32 tbp = 0;
    std::memcpy(&tbp, transfer.data + 16 + i * sizeof(tbp), sizeof(tbp));
    if (tbp != plan.destination_tbps[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace

void MetalJak2PrisEyeBucketRenderer::render(DmaFollower& dma,
                                             MetalSharedRenderState* render_state,
                                             MetalFrameContext& ctx) {
  if (!render_state || render_state->version != GameVersion::Jak2 ||
      !render_state->eye_renderer || !render_state->host_bucket_callback) {
    throw std::runtime_error("Jak 2 PRIS eye renderer dispatch is incomplete");
  }
  const u32 bucket_id = static_cast<u32>(m_my_id);
  const auto& plan = pris_plan_for_bucket(*render_state, bucket_id);
  const u32 bucket_offset = pris_expected_offset(*render_state, bucket_id, 0);
  pris_expect_offset(dma, bucket_offset, "bucket entry");

  // This is deliberately first: ordinary page publication has source ordering at bucket entry.
  render_state->host_bucket_callback(render_state->host_bucket_context, bucket_id);

  if (!plan.present) {
    pris_take(dma, DmaTag::Kind::CNT, 0, VifCode::Kind::NOP, 0, VifCode::Kind::NOP, 0,
              "absent terminal");
    pris_expect_offset(dma, render_state->next_bucket, "absent boundary");
    return;
  }

  pris_take(dma, DmaTag::Kind::NEXT, 0, VifCode::Kind::NOP, 0, VifCode::Kind::NOP, 0,
            "opening linker");
  const auto descriptor =
      pris_take(dma, DmaTag::Kind::CNT, 1, VifCode::Kind::PC_PORT, 0,
                VifCode::Kind::NOP, 3, "ordinary descriptor");
  u64 page_offset = 0;
  s64 mode = 0;
  std::memcpy(&page_offset, descriptor.data, sizeof(page_offset));
  std::memcpy(&mode, descriptor.data + sizeof(page_offset), sizeof(mode));
  if (page_offset != plan.ordinary.page_offset || mode != plan.ordinary.mode) {
    throw std::runtime_error("Jak 2 PRIS eye ordinary descriptor changed after copied preflight");
  }
  pris_take(dma, DmaTag::Kind::NEXT, 0, VifCode::Kind::NOP, 0, VifCode::Kind::NOP, 0,
            "ordinary linker");

  if (plan.has_prison_jak_animator || plan.has_highres_jak_animator) {
    const auto& animator = plan.prison_jak_animator;
    if (animator.semantic_fingerprint == 0) {
      throw std::runtime_error("Jak 2 PRIS Jak CLUT animator plan is not fingerprinted");
    }
    pris_expect_offset(
        dma, pris_expected_offset(*render_state, bucket_id, animator.start_relative_tag_offset),
        "Jak CLUT animator start");
    pris_take(dma, DmaTag::Kind::CNT, 0, VifCode::Kind::PC_PORT,
              metal_renderer::kJak2PrisPrisonJakAnimatorStartOpcode, VifCode::Kind::NOP, 0,
              "Jak CLUT animator start");
    pris_expect_offset(
        dma, pris_expected_offset(*render_state, bucket_id, animator.body_relative_tag_offset),
        "Jak CLUT animator body");
    const auto body = pris_take(
        dma, DmaTag::Kind::CNT, metal_renderer::kJak2PrisPrisonJakAnimatorBodyBytes / 16,
        VifCode::Kind::PC_PORT, animator.opcode, VifCode::Kind::NOP, 0, "Jak CLUT animator body");
    if (!prison_jak_animator_body_matches(body, animator)) {
      throw std::runtime_error("Jak 2 PRIS Jak CLUT animator body changed after copied preflight");
    }
    pris_expect_offset(
        dma, pris_expected_offset(*render_state, bucket_id, animator.finish_relative_tag_offset),
        "Jak CLUT animator finish");
    pris_take(dma, DmaTag::Kind::CNT, 0, VifCode::Kind::PC_PORT,
              metal_renderer::kJak2PrisPrisonJakAnimatorFinishOpcode, VifCode::Kind::NOP, 0,
              "Jak CLUT animator finish");
    pris_expect_offset(
        dma, pris_expected_offset(*render_state, bucket_id, animator.linker_relative_tag_offset),
        "Jak CLUT animator linker");
    pris_take(dma, DmaTag::Kind::NEXT, 0, VifCode::Kind::NOP, 0, VifCode::Kind::NOP, 0,
              "Jak CLUT animator linker");
  }

  for (std::size_t i = 0; i < plan.chunk_count; ++i) {
    const auto& chunk = plan.chunks[i];
    pris_expect_offset(dma, pris_expected_offset(*render_state, bucket_id,
                                                 chunk.start_relative_tag_offset),
                       "chunk start");
    const auto before = render_state->eye_renderer->stats();
    render_state->eye_renderer->render_from_texture_bucket(dma, render_state, ctx,
                                                           bucket_id);
    const auto after = render_state->eye_renderer->stats();
    if (after.eyes != before.eyes + 2 || after.draw_calls != before.draw_calls + 8 ||
        after.triangles != before.triangles + 16 ||
        after.missing_textures != before.missing_textures ||
        after.command_buffers_committed != before.command_buffers_committed + 1 ||
        after.command_buffers_completed != before.command_buffers_completed + 1 ||
        after.unexpected_dma != before.unexpected_dma ||
        after.duplicate_slot_writes != before.duplicate_slot_writes ||
        after.command_buffer_errors != before.command_buffer_errors) {
      throw std::runtime_error(
          fmt::format("Jak 2 PRIS eye chunk {} renderer execution failed", i));
    }

    const u32 linker_offset = pris_expected_offset(*render_state, bucket_id,
                                                   chunk.linker_relative_tag_offset);
    pris_expect_offset(dma, linker_offset - (16 + 8 * 16 + 16 + 2 * 16),
                       "chunk terminal qwc8");
    pris_take(dma, DmaTag::Kind::CNT, 8, VifCode::Kind::FLUSHA, 0,
              VifCode::Kind::DIRECT, 8, "chunk terminal qwc8");
    pris_expect_offset(dma, linker_offset - (16 + 2 * 16), "chunk trailing qwc2");
    pris_take(dma, DmaTag::Kind::CNT, 2, VifCode::Kind::NOP, 0,
              VifCode::Kind::DIRECT, 2, "chunk trailing qwc2");
    pris_expect_offset(dma, linker_offset, "chunk linker");
    pris_take(dma, DmaTag::Kind::NEXT, 0, VifCode::Kind::NOP, 0, VifCode::Kind::NOP, 0,
              "chunk linker");
  }

  pris_expect_offset(dma, pris_expected_offset(*render_state, bucket_id,
                                               plan.direct_reset_relative_tag_offset),
                     "default reset");
  pris_take(dma, DmaTag::Kind::CNT, 10, VifCode::Kind::FLUSHA, 0,
            VifCode::Kind::DIRECT, 10, "default reset");
  pris_expect_offset(dma, pris_expected_offset(*render_state, bucket_id,
                                               plan.terminal_relative_tag_offset),
                     "terminal linker");
  pris_take(dma, DmaTag::Kind::NEXT, 0, VifCode::Kind::NOP, 0, VifCode::Kind::NOP, 0,
            "terminal linker");
  pris_expect_offset(dma, render_state->next_bucket, "bucket boundary");
}

void MetalJak2CommonPrisBucketRenderer::render(DmaFollower& dma,
                                                MetalSharedRenderState* render_state,
                                                MetalFrameContext& ctx) {
  if (!render_state || render_state->version != GameVersion::Jak2 ||
      !render_state->eye_renderer || !render_state->host_bucket_callback ||
      !render_state->jak2_common_pris_plan ||
      static_cast<u32>(m_my_id) != metal_renderer::kJak2CommonPrisTextureUploadBucket) {
    throw std::runtime_error("Jak 2 common PRIS renderer dispatch is incomplete");
  }
  const u32 bucket_id = static_cast<u32>(m_my_id);
  const auto& plan = *render_state->jak2_common_pris_plan;
  if (plan.bucket_id != bucket_id) {
    throw std::runtime_error("Jak 2 common PRIS renderer received the wrong copied plan");
  }
  pris_expect_offset(dma, pris_expected_offset(*render_state, bucket_id, 0), "bucket entry");
  render_state->host_bucket_callback(render_state->host_bucket_context, bucket_id);

  if (!plan.present) {
    pris_take(dma, DmaTag::Kind::CNT, 0, VifCode::Kind::NOP, 0, VifCode::Kind::NOP, 0,
              "absent terminal");
    pris_expect_offset(dma, render_state->next_bucket, "absent boundary");
    return;
  }

  pris_take(dma, DmaTag::Kind::NEXT, 0, VifCode::Kind::NOP, 0, VifCode::Kind::NOP, 0,
            "opening linker");
  const auto descriptor =
      pris_take(dma, DmaTag::Kind::CNT, 1, VifCode::Kind::PC_PORT, 0,
                VifCode::Kind::NOP, 3, "ordinary descriptor");
  u64 page_offset = 0;
  s64 mode = 0;
  std::memcpy(&page_offset, descriptor.data, sizeof(page_offset));
  std::memcpy(&mode, descriptor.data + sizeof(page_offset), sizeof(mode));
  if (page_offset != plan.ordinary.page_offset || mode != plan.ordinary.mode) {
    throw std::runtime_error("Jak 2 common PRIS ordinary descriptor changed after preflight");
  }
  pris_take(dma, DmaTag::Kind::NEXT, 0, VifCode::Kind::NOP, 0, VifCode::Kind::NOP, 0,
            "ordinary linker");

  const auto& animator = plan.dark_jak_animator;
  if (animator.semantic_fingerprint == 0) {
    throw std::runtime_error("Jak 2 common PRIS Dark Jak animator is not fingerprinted");
  }
  pris_expect_offset(dma, pris_expected_offset(*render_state, bucket_id,
                                               animator.start_relative_tag_offset),
                     "Dark Jak animator start");
  pris_take(dma, DmaTag::Kind::CNT, 0, VifCode::Kind::PC_PORT,
            metal_renderer::kJak2PrisPrisonJakAnimatorStartOpcode, VifCode::Kind::NOP, 0,
            "Dark Jak animator start");
  pris_expect_offset(dma, pris_expected_offset(*render_state, bucket_id,
                                               animator.body_relative_tag_offset),
                     "Dark Jak animator body");
  const auto body = pris_take(
      dma, DmaTag::Kind::CNT, metal_renderer::kJak2CommonPrisDarkJakAnimatorBodyBytes / 16,
      VifCode::Kind::PC_PORT, metal_renderer::kJak2CommonPrisDarkJakAnimatorOpcode,
      VifCode::Kind::NOP, 0, "Dark Jak animator body");
  if (!dark_jak_animator_body_matches(body, animator)) {
    throw std::runtime_error("Jak 2 common PRIS Dark Jak animator changed after preflight");
  }
  pris_expect_offset(dma, pris_expected_offset(*render_state, bucket_id,
                                               animator.finish_relative_tag_offset),
                     "Dark Jak animator finish");
  pris_take(dma, DmaTag::Kind::CNT, 0, VifCode::Kind::PC_PORT,
            metal_renderer::kJak2PrisPrisonJakAnimatorFinishOpcode,
            VifCode::Kind::NOP, 0, "Dark Jak animator finish");
  pris_expect_offset(dma, pris_expected_offset(*render_state, bucket_id,
                                               animator.linker_relative_tag_offset),
                     "Dark Jak animator linker");
  pris_take(dma, DmaTag::Kind::NEXT, 0, VifCode::Kind::NOP, 0, VifCode::Kind::NOP, 0,
            "Dark Jak animator linker");

  for (std::size_t i = 0; i < plan.chunk_count; ++i) {
    const auto& chunk = plan.chunks[i];
    pris_expect_offset(dma, pris_expected_offset(*render_state, bucket_id,
                                                 chunk.start_relative_tag_offset),
                       "chunk start");
    const auto before = render_state->eye_renderer->stats();
    render_state->eye_renderer->render_from_texture_bucket(dma, render_state, ctx,
                                                           bucket_id);
    const auto after = render_state->eye_renderer->stats();
    if (after.eyes != before.eyes + 2 || after.draw_calls != before.draw_calls + 8 ||
        after.triangles != before.triangles + 16 ||
        after.missing_textures != before.missing_textures ||
        after.command_buffers_committed != before.command_buffers_committed + 1 ||
        after.command_buffers_completed != before.command_buffers_completed + 1 ||
        after.unexpected_dma != before.unexpected_dma ||
        after.duplicate_slot_writes != before.duplicate_slot_writes ||
        after.command_buffer_errors != before.command_buffer_errors) {
      throw std::runtime_error(
          fmt::format("Jak 2 common PRIS eye chunk {} renderer execution failed", i));
    }
    const u32 linker_offset = pris_expected_offset(*render_state, bucket_id,
                                                   chunk.linker_relative_tag_offset);
    pris_expect_offset(dma, linker_offset - (16 + 8 * 16 + 16 + 2 * 16),
                       "chunk terminal qwc8");
    pris_take(dma, DmaTag::Kind::CNT, 8, VifCode::Kind::FLUSHA, 0,
              VifCode::Kind::DIRECT, 8, "chunk terminal qwc8");
    pris_expect_offset(dma, linker_offset - (16 + 2 * 16), "chunk trailing qwc2");
    pris_take(dma, DmaTag::Kind::CNT, 2, VifCode::Kind::NOP, 0,
              VifCode::Kind::DIRECT, 2, "chunk trailing qwc2");
    pris_expect_offset(dma, linker_offset, "chunk linker");
    pris_take(dma, DmaTag::Kind::NEXT, 0, VifCode::Kind::NOP, 0, VifCode::Kind::NOP, 0,
              "chunk linker");
  }

  pris_expect_offset(dma, pris_expected_offset(*render_state, bucket_id,
                                               plan.direct_reset_relative_tag_offset),
                     "default reset");
  pris_take(dma, DmaTag::Kind::CNT, 10, VifCode::Kind::FLUSHA, 0,
            VifCode::Kind::DIRECT, 10, "default reset");
  pris_expect_offset(dma, pris_expected_offset(*render_state, bucket_id,
                                               plan.terminal_relative_tag_offset),
                     "terminal linker");
  pris_take(dma, DmaTag::Kind::NEXT, 0, VifCode::Kind::NOP, 0, VifCode::Kind::NOP, 0,
            "terminal linker");
  pris_expect_offset(dma, render_state->next_bucket, "bucket boundary");
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
