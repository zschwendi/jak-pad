#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/util/Assert.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_generic2.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_jak2_warp_renderer.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>
#import <TargetConditionals.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kTargetSize = 64;
// With warp_off 0.1875 and scissor_adjust 512/416 this maps exactly to v=0.125.
constexpr s16 kWarpT = 2912;
constexpr u64 kSourceOverDestinationAlpha = 0x44;

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  failures += !condition;
}

constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a) {
  return static_cast<u32>(r) | (static_cast<u32>(g) << 8) |
         (static_cast<u32>(b) << 16) | (static_cast<u32>(a) << 24);
}

u32 vif(VifCode::Kind kind, u16 immediate = 0, u8 num = 0) {
  return (static_cast<u32>(kind) << 24) | (static_cast<u32>(num) << 16) | immediate;
}

u32 vif_stcycl(u16 cl, u16 wl) {
  return vif(VifCode::Kind::STCYCL, cl | (wl << 8));
}

u32 vif_unpack_v4_32(u8 qwc) {
  return vif(VifCode::Kind::UNPACK_V4_32, 1 << 15, qwc);
}

void write_u32(std::vector<u8>* data, std::size_t offset, u32 value) {
  ASSERT(offset + sizeof(value) <= data->size());
  std::memcpy(data->data() + offset, &value, sizeof(value));
}

void write_u64(std::vector<u8>* data, std::size_t offset, u64 value) {
  ASSERT(offset + sizeof(value) <= data->size());
  std::memcpy(data->data() + offset, &value, sizeof(value));
}

void write_float(std::vector<u8>* data, std::size_t offset, float value) {
  ASSERT(offset + sizeof(value) <= data->size());
  std::memcpy(data->data() + offset, &value, sizeof(value));
}

void append_u32(std::vector<u8>* data, u32 value) {
  const auto offset = data->size();
  data->resize(offset + sizeof(value));
  write_u32(data, offset, value);
}

void append_float(std::vector<u8>* data, float value) {
  const auto offset = data->size();
  data->resize(offset + sizeof(value));
  write_float(data, offset, value);
}

void put_tag(std::vector<u8>* data,
             std::size_t offset,
             DmaTag::Kind kind,
             u16 qwc = 0,
             u32 address = 0,
             u32 vif0 = 0,
             u32 vif1 = 0) {
  ASSERT(offset + 16 <= data->size());
  const u64 tag = static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                  (static_cast<u64>(address) << 32);
  write_u64(data, offset, tag);
  write_u32(data, offset + 8, vif0);
  write_u32(data, offset + 12, vif1);
}

struct LinearChain {
  std::vector<u8> bytes;

  void transfer(u32 vif0, u32 vif1, const std::vector<u8>& payload = {}) {
    ASSERT((payload.size() & 0xf) == 0);
    const auto offset = bytes.size();
    bytes.resize(offset + 16 + payload.size(), 0);
    put_tag(&bytes, offset, DmaTag::Kind::CNT, static_cast<u16>(payload.size() / 16), 0,
            vif0, vif1);
    if (!payload.empty()) {
      std::memcpy(bytes.data() + offset + 16, payload.data(), payload.size());
    }
  }
};

u64 zbuf() {
  return 0x130ull | (1ull << 24);
}

std::vector<u8> make_zbuf_direct() {
  std::vector<u8> data(32, 0);
  const u64 gif_tag = 1ull | (1ull << 15) | (1ull << 60);
  write_u64(&data, 0, gif_tag);
  write_u64(&data, 8, static_cast<u64>(GifTag::RegisterDescriptor::AD));
  write_u64(&data, 16, zbuf());
  write_u64(&data, 24, static_cast<u64>(GsRegisterAddress::ZBUF_1));
  return data;
}

u64 tex0() {
  return metal_renderer::kJak2WarpTextureTbp | (1ull << 14) | (6ull << 26) |
         (6ull << 30) | (1ull << 34);
}

std::vector<u8> make_fragment(bool filtered, bool fog, s16 s_coord) {
  std::vector<u8> data(112, 0);
  float matrix[16] = {};
  matrix[0] = 1.f;
  matrix[5] = 1.f;
  matrix[10] = 1.f;
  matrix[11] = 1.f;
  std::memcpy(data.data(), matrix, sizeof(matrix));

  const u64 prim = fog ? (1ull << 5) : 0;
  write_u64(&data, 64, (1ull << 46) | (prim << 47));
  write_u64(&data, 80, kSourceOverDestinationAlpha);
  write_u64(&data, 88, static_cast<u64>(GsRegisterAddress::ALPHA_1));
  write_u64(&data, 96,
            (1ull << 16) | (static_cast<u64>(GsTest::ZTest::GEQUAL) << 17));
  write_u64(&data, 104, static_cast<u64>(GsRegisterAddress::TEST_1));

  AdGifData adgif = {};
  adgif.tex0_data = tex0();
  adgif.tex0_addr = static_cast<u64>(GsRegisterAddress::TEX0_1);
  adgif.tex1_data = filtered ? (1ull << 5) : 0;
  adgif.tex1_addr = static_cast<u64>(GsRegisterAddress::TEX1_1) | (4ull << 32);
  adgif.mip_addr = static_cast<u64>(GsRegisterAddress::MIPTBP1_1);
  adgif.clamp_data = 0;
  adgif.clamp_addr = static_cast<u64>(GsRegisterAddress::CLAMP_1);
  adgif.alpha_addr = static_cast<u64>(GsRegisterAddress::MIPTBP2_1);
  const auto adgif_offset = data.size();
  data.resize(adgif_offset + sizeof(adgif));
  std::memcpy(data.data() + adgif_offset, &adgif, sizeof(adgif));

  append_u32(&data, vif_stcycl(3, 1));
  append_u32(&data, vif(VifCode::Kind::UNPACK_V3_32, 0, 4));
  constexpr float x[4] = {128.f, -128.f, 128.f, -128.f};
  constexpr float y[4] = {128.f, 128.f, -128.f, -128.f};
  for (int i = 0; i < 4; i++) {
    append_float(&data, x[i]);
    append_float(&data, y[i]);
    append_float(&data, -1.f);
  }

  append_u32(&data, vif(VifCode::Kind::UNPACK_V4_8, 0, 4));
  for (int i = 0; i < 4; i++) {
    append_u32(&data, 0x80808080);
  }

  append_u32(&data, vif(VifCode::Kind::UNPACK_V2_16, 0, 4));
  for (int i = 0; i < 4; i++) {
    const auto offset = data.size();
    data.resize(offset + 4);
    std::memcpy(data.data() + offset, &s_coord, sizeof(s_coord));
    std::memcpy(data.data() + offset + 2, &kWarpT, sizeof(kWarpT));
  }
  append_u32(&data, vif_stcycl(4, 4));
  append_u32(&data, vif(VifCode::Kind::MSCAL, 0x24));
  while (data.size() % 16) {
    append_u32(&data, 0);
  }
  return data;
}

struct SyntheticChain {
  std::vector<u8> bytes;
  u32 next_bucket = 0;
};

SyntheticChain make_chain(bool filtered, bool fog, s16 s_coord) {
  LinearChain chain;
  chain.transfer(vif(VifCode::Kind::MARK), 0);
  chain.transfer(0, vif(VifCode::Kind::DIRECT, 2), make_zbuf_direct());

  std::vector<u8> constants(128, 0);
  write_float(&constants, 0, 1.f);
  write_float(&constants, 4, 0.f);
  write_float(&constants, 8, 255.f);
  constexpr float hvdf[4] = {2048.f, 2048.f, 8388607.f, 0.f};
  std::memcpy(constants.data() + 32, hvdf, sizeof(hvdf));
  chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(8), constants);
  chain.transfer(vif(VifCode::Kind::MSCALF), vif(VifCode::Kind::STMOD),
                 std::vector<u8>(32, 0));
  chain.transfer(0, 0);
  chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(12),
                 make_fragment(filtered, fog, s_coord));
  chain.transfer(vif(VifCode::Kind::FLUSHA), vif(VifCode::Kind::DIRECT, 10),
                 std::vector<u8>(160, 0));
  chain.transfer(0, 0);

  SyntheticChain result;
  result.next_bucket = static_cast<u32>(chain.bytes.size());
  chain.bytes.resize(chain.bytes.size() + 16, 0);
  put_tag(&chain.bytes, result.next_bucket, DmaTag::Kind::END);
  result.bytes = std::move(chain.bytes);
  return result;
}

struct Pixel {
  u8 r, g, b, a;

  bool operator==(const Pixel& other) const {
    return r == other.r && g == other.g && b == other.b && a == other.a;
  }
};

Pixel pixel_at(const std::vector<u8>& pixels, int width, int x, int y) {
  const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
  return {pixels.at(offset), pixels.at(offset + 1), pixels.at(offset + 2),
          pixels.at(offset + 3)};
}

bool near(Pixel actual, Pixel expected, int tolerance) {
  const auto close = [tolerance](u8 a, u8 b) {
    return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= tolerance;
  };
  return close(actual.r, expected.r) && close(actual.g, expected.g) &&
         close(actual.b, expected.b) && close(actual.a, expected.a);
}

std::vector<u32> source_pixels(int width, int height) {
  std::vector<u32> result(static_cast<std::size_t>(width) * height);
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const bool bottom = y >= height / 2;
      const bool middle = y >= height / 4;
      const bool right = x >= width / 2;
      result[y * width + x] =
          bottom ? rgba(8, 8, 160, 64)
                 : (middle ? rgba(160, 160, 8, 64)
                           : (right ? rgba(8, 160, 8, 64) : rgba(160, 8, 8, 64)));
    }
  }
  return result;
}

struct RenderResult {
  MetalGeneric2::Stats stats;
  u32 final_offset = 0;
  bool published = false;
  bool completed = false;
  int draw_calls = 0;
  int triangles = 0;
  std::vector<u8> pixels;
  std::vector<float> depths;
};

id<MTLTexture> make_color(id<MTLDevice> device, int width, int height, MTLPixelFormat format) {
  auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                  width:width
                                                                 height:height
                                                              mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget;
#if TARGET_OS_OSX
  desc.storageMode = MTLStorageModeManaged;
#else
  desc.storageMode = MTLStorageModeShared;
#endif
  return [device newTextureWithDescriptor:desc];
}

id<MTLTexture> make_depth(id<MTLDevice> device, int width, int height) {
  auto* desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                   width:width
                                  height:height
                               mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget;
  desc.storageMode = MTLStorageModePrivate;
  return [device newTextureWithDescriptor:desc];
}

id<MTLRenderCommandEncoder> begin_pass(id<MTLCommandBuffer> commands,
                                      id<MTLTexture> color,
                                      id<MTLTexture> depth,
                                      MTLLoadAction load_action) {
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = color;
  pass.colorAttachments[0].loadAction = load_action;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
  pass.depthAttachment.texture = depth;
  pass.depthAttachment.loadAction = MTLLoadActionClear;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = 0;
  pass.stencilAttachment.texture = depth;
  pass.stencilAttachment.loadAction = MTLLoadActionClear;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;
  pass.stencilAttachment.clearStencil = 0;
  return [commands renderCommandEncoderWithDescriptor:pass];
}

RenderResult render(id<MTLDevice> device,
                    id<MTLCommandQueue> queue,
                    MetalPsoCache* pso_cache,
                    MetalSamplerCache* sampler_cache,
                    TexturePool* texture_pool,
                    metal_renderer::Jak2WarpSnapshotPublisher* publisher,
                    bool filtered,
                    bool fog,
                    s16 s_coord) {
  RenderResult result;
  id<MTLTexture> color = make_color(device, kTargetSize, kTargetSize, MTLPixelFormatRGBA8Unorm);
  const auto source = source_pixels(kTargetSize, kTargetSize);
  [color replaceRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
            mipmapLevel:0
              withBytes:source.data()
            bytesPerRow:kTargetSize * sizeof(u32)];
  id<MTLTexture> depth = make_depth(device, kTargetSize, kTargetSize);
  id<MTLCommandBuffer> commands = [queue commandBuffer];

  MetalStreamBuffer stream;
  stream.init(device);
  stream.reset();
  MetalFrameContext context;
  context.enc = begin_pass(commands, color, depth, MTLLoadActionLoad);
  context.pso_cache = pso_cache;
  context.sampler_cache = sampler_cache;
  context.stream = &stream;
  context.color_format = MTLPixelFormatRGBA8Unorm;
  context.depth_format = MTLPixelFormatDepth32Float_Stencil8;
  context.cmds = commands;
  context.game_color = color;
  context.game_depth = depth;
  context.game_viewport = {0, 0, kTargetSize, kTargetSize, 0, 1};
  [context.enc setViewport:context.game_viewport];

  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.texture_pool = texture_pool;
  state.game_res_w = kTargetSize;
  state.game_res_h = kTargetSize;
  state.fog_color = math::Vector<u8, 4>{40, 120, 200, 0};
  state.fog_intensity = 1.f;
  if (publisher) {
    result.published = publisher->capture_and_publish(&state, context);
  }

  // capture_and_publish ends and resumes this pass on the same target. The
  // warp draw follows immediately, matching OpenGL's copy-then-draw order.
  [context.enc setCullMode:MTLCullModeNone];
  [context.enc setViewport:context.game_viewport];

  auto chain = make_chain(filtered, fog, s_coord);
  state.next_bucket = chain.next_bucket;
  DmaFollower dma(chain.bytes.data(), 0, chain.bytes.size());
  MetalGeneric2 generic;
  generic.render_in_mode(dma, &state, context, MetalGeneric2::Mode::WARP, &result.stats);
  result.final_offset = dma.current_tag_offset();
  result.draw_calls = context.draw_calls;
  result.triangles = context.triangles;
  [context.enc endEncoding];

  constexpr std::size_t kDepthBytesPerRow = kTargetSize * sizeof(float);
  id<MTLBuffer> depth_readback =
      [device newBufferWithLength:kDepthBytesPerRow * kTargetSize
                          options:MTLResourceStorageModeShared];
  id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
  [blit copyFromTexture:depth
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(kTargetSize, kTargetSize, 1)
               toBuffer:depth_readback
      destinationOffset:0
 destinationBytesPerRow:kDepthBytesPerRow
destinationBytesPerImage:kDepthBytesPerRow * kTargetSize
                options:MTLBlitOptionDepthFromDepthStencil];
#if TARGET_OS_OSX
  [blit synchronizeResource:color];
#endif
  [blit endEncoding];
  [commands commit];
  [commands waitUntilCompleted];
  result.completed = commands.status == MTLCommandBufferStatusCompleted;
  if (result.completed) {
    result.pixels.resize(kTargetSize * kTargetSize * 4);
    [color getBytes:result.pixels.data()
        bytesPerRow:kTargetSize * 4
         fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
        mipmapLevel:0];
    result.depths.resize(kTargetSize * kTargetSize);
    std::memcpy(result.depths.data(), depth_readback.contents,
                result.depths.size() * sizeof(float));
  }
  return result;
}

bool publish_only(id<MTLDevice> device,
                  id<MTLCommandQueue> queue,
                  TexturePool* texture_pool,
                  metal_renderer::Jak2WarpSnapshotPublisher* publisher,
                  int width,
                  int height,
                  MTLPixelFormat format) {
  id<MTLTexture> color = make_color(device, width, height, format);
  id<MTLTexture> depth = make_depth(device, width, height);
  id<MTLCommandBuffer> commands = [queue commandBuffer];
  MetalFrameContext context;
  context.enc = begin_pass(commands, color, depth, MTLLoadActionClear);
  context.cmds = commands;
  context.game_color = color;
  context.game_depth = depth;
  context.game_viewport = {0, 0, static_cast<double>(width), static_cast<double>(height), 0, 1};
  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.texture_pool = texture_pool;
  const bool published = publisher->capture_and_publish(&state, context);
  [context.enc endEncoding];
  [commands commit];
  [commands waitUntilCompleted];
  return published && commands.status == MTLCommandBufferStatusCompleted;
}

int changed_pixel_count(const RenderResult& result) {
  const auto original = source_pixels(kTargetSize, kTargetSize);
  int count = 0;
  for (int y = 0; y < kTargetSize; y++) {
    for (int x = 0; x < kTargetSize; x++) {
      Pixel expected = {};
      std::memcpy(&expected, &original[y * kTargetSize + x], sizeof(expected));
      count += !(pixel_at(result.pixels, kTargetSize, x, y) == expected);
    }
  }
  return count;
}

int written_depth_count(const RenderResult& result) {
  return static_cast<int>(std::count_if(result.depths.begin(), result.depths.end(),
                                        [](float depth) { return depth != 0.f; }));
}

bool exact_draw(const RenderResult& result, const SyntheticChain& chain) {
  return result.completed && result.published && result.final_offset == chain.next_bucket &&
         result.stats.fragments == 1 && result.stats.vertices == 4 &&
         result.stats.adgifs == 1 && result.stats.draw_buckets == 1 &&
         result.stats.draw_calls == 1 && result.stats.triangles == 2 &&
         result.stats.missing_textures == 0 && result.stats.missing_warp_publications == 0 &&
         result.stats.unsupported_blends == 0 && result.stats.unexpected_dma == 0 &&
         result.stats.overflow == 0 && result.draw_calls == 1 && result.triangles == 2;
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    check(device != nil, "a default Metal device is available");
    if (!device) {
      return 1;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    dispatch_data_t library_data = dispatch_data_create(g_goalpad_metallib, g_goalpad_metallib_size,
                                                        nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError* library_error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:library_data error:&library_error];
    check(queue != nil && library != nil, "initialized Metal and the embedded shader library");
    if (!queue || !library) {
      return 1;
    }

    MetalPsoCache pso_cache;
    MetalSamplerCache sampler_cache;
    check(pso_cache.init(device, library), "initialized the Metal pipeline cache");
    sampler_cache.init(device);
    const std::size_t initial_live_textures = metal_texture_live_count();
    TexturePool texture_pool(GameVersion::Jak2);
    check(metal_setup_placeholder(device, queue, texture_pool),
          "published the synthetic-safe placeholder");
    const u64 placeholder = texture_pool.get_placeholder_texture();

    const auto& policy = metal_renderer::jak2_metal_bucket_table();
    check(policy.at(static_cast<std::size_t>(jak2::BucketId::TEX_ALL_WARP)).behavior ==
                  metal_renderer::Jak2MetalBucketBehavior::HostTextureUpload &&
              policy.at(static_cast<std::size_t>(jak2::BucketId::GMERC_WARP)).behavior ==
                  metal_renderer::Jak2MetalBucketBehavior::Warp,
          "bucket 316 uploads before the exact production bucket-317 warp route");

    auto missing_chain = make_chain(false, false, 2048);
    const auto missing = render(device, queue, &pso_cache, &sampler_cache, &texture_pool, nullptr,
                                false, false, 2048);
    check(missing.completed && !missing.published &&
              missing.final_offset == missing_chain.next_bucket &&
              missing.stats.missing_textures == 1 &&
              missing.stats.missing_warp_publications == 1 && missing.draw_calls == 0 &&
              missing.triangles == 0 && changed_pixel_count(missing) == 0 &&
              written_depth_count(missing) == 0,
          "missing TBP1216 publication fails safe without a placeholder draw");

    metal_renderer::Jak2WarpSnapshotPublisher publisher(&texture_pool);
    auto edge_chain = make_chain(false, false, -1024);
    const auto edge = render(device, queue, &pso_cache, &sampler_cache, &texture_pool, &publisher,
                             false, false, -1024);
    const u64 stable_handle = publisher.texture_handle();
    const PcTextureId stable_id = publisher.texture_id();
    id<MTLTexture> snapshot = metal_texture_lookup(stable_handle);
    check(exact_draw(edge, edge_chain),
          "the snapshot-fed WARP quad draws once and completes its exact follower");
    check(snapshot && snapshot.width == kTargetSize && snapshot.height == kTargetSize &&
              snapshot.pixelFormat == MTLPixelFormatRGBA8Unorm &&
              texture_pool.lookup(metal_renderer::kJak2WarpTextureTbp).value_or(0) == stable_handle,
          "TBP1216 publishes a private shader-readable target matching game color");
    check(changed_pixel_count(edge) == 1280 && written_depth_count(edge) == 1280,
          "WARP keeps Jak II 0.5 height, 512/416 scissor, and depth-write coverage");
    const Pixel edge_center = pixel_at(edge.pixels, kTargetSize, kTargetSize / 2, kTargetSize / 2);
    check(near(edge_center, {84, 8, 84, 129}, 3) && edge_center.r > 70 &&
              edge_center.b > 70,
          "same-command-buffer snapshot blends the clamped edge over the original target");

    auto nearest_chain = make_chain(false, false, 2048);
    auto linear_chain = make_chain(true, false, 2048);
    const auto nearest = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                &publisher, false, false, 2048);
    const auto linear = render(device, queue, &pso_cache, &sampler_cache, &texture_pool, &publisher,
                               true, false, 2048);
    const Pixel nearest_center =
        pixel_at(nearest.pixels, kTargetSize, kTargetSize / 2, kTargetSize / 2);
    const Pixel linear_center =
        pixel_at(linear.pixels, kTargetSize, kTargetSize / 2, kTargetSize / 2);
    check(exact_draw(nearest, nearest_chain) && exact_draw(linear, linear_chain) &&
              nearest_center.g > nearest_center.r + 40 &&
              std::abs(static_cast<int>(linear_center.r) - static_cast<int>(linear_center.g)) < 15,
          "WARP retains nearest versus linear draw filtering while clamp stays forced");

    auto fog_chain = make_chain(false, true, 2048);
    const auto fog = render(device, queue, &pso_cache, &sampler_cache, &texture_pool, &publisher,
                            false, true, 2048);
    const Pixel fog_center = pixel_at(fog.pixels, kTargetSize, kTargetSize / 2, kTargetSize / 2);
    check(exact_draw(fog, fog_chain) && near(fog_center, {24, 64, 180, 129}, 3) &&
              written_depth_count(fog) == 1280,
          "WARP preserves source-over blend, header fog, and depth behavior");

    check(publisher.texture_handle() == stable_handle && publisher.texture_id() == stable_id &&
              publisher.stats().allocations == 1 && publisher.stats().replacements == 0,
          "same-size frames retain one registry handle and one PcTextureId");
    const std::size_t live_before_resize = metal_texture_live_count();
    check(publish_only(device, queue, &texture_pool, &publisher, 32, 48,
                       MTLPixelFormatBGRA8Unorm),
          "a resized next frame publishes without a CPU upload path");
    snapshot = metal_texture_lookup(stable_handle);
    GpuTexture* pool_texture =
        texture_pool.lookup_gpu_texture(metal_renderer::kJak2WarpTextureTbp);
    check(publisher.texture_handle() == stable_handle && publisher.texture_id() == stable_id &&
              publisher.stats().allocations == 1 && publisher.stats().replacements == 1 &&
              snapshot && snapshot.width == 32 && snapshot.height == 48 &&
              snapshot.pixelFormat == MTLPixelFormatBGRA8Unorm && pool_texture &&
              pool_texture->w == 32 && pool_texture->h == 48 &&
              metal_texture_live_count() == live_before_resize,
          "resize atomically replaces the texture behind the stable pool handle");

    publisher.reset();
    check(!metal_texture_lookup(stable_handle) &&
              texture_pool.lookup(metal_renderer::kJak2WarpTextureTbp).value_or(0) == placeholder &&
              metal_texture_live_count() + 1 == live_before_resize,
          "cleanup releases the snapshot and leaves no stale TBP1216 registry handle");

    if (placeholder) {
      metal_texture_release(placeholder);
    }
    texture_pool.set_placeholder(0);
    check(metal_texture_live_count() == initial_live_textures,
          "released every synthetic Metal texture");
    if (failures) {
      std::printf("FAIL: %d Jak II GMERC_WARP snapshot checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak II GMERC_WARP snapshot and Generic2 source-order proof\n");
    return 0;
  }
}
