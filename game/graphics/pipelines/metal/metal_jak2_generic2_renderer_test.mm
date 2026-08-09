#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "common/util/Assert.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_generic2.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>
#import <TargetConditionals.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kTargetSize = 64;
constexpr u32 kGenericTextureTbp = 0x7e0;

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    failures++;
  }
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

u64 zbuf(bool masked) {
  return 0x130ull | (1ull << 24) | (static_cast<u64>(masked) << 32);
}

u64 tex0() {
  return kGenericTextureTbp | (1ull << 14) | (4ull << 26) | (4ull << 30) | (1ull << 34);
}

std::vector<u8> make_fragment() {
  std::vector<u8> data(112, 0);

  float matrix[16] = {};
  matrix[0] = 1.f;
  matrix[5] = 1.f;
  matrix[10] = 1.f;
  matrix[11] = 1.f;
  std::memcpy(data.data(), matrix, sizeof(matrix));

  const u64 giftag = 1ull << 46;
  write_u64(&data, 64, giftag);
  write_u64(&data, 80, 0);
  write_u64(&data, 88, static_cast<u64>(GsRegisterAddress::ALPHA_1));
  write_u64(&data, 96,
            (1ull << 16) | (static_cast<u64>(GsTest::ZTest::GEQUAL) << 17));
  write_u64(&data, 104, static_cast<u64>(GsRegisterAddress::TEST_1));

  AdGifData adgif = {};
  adgif.tex0_data = tex0();
  adgif.tex0_addr = static_cast<u64>(GsRegisterAddress::TEX0_1);
  adgif.tex1_data = 0;
  adgif.tex1_addr = static_cast<u64>(GsRegisterAddress::TEX1_1) | (4ull << 32);
  adgif.mip_addr = static_cast<u64>(GsRegisterAddress::MIPTBP1_1);
  adgif.clamp_data = 0b101;
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
  constexpr s16 s[4] = {0, 4096, 0, 4096};
  constexpr s16 t[4] = {0, 0, 4096, 4096};
  for (int i = 0; i < 4; i++) {
    const auto offset = data.size();
    data.resize(offset + 4);
    std::memcpy(data.data() + offset, &s[i], sizeof(s[i]));
    std::memcpy(data.data() + offset + 2, &t[i], sizeof(t[i]));
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

SyntheticChain make_jak2_chain(bool masked, bool malformed) {
  LinearChain chain;
  chain.transfer(vif(VifCode::Kind::MARK), 0);

  std::vector<u8> direct(32, 0);
  write_u64(&direct, 16, zbuf(masked));
  chain.transfer(0, vif(VifCode::Kind::DIRECT, 2), direct);

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

  auto fragment = make_fragment();
  chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(12), fragment);
  chain.transfer(vif(VifCode::Kind::FLUSHA), vif(VifCode::Kind::DIRECT, 10),
                 std::vector<u8>(160, 0));
  chain.transfer(0, 0);

  SyntheticChain result;
  result.next_bucket = static_cast<u32>(chain.bytes.size());
  chain.bytes.resize(chain.bytes.size() + 16, 0);
  put_tag(&chain.bytes, result.next_bucket, DmaTag::Kind::END);
  if (malformed) {
    // The second tag is the source-shaped DIRECT setup. Keep the bounded chain
    // valid, but give the renderer the wrong VIF command.
    write_u32(&chain.bytes, 16 + 12, vif(VifCode::Kind::NOP));
  }
  result.bytes = std::move(chain.bytes);
  return result;
}

SyntheticChain make_empty_jak2_chain() {
  LinearChain chain;
  chain.transfer(0, 0);

  std::vector<u8> direct(32, 0);
  write_u64(&direct, 16, zbuf(false));
  chain.transfer(0, vif(VifCode::Kind::DIRECT, 2), direct);

  std::vector<u8> constants(128, 0);
  chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(8), constants);
  chain.transfer(vif(VifCode::Kind::MSCALF), vif(VifCode::Kind::STMOD),
                 std::vector<u8>(32, 0));

  SyntheticChain result;
  result.next_bucket = static_cast<u32>(chain.bytes.size());
  chain.bytes.resize(chain.bytes.size() + 16, 0);
  put_tag(&chain.bytes, result.next_bucket, DmaTag::Kind::END);
  result.bytes = std::move(chain.bytes);
  return result;
}

SyntheticChain make_jak1_chain() {
  LinearChain chain;
  chain.transfer(0, 0, std::vector<u8>(48, 0));

  std::vector<u8> constants(160, 0);
  write_float(&constants, 0, 1.f);
  write_float(&constants, 4, 0.f);
  write_float(&constants, 8, 255.f);
  constexpr float hvdf[4] = {2048.f, 2048.f, 8388607.f, 0.f};
  std::memcpy(constants.data() + 48, hvdf, sizeof(hvdf));
  chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(10), constants);
  chain.transfer(0, 0, std::vector<u8>(32, 0));

  auto fragment = make_fragment();
  chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(12), fragment);

  const u32 call_offset = static_cast<u32>(chain.bytes.size());
  const u32 next_bucket = call_offset + 32;
  const u32 subroutine = call_offset + 48;
  chain.bytes.resize(call_offset + 80, 0);
  put_tag(&chain.bytes, call_offset, DmaTag::Kind::CALL, 0, subroutine);
  put_tag(&chain.bytes, call_offset + 16, DmaTag::Kind::CNT);
  put_tag(&chain.bytes, next_bucket, DmaTag::Kind::END);
  put_tag(&chain.bytes, subroutine, DmaTag::Kind::CNT);
  put_tag(&chain.bytes, subroutine + 16, DmaTag::Kind::RET);
  return {std::move(chain.bytes), next_bucket};
}

struct RenderResult {
  MetalGeneric2::Stats stats;
  u32 final_offset = 0;
  int draw_calls = 0;
  int triangles = 0;
  bool completed = false;
  std::vector<u8> pixels;
  std::vector<float> depths;
};

RenderResult render(id<MTLDevice> device,
                    id<MTLCommandQueue> queue,
                    MetalPsoCache* pso_cache,
                    MetalSamplerCache* sampler_cache,
                    TexturePool* texture_pool,
                    MetalGeneric2BucketRenderer* renderer,
                    SyntheticChain* chain,
                    GameVersion version) {
  RenderResult result;
  auto* color_desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:kTargetSize
                                  height:kTargetSize
                               mipmapped:NO];
  color_desc.usage = MTLTextureUsageRenderTarget;
#if TARGET_OS_OSX
  color_desc.storageMode = MTLStorageModeManaged;
#else
  color_desc.storageMode = MTLStorageModeShared;
#endif
  id<MTLTexture> color = [device newTextureWithDescriptor:color_desc];

  auto* depth_desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                   width:kTargetSize
                                  height:kTargetSize
                               mipmapped:NO];
  depth_desc.usage = MTLTextureUsageRenderTarget;
  depth_desc.storageMode = MTLStorageModePrivate;
  id<MTLTexture> depth = [device newTextureWithDescriptor:depth_desc];

  id<MTLCommandBuffer> commands = [queue commandBuffer];
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = color;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
  pass.depthAttachment.texture = depth;
  pass.depthAttachment.loadAction = MTLLoadActionClear;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = 0;
  pass.stencilAttachment.texture = depth;
  pass.stencilAttachment.loadAction = MTLLoadActionClear;
  pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
  pass.stencilAttachment.clearStencil = 0;
  id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
  [encoder setCullMode:MTLCullModeNone];

  MetalStreamBuffer stream;
  stream.init(device);
  stream.reset();
  MetalFrameContext context;
  context.enc = encoder;
  context.pso_cache = pso_cache;
  context.sampler_cache = sampler_cache;
  context.stream = &stream;
  context.color_format = MTLPixelFormatRGBA8Unorm;
  context.depth_format = MTLPixelFormatDepth32Float_Stencil8;
  context.cmds = commands;
  context.game_color = color;
  context.game_depth = depth;

  MetalSharedRenderState state;
  state.version = version;
  state.texture_pool = texture_pool;
  state.next_bucket = chain->next_bucket;
  state.game_res_w = kTargetSize;
  state.game_res_h = kTargetSize;

  DmaFollower dma(chain->bytes.data(), 0, chain->bytes.size());
  renderer->render(dma, &state, context);
  result.stats = renderer->stats();
  result.final_offset = dma.current_tag_offset();
  result.draw_calls = context.draw_calls;
  result.triangles = context.triangles;

  [encoder endEncoding];
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
  result.pixels.resize(kTargetSize * kTargetSize * 4);
  if (result.completed) {
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

int count_non_black(const std::vector<u8>& pixels) {
  int count = 0;
  for (std::size_t offset = 0; offset + 2 < pixels.size(); offset += 4) {
    count += pixels[offset] || pixels[offset + 1] || pixels[offset + 2];
  }
  return count;
}

int count_written_depth(const std::vector<float>& depths) {
  int count = 0;
  for (float depth : depths) {
    count += depth != 0.f;
  }
  return count;
}

bool is_one_quad(const RenderResult& result) {
  return result.completed && result.stats.fragments == 1 && result.stats.vertices == 4 &&
         result.stats.adgifs == 1 && result.stats.draw_buckets == 1 &&
         result.stats.draw_calls == 1 && result.stats.triangles == 2 &&
         result.stats.unexpected_dma == 0 && result.stats.overflow == 0 &&
         result.draw_calls == 1 && result.triangles == 2;
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
    dispatch_data_t library_data = dispatch_data_create(
        g_goalpad_metallib, g_goalpad_metallib_size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError* library_error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:library_data error:&library_error];
    check(queue != nil && library != nil, "initialized Metal and the embedded shader library");
    if (!queue || !library) {
      if (library_error) {
        std::printf("Metal library error: %s\n", library_error.localizedDescription.UTF8String);
      }
      return 1;
    }

    MetalPsoCache pso_cache;
    MetalSamplerCache sampler_cache;
    check(pso_cache.init(device, library), "initialized the Metal pipeline cache");
    sampler_cache.init(device);
    TexturePool texture_pool(GameVersion::Jak2);
    check(metal_setup_placeholder(device, queue, texture_pool),
          "published the synthetic-safe Metal placeholder texture");
    const u64 placeholder = texture_pool.get_placeholder_texture();

    auto shared = std::make_shared<MetalGeneric2>();
    MetalGeneric2BucketRenderer alpha_renderer(
        "gmerc-l0-alpha", static_cast<int>(jak2::BucketId::GMERC_L0_ALPHA), shared);
    MetalGeneric2BucketRenderer water_renderer(
        "gmerc-l0-water", static_cast<int>(jak2::BucketId::GMERC_L0_WATER), shared);
    MetalGeneric2BucketRenderer jak1_renderer(
        "generic-pris-l0", static_cast<int>(jak1::BucketId::GENERIC_PRIS_LEVEL0), shared);

    auto alpha_chain = make_jak2_chain(false, false);
    auto water_chain = make_jak2_chain(true, false);
    const auto alpha = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                              &alpha_renderer, &alpha_chain, GameVersion::Jak2);
    const auto water = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                              &water_renderer, &water_chain, GameVersion::Jak2);
    check(is_one_quad(alpha) && alpha.final_offset == alpha_chain.next_bucket,
          "source-shaped Jak 2 alpha Generic2 DMA renders one two-triangle quad");
    check(is_one_quad(water) && water.final_offset == water_chain.next_bucket,
          "source-shaped Jak 2 water Generic2 DMA renders one two-triangle quad");
    check(count_non_black(alpha.pixels) > 0 && count_non_black(water.pixels) > 0,
          "both Jak 2 Generic2 variants produce non-black Metal pixels");
    check(count_written_depth(alpha.depths) > 0 && count_written_depth(water.depths) == 0,
          "alpha writes depth while source-masked water preserves cleared depth");

    auto empty_chain = make_empty_jak2_chain();
    const auto empty = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                              &alpha_renderer, &empty_chain, GameVersion::Jak2);
    check(empty.completed && empty.final_offset == empty_chain.next_bucket &&
              empty.stats.unexpected_dma == 0 && empty.stats.draw_calls == 0 &&
              empty.stats.triangles == 0 && empty.draw_calls == 0 && empty.triangles == 0,
          "source-shaped empty Jak 2 Generic2 setup reaches the boundary without error");

    auto jak1_chain = make_jak1_chain();
    const auto jak1 = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                             &jak1_renderer, &jak1_chain, GameVersion::Jak1);
    check(is_one_quad(jak1) && jak1.final_offset == jak1_chain.next_bucket &&
              count_non_black(jak1.pixels) > 0,
          "the existing Jak 1 NORMAL Generic2 walk remains renderable");

    auto malformed_chain = make_jak2_chain(false, true);
    const auto malformed = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                  &alpha_renderer, &malformed_chain, GameVersion::Jak2);
    check(malformed.completed && malformed.final_offset == malformed_chain.next_bucket &&
              malformed.stats.unexpected_dma == 1 && malformed.stats.draw_calls == 0 &&
              malformed.stats.triangles == 0 && malformed.draw_calls == 0 &&
              malformed.triangles == 0,
          "malformed Jak 2 Generic2 DMA reaches the boundary without publishing draws");
    check(count_non_black(malformed.pixels) == 0 && count_written_depth(malformed.depths) == 0,
          "malformed Jak 2 Generic2 DMA leaves color and depth untouched");

    if (placeholder) {
      metal_texture_release(placeholder);
    }
    if (failures) {
      std::printf("FAIL: %d Jak 2 Generic2 renderer checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak 2 alpha/water Generic2 rendered asset-free source-shaped DMA\n");
    return 0;
  }
}
