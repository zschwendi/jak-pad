#include "game/graphics/pipelines/metal/metal_glow_renderer.h"

#include <cmath>
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
  float probe_uv[2];
};
static_assert(sizeof(GlowVertex) == 48);

// Must match GlowProbeVertexIn in shaders/sprite_glow.metal.
struct GlowProbeVertex {
  float position[3];
  float sample_uv[2];
};
static_assert(sizeof(GlowProbeVertex) == 20);

// Must match GlowVsParams in shaders/sprite_glow.metal.
struct GlowVsParams {
  float height_scale;
  float scissor_adjust;
};
static_assert(sizeof(GlowVsParams) == 8);

// Must match GlowFsParams in shaders/sprite_glow.metal.
struct GlowFsParams {
  float glow_boost;
};
static_assert(sizeof(GlowFsParams) == 4);

struct GlowDrawRecord {
  u32 tbp = 0;
  u32 index_offset = 0;
  MetalSamplerKey sampler;
};

constexpr int kScissorWidth = 512;
constexpr int kScissorHeight = 416;

bool try_make_record(const SpriteGlowOutput& sprite,
                     u32 index_offset,
                     GlowDrawRecord* result) {
  const AdGifData& adgif = sprite.adgif;
  if ((u8)adgif.tex0_addr != (u8)GsRegisterAddress::TEX0_1) {
    return false;
  }
  const GsTex0 tex0(adgif.tex0_data);
  if (tex0.tcc() != 1) {
    return false;
  }
  if (tex0.tfx() != GsTex0::TextureFunction::MODULATE) {
    return false;
  }
  if ((u8)adgif.tex1_addr != (u8)GsRegisterAddress::TEX1_1) {
    return false;
  }
  if (adgif.mip_addr != (u32)GsRegisterAddress::MIPTBP1_1) {
    return false;
  }
  if (adgif.alpha_addr != (u32)GsRegisterAddress::ALPHA_1) {
    return false;
  }

  const GsTex1 tex1(adgif.tex1_data);
  MetalSamplerKey sampler;
  sampler.min_filter = tex1.mmag() ? MTLSamplerMinMagFilterLinear
                                   : MTLSamplerMinMagFilterNearest;
  sampler.mag_filter = sampler.min_filter;
  sampler.wrap_s = MTLSamplerAddressModeRepeat;
  sampler.wrap_t = MTLSamplerAddressModeRepeat;

  const u8 clamp_address = (u8)adgif.clamp_addr;
  if (clamp_address == (u8)GsRegisterAddress::ZBUF_1) {
    // The final pass disables depth writes regardless of this inherited GS
    // register; the GL path also leaves the default texture wrapping intact.
  } else if (clamp_address == (u8)GsRegisterAddress::CLAMP_1) {
    const u32 clamp = static_cast<u32>(adgif.clamp_data);
    if (clamp != 0b101 && clamp != 0 && clamp != 1 && clamp != 0b100) {
      return false;
    }
    sampler.wrap_s = (clamp & 0b001) ? MTLSamplerAddressModeClampToEdge
                                     : MTLSamplerAddressModeRepeat;
    sampler.wrap_t = (clamp & 0b100) ? MTLSamplerAddressModeClampToEdge
                                     : MTLSamplerAddressModeRepeat;
  } else {
    return false;
  }

  result->tbp = tex0.tbp0();
  result->index_offset = index_offset;
  result->sampler = sampler;
  return true;
}

}  // namespace

bool MetalGlowRenderer::ensure_probe_targets(id<MTLDevice> device) {
  int cell_size = kFirstDownsampleSize;
  for (int i = 0; i < kDownsampleIterations; i++) {
    const int size = cell_size * kDownsampleBatchWidth;
    if (!m_probe_color[i] || m_probe_color[i].width != static_cast<NSUInteger>(size)) {
      auto* descriptor =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                              width:size
                                                             height:size
                                                          mipmapped:NO];
      descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      descriptor.storageMode = MTLStorageModePrivate;
      m_probe_color[i] = [device newTextureWithDescriptor:descriptor];
    }
    if (!m_probe_color[i]) {
      return false;
    }
    cell_size /= 2;
  }

  const int first_size = kFirstDownsampleSize * kDownsampleBatchWidth;
  if (!m_probe_depth || m_probe_depth.width != static_cast<NSUInteger>(first_size)) {
    auto* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                            width:first_size
                                                           height:first_size
                                                        mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget;
    descriptor.storageMode = MTLStorageModePrivate;
    m_probe_depth = [device newTextureWithDescriptor:descriptor];
  }
  return m_probe_depth != nil;
}

bool MetalGlowRenderer::ensure_game_depth_snapshot(id<MTLTexture> source) {
  if (m_game_depth_snapshot && m_game_depth_snapshot.width == source.width &&
      m_game_depth_snapshot.height == source.height &&
      m_game_depth_snapshot.pixelFormat == source.pixelFormat) {
    return true;
  }
  auto* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.pixelFormat
                                                         width:source.width
                                                        height:source.height
                                                     mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModePrivate;
  m_game_depth_snapshot = [source.device newTextureWithDescriptor:descriptor];
  return m_game_depth_snapshot != nil;
}

void MetalGlowRenderer::draw(const SpriteGlowOutput* sprites,
                             std::size_t count,
                             MetalSharedRenderState* render_state,
                             MetalFrameContext& ctx) {
  m_stats = {};
  if (count == 0) {
    return;
  }

  ASSERT(sprites);
  ASSERT(count <= kDownsampleBatchWidth * kDownsampleBatchWidth);
  ASSERT(count <= std::numeric_limits<u32>::max() / 5);
  ASSERT(count * 4 * sizeof(GlowVertex) <= MetalStreamBuffer::kPageSize);
  ASSERT(count * 5 * sizeof(u32) <= MetalStreamBuffer::kPageSize);

  m_stats.sprites_submitted = static_cast<int>(count);

  std::vector<GlowVertex> vertices;
  vertices.reserve(count * 4);
  std::vector<u32> indices;
  indices.reserve(count * 5);
  std::vector<GlowDrawRecord> records;
  records.reserve(count);
  std::vector<GlowProbeVertex> probe_vertices;
  probe_vertices.reserve(count * 4);
  std::vector<u32> probe_indices;
  probe_indices.reserve(count * 5);

  constexpr float kUvs[4][2] = {{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};
  for (std::size_t sprite_idx = 0; sprite_idx < count; sprite_idx++) {
    const SpriteGlowOutput& sprite = sprites[sprite_idx];
    GlowDrawRecord record;
    if (!try_make_record(sprite, static_cast<u32>(indices.size()), &record)) {
      m_stats.invalid_records++;
      continue;
    }

    const u32 vertex_base = static_cast<u32>(vertices.size());
    vertices.resize(vertices.size() + 4);

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
      const float cell_x = static_cast<float>(sprite_idx / kDownsampleBatchWidth);
      const float cell_y = static_cast<float>(sprite_idx % kDownsampleBatchWidth);
      vertex.probe_uv[0] = (cell_x + 0.5f) / kDownsampleBatchWidth;
      vertex.probe_uv[1] = (cell_y + 0.5f) / kDownsampleBatchWidth;
    }

    // Exact GL fan-to-strip order, including the fixed restart sentinel.
    indices.push_back(vertex_base + 1);
    indices.push_back(vertex_base);
    indices.push_back(vertex_base + 2);
    indices.push_back(vertex_base + 3);
    indices.push_back(UINT32_MAX);

    const float cell_x = static_cast<float>(sprite_idx / kDownsampleBatchWidth);
    const float cell_y = static_cast<float>(sprite_idx % kDownsampleBatchWidth);
    const float x0 = cell_x / kDownsampleBatchWidth;
    const float y0 = cell_y / kDownsampleBatchWidth;
    const float x1 = (cell_x + 1.f) / kDownsampleBatchWidth;
    const float y1 = (cell_y + 1.f) / kDownsampleBatchWidth;
    const float u0 = sprite.offscreen_uv[0][0] / kScissorWidth;
    const float v0 = sprite.offscreen_uv[0][1] / kScissorHeight;
    const float u1 = sprite.offscreen_uv[1][0] / kScissorWidth;
    const float v1 = sprite.offscreen_uv[1][1] / kScissorHeight;
    const float probe_z = sprite.second_clear_pos[0].z() / 16777216.f;
    const u32 probe_base = static_cast<u32>(probe_vertices.size());
    probe_vertices.push_back({{x0, y0, probe_z}, {u0, v0}});
    probe_vertices.push_back({{x1, y0, probe_z}, {u1, v0}});
    probe_vertices.push_back({{x0, y1, probe_z}, {u0, v1}});
    probe_vertices.push_back({{x1, y1, probe_z}, {u1, v1}});
    probe_indices.push_back(probe_base);
    probe_indices.push_back(probe_base + 1);
    probe_indices.push_back(probe_base + 2);
    probe_indices.push_back(probe_base + 3);
    probe_indices.push_back(UINT32_MAX);
    records.push_back(record);
  }

  if (records.empty()) {
    return;
  }

  ASSERT(render_state);
  ASSERT(render_state->version == GameVersion::Jak2);
  ASSERT(render_state->texture_pool);
  ASSERT(ctx.enc);
  ASSERT(ctx.pso_cache);
  ASSERT(ctx.sampler_cache);
  ASSERT(ctx.stream);
  ASSERT(ctx.cmds);
  ASSERT(ctx.game_color);
  ASSERT(ctx.game_depth);
  if (!ensure_probe_targets(ctx.game_depth.device) || !ensure_game_depth_snapshot(ctx.game_depth)) {
    lg::error("Metal glow: failed to allocate visibility-probe targets");
    return;
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

  id<MTLBuffer> probe_vertex_buffer;
  u32 probe_vertex_offset = 0;
  const u32 probe_vertex_bytes =
      static_cast<u32>(probe_vertices.size() * sizeof(GlowProbeVertex));
  std::memcpy(ctx.stream->alloc(probe_vertex_bytes, &probe_vertex_buffer, &probe_vertex_offset),
              probe_vertices.data(), probe_vertex_bytes);

  id<MTLBuffer> probe_index_buffer;
  u32 probe_index_offset = 0;
  const u32 probe_index_bytes = static_cast<u32>(probe_indices.size() * sizeof(u32));
  std::memcpy(ctx.stream->alloc(probe_index_bytes, &probe_index_buffer, &probe_index_offset),
              probe_indices.data(), probe_index_bytes);

  MetalSamplerKey probe_sampler_key;
  probe_sampler_key.min_filter = MTLSamplerMinMagFilterLinear;
  probe_sampler_key.mag_filter = MTLSamplerMinMagFilterLinear;
  probe_sampler_key.wrap_s = MTLSamplerAddressModeClampToEdge;
  probe_sampler_key.wrap_t = MTLSamplerAddressModeClampToEdge;
  id<MTLSamplerState> probe_sampler = ctx.sampler_cache->get(probe_sampler_key);

  // OpenGL first blits the game depth to a sampling texture, then paints each
  // flare's sampled rectangle into one cell of a 20x20 depth grid. Keep that
  // snapshot explicit so a selected slice of a render-target-only host texture
  // remains a valid immutable depth source for the probe shader.
  [ctx.enc endEncoding];
  id<MTLBlitCommandEncoder> depth_snapshot = [ctx.cmds blitCommandEncoder];
  [depth_snapshot copyFromTexture:ctx.game_depth
                      sourceSlice:ctx.game_depth_slice
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(ctx.game_depth.width, ctx.game_depth.height, 1)
                        toTexture:m_game_depth_snapshot
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
  [depth_snapshot endEncoding];

  auto* probe_pass = [MTLRenderPassDescriptor renderPassDescriptor];
  probe_pass.colorAttachments[0].texture = m_probe_color[0];
  probe_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  probe_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  probe_pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  probe_pass.depthAttachment.texture = m_probe_depth;
  probe_pass.depthAttachment.loadAction = MTLLoadActionClear;
  probe_pass.depthAttachment.storeAction = MTLStoreActionDontCare;
  probe_pass.depthAttachment.clearDepth = 1.0;
  id<MTLRenderCommandEncoder> probe_encoder =
      [ctx.cmds renderCommandEncoderWithDescriptor:probe_pass];
  ASSERT(probe_encoder);
  [probe_encoder setCullMode:MTLCullModeNone];
  [probe_encoder setVertexBuffer:probe_vertex_buffer offset:probe_vertex_offset atIndex:0];

  MetalPsoKey depth_copy_pso;
  depth_copy_pso.shader = MetalShaderId::SPRITE_GLOW_DEPTH_COPY;
  depth_copy_pso.color_format = MTLPixelFormatRGBA8Unorm;
  depth_copy_pso.depth_format = MTLPixelFormatDepth32Float;
  depth_copy_pso.color_write_mask = MTLColorWriteMaskNone;
  MetalDepthStencilKey depth_copy_state;
  depth_copy_state.depth_test = true;
  depth_copy_state.compare = MTLCompareFunctionAlways;
  depth_copy_state.depth_write = true;
  [probe_encoder setRenderPipelineState:ctx.pso_cache->get_pipeline(depth_copy_pso)];
  [probe_encoder setDepthStencilState:ctx.pso_cache->get_depth_stencil(depth_copy_state)];
  [probe_encoder setFragmentTexture:m_game_depth_snapshot atIndex:0];
  [probe_encoder setFragmentSamplerState:probe_sampler atIndex:0];
  [probe_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                             indexCount:probe_indices.size()
                              indexType:MTLIndexTypeUInt32
                            indexBuffer:probe_index_buffer
                      indexBufferOffset:probe_index_offset];

  MetalPsoKey probe_pso;
  probe_pso.shader = MetalShaderId::SPRITE_GLOW_PROBE;
  probe_pso.color_format = MTLPixelFormatRGBA8Unorm;
  probe_pso.depth_format = MTLPixelFormatDepth32Float;
  MetalDepthStencilKey probe_state;
  probe_state.depth_test = true;
  probe_state.compare = MTLCompareFunctionGreater;
  probe_state.depth_write = false;
  [probe_encoder setRenderPipelineState:ctx.pso_cache->get_pipeline(probe_pso)];
  [probe_encoder setDepthStencilState:ctx.pso_cache->get_depth_stencil(probe_state)];
  [probe_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                             indexCount:probe_indices.size()
                              indexType:MTLIndexTypeUInt32
                            indexBuffer:probe_index_buffer
                      indexBufferOffset:probe_index_offset];
  [probe_encoder endEncoding];

  MetalPsoKey downsample_pso;
  downsample_pso.shader = MetalShaderId::SPRITE_GLOW_DOWNSAMPLE;
  downsample_pso.color_format = MTLPixelFormatRGBA8Unorm;
  downsample_pso.depth_format = MTLPixelFormatInvalid;
  id<MTLRenderPipelineState> downsample_pipeline =
      ctx.pso_cache->get_pipeline(downsample_pso);
  ASSERT(downsample_pipeline);
  for (int i = 0; i < kDownsampleIterations - 1; i++) {
    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = m_probe_color[i + 1];
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    id<MTLRenderCommandEncoder> encoder = [ctx.cmds renderCommandEncoderWithDescriptor:pass];
    ASSERT(encoder);
    [encoder setRenderPipelineState:downsample_pipeline];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setVertexBuffer:probe_vertex_buffer offset:probe_vertex_offset atIndex:0];
    [encoder setFragmentTexture:m_probe_color[i] atIndex:0];
    [encoder setFragmentSamplerState:probe_sampler atIndex:0];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
                         indexCount:probe_indices.size()
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:probe_index_buffer
                  indexBufferOffset:probe_index_offset];
    [encoder endEncoding];
  }

  auto* game_pass = [MTLRenderPassDescriptor renderPassDescriptor];
  game_pass.colorAttachments[0].texture = ctx.game_color;
  game_pass.colorAttachments[0].slice = ctx.game_color_slice;
  game_pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  game_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  game_pass.depthAttachment.texture = ctx.game_depth;
  game_pass.depthAttachment.slice = ctx.game_depth_slice;
  game_pass.depthAttachment.loadAction = MTLLoadActionLoad;
  game_pass.depthAttachment.storeAction = MTLStoreActionStore;
  game_pass.stencilAttachment.texture = ctx.game_depth;
  game_pass.stencilAttachment.slice = ctx.game_depth_slice;
  game_pass.stencilAttachment.loadAction = MTLLoadActionLoad;
  game_pass.stencilAttachment.storeAction = MTLStoreActionStore;
  ctx.enc = [ctx.cmds renderCommandEncoderWithDescriptor:game_pass];
  ASSERT(ctx.enc);
  [ctx.enc setCullMode:MTLCullModeNone];
  [ctx.enc setViewport:ctx.game_viewport];

  m_stats.visibility_draw_calls = 2 + (kDownsampleIterations - 1);
  m_stats.visibility_triangles =
      static_cast<int>(records.size()) * 2 * m_stats.visibility_draw_calls;
  ctx.draw_calls += m_stats.visibility_draw_calls;
  ctx.triangles += m_stats.visibility_triangles;

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
  const float target_fps = render_state->target_fps;
  const GlowFsParams fragment_params = {
      .glow_boost = std::isfinite(target_fps) && target_fps > 60.f ? 60.f / target_fps : 1.f,
  };

  id<MTLRenderPipelineState> pipeline = ctx.pso_cache->get_pipeline(pso_key);
  ASSERT(pipeline);
  [ctx.enc setRenderPipelineState:pipeline];
  [ctx.enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(depth_key)];
  [ctx.enc setCullMode:MTLCullModeNone];
  [ctx.enc setVertexBuffer:vertex_buffer offset:vertex_offset atIndex:0];
  [ctx.enc setVertexBytes:&kJak2Params length:sizeof(kJak2Params) atIndex:1];
  [ctx.enc setFragmentBytes:&fragment_params length:sizeof(fragment_params) atIndex:0];
  [ctx.enc setFragmentTexture:m_probe_color[kDownsampleIterations - 1] atIndex:1];
  [ctx.enc setFragmentSamplerState:probe_sampler atIndex:1];

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
