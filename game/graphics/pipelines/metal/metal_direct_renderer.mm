#include "metal_direct_renderer.h"

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/pipelines/metal/metal_bucket_chain_semantics.h"
#include "game/graphics/texture/TexturePool.h"

#include "fmt/format.h"

MetalDirectRenderer::ScissorState MetalDirectRenderer::m_scissor;

namespace {

constexpr PerGameVersion<int> game_height(448, 416, 416, 416);
constexpr PerGameVersion<u32> normal_zbp(448, 304, 304, 304);

// must match DirectVsParams in shaders/direct.metal
struct DirectVsParams {
  float height_scale;
  float scissor_adjust;
  s32 offscreen_mode;
};

// must match DirectFsParams in shaders/direct.metal (float4s are 16-aligned)
struct DirectFsParams {
  float fog_color[4];
  float game_sizes[4];
  float alpha_min;
  float alpha_max;
  float color_mult;
  float alpha_mult;
  float ta0;
  s32 scissor_enable;
  s32 greater;
  s32 pad;
};
static_assert(sizeof(DirectFsParams) == 64);

/*!
 * If it's a direct, returns the qwc. If it's ignorable (nop, flush), returns 0.
 */
u32 get_direct_qwc_or_nop(const VifCode& code) {
  switch (code.kind) {
    case VifCode::Kind::NOP:
    case VifCode::Kind::FLUSHA:
      return 0;
    case VifCode::Kind::DIRECT:
      if (code.immediate == 0) {
        return 65536;
      } else {
        return code.immediate;
      }
    default:
      ASSERT_MSG(false, fmt::format("expected direct, got {}", code.print()));
      return 0;
  }
}

}  // namespace

MetalDirectRenderer::MetalDirectRenderer(const std::string& name, int my_id, int batch_size)
    : MetalBucketRenderer(name, my_id), m_prim_buffer(batch_size) {}

void MetalHostTextureUploadDirectRenderer::render(DmaFollower& dma,
                                                  MetalSharedRenderState* render_state,
                                                  MetalFrameContext& ctx) {
  ASSERT(metal_renderer::bucket_chain_layout(render_state->version) ==
         metal_renderer::MetalBucketChainLayout::Jak2Direct);
  if (render_state->host_bucket_callback) {
    render_state->host_bucket_callback(render_state->host_bucket_context,
                                       static_cast<u32>(m_my_id));
  }

  reset_state();
  while (dma.current_tag_offset() != render_state->next_bucket) {
    const auto data = dma.read_and_advance();
    if (!data.size_bytes) {
      continue;
    }
    if (data.vifcode0().kind == VifCode::Kind::PC_PORT) {
      ASSERT(data.vifcode1().kind == VifCode::Kind::NOP);
      continue;
    }
    render_vif(data.vif0(), data.vif1(), data.data, data.size_bytes, render_state, ctx);
  }
  flush_pending(render_state, ctx);
}

/*!
 * Render from a DMA bucket (same walk as the GL DirectRenderer::render).
 */
void MetalDirectRenderer::render(DmaFollower& dma,
                                 MetalSharedRenderState* render_state,
                                 MetalFrameContext& ctx) {
  const auto layout = metal_renderer::bucket_chain_layout(render_state->version);
  ASSERT(layout != metal_renderer::MetalBucketChainLayout::Unsupported);
  // rendering from a bucket starts from a totally reset state
  reset_state();

  while (dma.current_tag_offset() != render_state->next_bucket) {
    auto data = dma.read_and_advance();
    if (data.size_bytes) {
      render_vif(data.vif0(), data.vif1(), data.data, data.size_bytes, render_state, ctx);
    }

    if (layout == metal_renderer::MetalBucketChainLayout::Jak1DefaultRegs &&
        dma.current_tag_offset() == render_state->default_regs_buffer) {
      dma.read_and_advance();  // cnt
      ASSERT(dma.current_tag().kind == DmaTag::Kind::RET);
      dma.read_and_advance();  // ret
    }
  }

  flush_pending(render_state, ctx);
}

void MetalDirectRenderer::reset_state() {
  m_test_state = TestState();
  m_blend_state = BlendState();
  m_prim_state = PrimState();

  for (int i = 0; i < TEXTURE_STATE_COUNT; ++i) {
    m_buffered_tex_state[i] = TextureState();
  }
  m_tex_state_from_reg = {};
  m_next_free_tex_state = 0;
  m_current_tex_state_idx = -1;

  m_prim_building = PrimBuildState();
  m_test_state_needs_double_draw = false;

  m_stats = {};
}

/*!
 * Translate the GS alpha-blend equation to a PSO blend configuration. Same
 * mode set as DirectRenderer::update_gl_blend; color_mult/alpha_mult are
 * derived here and applied through the fragment uniforms.
 */
MetalPsoKey MetalDirectRenderer::blend_to_pso_key(MetalFrameContext& ctx) {
  using BM = GsAlpha::BlendMode;
  const auto& state = m_blend_state;

  MetalPsoKey key;
  key.color_format = ctx.color_format;
  key.depth_format = ctx.depth_format;
  key.color_write_mask = m_test_state.write_rgb
                             ? MTLColorWriteMaskAll
                             : MTLColorWriteMaskAlpha;

  m_color_mult = 1.f;
  m_alpha_mult = 1.f;

  if (!state.alpha_blend_enable) {
    return key;
  }

  // the GL path always writes alpha with (ONE, ZERO)
  key.blend_enable = true;
  key.blend_src_alpha = MTLBlendFactorOne;
  key.blend_dst_alpha = MTLBlendFactorZero;

  if (state.a == BM::SOURCE && state.b == BM::DEST && state.c == BM::SOURCE &&
      state.d == BM::DEST) {
    // (Cs - Cd) * As + Cd = Cs * As + (1 - As) * Cd
    key.blend_src_rgb = MTLBlendFactorSourceAlpha;
    key.blend_dst_rgb = MTLBlendFactorOneMinusSourceAlpha;
  } else if (state.a == BM::SOURCE && state.b == BM::ZERO_OR_FIXED && state.c == BM::SOURCE &&
             state.d == BM::DEST) {
    // (Cs - 0) * As + Cd
    ASSERT(state.fix == 0);
    key.blend_src_rgb = MTLBlendFactorSourceAlpha;
    key.blend_dst_rgb = MTLBlendFactorOne;
  } else if (state.a == BM::ZERO_OR_FIXED && state.b == BM::SOURCE && state.c == BM::SOURCE &&
             state.d == BM::DEST) {
    // (0 - Cs) * As + Cd = Cd - Cs * As
    key.blend_src_rgb = MTLBlendFactorSourceAlpha;
    key.blend_dst_rgb = MTLBlendFactorOne;
    key.blend_op_rgb = MTLBlendOperationReverseSubtract;
  } else if (state.a == BM::SOURCE && state.b == BM::DEST && state.c == BM::ZERO_OR_FIXED &&
             state.d == BM::DEST) {
    // (Cs - Cd) * fix + Cd; fix arrives via setBlendColor at encode time
    key.blend_src_rgb = MTLBlendFactorBlendAlpha;
    key.blend_dst_rgb = MTLBlendFactorOneMinusBlendAlpha;
  } else if (state.a == BM::SOURCE && state.b == BM::SOURCE && state.c == BM::SOURCE &&
             state.d == BM::SOURCE) {
    // trick to disable alpha blending
    key.blend_enable = false;
  } else if (state.a == BM::SOURCE && state.b == BM::ZERO_OR_FIXED && state.c == BM::DEST &&
             state.d == BM::DEST) {
    // (Cs - 0) * Ad + Cd
    key.blend_src_rgb = MTLBlendFactorDestinationAlpha;
    key.blend_dst_rgb = MTLBlendFactorOne;
    m_color_mult = 0.5f;
  } else {
    m_stats.unsupported_blends++;
    if (!m_warned_unsupported_blend) {
      lg::error("Metal direct {}: unsupported GS blend a {} b {} c {} d {} (logged once, counted)",
                m_name, (int)state.a, (int)state.b, (int)state.c, (int)state.d);
      m_warned_unsupported_blend = true;
    }
    key.blend_enable = false;
  }
  return key;
}

/*!
 * Translate the GS TEST register to a depth-stencil key. Mirror of
 * DirectRenderer::update_gl_test.
 */
MetalDepthStencilKey MetalDirectRenderer::test_to_depth_key(bool depth_write_override,
                                                            bool has_override) {
  const auto& state = m_test_state;
  MetalDepthStencilKey key;

  // you aren't supposed to turn off z test enable, the GS had some bugs
  ASSERT(state.zte);
  key.depth_test = true;
  switch (state.ztst) {
    case GsTest::ZTest::NEVER:
      key.compare = MTLCompareFunctionNever;
      break;
    case GsTest::ZTest::ALWAYS:
      key.compare = MTLCompareFunctionAlways;
      break;
    case GsTest::ZTest::GEQUAL:
      key.compare = MTLCompareFunctionGreaterEqual;
      break;
    case GsTest::ZTest::GREATER:
      key.compare = MTLCompareFunctionGreater;
      break;
    default:
      ASSERT(false);
  }

  if (state.date) {
    ASSERT(false);
  }

  if (has_override) {
    key.depth_write = depth_write_override;
    return key;
  }

  bool alpha_trick_to_disable = state.alpha_test_enable &&
                                state.alpha_test == GsTest::AlphaTest::NEVER &&
                                state.afail == GsTest::AlphaFail::FB_ONLY;
  key.depth_write = state.depth_writes && !alpha_trick_to_disable;
  return key;
}

void MetalDirectRenderer::flush_pending(MetalSharedRenderState* render_state,
                                        MetalFrameContext& ctx) {
  // capture the batch's texture state, then release the slots (the GL flush's
  // bookkeeping: states become free again after a flush)
  TextureState batch_tex = m_buffered_tex_state[0];
  for (int i = 0; i < TEXTURE_STATE_COUNT; i++) {
    m_buffered_tex_state[i].used = false;
  }
  m_next_free_tex_state = 0;
  m_current_tex_state_idx = -1;

  if (m_prim_buffer.vert_count == 0) {
    return;
  }

  m_test_state_needs_double_draw = m_test_state.afail == GsTest::AlphaFail::FB_ONLY ||
                                   m_test_state.afail == GsTest::AlphaFail::RGB_ONLY;

  // blend + color mask -> PSO key (also derives color_mult / alpha_mult)
  MetalPsoKey pso_key = blend_to_pso_key(ctx);

  // prim state -> shader + alpha test uniforms (DirectRenderer::update_gl_prim)
  float alpha_min = 0.f;
  float alpha_max = 10.f;
  int greater = 0;
  if (m_prim_state.texture_enable) {
    if (m_test_state.alpha_test_enable) {
      switch (m_test_state.alpha_test) {
        case GsTest::AlphaTest::ALWAYS:
          break;
        case GsTest::AlphaTest::GEQUAL:
          alpha_min = m_test_state.aref / 128.f;
          m_double_draw_aref = alpha_min;
          greater = 0;
          break;
        case GsTest::AlphaTest::GREATER:
          alpha_min = (1 + m_test_state.aref) / 128.f;
          m_double_draw_aref = alpha_min;
          greater = 1;
          break;
        case GsTest::AlphaTest::NEVER:
          break;
        default:
          ASSERT_MSG(false, fmt::format("unknown alpha test: {}", (int)m_test_state.alpha_test));
      }
    }
    pso_key.shader = MetalShaderId::DIRECT_TEXTURED;
  } else {
    pso_key.shader = MetalShaderId::DIRECT_BASIC;
  }
  if (m_prim_state.aa_enable) {
    ASSERT(false);
  }
  if (m_prim_state.ctxt) {
    ASSERT(false);
  }
  if (m_prim_state.fix) {
    ASSERT(false);
  }

  auto& last_batch = m_stats.last_batch;
  last_batch = {};
  last_batch.valid = true;
  last_batch.textured = pso_key.shader == MetalShaderId::DIRECT_TEXTURED;
  last_batch.vertices = m_prim_buffer.vert_count;
  for (int i = 0; i < m_prim_buffer.vert_count; i++) {
    const auto& rgba = m_prim_buffer.vertices[i].rgba;
    last_batch.nonzero_rgb_vertices += rgba[0] != 0 || rgba[1] != 0 || rgba[2] != 0;
  }
  last_batch.tex0_tbp = batch_tex.texture_base_ptr;
  last_batch.tex0_tcc = batch_tex.tcc;
  last_batch.tex0_decal = batch_tex.decal;
  last_batch.write_rgb = m_test_state.write_rgb;
  last_batch.blend_enabled = m_blend_state.alpha_blend_enable;
  last_batch.blend_a = static_cast<u8>(m_blend_state.a);
  last_batch.blend_b = static_cast<u8>(m_blend_state.b);
  last_batch.blend_c = static_cast<u8>(m_blend_state.c);
  last_batch.blend_d = static_cast<u8>(m_blend_state.d);
  last_batch.alpha_test_enabled = m_test_state.alpha_test_enable;
  last_batch.alpha_test_mode = static_cast<u8>(m_test_state.alpha_test);
  last_batch.alpha_aref = m_test_state.aref;
  last_batch.alpha_afail = static_cast<u8>(m_test_state.afail);

  // vertices into the frame's stream buffer
  const u32 bytes = m_prim_buffer.vert_count * sizeof(Vertex);
  id<MTLBuffer> vbuf;
  u32 voffset;
  void* dst = ctx.stream->alloc(bytes, &vbuf, &voffset);
  memcpy(dst, m_prim_buffer.vertices.data(), bytes);

  DirectVsParams vs_params;
  vs_params.height_scale = render_state->version == GameVersion::Jak1 ? 1.f : 0.5f;
  vs_params.scissor_adjust = 512.f / game_height[render_state->version];
  vs_params.offscreen_mode = 0;

  DirectFsParams fs_params = {};
  fs_params.fog_color[0] = render_state->fog_color[0] / 255.f;
  fs_params.fog_color[1] = render_state->fog_color[1] / 255.f;
  fs_params.fog_color[2] = render_state->fog_color[2] / 255.f;
  fs_params.fog_color[3] = render_state->fog_intensity / 255.f;
  fs_params.game_sizes[0] = 512.f;
  fs_params.game_sizes[1] = (float)game_height[render_state->version];
  fs_params.game_sizes[2] = (float)render_state->game_res_w;
  fs_params.game_sizes[3] = (float)render_state->game_res_h;
  fs_params.alpha_min = alpha_min;
  fs_params.alpha_max = alpha_max;
  fs_params.color_mult = m_color_mult;
  fs_params.alpha_mult = m_alpha_mult;
  fs_params.ta0 = m_prim_state.ta0 / 255.f;
  fs_params.scissor_enable = m_scissor_enable ? 1 : 0;
  fs_params.greater = greater;

  id<MTLRenderCommandEncoder> enc = ctx.enc;
  id<MTLRenderPipelineState> pso = ctx.pso_cache->get_pipeline(pso_key);
  ASSERT(pso);
  [enc setRenderPipelineState:pso];
  [enc setVertexBuffer:vbuf offset:voffset atIndex:0];
  [enc setVertexBytes:&vs_params length:sizeof(vs_params) atIndex:1];
  [enc setFragmentBytes:&fs_params length:sizeof(fs_params) atIndex:0];
  // the constant-alpha blend mode reads the fix value from the blend color
  [enc setBlendColorRed:0.f green:0.f blue:0.f alpha:m_blend_state.fix / 127.f];

  if (pso_key.shader == MetalShaderId::DIRECT_TEXTURED) {
    // DirectRenderer::update_gl_texture, unit 0 (TEXTURE_STATE_COUNT == 1)
    std::optional<u64> tex;
    if (batch_tex.used) {
      if (batch_tex.using_mt4hh) {
        tex = render_state->texture_pool->lookup_mt4hh(batch_tex.texture_base_ptr);
      } else {
        tex = render_state->texture_pool->lookup(batch_tex.texture_base_ptr);
      }
    }
    last_batch.texture_lookup_hit = tex.has_value();
    const u64 placeholder = render_state->texture_pool->get_placeholder_texture();
    if (!tex) {
      lg::warn("Metal direct {}: failed to find texture at {}, using placeholder", m_name,
               batch_tex.texture_base_ptr);
      tex = placeholder;
    }
    last_batch.used_placeholder = *tex == placeholder;
    id<MTLTexture> mtl_tex = metal_texture_lookup(*tex);
    if (!mtl_tex) {
      last_batch.used_placeholder = true;
      mtl_tex = metal_texture_lookup(placeholder);
    }
    ASSERT(mtl_tex);

    MetalSamplerKey sampler_key;
    // the GL renderer runs with mipmaps disabled for direct (disable_mipmap
    // debug default), so the mip filter stays off here too
    sampler_key.min_filter = batch_tex.enable_tex_filt ? MTLSamplerMinMagFilterLinear
                                                       : MTLSamplerMinMagFilterNearest;
    sampler_key.mag_filter = sampler_key.min_filter;
    sampler_key.wrap_s = batch_tex.m_clamp_state.clamp_s ? MTLSamplerAddressModeClampToEdge
                                                         : MTLSamplerAddressModeRepeat;
    sampler_key.wrap_t = batch_tex.m_clamp_state.clamp_t ? MTLSamplerAddressModeClampToEdge
                                                         : MTLSamplerAddressModeRepeat;
    [enc setFragmentTexture:mtl_tex atIndex:0];
    [enc setFragmentSamplerState:ctx.sampler_cache->get(sampler_key) atIndex:0];
  }

  int draw_count = 0;
  int num_tris = 0;

  if (m_test_state_needs_double_draw && pso_key.shader == MetalShaderId::DIRECT_TEXTURED) {
    // afail FB_ONLY/RGB_ONLY: draw alpha-passing fragments with depth writes,
    // then failing fragments without them (DirectRenderer::flush_pending)
    int n_batch = m_prim_buffer.vert_count;
    if (n_batch > 50 && n_batch < 700 && (n_batch % 2) == 0) {
      n_batch = n_batch / 2;
    }
    int offset = 0;
    while (offset < m_prim_buffer.vert_count) {
      [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(test_to_depth_key(true, true))];
      fs_params.alpha_min = m_double_draw_aref;
      fs_params.alpha_max = 10.f;
      [enc setFragmentBytes:&fs_params length:sizeof(fs_params) atIndex:0];
      [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:offset vertexCount:n_batch];

      [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(test_to_depth_key(false, true))];
      fs_params.alpha_min = -10.f;
      fs_params.alpha_max = m_double_draw_aref;
      [enc setFragmentBytes:&fs_params length:sizeof(fs_params) atIndex:0];
      [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:offset vertexCount:n_batch];

      offset += n_batch;
      draw_count += 2;
      num_tris += n_batch / 3;
    }
    m_test_state_needs_double_draw = false;
  } else {
    [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(test_to_depth_key(false, false))];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle
            vertexStart:0
            vertexCount:m_prim_buffer.vert_count];
    num_tris += m_prim_buffer.vert_count / 3;
    draw_count++;
  }

  ctx.draw_calls += draw_count;
  ctx.triangles += num_tris;
  m_stats.draw_calls += draw_count;
  m_stats.triangles += num_tris;
  if (last_batch.textured) {
    m_stats.textured_draw_calls += draw_count;
    if (last_batch.used_placeholder) {
      m_stats.missing_texture_draw_calls += draw_count;
    }
  }
  m_prim_buffer.vert_count = 0;
}

/*!
 * Render VIF data: walk forward looking for DIRECTs, skipping nops/flushes.
 * Identical to DirectRenderer::render_vif.
 */
void MetalDirectRenderer::render_vif(u32 vif0,
                                     u32 vif1,
                                     const u8* data,
                                     u32 size,
                                     MetalSharedRenderState* render_state,
                                     MetalFrameContext& ctx) {
  u32 gif_qwc = get_direct_qwc_or_nop(VifCode(vif0));
  if (gif_qwc) {
    ASSERT(get_direct_qwc_or_nop(VifCode(vif1)) == 0);
  } else {
    gif_qwc = get_direct_qwc_or_nop(VifCode(vif1));
  }

  u32 offset_into_data = 0;
  while (offset_into_data < size) {
    if (gif_qwc) {
      if (offset_into_data & 0xf) {
        // not aligned. should get nops.
        u32 vif;
        memcpy(&vif, data + offset_into_data, 4);
        offset_into_data += 4;
        ASSERT(get_direct_qwc_or_nop(VifCode(vif)) == 0);
      } else {
        // aligned! do a gif transfer!
        render_gif(data + offset_into_data, gif_qwc * 16, render_state, ctx);
        offset_into_data += gif_qwc * 16;
      }
    } else {
      // we are reading VIF data.
      u32 vif;
      memcpy(&vif, data + offset_into_data, 4);
      offset_into_data += 4;
      gif_qwc = get_direct_qwc_or_nop(VifCode(vif));
    }
  }
}

/*!
 * Render GIF data. Identical GIF tag walk to DirectRenderer::render_gif; the
 * IMAGE format (GS blit path) is Jak 2/3 territory and asserts like the GL
 * renderer's blit machinery entry points.
 */
void MetalDirectRenderer::render_gif(const u8* data,
                                     u32 size,
                                     MetalSharedRenderState* render_state,
                                     MetalFrameContext& ctx) {
  if (size != UINT32_MAX) {
    ASSERT(size >= 16);
  }

  bool eop = false;
  u32 offset = 0;
  while (!eop) {
    if (size != UINT32_MAX) {
      ASSERT(offset < size);
    }
    GifTag tag(data + offset);
    offset += 16;
    eop = tag.eop();

    GifTag::RegisterDescriptor reg_desc[16];
    u32 nreg = tag.nreg();
    for (u32 i = 0; i < nreg; i++) {
      reg_desc[i] = tag.reg(i);
    }

    auto format = tag.flg();
    if (format == GifTag::Format::PACKED) {
      if (tag.pre()) {
        handle_prim(tag.prim(), render_state, ctx);
      }
      for (u32 loop = 0; loop < tag.nloop(); loop++) {
        for (u32 reg = 0; reg < nreg; reg++) {
          switch (reg_desc[reg]) {
            case GifTag::RegisterDescriptor::AD:
              handle_ad(data + offset, render_state, ctx);
              break;
            case GifTag::RegisterDescriptor::ST:
              handle_st_packed(data + offset);
              break;
            case GifTag::RegisterDescriptor::RGBAQ:
              handle_rgbaq_packed(data + offset);
              break;
            case GifTag::RegisterDescriptor::XYZF2:
              handle_xyzf2_packed(data + offset, render_state, ctx);
              break;
            case GifTag::RegisterDescriptor::XYZ2:
              handle_xyz2_packed(data + offset, render_state, ctx);
              break;
            case GifTag::RegisterDescriptor::PRIM:
              handle_prim_packed(data + offset, render_state, ctx);
              break;
            case GifTag::RegisterDescriptor::TEX0_1:
              handle_tex0_1_packed(data + offset);
              break;
            case GifTag::RegisterDescriptor::UV:
              handle_uv_packed(data + offset);
              break;
            default:
              ASSERT_MSG(false, fmt::format("Register {} is not supported in packed mode yet",
                                            reg_descriptor_name(reg_desc[reg])));
          }
          offset += 16;  // PACKED = quadwords
        }
      }
    } else if (format == GifTag::Format::REGLIST) {
      for (u32 loop = 0; loop < tag.nloop(); loop++) {
        for (u32 reg = 0; reg < nreg; reg++) {
          u64 register_data;
          memcpy(&register_data, data + offset, 8);
          switch (reg_desc[reg]) {
            case GifTag::RegisterDescriptor::PRIM:
              handle_prim(register_data, render_state, ctx);
              break;
            case GifTag::RegisterDescriptor::RGBAQ:
              handle_rgbaq(register_data);
              break;
            case GifTag::RegisterDescriptor::XYZF2:
              handle_xyzf2(register_data, render_state, ctx);
              break;
            default:
              ASSERT_MSG(false, fmt::format("Register {} is not supported in reglist mode yet",
                                            reg_descriptor_name(reg_desc[reg])));
          }
          offset += 8;  // REGLIST = doublewords
        }
      }
    } else {
      // IMAGE is only produced by the GS blit path (Jak 2/3), which the GL
      // renderer also rejects at its entry points
      ASSERT_MSG(false, "Metal direct: unsupported GIF format");
    }
  }

  if (size != UINT32_MAX) {
    if ((offset + 15) / 16 != size / 16) {
      ASSERT_MSG(false, fmt::format("MetalDirectRenderer size failed in {}. expected: {}, got: {}",
                                    m_name, size, offset));
    }
  }
}

void MetalDirectRenderer::handle_ad(const u8* data,
                                    MetalSharedRenderState* render_state,
                                    MetalFrameContext& ctx) {
  u64 value;
  GsRegisterAddress addr;
  memcpy(&value, data, sizeof(u64));
  memcpy(&addr, data + 8, sizeof(GsRegisterAddress));

  switch (addr) {
    case GsRegisterAddress::ZBUF_1:
      handle_zbuf1(value, render_state, ctx);
      break;
    case GsRegisterAddress::TEST_1:
      handle_test1(value, render_state, ctx);
      break;
    case GsRegisterAddress::ALPHA_1:
      handle_alpha1(value, render_state, ctx);
      break;
    case GsRegisterAddress::PABE:
      handle_pabe(value);
      break;
    case GsRegisterAddress::CLAMP_1:
      handle_clamp1(value);
      break;
    case GsRegisterAddress::PRIM:
      handle_prim(value, render_state, ctx);
      break;
    case GsRegisterAddress::TEX1_1:
      handle_tex1_1(value);
      break;
    case GsRegisterAddress::TEXA:
      handle_texa(value, render_state, ctx);
      break;
    case GsRegisterAddress::TEXCLUT:
      // texture upload handles CLUTs; nothing to do here (see GL renderer)
      break;
    case GsRegisterAddress::FOGCOL:
      // TODO (also TODO in the GL renderer)
      break;
    case GsRegisterAddress::TEX0_1:
      handle_tex0_1(value);
      break;
    case GsRegisterAddress::MIPTBP1_1:
    case GsRegisterAddress::MIPTBP2_1:
      // mip level addresses; direct rendering runs without mipmaps
      break;
    case GsRegisterAddress::TEXFLUSH:
      break;
    case GsRegisterAddress::FRAME_1:
      // ignored (the GL base DirectRenderer ignores it too)
      break;
    case GsRegisterAddress::RGBAQ: {  // shadow scissor does this
      m_prim_building.rgba_reg[0] = data[0];
      m_prim_building.rgba_reg[1] = data[1];
      m_prim_building.rgba_reg[2] = data[2];
      m_prim_building.rgba_reg[3] = data[3];
      memcpy(&m_prim_building.Q, data + 4, 4);
    } break;
    case GsRegisterAddress::SCISSOR_1:
      handle_scissor(value);
      break;
    case GsRegisterAddress::XYOFFSET_1:
      ASSERT(render_state->version >= GameVersion::Jak2);  // Jak 1 offsets are baked in the shader
      {
        GsXYOffset xyo(value);
        float scale = -65536;
        m_prim_buffer.x_off = scale * ((s32)xyo.ofx() - 0x7000) / float(UINT32_MAX);
        m_prim_buffer.y_off = scale * ((s32)xyo.ofy() - 0x7300) / float(UINT32_MAX);
      }
      break;
    case GsRegisterAddress::COLCLAMP:
      ASSERT(value == 1);
      break;
    case GsRegisterAddress::BITBLTBUF:
    case GsRegisterAddress::TRXPOS:
    case GsRegisterAddress::TRXREG:
    case GsRegisterAddress::TRXDIR:
      // GS blit path; the GL renderer asserts on these too
      ASSERT_MSG(false, fmt::format("Metal direct: GS blit register {} not supported",
                                    register_address_name(addr)));
      break;
    default:
      ASSERT_MSG(false, fmt::format("Address {} is not supported", register_address_name(addr)));
  }
}

void MetalDirectRenderer::handle_scissor(u64 val) {
  m_scissor.scax0 = (val >> 0) & 0x7ff;
  m_scissor.scax1 = (val >> 16) & 0x7ff;
  m_scissor.scay0 = (val >> 32) & 0x7ff;
  m_scissor.scay1 = (val >> 48) & 0x7ff;
  m_scissor_enable = true;
}

void MetalDirectRenderer::handle_tex1_1(u64 val) {
  GsTex1 reg(val);
  // no mipmapping in direct (same simplification as the GL renderer)
  bool want_tex_filt = reg.mmag();
  if (want_tex_filt != m_tex_state_from_reg.enable_tex_filt) {
    m_tex_state_from_reg.enable_tex_filt = want_tex_filt;
    m_current_tex_state_idx = -1;
  }
}

void MetalDirectRenderer::handle_tex0_1_packed(const u8* data) {
  u64 val;
  memcpy(&val, data, sizeof(u64));
  handle_tex0_1(val);
}

void MetalDirectRenderer::handle_tex0_1(u64 val) {
  GsTex0 reg(val);
  if (m_tex_state_from_reg.current_register != reg) {
    m_tex_state_from_reg.texture_base_ptr = reg.tbp0();
    m_tex_state_from_reg.using_mt4hh = reg.psm() == GsTex0::PSM::PSMT4HH;
    m_tex_state_from_reg.current_register = reg;
    m_tex_state_from_reg.tcc = reg.tcc();
    m_tex_state_from_reg.decal = reg.tfx() == GsTex0::TextureFunction::DECAL;
    ASSERT(reg.tfx() == GsTex0::TextureFunction::DECAL ||
           reg.tfx() == GsTex0::TextureFunction::MODULATE);
    m_current_tex_state_idx = -1;
  }
}

void MetalDirectRenderer::handle_st_packed(const u8* data) {
  memcpy(&m_prim_building.st_reg.x(), data + 0, 4);
  memcpy(&m_prim_building.st_reg.y(), data + 4, 4);
  memcpy(&m_prim_building.Q, data + 8, 4);
}

void MetalDirectRenderer::handle_uv_packed(const u8* data) {
  u32 u, v;
  memcpy(&u, data, 4);
  memcpy(&v, data + 4, 4);
  m_prim_building.st_reg.x() = u;
  m_prim_building.st_reg.y() = v;
  m_prim_building.Q = 1;
}

void MetalDirectRenderer::handle_rgbaq_packed(const u8* data) {
  m_prim_building.rgba_reg[0] = data[0];
  m_prim_building.rgba_reg[1] = data[4];
  m_prim_building.rgba_reg[2] = data[8];
  m_prim_building.rgba_reg[3] = data[12];
}

void MetalDirectRenderer::handle_xyzf2_packed(const u8* data,
                                              MetalSharedRenderState* render_state,
                                              MetalFrameContext& ctx) {
  u32 x, y;
  memcpy(&x, data, 4);
  memcpy(&y, data + 4, 4);

  u64 upper;
  memcpy(&upper, data + 8, 8);
  u32 z = (upper >> 4) & 0xffffff;

  u8 f = (upper >> 36);
  bool adc = upper & (1ull << 47);
  handle_xyzf2_common(x << 16, y << 16, z, f, render_state, ctx, !adc);
}

void MetalDirectRenderer::handle_xyz2_packed(const u8* data,
                                             MetalSharedRenderState* render_state,
                                             MetalFrameContext& ctx) {
  u32 x, y;
  memcpy(&x, data, 4);
  memcpy(&y, data + 4, 4);

  u64 upper;
  memcpy(&upper, data + 8, 8);
  u32 z = upper;

  bool adc = upper & (1ull << 47);
  handle_xyzf2_common(x << 16, y << 16, z, 0, render_state, ctx, !adc);
}

void MetalDirectRenderer::handle_zbuf1(u64 val,
                                       MetalSharedRenderState* render_state,
                                       MetalFrameContext& ctx) {
  // a single z buffer, always configured the same way: 24-bit, at a fixed zbp
  GsZbuf x(val);
  ASSERT(x.psm() == TextureFormat::PSMZ24);
  ASSERT(x.zbp() == normal_zbp[render_state->version]);

  bool write = !x.zmsk();
  if (write != m_test_state.depth_writes) {
    m_stats.flush_from_zbuf++;
    flush_pending(render_state, ctx);
    m_test_state.depth_writes = write;
  }
}

void MetalDirectRenderer::handle_test1(u64 val,
                                       MetalSharedRenderState* render_state,
                                       MetalFrameContext& ctx) {
  GsTest reg(val);
  ASSERT(!reg.date());
  if (m_test_state.current_register != reg) {
    m_stats.flush_from_test++;
    flush_pending(render_state, ctx);
    m_test_state.from_register(reg);
  }
}

void MetalDirectRenderer::handle_texa(u64 val,
                                      MetalSharedRenderState* render_state,
                                      MetalFrameContext& ctx) {
  GsTexa reg(val);
  if (m_prim_state.ta0 != reg.ta0()) {
    m_stats.flush_from_ta0++;
    flush_pending(render_state, ctx);
    m_prim_state.ta0 = reg.ta0();
  }
  ASSERT(reg.ta1() == 0x80);  // note: check rgba16_to_rgba32 if this changes
  ASSERT(reg.aem() == false);
}

void MetalDirectRenderer::handle_alpha1(u64 val,
                                        MetalSharedRenderState* render_state,
                                        MetalFrameContext& ctx) {
  GsAlpha reg(val);
  if (m_blend_state.current_register != reg) {
    m_stats.flush_from_alpha++;
    flush_pending(render_state, ctx);
    m_blend_state.from_register(reg);
  }
}

void MetalDirectRenderer::handle_pabe(u64 val) {
  ASSERT(val == 0);  // not really sure how to handle this yet (same as GL)
}

void MetalDirectRenderer::handle_clamp1(u64 val) {
  if (m_tex_state_from_reg.m_clamp_state.current_register != val) {
    m_current_tex_state_idx = -1;
    m_tex_state_from_reg.m_clamp_state.current_register = val;
    m_tex_state_from_reg.m_clamp_state.clamp_s = val & 0b001;
    m_tex_state_from_reg.m_clamp_state.clamp_t = val & 0b100;
  }
}

void MetalDirectRenderer::handle_prim_packed(const u8* data,
                                             MetalSharedRenderState* render_state,
                                             MetalFrameContext& ctx) {
  u64 val;
  memcpy(&val, data, sizeof(u64));
  handle_prim(val, render_state, ctx);
}

void MetalDirectRenderer::handle_prim(u64 val,
                                      MetalSharedRenderState* render_state,
                                      MetalFrameContext& ctx) {
  if (m_prim_building.tri_strip_startup) {
    m_prim_building.tri_strip_startup = 0;
    m_prim_building.building_idx = 0;
  } else {
    if (m_prim_building.building_idx > 0) {
      ASSERT(false);  // shouldn't leave any half-finished prims
    }
  }

  GsPrim prim(val);
  if (m_prim_state.current_register != prim || m_blend_state.alpha_blend_enable != prim.abe()) {
    m_stats.flush_from_prim++;
    flush_pending(render_state, ctx);
    m_prim_state.from_register(prim);
    m_blend_state.alpha_blend_enable = prim.abe();
  }

  m_prim_building.kind = prim.kind();
}

void MetalDirectRenderer::handle_rgbaq(u64 val) {
  ASSERT((val >> 32) == 0);  // q = 0
  memcpy(m_prim_building.rgba_reg.data(), &val, 4);
}

int MetalDirectRenderer::get_texture_unit_for_current_reg(MetalSharedRenderState* render_state,
                                                          MetalFrameContext& ctx) {
  if (m_current_tex_state_idx != -1) {
    return m_current_tex_state_idx;
  }

  if (m_next_free_tex_state >= TEXTURE_STATE_COUNT) {
    m_stats.flush_from_state_exhaust++;
    flush_pending(render_state, ctx);
    return get_texture_unit_for_current_reg(render_state, ctx);
  } else {
    ASSERT(!m_buffered_tex_state[m_next_free_tex_state].used);
    m_buffered_tex_state[m_next_free_tex_state] = m_tex_state_from_reg;
    m_buffered_tex_state[m_next_free_tex_state].used = true;
    m_current_tex_state_idx = m_next_free_tex_state++;
    return m_current_tex_state_idx;
  }
}

void MetalDirectRenderer::handle_xyzf2_common(u32 x,
                                              u32 y,
                                              u32 z,
                                              u8 f,
                                              MetalSharedRenderState* render_state,
                                              MetalFrameContext& ctx,
                                              bool advance) {
  if (m_prim_buffer.is_full()) {
    lg::warn("Metal direct buffer wrapped in {} ({} verts)", m_name, m_prim_buffer.vert_count);
    flush_pending(render_state, ctx);
  }

  m_prim_building.building_stq.at(m_prim_building.building_idx) = math::Vector<float, 3>(
      m_prim_building.st_reg.x(), m_prim_building.st_reg.y(), m_prim_building.Q);
  m_prim_building.building_rgba.at(m_prim_building.building_idx) = m_prim_building.rgba_reg;
  m_prim_building.building_vert.at(m_prim_building.building_idx) = math::Vector<u32, 4>{x, y, z, f};

  m_prim_building.building_idx++;

  int tex_unit = get_texture_unit_for_current_reg(render_state, ctx);
  bool tcc = m_buffered_tex_state[tex_unit].tcc;
  bool decal = m_buffered_tex_state[tex_unit].decal;
  bool fge = m_prim_state.fogging_enable;
  bool use_uv = m_prim_state.use_uv;

  math::Vector<float, 4> scissor(m_scissor.scax0, m_scissor.scax1, m_scissor.scay0,
                                 m_scissor.scay1);

  switch (m_prim_building.kind) {
    case GsPrim::Kind::SPRITE: {
      if (m_prim_building.building_idx == 2) {
        // build triangles from the sprite
        auto& corner1_vert = m_prim_building.building_vert[0];
        auto& corner1_rgba = m_prim_building.building_rgba[0];
        auto& corner2_vert = m_prim_building.building_vert[1];
        auto& corner2_rgba = m_prim_building.building_rgba[1];
        auto& corner1_stq = m_prim_building.building_stq[0];
        auto& corner2_stq = m_prim_building.building_stq[1];

        // use the most recent vertex z
        math::Vector<u32, 4> corner3_vert{corner1_vert[0], corner2_vert[1], corner2_vert[2], 0};
        math::Vector<u32, 4> corner4_vert{corner2_vert[0], corner1_vert[1], corner2_vert[2], 0};
        math::Vector<float, 3> corner3_stq{corner1_stq[0], corner2_stq[1], corner2_stq[2]};
        math::Vector<float, 3> corner4_stq{corner2_stq[0], corner1_stq[1], corner2_stq[2]};

        if (m_prim_state.gouraud_enable) {
          ASSERT(false);
        }
        auto& corner3_rgba = corner2_rgba;
        auto& corner4_rgba = corner2_rgba;

        m_prim_buffer.push(corner1_rgba, corner1_vert, corner1_stq, scissor, 0, tcc, decal, fge,
                           use_uv);
        m_prim_buffer.push(corner3_rgba, corner3_vert, corner3_stq, scissor, 0, tcc, decal, fge,
                           use_uv);
        m_prim_buffer.push(corner2_rgba, corner2_vert, corner2_stq, scissor, 0, tcc, decal, fge,
                           use_uv);
        m_prim_buffer.push(corner2_rgba, corner2_vert, corner2_stq, scissor, 0, tcc, decal, fge,
                           use_uv);
        m_prim_buffer.push(corner4_rgba, corner4_vert, corner4_stq, scissor, 0, tcc, decal, fge,
                           use_uv);
        m_prim_buffer.push(corner1_rgba, corner1_vert, corner1_stq, scissor, 0, tcc, decal, fge,
                           use_uv);
        m_prim_building.building_idx = 0;
      }
    } break;
    case GsPrim::Kind::TRI_STRIP: {
      if (m_prim_building.building_idx == 3) {
        m_prim_building.building_idx = 0;
      }
      if (m_prim_building.tri_strip_startup < 3) {
        m_prim_building.tri_strip_startup++;
      }
      if (m_prim_building.tri_strip_startup >= 3) {
        if (advance) {
          for (int i = 0; i < 3; i++) {
            m_prim_buffer.push(m_prim_building.building_rgba[i], m_prim_building.building_vert[i],
                               m_prim_building.building_stq[i], scissor, tex_unit, tcc, decal, fge,
                               use_uv);
          }
        }
      }
    } break;

    case GsPrim::Kind::TRI:
      if (m_prim_building.building_idx == 3) {
        m_prim_building.building_idx = 0;
        for (int i = 0; i < 3; i++) {
          m_prim_buffer.push(m_prim_building.building_rgba[i], m_prim_building.building_vert[i],
                             m_prim_building.building_stq[i], scissor, tex_unit, tcc, decal, fge,
                             use_uv);
        }
      }
      break;

    case GsPrim::Kind::TRI_FAN: {
      if (m_prim_building.tri_strip_startup < 2) {
        m_prim_building.tri_strip_startup++;
      } else {
        if (m_prim_building.building_idx == 2) {
          // nothing.
        } else if (m_prim_building.building_idx == 3) {
          m_prim_building.building_idx = 1;
        }
        for (int i = 0; i < 3; i++) {
          m_prim_buffer.push(m_prim_building.building_rgba[i], m_prim_building.building_vert[i],
                             m_prim_building.building_stq[i], scissor, tex_unit, tcc, decal, fge,
                             use_uv);
        }
      }
    } break;

    case GsPrim::Kind::LINE: {
      if (m_prim_building.building_idx == 2) {
        math::Vector<double, 3> pt0 = m_prim_building.building_vert[0].xyz().cast<double>();
        math::Vector<double, 3> pt1 = m_prim_building.building_vert[1].xyz().cast<double>();
        auto normal = (pt1 - pt0).normalized().cross(math::Vector<double, 3>{0, 0, 1});

        double line_width = (1 << 19);
        math::Vector<double, 3> a = pt0 + normal * line_width;
        math::Vector<double, 3> b = pt1 + normal * line_width;
        math::Vector<double, 3> c = pt0 - normal * line_width;
        math::Vector<double, 3> d = pt1 - normal * line_width;
        math::Vector<u32, 4> ai{(u32)a.x(), (u32)a.y(), (u32)a.z(), 0};
        math::Vector<u32, 4> bi{(u32)b.x(), (u32)b.y(), (u32)b.z(), 0};
        math::Vector<u32, 4> ci{(u32)c.x(), (u32)c.y(), (u32)c.z(), 0};
        math::Vector<u32, 4> di{(u32)d.x(), (u32)d.y(), (u32)d.z(), 0};

        // ACB:
        m_prim_buffer.push(m_prim_building.building_rgba[0], ai, {}, scissor, 0, false, false,
                           false, false);
        m_prim_buffer.push(m_prim_building.building_rgba[0], ci, {}, scissor, 0, false, false,
                           false, false);
        m_prim_buffer.push(m_prim_building.building_rgba[1], bi, {}, scissor, 0, false, false,
                           false, false);
        // BCD:
        m_prim_buffer.push(m_prim_building.building_rgba[1], bi, {}, scissor, 0, false, false,
                           false, false);
        m_prim_buffer.push(m_prim_building.building_rgba[0], ci, {}, scissor, 0, false, false,
                           false, false);
        m_prim_buffer.push(m_prim_building.building_rgba[1], di, {}, scissor, 0, false, false,
                           false, false);

        m_prim_building.building_idx = 0;
      }
    } break;

    case GsPrim::Kind::LINE_STRIP: {
      if (m_prim_building.building_idx == 2) {
        m_prim_building.building_idx = 0;
      }
      if (m_prim_building.tri_strip_startup < 2) {
        m_prim_building.tri_strip_startup++;
      }
      if (m_prim_building.tri_strip_startup >= 2) {
        if (advance) {
          math::Vector<double, 3> pt0 = m_prim_building.building_vert[0].xyz().cast<double>();
          math::Vector<double, 3> pt1 = m_prim_building.building_vert[1].xyz().cast<double>();
          auto normal = (pt1 - pt0).normalized().cross(math::Vector<double, 3>{0, 0, 1});

          double line_width = (1 << 19);
          math::Vector<double, 3> a = pt0 + normal * line_width;
          math::Vector<double, 3> b = pt1 + normal * line_width;
          math::Vector<double, 3> c = pt0 - normal * line_width;
          math::Vector<double, 3> d = pt1 - normal * line_width;
          math::Vector<u32, 4> ai{(u32)a.x(), (u32)a.y(), (u32)a.z(), 0};
          math::Vector<u32, 4> bi{(u32)b.x(), (u32)b.y(), (u32)b.z(), 0};
          math::Vector<u32, 4> ci{(u32)c.x(), (u32)c.y(), (u32)c.z(), 0};
          math::Vector<u32, 4> di{(u32)d.x(), (u32)d.y(), (u32)d.z(), 0};

          // ACB:
          m_prim_buffer.push(m_prim_building.building_rgba[0], ai, {}, scissor, 0, false, false,
                             false, false);
          m_prim_buffer.push(m_prim_building.building_rgba[0], ci, {}, scissor, 0, false, false,
                             false, false);
          m_prim_buffer.push(m_prim_building.building_rgba[1], bi, {}, scissor, 0, false, false,
                             false, false);
          // BCD:
          m_prim_buffer.push(m_prim_building.building_rgba[1], bi, {}, scissor, 0, false, false,
                             false, false);
          m_prim_buffer.push(m_prim_building.building_rgba[0], ci, {}, scissor, 0, false, false,
                             false, false);
          m_prim_buffer.push(m_prim_building.building_rgba[1], di, {}, scissor, 0, false, false,
                             false, false);
        }
      }
    } break;

    default:
      ASSERT_MSG(false, fmt::format("prim type {} is unsupported in Metal direct {}.",
                                    (int)m_prim_building.kind, m_name));
  }
}

void MetalDirectRenderer::handle_xyzf2(u64 val,
                                       MetalSharedRenderState* render_state,
                                       MetalFrameContext& ctx) {
  u32 x = val & 0xffff;
  u32 y = (val >> 16) & 0xffff;
  u32 z = (val >> 32) & 0xffffff;
  u32 f = (val >> 56) & 0xff;

  handle_xyzf2_common(x << 16, y << 16, z, f, render_state, ctx, true);
}

void MetalDirectRenderer::TestState::from_register(GsTest reg) {
  current_register = reg;
  alpha_test_enable = reg.alpha_test_enable();
  if (alpha_test_enable) {
    alpha_test = reg.alpha_test();
    aref = reg.aref();
    afail = reg.afail();
  }

  date = reg.date();
  if (date) {
    datm = reg.datm();
  }

  zte = reg.zte();
  ztst = reg.ztest();
}

void MetalDirectRenderer::BlendState::from_register(GsAlpha reg) {
  current_register = reg;
  a = reg.a_mode();
  b = reg.b_mode();
  c = reg.c_mode();
  d = reg.d_mode();
  fix = reg.fix();
}

void MetalDirectRenderer::PrimState::from_register(GsPrim reg) {
  current_register = reg;
  gouraud_enable = reg.gouraud();
  texture_enable = reg.tme();
  fogging_enable = reg.fge();
  aa_enable = reg.aa1();
  use_uv = reg.fst();
  ctxt = reg.ctxt();
  fix = reg.fix();
}

MetalDirectRenderer::PrimitiveBuffer::PrimitiveBuffer(int max_triangles) {
  vertices.resize(max_triangles * 3);
  max_verts = max_triangles * 3;
}

void MetalDirectRenderer::PrimitiveBuffer::push(const math::Vector<u8, 4>& rgba,
                                                const math::Vector<u32, 4>& vert,
                                                const math::Vector<float, 3>& stq,
                                                const math::Vector<float, 4>& scissor,
                                                int unit,
                                                bool tcc,
                                                bool decal,
                                                bool fog_enable,
                                                bool use_uv) {
  auto& v = vertices[vert_count];
  v.rgba = rgba;
  v.xyzf[0] = (float)vert[0] / (float)UINT32_MAX;
  v.xyzf[0] += x_off;
  v.xyzf[1] = (float)vert[1] / (float)UINT32_MAX;
  v.xyzf[1] += y_off;
  v.xyzf[2] = (float)vert[2] / (float)0xffffff;
  v.xyzf[3] = (float)vert[3];
  v.stq = stq;
  v.tex_unit = unit;
  v.tcc = tcc;
  v.decal = decal;
  v.fog_enable = fog_enable;
  v.use_uv = use_uv;
  v.scissor = scissor;
  vert_count++;
}
