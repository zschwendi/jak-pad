#include "metal_ocean_renderer.h"

#include <algorithm>

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/texture/TexturePool.h"

namespace {

// Jak 1's generated-ocean-texture VRAM slot. Both ocean buckets publish their
// texture here; the last one of the frame wins (same as the GL renderer).
constexpr int OCEAN_TEX_TBP_JAK1 = 8160;

// Jak 2's slot, only referenced by the mid adgif handler's "is this the
// generated texture" test, which the GL renderer never gave a Jak 1 branch.
constexpr int OCEAN_TEX_TBP_JAK2 = 672;

constexpr float kGameHeightJak1 = 448.f;

// Must match OceanCommonParams in shaders/ocean.metal. MSL aligns the struct to
// its float4 member, so the tail is padded to a multiple of 16.
struct OceanCommonParams {
  float fog_color[4];
  int bucket;
  float scissor_adjust;
  float pad[2] = {0.f, 0.f};
};
static_assert(sizeof(OceanCommonParams) == 32);

// Mirror of OceanMidAndFar.cpp's is_end_tag.
bool mid_far_is_end_tag(const DmaTag& tag, const VifCode& v0, const VifCode& v1) {
  return tag.qwc == 0 && tag.kind == DmaTag::Kind::NEXT && v0.kind == VifCode::Kind::NOP &&
         v1.kind == VifCode::Kind::NOP;
}

// Mirror of the is_end_tag in OceanMid.cpp / OceanNear.cpp.
bool vu_loop_is_end_tag(const DmaTag& tag, const VifCode& v0, const VifCode& v1) {
  return tag.qwc == 2 && tag.kind == DmaTag::Kind::CNT && v0.kind == VifCode::Kind::NOP &&
         v1.kind == VifCode::Kind::DIRECT;
}

void reverse_indices(u32* indices, u32 count) {
  if (count) {
    for (u32 a = 0, b = count - 1; a < b; a++, b--) {
      std::swap(indices[a], indices[b]);
    }
  }
}

id<MTLTexture> make_ocean_target(id<MTLDevice> device, int size, int mip_levels) {
  auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                  width:size
                                                                 height:size
                                                              mipmapped:mip_levels > 1];
  desc.mipmapLevelCount = mip_levels;
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModePrivate;
  return [device newTextureWithDescriptor:desc];
}

// Resolves a VRAM slot to a Metal texture, falling back to the pool's
// placeholder exactly like the GL renderers do.
id<MTLTexture> lookup_or_placeholder(TexturePool* pool, u32 tbp, int* missing_counter) {
  auto handle = pool->lookup(tbp);
  if (!handle) {
    if (missing_counter) {
      (*missing_counter)++;
    }
    handle = pool->get_placeholder_texture();
  }
  id<MTLTexture> tex = metal_texture_lookup(*handle);
  if (!tex) {
    tex = metal_texture_lookup(pool->get_placeholder_texture());
  }
  if (!tex) {
    static bool warned = false;
    if (!warned) {
      lg::warn("Metal ocean: VRAM slot {} has no Metal texture and neither does the placeholder",
               tbp);
      warned = true;
    }
  }
  return tex;
}

// The Metal bucket table is Jak 1 only. If another version ever reaches here,
// consume the bucket and say so rather than aborting the process.
bool skip_if_not_jak1(DmaFollower& dma,
                      MetalSharedRenderState* render_state,
                      const std::string& name) {
  if (render_state->version == GameVersion::Jak1) {
    return true;
  }
  lg::warn("Metal ocean bucket {} only supports Jak 1; consuming the bucket", name);
  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// MetalOceanTexture
// ---------------------------------------------------------------------------

MetalOceanTexture::MetalOceanTexture(bool generate_mipmaps,
                                     id<MTLDevice> device,
                                     id<MTLCommandQueue> queue)
    : m_generate_mipmaps(generate_mipmaps), m_device(device), m_queue(queue) {
  m_result_texture = make_ocean_target(device, TEX0_SIZE, m_generate_mipmaps ? NUM_MIPS : 1);
  m_result_handle = metal_texture_register(m_result_texture);
  if (m_generate_mipmaps) {
    m_temp_texture = make_ocean_target(device, TEX0_SIZE, 1);
  }

  // the positions and indices are fixed for the life of the renderer
  std::vector<float> positions(NUM_VERTS * 2);
  for (u32 i = 0; i < NUM_VERTS; i++) {
    positions[i * 2 + 0] = m_pc.vertex_positions[i].x();
    positions[i * 2 + 1] = m_pc.vertex_positions[i].y();
  }
  m_position_buffer = [device newBufferWithBytes:positions.data()
                                          length:positions.size() * sizeof(float)
                                         options:MTLResourceStorageModeShared];
  m_index_buffer = [device newBufferWithBytes:m_pc.index_buffer.data()
                                       length:m_pc.index_buffer.size() * sizeof(u32)
                                      options:MTLResourceStorageModeShared];
  m_dynamic_buffer = [device newBufferWithLength:NUM_VERTS * sizeof(Vertex)
                                         options:MTLResourceStorageModeShared];
}

void MetalOceanTexture::init_textures(TexturePool& pool, GameVersion version) {
  if (version != GameVersion::Jak1) {
    lg::warn("Metal ocean renderer is Jak 1 only; publishing to the Jak 1 ocean VRAM slot anyway");
  }
  m_tbp = OCEAN_TEX_TBP_JAK1;
  TextureInput in;
  in.gpu_texture = m_result_handle;
  in.w = TEX0_SIZE;
  in.h = TEX0_SIZE;
  in.debug_page_name = "PC-OCEAN";
  in.debug_name = fmt::format("pc-ocean-mip-{}", m_generate_mipmaps);
  in.id = pool.allocate_pc_port_texture(version);
  m_tex0_gpu = pool.give_texture_and_load_to_vram(in, m_tbp);
}

/*!
 * Same DMA walk and VU calls as OceanTexture::handle_ocean_texture_jak1. The
 * only difference is where the result lands: the GL version binds an FBO for
 * the whole function, this one runs the GPU passes at the end.
 */
void MetalOceanTexture::handle_ocean_texture_jak1(DmaFollower& dma,
                                                  MetalSharedRenderState* render_state,
                                                  MetalFrameContext& ctx) {
  m_stats = {};

  // (set-display-gs-state arg0 ocean-tex-page-0 128 128 0 0)
  dma.read_and_advance();

  // set up VIF
  {
    // (new 'static 'vif-tag :cmd (vif-cmd base))
    // (new 'static 'vif-tag :imm #xc0 :cmd (vif-cmd offset))
    auto data = dma.read_and_advance();
    ASSERT(data.size_bytes == 0);
    ASSERT(data.vifcode0().kind == VifCode::Kind::BASE);
    ASSERT(data.vifcode1().kind == VifCode::Kind::OFFSET);
    ASSERT(data.vifcode0().immediate == 0);
    ASSERT(data.vifcode1().immediate == 0xc0);
  }

  // load texture constants
  {
    auto data = dma.read_and_advance();
    ASSERT(data.size_bytes == sizeof(OceanTextureConstants));
    ASSERT(data.vifcode0().kind == VifCode::Kind::STCYCL);
    ASSERT(data.vifcode0().immediate == 0x404);
    ASSERT(data.vifcode1().kind == VifCode::Kind::UNPACK_V4_32);
    ASSERT(data.vifcode1().num == data.size_bytes / 16);
    ASSERT(data.vifcode1().immediate == TexVu1Data::CONSTANTS);
    memcpy(&m_texture_constants, data.data, sizeof(OceanTextureConstants));
  }

  // set up GS for envmap texture drawing
  {
    auto data = dma.read_and_advance();
    ASSERT(data.size_bytes == sizeof(AdGifData) + 16);  // 16 for the giftag.
    ASSERT(data.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(data.vifcode1().kind == VifCode::Kind::DIRECT);
    memcpy(&m_envmap_adgif, data.data + 16, sizeof(AdGifData));
    setup_renderer();
  }

  // vertices are uploaded double buffered
  m_texture_vertices_loading = m_texture_vertices_a;
  m_texture_vertices_drawing = m_texture_vertices_b;

  // add first group of vertices
  {
    auto data = dma.read_and_advance();
    ASSERT(data.size_bytes == sizeof(m_texture_vertices_a));
    ASSERT(data.vifcode0().kind == VifCode::Kind::STCYCL);
    ASSERT(data.vifcode0().immediate == 0x404);
    ASSERT(data.vifcode1().kind == VifCode::Kind::UNPACK_V4_32);
    ASSERT(data.vifcode1().num == data.size_bytes / 16);
    VifCodeUnpack up(data.vifcode1());
    ASSERT(up.addr_qw == 0);
    ASSERT(up.use_tops_flag == true);
    memcpy(m_texture_vertices_loading, data.data, sizeof(m_texture_vertices_a));
  }

  // first call
  {
    auto data = dma.read_and_advance();
    ASSERT(data.size_bytes == 0);
    ASSERT(data.vifcode0().kind == VifCode::Kind::MSCALF);
    ASSERT(data.vifcode0().immediate == TexVu1Prog::START);
    ASSERT(data.vifcode1().kind == VifCode::Kind::STMOD);
    run_L1_PC();
  }

  // loop over vertex groups
  for (int i = 0; i < NUM_FRAG_LOOPS; i++) {
    auto verts = dma.read_and_advance();
    ASSERT(verts.size_bytes == sizeof(m_texture_vertices_a));
    ASSERT(verts.vifcode0().kind == VifCode::Kind::STCYCL);
    ASSERT(verts.vifcode0().immediate == 0x404);
    ASSERT(verts.vifcode1().kind == VifCode::Kind::UNPACK_V4_32);
    ASSERT(verts.vifcode1().num == verts.size_bytes / 16);
    VifCodeUnpack up(verts.vifcode1());
    ASSERT(up.addr_qw == 0);
    ASSERT(up.use_tops_flag == true);
    memcpy(m_texture_vertices_loading, verts.data, sizeof(m_texture_vertices_a));

    auto call = dma.read_and_advance();
    ASSERT(call.size_bytes == 0);
    ASSERT(call.vifcode0().kind == VifCode::Kind::MSCALF);
    ASSERT(call.vifcode0().immediate == TexVu1Prog::REST);
    ASSERT(call.vifcode1().kind == VifCode::Kind::STMOD);
    run_L2_PC();
  }

  // last upload does something weird...
  {
    auto data0 = dma.read_and_advance();
    ASSERT(data0.size_bytes == 128 * 16);
    ASSERT(data0.vifcode0().kind == VifCode::Kind::STCYCL);
    ASSERT(data0.vifcode0().immediate == 0x404);
    ASSERT(data0.vifcode1().kind == VifCode::Kind::UNPACK_V4_32);
    ASSERT(data0.vifcode1().num == data0.size_bytes / 16);
    VifCodeUnpack up0(data0.vifcode1());
    ASSERT(up0.addr_qw == 0);
    ASSERT(up0.use_tops_flag == true);
    memcpy(m_texture_vertices_loading, data0.data, 128 * 16);

    auto data1 = dma.read_and_advance();
    ASSERT(data1.size_bytes == 64 * 16);
    ASSERT(data1.vifcode0().kind == VifCode::Kind::STCYCL);
    ASSERT(data1.vifcode0().immediate == 0x404);
    ASSERT(data1.vifcode1().kind == VifCode::Kind::UNPACK_V4_32);
    ASSERT(data1.vifcode1().num == data1.size_bytes / 16);
    VifCodeUnpack up1(data1.vifcode1());
    ASSERT(up1.addr_qw == 128);
    ASSERT(up1.use_tops_flag == true);
    memcpy(m_texture_vertices_loading + 128, data1.data, 64 * 16);
  }

  // last rest call
  {
    auto data = dma.read_and_advance();
    ASSERT(data.size_bytes == 0);
    ASSERT(data.vifcode0().kind == VifCode::Kind::MSCALF);
    ASSERT(data.vifcode0().immediate == TexVu1Prog::REST);
    ASSERT(data.vifcode1().kind == VifCode::Kind::STMOD);
    run_L2_PC();
  }

  // last call - this program does nothing.
  {
    auto data = dma.read_and_advance();
    ASSERT(data.size_bytes == 0);
    ASSERT(data.vifcode0().kind == VifCode::Kind::MSCALF);
    ASSERT(data.vifcode0().immediate == TexVu1Prog::DONE);
    ASSERT(data.vifcode1().kind == VifCode::Kind::STMOD);
  }

  if (!render_state->secondary_view) {
    run_gpu_passes(render_state, ctx);

    // Publish once per engine frame. The secondary view samples the result prepared by primary.
    render_state->texture_pool->move_existing_to_vram(m_tex0_gpu, m_tbp);
  }
}

/*!
 * The GL renderer's flush() plus make_texture_with_mipmaps(), as Metal render
 * passes. These run on their own command buffer because the frame's encoder is
 * open; see the file comment.
 */
void MetalOceanTexture::run_gpu_passes(MetalSharedRenderState* render_state,
                                       MetalFrameContext& ctx) {
  m_stats.vertices = (int)m_pc.vtx_idx;
  if (m_pc.vtx_idx != NUM_VERTS) {
    lg::warn("Metal ocean texture: VU produced {} of {} vertices; skipping the generation passes",
             m_pc.vtx_idx, NUM_VERTS);
    return;
  }
  memcpy(m_dynamic_buffer.contents, m_pc.vertex_dynamic.data(), NUM_VERTS * sizeof(Vertex));

  GsTex0 tex0(m_envmap_adgif.tex0_data);
  id<MTLTexture> envmap =
      lookup_or_placeholder(render_state->texture_pool, tex0.tbp0(), &m_stats.missing_textures);
  id<MTLTexture> base_target = m_generate_mipmaps ? m_temp_texture : m_result_texture;

  id<MTLCommandBuffer> cmds = [m_queue commandBuffer];

  {
    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = base_target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    id<MTLRenderCommandEncoder> enc = [cmds renderCommandEncoderWithDescriptor:pass];
    [enc setCullMode:MTLCullModeNone];

    MetalPsoKey pso_key;
    pso_key.shader = MetalShaderId::OCEAN_TEXTURE;
    pso_key.color_format = MTLPixelFormatRGBA8Unorm;
    [enc setRenderPipelineState:ctx.pso_cache->get_pipeline(pso_key)];
    // no depth attachment: the GL renderer disables depth test and blending here
    [enc setVertexBuffer:m_position_buffer offset:0 atIndex:0];
    [enc setVertexBuffer:m_dynamic_buffer offset:0 atIndex:1];

    MetalSamplerKey sampler_key;
    sampler_key.min_filter = MTLSamplerMinMagFilterLinear;
    sampler_key.mag_filter = MTLSamplerMinMagFilterLinear;
    sampler_key.mip_filter = MTLSamplerMipFilterLinear;
    sampler_key.wrap_s = MTLSamplerAddressModeRepeat;
    sampler_key.wrap_t = MTLSamplerAddressModeRepeat;
    [enc setFragmentTexture:envmap atIndex:0];
    [enc setFragmentSamplerState:ctx.sampler_cache->get(sampler_key) atIndex:0];

    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                    indexCount:(NSUInteger)m_pc.index_buffer.size()
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:m_index_buffer
             indexBufferOffset:0];
    [enc endEncoding];
    m_stats.draw_calls++;
    m_stats.triangles += NUM_STRIPS * NUM_STRIPS * 2;
  }

  if (m_generate_mipmaps) {
    // The alpha of the lower levels is reduced so texture filtering fades the
    // ocean out with distance (OceanTexture::make_texture_with_mipmaps).
    MetalPsoKey pso_key;
    pso_key.shader = MetalShaderId::OCEAN_TEXTURE_MIPMAP;
    pso_key.color_format = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pso = ctx.pso_cache->get_pipeline(pso_key);

    MetalSamplerKey sampler_key;
    sampler_key.min_filter = MTLSamplerMinMagFilterLinear;
    sampler_key.mag_filter = MTLSamplerMinMagFilterLinear;
    id<MTLSamplerState> sampler = ctx.sampler_cache->get(sampler_key);

    for (int i = 0; i < NUM_MIPS; i++) {
      auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = m_result_texture;
      pass.colorAttachments[0].level = i;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
      id<MTLRenderCommandEncoder> enc = [cmds renderCommandEncoderWithDescriptor:pass];
      [enc setRenderPipelineState:pso];
      [enc setFragmentTexture:m_temp_texture atIndex:0];
      [enc setFragmentSamplerState:sampler atIndex:0];
      float alpha_intensity = std::max(0.f, 1.f - 0.51f * i);
      [enc setFragmentBytes:&alpha_intensity length:sizeof(alpha_intensity) atIndex:0];
      [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      [enc endEncoding];
      m_stats.draw_calls++;
      m_stats.triangles += 2;
    }
  }

  [cmds commit];
  // the frame's command buffer is committed later, so this is only needed to
  // keep the CPU-side dynamic vertex buffer safe to overwrite next frame
  [cmds waitUntilCompleted];
}

// ---------------------------------------------------------------------------
// MetalCommonOceanRenderer
// ---------------------------------------------------------------------------

MetalCommonOceanRenderer::MetalCommonOceanRenderer() {
  m_vertices.resize(4096 * 10);
  for (auto& buf : m_indices) {
    buf.resize(4096 * 10);
  }
}

void MetalCommonOceanRenderer::init_for_near() {
  m_next_free_vertex = 0;
  for (auto& x : m_next_free_index) {
    x = 0;
  }
  m_stats = {};
}

void MetalCommonOceanRenderer::init_for_mid() {
  init_for_near();
}

void MetalCommonOceanRenderer::kick_from_near(const u8* data) {
  bool eop = false;

  u32 offset = 0;
  while (!eop) {
    GifTag tag(data + offset);
    offset += 16;

    if (tag.nreg() == 3) {
      ASSERT(tag.pre());
      if (GsPrim(tag.prim()).kind() == GsPrim::Kind::TRI_STRIP) {
        handle_near_vertex_gif_data_strip(data, offset, tag.nloop());
      } else {
        handle_near_vertex_gif_data_fan(data, offset, tag.nloop());
      }
      offset += 16 * 3 * tag.nloop();
    } else if (tag.nreg() == 1) {
      handle_near_adgif(data, offset, tag.nloop());
      offset += 16 * 1 * tag.nloop();
    } else {
      ASSERT(false);
    }

    eop = tag.eop();
  }
}

void MetalCommonOceanRenderer::handle_near_vertex_gif_data_strip(const u8* data,
                                                                 u32 offset,
                                                                 u32 loop) {
  m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
  bool reset_last = false;
  for (u32 i = 0; i < loop; i++) {
    auto& dest_vert = m_vertices[m_next_free_vertex++];

    // stq
    memcpy(dest_vert.stq.data(), data + offset, 12);
    offset += 16;

    // rgba
    dest_vert.rgba[0] = data[offset];
    dest_vert.rgba[1] = data[offset + 4];
    dest_vert.rgba[2] = data[offset + 8];
    dest_vert.rgba[3] = data[offset + 12];
    offset += 16;

    // xyz
    u32 x = 0, y = 0;
    memcpy(&x, data + offset, 4);
    memcpy(&y, data + offset + 4, 4);

    u64 upper;
    memcpy(&upper, data + offset + 8, 8);
    u32 z = (upper >> 4) & 0xffffff;
    offset += 16;

    dest_vert.xyz[0] = (float)(x << 16) / (float)UINT32_MAX;
    dest_vert.xyz[1] = (float)(y << 16) / (float)UINT32_MAX;
    dest_vert.xyz[2] = (float)(z << 8) / (float)UINT32_MAX;

    u8 f = (upper >> 36);
    dest_vert.fog = f;

    auto vidx = m_next_free_vertex - 1;
    bool adc = upper & (1ull << 47);
    if (!adc) {
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
      reset_last = false;
    } else {
      if (reset_last) {
        m_next_free_index[m_current_bucket] -= 3;
      }
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx - 1;
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
      reset_last = true;
    }
  }
}

void MetalCommonOceanRenderer::handle_near_vertex_gif_data_fan(const u8* data,
                                                               u32 offset,
                                                               u32 loop) {
  u32 ind_of_fan_start = UINT32_MAX;
  bool fan_running = false;
  // :regs0 (gif-reg-id st) :regs1 (gif-reg-id rgbaq) :regs2 (gif-reg-id xyzf2)
  for (u32 i = 0; i < loop; i++) {
    auto& dest_vert = m_vertices[m_next_free_vertex++];

    // stq
    memcpy(dest_vert.stq.data(), data + offset, 12);
    offset += 16;

    // rgba
    dest_vert.rgba[0] = data[offset];
    dest_vert.rgba[1] = data[offset + 4];
    dest_vert.rgba[2] = data[offset + 8];
    dest_vert.rgba[3] = data[offset + 12];
    offset += 16;

    // xyz
    u32 x = 0, y = 0;
    memcpy(&x, data + offset, 4);
    memcpy(&y, data + offset + 4, 4);

    u64 upper;
    memcpy(&upper, data + offset + 8, 8);
    u32 z = (upper >> 4) & 0xffffff;
    offset += 16;

    dest_vert.xyz[0] = (float)(x << 16) / (float)UINT32_MAX;
    dest_vert.xyz[1] = (float)(y << 16) / (float)UINT32_MAX;
    dest_vert.xyz[2] = (float)(z << 8) / (float)UINT32_MAX;

    u8 f = (upper >> 36);
    dest_vert.fog = f;

    auto vidx = m_next_free_vertex - 1;

    if (ind_of_fan_start == UINT32_MAX) {
      ind_of_fan_start = vidx;
    } else {
      if (fan_running) {
        // hack to draw fans with strips. this isn't efficient, but fans happen extremely rarely
        // (you basically have to put the camera intersecting the ocean and looking fwd)
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx - 1;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = ind_of_fan_start;
      } else {
        fan_running = true;
      }
    }
  }
}

void MetalCommonOceanRenderer::handle_near_adgif(const u8* data, u32 offset, u32 count) {
  u32 most_recent_tbp = 0;

  for (u32 i = 0; i < count; i++) {
    u64 value;
    GsRegisterAddress addr;
    memcpy(&value, data + offset + 16 * i, sizeof(u64));
    memcpy(&addr, data + offset + 16 * i + 8, sizeof(GsRegisterAddress));
    switch (addr) {
      case GsRegisterAddress::MIPTBP1_1:
        // ignore this, it's just mipmapping settings
        break;
      case GsRegisterAddress::TEX1_1: {
        GsTex1 reg(value);
        ASSERT(reg.mmag());
      } break;
      case GsRegisterAddress::CLAMP_1: {
        bool s = value & 0b001;
        bool t = value & 0b100;
        ASSERT(s == t);
        if (s) {
          m_current_bucket = VertexBucket::ENV_MAP;
        }
      } break;
      case GsRegisterAddress::TEX0_1: {
        GsTex0 reg(value);
        ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);
        if (!reg.tcc()) {
          m_current_bucket = VertexBucket::RGB_TEXTURE;
        }
        most_recent_tbp = reg.tbp0();
      } break;
      case GsRegisterAddress::ALPHA_1: {
        GsAlpha reg(value);
        if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
            reg.b_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
            reg.c_mode() == GsAlpha::BlendMode::DEST && reg.d_mode() == GsAlpha::BlendMode::DEST) {
          m_current_bucket = VertexBucket::ENV_MAP;
        }
      } break;

      case GsRegisterAddress::FRAME_1: {
        u32 mask = value >> 32;
        if (mask) {
          m_current_bucket = VertexBucket::ALPHA;
        }
      } break;

      default:
        lg::debug("reg: {}", register_address_name(addr));
        break;
    }
  }

  if (m_current_bucket == VertexBucket::ENV_MAP) {
    m_envmap_tex = most_recent_tbp;
  }

  if (m_vertices.size() - 128 < m_next_free_vertex) {
    ASSERT(false);  // add more vertices.
  }
}

void MetalCommonOceanRenderer::kick_from_mid(const u8* data) {
  bool eop = false;

  u32 offset = 0;
  while (!eop) {
    GifTag tag(data + offset);
    offset += 16;

    auto format = tag.flg();
    if (format == GifTag::Format::PACKED) {
      if (tag.nreg() == 1) {
        ASSERT(!tag.pre());
        ASSERT(tag.nloop() == 5);
        handle_mid_adgif(data, offset);
        offset += 5 * 16;
      } else {
        ASSERT(tag.nreg() == 3);
        ASSERT(tag.pre());
        m_current_bucket = GsPrim(tag.prim()).abe() ? 1 : 0;

        int count = tag.nloop();
        if (GsPrim(tag.prim()).kind() == GsPrim::Kind::TRI_STRIP) {
          handle_near_vertex_gif_data_strip(data, offset, tag.nloop());
        } else {
          handle_near_vertex_gif_data_fan(data, offset, tag.nloop());
        }
        offset += 3 * 16 * count;
      }
    } else {
      ASSERT(false);  // format not packed or reglist.
    }

    eop = tag.eop();
  }
}

void MetalCommonOceanRenderer::handle_mid_adgif(const u8* data, u32 offset) {
  u32 most_recent_tbp = 0;

  for (u32 i = 0; i < 5; i++) {
    u64 value;
    GsRegisterAddress addr;
    memcpy(&value, data + offset + 16 * i, sizeof(u64));
    memcpy(&addr, data + offset + 16 * i + 8, sizeof(GsRegisterAddress));
    switch (addr) {
      case GsRegisterAddress::MIPTBP1_1:
      case GsRegisterAddress::MIPTBP2_1:
        // ignore this, it's just mipmapping settings
        break;
      case GsRegisterAddress::TEX1_1: {
        GsTex1 reg(value);
        ASSERT(reg.mmag());
      } break;
      case GsRegisterAddress::CLAMP_1: {
        bool s = value & 0b001;
        bool t = value & 0b100;
        ASSERT(s == t);
      } break;
      case GsRegisterAddress::TEX0_1: {
        GsTex0 reg(value);
        ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);
        most_recent_tbp = reg.tbp0();
      } break;
      case GsRegisterAddress::ALPHA_1: {
      } break;

      default:
        lg::debug("reg: {}", register_address_name(addr));
        break;
    }
  }

  // faithfulness note: the GL renderer compares against the Jak 2 slot with no
  // Jak 1 branch, so the generated texture also ends up as the "envmap" here.
  if (most_recent_tbp != OCEAN_TEX_TBP_JAK2) {
    m_envmap_tex = most_recent_tbp;
  }

  if (m_vertices.size() - 128 < m_next_free_vertex) {
    ASSERT(false);  // add more vertices.
  }
}

void MetalCommonOceanRenderer::bind_bucket(MetalSharedRenderState* render_state,
                                           MetalFrameContext& ctx,
                                           u32 tbp,
                                           int shader_bucket,
                                           bool mipmap) {
  id<MTLTexture> tex =
      lookup_or_placeholder(render_state->texture_pool, tbp, &m_stats.missing_textures);
  MetalSamplerKey sampler_key;
  sampler_key.min_filter = MTLSamplerMinMagFilterLinear;
  sampler_key.mag_filter = MTLSamplerMinMagFilterLinear;
  sampler_key.mip_filter =
      mipmap ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNotMipmapped;
  sampler_key.wrap_s = MTLSamplerAddressModeRepeat;
  sampler_key.wrap_t = MTLSamplerAddressModeRepeat;

  OceanCommonParams params;
  params.fog_color[0] = render_state->fog_color[0] / 255.f;
  params.fog_color[1] = render_state->fog_color[1] / 255.f;
  params.fog_color[2] = render_state->fog_color[2] / 255.f;
  params.fog_color[3] = render_state->fog_intensity / 255.f;
  params.bucket = shader_bucket;
  params.scissor_adjust = 512.f / kGameHeightJak1;  // SCISSOR_ADJUST * HEIGHT_SCALE, Jak 1

  [ctx.enc setFragmentTexture:tex atIndex:0];
  [ctx.enc setFragmentSamplerState:ctx.sampler_cache->get(sampler_key) atIndex:0];
  [ctx.enc setVertexBytes:&params length:sizeof(params) atIndex:1];
  [ctx.enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
}

/*!
 * Mirror of CommonOceanRenderer::flush_near: three indexed triangle-strip draws
 * with the GL renderer's per-bucket blend states baked into PSOs.
 */
void MetalCommonOceanRenderer::flush_near(MetalSharedRenderState* render_state,
                                          MetalFrameContext& ctx) {
  if (m_next_free_vertex == 0) {
    return;
  }
  m_stats.vertices = (int)m_next_free_vertex;

  id<MTLBuffer> vbuf;
  u32 voffset;
  memcpy(ctx.stream->alloc(m_next_free_vertex * sizeof(Vertex), &vbuf, &voffset), m_vertices.data(),
         m_next_free_vertex * sizeof(Vertex));

  id<MTLRenderCommandEncoder> enc = ctx.enc;
  [enc setVertexBuffer:vbuf offset:voffset atIndex:0];

  // depth-tested, but no depth writes
  MetalDepthStencilKey depth{true, MTLCompareFunctionGreaterEqual, false};
  [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(depth)];

  MetalPsoKey pso_key;
  pso_key.shader = MetalShaderId::OCEAN_COMMON;
  pso_key.color_format = ctx.color_format;
  pso_key.depth_format = ctx.depth_format;
  pso_key.blend_enable = true;
  pso_key.blend_src_alpha = MTLBlendFactorOne;
  pso_key.blend_dst_alpha = MTLBlendFactorZero;

  for (int bucket = 0; bucket < NUM_BUCKETS; bucket++) {
    if (m_next_free_index[bucket] == 0) {
      continue;
    }
    switch (bucket) {
      case 0:
        pso_key.blend_src_rgb = MTLBlendFactorSourceAlpha;
        pso_key.blend_dst_rgb = MTLBlendFactorOneMinusSourceAlpha;
        bind_bucket(render_state, ctx, OCEAN_TEX_TBP_JAK1, 0, false);
        break;
      case 1:
        pso_key.blend_src_rgb = MTLBlendFactorZero;
        pso_key.blend_dst_rgb = MTLBlendFactorOne;
        // the GL renderer leaves the ocean texture bound from bucket 0
        bind_bucket(render_state, ctx, OCEAN_TEX_TBP_JAK1, 1, false);
        break;
      default:
        pso_key.blend_src_rgb = MTLBlendFactorDestinationAlpha;
        pso_key.blend_dst_rgb = MTLBlendFactorOne;
        bind_bucket(render_state, ctx, m_envmap_tex, 2, true);
        break;
    }
    [enc setRenderPipelineState:ctx.pso_cache->get_pipeline(pso_key)];

    id<MTLBuffer> ibuf;
    u32 ioffset;
    memcpy(ctx.stream->alloc(m_next_free_index[bucket] * sizeof(u32), &ibuf, &ioffset),
           m_indices[bucket].data(), m_next_free_index[bucket] * sizeof(u32));
    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                    indexCount:m_next_free_index[bucket]
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:ibuf
             indexBufferOffset:ioffset];
    m_stats.draw_calls++;
    m_stats.triangles += m_next_free_index[bucket];
  }

  ctx.draw_calls += m_stats.draw_calls;
  ctx.triangles += m_stats.triangles;
}

/*!
 * Mirror of CommonOceanRenderer::flush_mid, including the reversed envmap
 * indices and the destination-alpha trick that keeps the low-poly pass from
 * drawing over the high-poly one.
 */
void MetalCommonOceanRenderer::flush_mid(MetalSharedRenderState* render_state,
                                         MetalFrameContext& ctx) {
  if (m_next_free_vertex == 0) {
    return;
  }
  m_stats.vertices = (int)m_next_free_vertex;
  id<MTLBuffer> vbuf;
  u32 voffset;
  memcpy(ctx.stream->alloc(m_next_free_vertex * sizeof(Vertex), &vbuf, &voffset), m_vertices.data(),
         m_next_free_vertex * sizeof(Vertex));

  id<MTLRenderCommandEncoder> enc = ctx.enc;
  [enc setVertexBuffer:vbuf offset:voffset atIndex:0];

  // depth writes, but the test always passes
  MetalDepthStencilKey depth{true, MTLCompareFunctionAlways, true};
  [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(depth)];

  // draw the envmaps in reverse so the high-poly versions land first
  reverse_indices(m_indices[1].data(), m_next_free_index[1]);

  MetalPsoKey pso_key;
  pso_key.shader = MetalShaderId::OCEAN_COMMON;
  pso_key.color_format = ctx.color_format;
  pso_key.depth_format = ctx.depth_format;

  for (int bucket = 0; bucket < 2; bucket++) {
    if (m_next_free_index[bucket] == 0) {
      continue;
    }
    if (bucket == 0) {
      pso_key.blend_enable = false;
      bind_bucket(render_state, ctx, OCEAN_TEX_TBP_JAK1, 3, true);
    } else {
      pso_key.blend_enable = true;
      pso_key.blend_src_rgb = MTLBlendFactorDestinationAlpha;
      pso_key.blend_dst_rgb = MTLBlendFactorOne;
      pso_key.blend_src_alpha = MTLBlendFactorOne;
      pso_key.blend_dst_alpha = MTLBlendFactorZero;
      bind_bucket(render_state, ctx, m_envmap_tex, 4, true);
    }
    [enc setRenderPipelineState:ctx.pso_cache->get_pipeline(pso_key)];

    id<MTLBuffer> ibuf;
    u32 ioffset;
    memcpy(ctx.stream->alloc(m_next_free_index[bucket] * sizeof(u32), &ibuf, &ioffset),
           m_indices[bucket].data(), m_next_free_index[bucket] * sizeof(u32));
    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                    indexCount:m_next_free_index[bucket]
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:ibuf
             indexBufferOffset:ioffset];
    m_stats.draw_calls++;
    m_stats.triangles += m_next_free_index[bucket];
  }

  ctx.draw_calls += m_stats.draw_calls;
  ctx.triangles += m_stats.triangles;
}

// ---------------------------------------------------------------------------
// MetalOceanMid
// ---------------------------------------------------------------------------

void MetalOceanMid::xgkick(u16 addr) {
  m_common_ocean_renderer.kick_from_mid((const u8*)&m_vu_data[addr]);
}

/*!
 * Same VIF dispatch loop as OceanMid::run.
 */
void MetalOceanMid::run(DmaFollower& dma,
                        MetalSharedRenderState* render_state,
                        MetalFrameContext& ctx) {
  m_common_ocean_renderer.init_for_mid();
  // first is setting base and offset
  {
    auto base_offset_tag = dma.read_and_advance();
    ASSERT(base_offset_tag.size_bytes == 0);
    auto base = base_offset_tag.vifcode0();
    ASSERT(base.kind == VifCode::Kind::BASE);
    ASSERT(base.immediate == VU1_INPUT_BUFFER_BASE);
    auto offset = base_offset_tag.vifcode1();
    ASSERT(offset.kind == VifCode::Kind::OFFSET);
    ASSERT(offset.immediate == VU1_INPUT_BUFFER_OFFSET);
  }

  // next is constants
  {
    auto constants = dma.read_and_advance();
    ASSERT(constants.size_bytes == sizeof(Constants));
    ASSERT(constants.vifcode0().kind == VifCode::Kind::STCYCL);
    auto unpack = constants.vifcode1();
    ASSERT(VifCodeUnpack(unpack).addr_qw == Vu1Data::CONSTANTS);
    memcpy(&m_constants, constants.data, sizeof(Constants));
    memcpy(m_vu_data + Vu1Data::CONSTANTS, &m_constants, sizeof(Constants));
  }

  // next is call 0
  {
    auto call0 = dma.read_and_advance();
    ASSERT(call0.vifcode0().kind == VifCode::Kind::STCYCL);
    auto c = call0.vifcode1();
    ASSERT(c.kind == VifCode::Kind::MSCALF);
    ASSERT(c.immediate == 0);
    run_call0_vu2c();
  }

  while (!vu_loop_is_end_tag(dma.current_tag(), dma.current_tag_vif0(), dma.current_tag_vif1())) {
    auto data = dma.read_and_advance();
    auto v0 = data.vifcode0();
    auto v1 = data.vifcode1();
    if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
        v1.kind == VifCode::Kind::UNPACK_V4_32) {
      auto up = VifCodeUnpack(v1);
      u16 addr = up.addr_qw + (up.use_tops_flag ? get_upload_buffer() : 0);
      ASSERT(addr + v1.num <= 1024);
      memcpy(m_vu_data + addr, data.data, 16 * v1.num);
      ASSERT(16 * v1.num == data.size_bytes);
    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
               v1.kind == VifCode::Kind::UNPACK_V4_8) {
      auto up = VifCodeUnpack(v1);
      ASSERT(up.use_tops_flag);
      ASSERT(up.is_unsigned);
      u16 addr = up.addr_qw + get_upload_buffer();
      ASSERT(addr + v1.num <= 1024);

      u32 temp[4];
      for (u32 i = 0; i < v1.num; i++) {
        for (u32 j = 0; j < 4; j++) {
          temp[j] = data.data[4 * i + j];
        }
        memcpy(m_vu_data + addr + i, temp, 16);
      }
      ASSERT(4 * v1.num == data.size_bytes);
    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x204 &&
               v1.kind == VifCode::Kind::UNPACK_V4_8) {
      auto up = VifCodeUnpack(v1);
      ASSERT(up.use_tops_flag);
      ASSERT(up.is_unsigned);
      u16 addr = up.addr_qw + get_upload_buffer();
      ASSERT(addr + v1.num <= 1024);

      u32 temp[4];
      for (u32 i = 0; i < v1.num; i++) {
        for (u32 j = 0; j < 4; j++) {
          temp[j] = data.data[4 * i + j];
        }
        // cl = 4
        // wl = 2
        u32 addr_off = 4 * (i / 2) + i % 2;
        memcpy(m_vu_data + addr + addr_off, temp, 16);
      }
      ASSERT(8 * v1.num == data.size_bytes);
    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
               v1.kind == VifCode::Kind::MSCALF) {
      switch (v1.immediate) {
        case 46:
          run_call46_vu2c();
          break;
        case 73:
          run_call73_vu2c();
          break;
        case 107:
          run_call107_vu2c();
          break;
        case 275:
          run_call275_vu2c();
          break;
        default:
          lg::warn("unknown call1: {}", v1.immediate);
      }
    } else if (v0.kind == VifCode::Kind::MSCALF && v1.kind == VifCode::Kind::FLUSHA) {
      switch (v0.immediate) {
        case 41:
          run_call41_vu2c();
          break;
        case 43:
          run_call43_vu2c();
          break;
        default:
          ASSERT_MSG(false, fmt::format("unknown call2: {}", v0.immediate));
      }
    } else {
      ASSERT_MSG(false, fmt::format("{} {}", data.vifcode0().print(), data.vifcode1().print()));
    }
  }
  m_common_ocean_renderer.flush_mid(render_state, ctx);
}

// ---------------------------------------------------------------------------
// MetalOceanMidAndFar
// ---------------------------------------------------------------------------

MetalOceanMidAndFar::MetalOceanMidAndFar(const std::string& name,
                                         int my_id,
                                         id<MTLDevice> device,
                                         id<MTLCommandQueue> queue)
    : MetalBucketRenderer(name, my_id),
      m_direct(name, my_id, 4096),
      m_texture_renderer(true, device, queue) {}

void MetalOceanMidAndFar::init_textures(TexturePool& pool, GameVersion version) {
  m_texture_renderer.init_textures(pool, version);
}

/*!
 * Same DMA walk as OceanMidAndFar::render_jak1.
 */
void MetalOceanMidAndFar::render(DmaFollower& dma,
                                 MetalSharedRenderState* render_state,
                                 MetalFrameContext& ctx) {
  if (!skip_if_not_jak1(dma, render_state, m_name)) {
    return;
  }

  // jump to bucket
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0);
  ASSERT(data0.vif0() == 0);
  ASSERT(data0.size_bytes == 0);

  // see if bucket is empty or not
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
    return;
  }
  m_direct.reset_state();

  m_texture_renderer.handle_ocean_texture_jak1(dma, render_state, ctx);

  handle_ocean_far(dma, render_state, ctx);
  m_direct.flush_pending(render_state, ctx);

  handle_ocean_mid(dma, render_state, ctx);

  auto final_next = dma.read_and_advance();
  ASSERT(final_next.vifcode0().kind == VifCode::Kind::NOP &&
         final_next.vifcode1().kind == VifCode::Kind::NOP && final_next.size_bytes == 0);
  for (int i = 0; i < 4; i++) {
    dma.read_and_advance();
  }
  ASSERT(dma.current_tag_offset() == render_state->next_bucket);

  m_direct.flush_pending(render_state, ctx);
}

void MetalOceanMidAndFar::handle_ocean_far(DmaFollower& dma,
                                           MetalSharedRenderState* render_state,
                                           MetalFrameContext& ctx) {
  auto init_data = dma.read_and_advance();
  ASSERT(init_data.size_bytes == 160);
  u8 init_data_buffer[160];
  memcpy(init_data_buffer, init_data.data, 160);

  // this is a bit of a hack, but it patches the ta0 to 0 in
  // (set! (-> (the-as (pointer gs-texa) s4-0) 8) (new 'static 'gs-texa :ta0 #x80 :ta1 #x80))
  u8 val = 0;
  memcpy(init_data_buffer + 80, &val, 1);
  m_direct.render_gif(init_data_buffer, 160, render_state, ctx);

  while (dma.current_tag().kind == DmaTag::Kind::CNT &&
         dma.current_tag_vifcode0().kind == VifCode::Kind::NOP) {
    auto data = dma.read_and_advance();
    ASSERT(data.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(data.vifcode1().kind == VifCode::Kind::DIRECT);
    ASSERT(data.size_bytes / 16 == data.vifcode1().immediate);
    m_direct.render_gif(data.data, data.size_bytes, render_state, ctx);
  }
}

void MetalOceanMidAndFar::handle_ocean_mid(DmaFollower& dma,
                                           MetalSharedRenderState* render_state,
                                           MetalFrameContext& ctx) {
  if (dma.current_tag_vifcode0().kind == VifCode::Kind::BASE) {
    m_mid_renderer.run(dma, render_state, ctx);
  } else {
    // not drawing
    return;
  }

  while (!mid_far_is_end_tag(dma.current_tag(), dma.current_tag_vifcode0(),
                             dma.current_tag_vifcode1())) {
    dma.read_and_advance();
  }
}

// ---------------------------------------------------------------------------
// MetalOceanNear
// ---------------------------------------------------------------------------

MetalOceanNear::MetalOceanNear(const std::string& name,
                               int my_id,
                               id<MTLDevice> device,
                               id<MTLCommandQueue> queue)
    : MetalBucketRenderer(name, my_id), m_texture_renderer(false, device, queue) {}

void MetalOceanNear::init_textures(TexturePool& pool, GameVersion version) {
  m_texture_renderer.init_textures(pool, version);
}

void MetalOceanNear::xgkick(u16 addr) {
  m_common_ocean_renderer.kick_from_near((const u8*)&m_vu_data[addr]);
}

/*!
 * Same DMA walk as OceanNear::render_jak1.
 */
void MetalOceanNear::render(DmaFollower& dma,
                            MetalSharedRenderState* render_state,
                            MetalFrameContext& ctx) {
  if (!skip_if_not_jak1(dma, render_state, m_name)) {
    return;
  }

  // jump to bucket
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0);
  ASSERT(data0.vif0() == 0);
  ASSERT(data0.size_bytes == 0);

  // see if bucket is empty or not
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
    return;
  }

  // TODO: this looks the same as the previous ocean renderer to me... why do it again?
  m_texture_renderer.handle_ocean_texture_jak1(dma, render_state, ctx);

  if (dma.current_tag().qwc != 2) {
    lg::error("abort MetalOceanNear::render!");
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
    }
    return;
  }

  // direct setup
  {
    m_common_ocean_renderer.init_for_near();
    auto setup = dma.read_and_advance();
    ASSERT(setup.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(setup.vifcode1().kind == VifCode::Kind::DIRECT);
    ASSERT(setup.size_bytes == 32);
  }

  // offset and base
  {
    auto ob = dma.read_and_advance();
    ASSERT(ob.size_bytes == 0);
    auto base = ob.vifcode0();
    auto off = ob.vifcode1();
    ASSERT(base.kind == VifCode::Kind::BASE);
    ASSERT(off.kind == VifCode::Kind::OFFSET);
    ASSERT(base.immediate == VU1_INPUT_BUFFER_BASE);
    ASSERT(off.immediate == VU1_INPUT_BUFFER_OFFSET);
  }

  while (!vu_loop_is_end_tag(dma.current_tag(), dma.current_tag_vif0(), dma.current_tag_vif1())) {
    auto data = dma.read_and_advance();
    auto v0 = data.vifcode0();
    auto v1 = data.vifcode1();

    if (v0.kind == VifCode::Kind::STCYCL && v1.kind == VifCode::Kind::UNPACK_V4_32) {
      ASSERT(v0.immediate == 0x404);
      auto up = VifCodeUnpack(v1);
      u16 addr = up.addr_qw + (up.use_tops_flag ? get_upload_buffer() : 0);
      ASSERT(addr + v1.num <= 1024);
      memcpy(m_vu_data + addr, data.data, 16 * v1.num);
    } else if (v0.kind == VifCode::Kind::MSCALF && v1.kind == VifCode::Kind::STMOD) {
      ASSERT(v1.immediate == 0);
      switch (v0.immediate) {
        case 0:
          run_call0_vu2c();
          break;
        case 39:
          run_call39_vu2c();
          break;
        default:
          ASSERT_MSG(false, fmt::format("unknown ocean near call: {}", v0.immediate));
      }
    }
  }

  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }
  m_common_ocean_renderer.flush_near(render_state, ctx);
}
