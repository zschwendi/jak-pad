#include "metal_sky_renderer.h"

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/opengl_renderer/AdgifHandler.h"
#include "game/graphics/texture/TexturePool.h"

namespace {

// Scalar equivalents of the GL SkyBlendCPU SSE kernels (which are x86-only and
// no-ops on arm64). Fixed-point semantics match: (texel * intensity) >> 7 with
// unsigned-saturating math.

void blend_sky_initial(u8 intensity, u8* out, const u8* in, u32 size) {
  for (u32 i = 0; i < size; i++) {
    u32 val = ((u32)in[i] * intensity) >> 7;
    out[i] = (u8)std::min<u32>(val, 255);
  }
}

void blend_sky_accumulate(u8 intensity, u8* out, const u8* in, u32 size) {
  for (u32 i = 0; i < size; i++) {
    u32 val = ((u32)in[i] * intensity) >> 7;
    val = std::min<u32>(val, 255);
    out[i] = (u8)std::min<u32>(out[i] + val, 255);
  }
}

}  // namespace

MetalSkyBlendCPU::MetalSkyBlendCPU(id<MTLDevice> device) {
  for (int i = 0; i < 2; i++) {
    auto* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:m_sizes[i]
                                                          height:m_sizes[i]
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    m_textures[i].texture = [device newTextureWithDescriptor:desc];
    m_textures[i].handle = metal_texture_register(m_textures[i].texture);
    m_texture_data[i].resize(4 * m_sizes[i] * m_sizes[i]);
  }
}

void MetalSkyBlendCPU::init_textures(TexturePool& tex_pool, GameVersion version) {
  for (int i = 0; i < 2; i++) {
    TextureInput in;
    in.gpu_texture = m_textures[i].handle;
    in.w = m_sizes[i];
    in.h = m_sizes[i];
    in.debug_name = fmt::format("PC-SKY-CPU-{}", i);
    in.id = tex_pool.allocate_pc_port_texture(version);
    u32 tbp = SKY_TEXTURE_VRAM_ADDRS[i];
    m_textures[i].pool_tex = tex_pool.give_texture_and_load_to_vram(in, tbp);
    m_textures[i].tbp = tbp;
  }
}

/*!
 * Same DMA walk and blend decisions as SkyBlendCPU::do_sky_blends.
 */
SkyBlendStats MetalSkyBlendCPU::do_sky_blends(DmaFollower& dma,
                                              MetalSharedRenderState* render_state) {
  SkyBlendStats stats;

  while (dma.current_tag().qwc == 6) {
    // assuming that the vif and gif-tag is correct
    auto setup_data = dma.read_and_advance();

    // first is an adgif
    AdgifHelper adgif(setup_data.data + 16);
    ASSERT(adgif.is_normal_adgif());
    ASSERT(adgif.alpha().data == 0x8000000068);  // Cs + Cd

    // next is the actual draw
    auto draw_data = dma.read_and_advance();
    ASSERT(draw_data.size_bytes == 6 * 16);

    GifTag draw_or_blend_tag(draw_data.data);

    // the first draw overwrites the previous frame's draw by disabling alpha blend (ABE = 0)
    bool is_first_draw = !GsPrim(draw_or_blend_tag.prim()).abe();

    // relying on the format of the drawing to get the alpha/offset
    u32 coord;
    u32 intensity;
    memcpy(&coord, draw_data.data + (5 * 16), 4);
    memcpy(&intensity, draw_data.data + 16, 4);

    // sky (small) or clouds (large), told apart by the drawing coordinates
    int buffer_idx = 0;
    if (coord == 0x200) {
      buffer_idx = 0;  // sky
    } else if (coord == 0x400) {
      buffer_idx = 1;  // clouds
    } else {
      ASSERT(false);  // bad data
    }

    // look up the source texture
    auto tex = render_state->texture_pool->lookup_gpu_texture(adgif.tex0().tbp0());
    ASSERT(tex);

    if (tex->get_data_ptr()) {
      if (m_texture_data[buffer_idx].size() == tex->data_size()) {
        ASSERT(intensity <= 128);
        if (is_first_draw) {
          blend_sky_initial(intensity, m_texture_data[buffer_idx].data(), tex->get_data_ptr(),
                            (u32)m_texture_data[buffer_idx].size());
        } else {
          blend_sky_accumulate(intensity, m_texture_data[buffer_idx].data(), tex->get_data_ptr(),
                               (u32)m_texture_data[buffer_idx].size());
        }
      }

      if (buffer_idx == 0) {
        is_first_draw ? stats.sky_draws++ : stats.sky_blends++;
      } else {
        is_first_draw ? stats.cloud_draws++ : stats.cloud_blends++;
      }

      [m_textures[buffer_idx].texture
          replaceRegion:MTLRegionMake2D(0, 0, m_sizes[buffer_idx], m_sizes[buffer_idx])
            mipmapLevel:0
              withBytes:m_texture_data[buffer_idx].data()
            bytesPerRow:m_sizes[buffer_idx] * 4];

      render_state->texture_pool->move_existing_to_vram(m_textures[buffer_idx].pool_tex,
                                                        m_textures[buffer_idx].tbp);
    }
  }

  return stats;
}

MetalSkyRenderer::MetalSkyRenderer(const std::string& name, int my_id)
    : MetalBucketRenderer(name, my_id), m_direct_renderer("sky-direct", my_id, 100) {}

/*!
 * Same DMA walk as SkyRenderer::render, drawing through the Metal
 * DirectRenderer.
 */
void MetalSkyRenderer::render(DmaFollower& dma,
                              MetalSharedRenderState* render_state,
                              MetalFrameContext& ctx) {
  m_direct_renderer.reset_state();

  // first thing should be a NEXT with two nops: the jump from buckets to sprite data
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0);
  ASSERT(data0.vif0() == 0);
  ASSERT(data0.size_bytes == 0);

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // sky renderer didn't run, just get out of here
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
    return;
  }

  auto setup_packet = dma.read_and_advance();
  ASSERT(setup_packet.size_bytes == 16 * 4);
  m_direct_renderer.render_gif(setup_packet.data, setup_packet.size_bytes, render_state, ctx);

  if (dma.current_tag().qwc == 5) {
    auto draw_setup_packet = dma.read_and_advance();
    m_direct_renderer.render_gif(draw_setup_packet.data, draw_setup_packet.size_bytes, render_state,
                                 ctx);
    // tex0: tbw = 1, th = 5, hw = 5, sky-base-block; mmag/mmin = 1; clamp; drawing.
    while (dma.current_tag().kind == DmaTag::Kind::CNT) {
      auto data = dma.read_and_advance();
      ASSERT(data.vifcode0().kind == VifCode::Kind::NOP);
      ASSERT(data.vifcode1().kind == VifCode::Kind::DIRECT);
      ASSERT(data.vifcode1().immediate == data.size_bytes / 16);
      m_direct_renderer.render_gif(data.data, data.size_bytes, render_state, ctx);
    }

    auto empty = dma.read_and_advance();
    ASSERT(empty.size_bytes == 0);
    ASSERT(empty.vif0() == 0);
    ASSERT(empty.vif1() == 0);

    ASSERT(dma.current_tag().kind == DmaTag::Kind::CALL);
    dma.read_and_advance();
    dma.read_and_advance();  // cnt
    ASSERT(dma.current_tag().kind == DmaTag::Kind::RET);
    dma.read_and_advance();  // ret
    dma.read_and_advance();  // next
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
  } else {
    while (dma.current_tag_offset() != render_state->next_bucket) {
      auto data = dma.read_and_advance();
      if (data.size_bytes) {
        m_direct_renderer.render_vif(data.vif0(), data.vif1(), data.data, data.size_bytes,
                                     render_state, ctx);
      }

      if (dma.current_tag_offset() == render_state->default_regs_buffer) {
        dma.read_and_advance();  // cnt
        ASSERT(dma.current_tag().kind == DmaTag::Kind::RET);
        dma.read_and_advance();  // ret
      }
    }
  }

  m_direct_renderer.flush_pending(render_state, ctx);
}

MetalSkyBlendHandler::MetalSkyBlendHandler(const std::string& name,
                                           int my_id,
                                           int level_id,
                                           std::shared_ptr<MetalSkyBlendCPU> shared_blender)
    : MetalBucketRenderer(name, my_id),
      m_shared_blender(shared_blender),
      m_tfrag_renderer(fmt::format("tfrag-{}", name),
                       my_id,
                       {tfrag3::TFragmentTreeKind::TRANS,
                        tfrag3::TFragmentTreeKind::LOWRES_TRANS},
                       level_id,
                       /*does_vis_copy=*/false,
                       /*child_mode=*/true) {}

/*!
 * Same DMA walk as SkyBlendHandler::render. The tfrag-trans content that
 * shares this bucket is drawn by the child MetalTFragment (TRANS /
 * LOWRES_TRANS trees), as the GL handler draws it with Tfrag3.
 */
void MetalSkyBlendHandler::render(DmaFollower& dma,
                                  MetalSharedRenderState* render_state,
                                  MetalFrameContext& ctx) {
  m_stats = {};
  // first thing should be a NEXT with two nops
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0);
  ASSERT(data0.vif0() == 0);
  ASSERT(data0.size_bytes == 0);

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run, get out of here
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
    return;
  }

  if (dma.current_tag().qwc != 8) {
    // no sky blends this frame: the bucket contains only tfrag-trans
    m_tfrag_renderer.render(dma, render_state, ctx);
    return;
  }

  // first is the set-display-gs-state
  auto set_display = dma.read_and_advance();
  ASSERT(set_display.size_bytes == 8 * 16);

  if (render_state->secondary_view) {
    // The primary already published this frame's blended sky/cloud textures. Consume the same
    // packet envelope without rewriting the shared texture-pool state for the second view.
    while (dma.current_tag().qwc == 6) {
      dma.read_and_advance();  // adgif setup
      dma.read_and_advance();  // draw or blend
    }
  } else {
    m_stats = m_shared_blender->do_sky_blends(dma, render_state);
  }

  auto reset_alpha = dma.read_and_advance();
  ASSERT(reset_alpha.size_bytes == 16 * 2);

  auto reset_gs = dma.read_and_advance();
  ASSERT(reset_gs.size_bytes == 16 * 8);

  auto empty = dma.read_and_advance();
  ASSERT(empty.size_bytes == 0);
  ASSERT(empty.vif0() == 0);
  ASSERT(empty.vif1() == 0);

  if (dma.current_tag().kind != DmaTag::Kind::CALL) {
    m_tfrag_renderer.render(dma, render_state, ctx);
  } else {
    dma.read_and_advance();
    dma.read_and_advance();  // cnt
    ASSERT(dma.current_tag().kind == DmaTag::Kind::RET);
    dma.read_and_advance();  // ret
    dma.read_and_advance();  // next
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
  }
}
