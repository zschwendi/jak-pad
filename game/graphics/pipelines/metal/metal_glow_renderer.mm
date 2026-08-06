#include "game/graphics/pipelines/metal/metal_glow_renderer.h"

#include <cstring>
#include <limits>
#include <vector>

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

namespace {

// Must match GlowVertexIn in shaders/sprite_glow.metal.
struct GlowVertex {
  float position[4];
  float color[4];
  float uv[2];
};
static_assert(sizeof(GlowVertex) == 40);

// Must match GlowVsParams in shaders/sprite_glow.metal.
struct GlowVsParams {
  float height_scale;
  float scissor_adjust;
};
static_assert(sizeof(GlowVsParams) == 8);

struct GlowDrawRecord {
  u32 tbp = 0;
  u32 index_offset = 0;
  MetalSamplerKey sampler;
};

MetalSamplerKey sampler_from_adgif(const AdGifData& adgif) {
  ASSERT((u8)adgif.tex1_addr == (u8)GsRegisterAddress::TEX1_1);
  const GsTex1 tex1(adgif.tex1_data);

  MetalSamplerKey sampler;
  sampler.min_filter = tex1.mmag() ? MTLSamplerMinMagFilterLinear
                                   : MTLSamplerMinMagFilterNearest;
  sampler.mag_filter = sampler.min_filter;
  sampler.wrap_s = MTLSamplerAddressModeRepeat;
  sampler.wrap_t = MTLSamplerAddressModeRepeat;

  const auto clamp_address = GsRegisterAddress(adgif.clamp_addr);
  if (clamp_address == GsRegisterAddress::ZBUF_1) {
    // The final pass disables depth writes regardless of this inherited GS
    // register; the GL path also leaves the default texture wrapping intact.
  } else if (clamp_address == GsRegisterAddress::CLAMP_1) {
    const u32 clamp = static_cast<u32>(adgif.clamp_data);
    ASSERT(clamp == 0b101 || clamp == 0 || clamp == 1 || clamp == 0b100);
    sampler.wrap_s = (clamp & 0b001) ? MTLSamplerAddressModeClampToEdge
                                     : MTLSamplerAddressModeRepeat;
    sampler.wrap_t = (clamp & 0b100) ? MTLSamplerAddressModeClampToEdge
                                     : MTLSamplerAddressModeRepeat;
  } else {
    ASSERT(false);
  }
  return sampler;
}

GlowDrawRecord make_record(const SpriteGlowOutput& sprite, u32 index_offset) {
  const AdGifData& adgif = sprite.adgif;
  ASSERT((u8)adgif.tex0_addr == (u8)GsRegisterAddress::TEX0_1);
  const GsTex0 tex0(adgif.tex0_data);
  ASSERT(tex0.tcc() == 1);
  ASSERT(tex0.tfx() == GsTex0::TextureFunction::MODULATE);
  ASSERT(adgif.mip_addr == (u32)GsRegisterAddress::MIPTBP1_1);
  ASSERT(adgif.alpha_addr == (u32)GsRegisterAddress::ALPHA_1);

  GlowDrawRecord result;
  result.tbp = tex0.tbp0();
  result.index_offset = index_offset;
  result.sampler = sampler_from_adgif(adgif);
  return result;
}

}  // namespace

void MetalGlowRenderer::draw_force_visible(const SpriteGlowOutput* sprites,
                                           std::size_t count,
                                           MetalSharedRenderState* render_state,
                                           MetalFrameContext& ctx) {
  m_stats = {};
  if (count == 0) {
    return;
  }

  ASSERT(sprites);
  ASSERT(render_state);
  ASSERT(render_state->version == GameVersion::Jak2);
  ASSERT(render_state->texture_pool);
  ASSERT(ctx.enc);
  ASSERT(ctx.pso_cache);
  ASSERT(ctx.sampler_cache);
  ASSERT(ctx.stream);
  ASSERT(count <= std::numeric_limits<u32>::max() / 5);
  ASSERT(count * 4 * sizeof(GlowVertex) <= MetalStreamBuffer::kPageSize);
  ASSERT(count * 5 * sizeof(u32) <= MetalStreamBuffer::kPageSize);

  m_stats.sprites_submitted = static_cast<int>(count);

  std::vector<GlowVertex> vertices(count * 4);
  std::vector<u32> indices(count * 5);
  std::vector<GlowDrawRecord> records;
  records.reserve(count);

  constexpr float kUvs[4][2] = {{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};
  for (std::size_t sprite_idx = 0; sprite_idx < count; sprite_idx++) {
    const SpriteGlowOutput& sprite = sprites[sprite_idx];
    const u32 vertex_base = static_cast<u32>(sprite_idx * 4);
    const u32 index_base = static_cast<u32>(sprite_idx * 5);

    for (u32 corner = 0; corner < 4; corner++) {
      GlowVertex& vertex = vertices[vertex_base + corner];
      vertex.position[0] = sprite.flare_xyzw[corner].x();
      vertex.position[1] = sprite.flare_xyzw[corner].y();
      vertex.position[2] = sprite.flare_xyzw[corner].z();
      // The VU program ignores the source w. GL forces one for clipping; keep
      // the identical vertex convention here.
      vertex.position[3] = 1.f;
      std::memcpy(vertex.color, sprite.flare_draw_color.data(), sizeof(vertex.color));
      std::memcpy(vertex.uv, kUvs[corner], sizeof(vertex.uv));
    }

    // Exact GL fan-to-strip order, including the fixed restart sentinel.
    indices[index_base + 0] = vertex_base + 1;
    indices[index_base + 1] = vertex_base;
    indices[index_base + 2] = vertex_base + 2;
    indices[index_base + 3] = vertex_base + 3;
    indices[index_base + 4] = UINT32_MAX;
    records.push_back(make_record(sprite, index_base));
  }

  id<MTLBuffer> vertex_buffer;
  u32 vertex_offset = 0;
  const u32 vertex_bytes = static_cast<u32>(vertices.size() * sizeof(GlowVertex));
  std::memcpy(ctx.stream->alloc(vertex_bytes, &vertex_buffer, &vertex_offset), vertices.data(),
              vertex_bytes);

  id<MTLBuffer> index_buffer;
  u32 index_offset = 0;
  const u32 index_bytes = static_cast<u32>(indices.size() * sizeof(u32));
  std::memcpy(ctx.stream->alloc(index_bytes, &index_buffer, &index_offset), indices.data(),
              index_bytes);

  MetalPsoKey pso_key;
  pso_key.shader = MetalShaderId::SPRITE_GLOW_DRAW;
  pso_key.color_format = ctx.color_format;
  pso_key.depth_format = ctx.depth_format;
  pso_key.blend_enable = true;
  pso_key.blend_op_rgb = MTLBlendOperationAdd;
  pso_key.blend_op_alpha = MTLBlendOperationAdd;
  pso_key.blend_src_rgb = MTLBlendFactorOne;
  pso_key.blend_dst_rgb = MTLBlendFactorOne;
  pso_key.blend_src_alpha = MTLBlendFactorOne;
  pso_key.blend_dst_alpha = MTLBlendFactorOne;
  pso_key.color_write_mask = MTLColorWriteMaskAll;

  MetalDepthStencilKey depth_key;
  depth_key.depth_test = false;
  depth_key.compare = MTLCompareFunctionAlways;
  depth_key.depth_write = false;

  constexpr GlowVsParams kJak2Params = {
      .height_scale = 0.5f,
      .scissor_adjust = 512.f / 416.f,
  };

  id<MTLRenderPipelineState> pipeline = ctx.pso_cache->get_pipeline(pso_key);
  ASSERT(pipeline);
  [ctx.enc setRenderPipelineState:pipeline];
  [ctx.enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(depth_key)];
  [ctx.enc setCullMode:MTLCullModeNone];
  [ctx.enc setVertexBuffer:vertex_buffer offset:vertex_offset atIndex:0];
  [ctx.enc setVertexBytes:&kJak2Params length:sizeof(kJak2Params) atIndex:1];

  for (const auto& record : records) {
    std::optional<u64> handle = render_state->texture_pool->lookup(record.tbp);
    id<MTLTexture> texture = handle ? metal_texture_lookup(*handle) : nil;
    if (!texture) {
      m_stats.missing_textures++;
      lg::warn("Metal glow: failed to resolve texture at {}, using placeholder", record.tbp);
      texture = metal_texture_lookup(render_state->texture_pool->get_placeholder_texture());
    }
    ASSERT(texture);

    [ctx.enc setFragmentTexture:texture atIndex:0];
    [ctx.enc setFragmentSamplerState:ctx.sampler_cache->get(record.sampler) atIndex:0];
    [ctx.enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                       indexCount:5
                        indexType:MTLIndexTypeUInt32
                      indexBuffer:index_buffer
                indexBufferOffset:index_offset + record.index_offset * sizeof(u32)];
    m_stats.sprites_drawn++;
    m_stats.draw_calls++;
    m_stats.triangles += 2;
  }

  ctx.draw_calls += m_stats.draw_calls;
  ctx.triangles += m_stats.triangles;
}
