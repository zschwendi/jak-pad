#include "game/graphics/pipelines/metal/metal_glow_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
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

bool glow_texture_diagnostic_enabled() {
  const char* value = std::getenv("GOALPAD_JAK2_DEBUG_LOG_GLOW_TEXTURES");
  return value && value[0] == '1' && value[1] == '\0';
}

struct TextureDiagnosticKey {
  u32 tbp = 0;
  u16 page = 0;
  u16 tex = 0;
  u64 handle = 0;
  bool pool_entry = false;
  bool placeholder = false;
};

struct TextureDiagnosticLogState {
  std::mutex mutex;
  std::array<TextureDiagnosticKey, 8> keys = {};
  std::size_t count = 0;
};

TextureDiagnosticLogState& texture_diagnostic_log_state() {
  static TextureDiagnosticLogState state;
  return state;
}

struct TextureRegionSummary {
  u64 count = 0;
  std::array<u8, 4> minimum = {255, 255, 255, 255};
  std::array<u8, 4> maximum = {};
  std::array<u64, 4> sum = {};

  void add(const u8* rgba) {
    count++;
    for (std::size_t channel = 0; channel < 4; channel++) {
      minimum[channel] = std::min(minimum[channel], rgba[channel]);
      maximum[channel] = std::max(maximum[channel], rgba[channel]);
      sum[channel] += rgba[channel];
    }
  }

  u32 min(std::size_t channel) const { return count ? minimum[channel] : 0; }
  u32 max(std::size_t channel) const { return count ? maximum[channel] : 0; }
  u64 average_x100(std::size_t channel) const {
    return count ? (sum[channel] * 100 + count / 2) / count : 0;
  }
};

void log_glow_texture_diagnostic(u32 tbp, TexturePool* texture_pool) {
  TextureDiagnosticKey key;
  u16 gpu_width = 0;
  u16 gpu_height = 0;
  u64 mtl_width = 0;
  u64 mtl_height = 0;
  bool source_available = false;
  TextureRegionSummary center;
  TextureRegionSummary edge;
  std::size_t index = 0;

  {
    std::lock_guard<std::mutex> pool_lock(texture_pool->mutex());
    GpuTexture* gpu_texture = texture_pool->lookup_gpu_texture(tbp);
    const auto handle = texture_pool->lookup(tbp);
    key.tbp = tbp;
    key.pool_entry = gpu_texture != nullptr;
    key.handle = handle.value_or(0);
    if (gpu_texture) {
      key.page = gpu_texture->tex_id.page;
      key.tex = gpu_texture->tex_id.tex;
      key.placeholder = gpu_texture->is_placeholder;
      if (!gpu_texture->is_placeholder) {
        gpu_width = gpu_texture->w;
        gpu_height = gpu_texture->h;
      }
    }

    auto& log_state = texture_diagnostic_log_state();
    {
      std::lock_guard<std::mutex> log_lock(log_state.mutex);
      for (std::size_t i = 0; i < log_state.count; i++) {
        const auto& logged = log_state.keys[i];
        if (logged.tbp == key.tbp && logged.page == key.page && logged.tex == key.tex &&
            logged.handle == key.handle && logged.pool_entry == key.pool_entry &&
            logged.placeholder == key.placeholder) {
          return;
        }
      }
      if (log_state.count >= log_state.keys.size()) {
        return;
      }
      index = log_state.count;
      log_state.keys[log_state.count++] = key;
    }

    id<MTLTexture> metal_texture = handle ? metal_texture_lookup(*handle) : nil;
    if (metal_texture) {
      mtl_width = metal_texture.width;
      mtl_height = metal_texture.height;
    }

    const u8* source =
        gpu_texture && !gpu_texture->is_placeholder ? gpu_texture->get_data_ptr() : nullptr;
    if (source && gpu_width && gpu_height) {
      source_available = true;
      const u32 center_x_begin = gpu_width / 4;
      const u32 center_x_end = gpu_width - center_x_begin;
      const u32 center_y_begin = gpu_height / 4;
      const u32 center_y_end = gpu_height - center_y_begin;
      for (u32 y = 0; y < gpu_height; y++) {
        for (u32 x = 0; x < gpu_width; x++) {
          const std::size_t offset = (static_cast<std::size_t>(y) * gpu_width + x) * 4;
          const bool is_center =
              x >= center_x_begin && x < center_x_end && y >= center_y_begin && y < center_y_end;
          (is_center ? center : edge).add(source + offset);
        }
      }
    }
  }

  lg::info("Metal glow texture diagnostic: index={} tbp={} pool={} page={} tex={} handle={} "
           "gpu_width={} gpu_height={} mtl_width={} mtl_height={} placeholder={} source={} "
           "center_count={} center_r_min={} center_r_max={} center_r_avg_x100={} center_g_min={} "
           "center_g_max={} center_g_avg_x100={} center_b_min={} center_b_max={} "
           "center_b_avg_x100={} center_a_min={} center_a_max={} center_a_avg_x100={} "
           "edge_count={} edge_r_min={} edge_r_max={} edge_r_avg_x100={} edge_g_min={} "
           "edge_g_max={} edge_g_avg_x100={} edge_b_min={} edge_b_max={} edge_b_avg_x100={} "
           "edge_a_min={} edge_a_max={} edge_a_avg_x100={}",
           index, key.tbp, key.pool_entry ? 1 : 0, key.page, key.tex, key.handle, gpu_width,
           gpu_height, mtl_width, mtl_height, key.placeholder ? 1 : 0, source_available ? 1 : 0,
           center.count, center.min(0), center.max(0), center.average_x100(0), center.min(1),
           center.max(1), center.average_x100(1), center.min(2), center.max(2),
           center.average_x100(2), center.min(3), center.max(3), center.average_x100(3), edge.count,
           edge.min(0), edge.max(0), edge.average_x100(0), edge.min(1), edge.max(1),
           edge.average_x100(1), edge.min(2), edge.max(2), edge.average_x100(2), edge.min(3),
           edge.max(3), edge.average_x100(3));
}

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

bool MetalGlowRenderer::ensure_missing_texture_fallback(id<MTLDevice> device) {
  if (m_missing_texture_fallback && m_missing_texture_fallback.device == device) {
    return true;
  }

  constexpr int kSize = 16;
  constexpr float kCenter = (kSize - 1) / 2.f;
  std::array<u32, kSize * kSize> pixels = {};
  for (int y = 0; y < kSize; y++) {
    for (int x = 0; x < kSize; x++) {
      const float dx = x - kCenter;
      const float dy = y - kCenter;
      const float radius = std::sqrt(dx * dx + dy * dy) / kCenter;
      const u8 intensity =
          static_cast<u8>(std::clamp((1.f - radius) * 384.f, 0.f, 255.f));
      pixels[y * kSize + x] = static_cast<u32>(intensity) |
                              (static_cast<u32>(intensity) << 8) |
                              (static_cast<u32>(intensity) << 16) |
                              (static_cast<u32>(intensity) << 24);
    }
  }

  auto* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                          width:kSize
                                                         height:kSize
                                                      mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> fallback = [device newTextureWithDescriptor:descriptor];
  if (!fallback) {
    return false;
  }
  [fallback replaceRegion:MTLRegionMake2D(0, 0, kSize, kSize)
               mipmapLevel:0
                 withBytes:pixels.data()
               bytesPerRow:kSize * sizeof(u32)];
  fallback.label = @"Unresolved final-glow fail-soft radial fallback";
  m_missing_texture_fallback = fallback;
  return true;
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

  const bool log_glow_textures = glow_texture_diagnostic_enabled();
  for (const auto& record : records) {
    if (log_glow_textures) {
      log_glow_texture_diagnostic(record.tbp, render_state->texture_pool);
    }
    std::optional<u64> handle = render_state->texture_pool->lookup(record.tbp);
    const bool placeholder_backed =
        handle && *handle == render_state->texture_pool->get_placeholder_texture();
    id<MTLTexture> texture = handle && !placeholder_backed ? metal_texture_lookup(*handle) : nil;
    if (!texture) {
      m_stats.missing_textures++;
      if (!m_warned_missing_texture) {
        lg::warn(
            "Metal glow: source texture at {} is {}; using synthetic fail-soft radial fallback "
            "(not source-faithful); further warnings are suppressed",
            record.tbp, placeholder_backed ? "placeholder-backed" : "unresolved");
        m_warned_missing_texture = true;
      }
      if (ensure_missing_texture_fallback(ctx.game_color.device)) {
        texture = m_missing_texture_fallback;
      } else {
        lg::error("Metal glow: failed to allocate fail-soft radial texture; using placeholder");
        texture = metal_texture_lookup(render_state->texture_pool->get_placeholder_texture());
      }
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
