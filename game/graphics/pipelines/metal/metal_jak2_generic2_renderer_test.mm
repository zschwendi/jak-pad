#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
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
constexpr u8 kClearR = 32;
constexpr u8 kClearG = 64;
constexpr u8 kClearB = 96;
constexpr u8 kClearA = 255;
constexpr u64 kSourceOverDestinationAlpha = 0x44;

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    failures++;
  }
}

constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a) {
  return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
         (static_cast<u32>(a) << 24);
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

std::vector<u8> make_zbuf_direct(bool masked) {
  std::vector<u8> data(32, 0);
  const u64 gif_tag = 1ull | (1ull << 15) | (1ull << 60);
  write_u64(&data, 0, gif_tag);
  write_u64(&data, 8, static_cast<u64>(GifTag::RegisterDescriptor::AD));
  write_u64(&data, 16, zbuf(masked));
  write_u64(&data, 24, static_cast<u64>(GsRegisterAddress::ZBUF_1));
  return data;
}

bool source_shaped_zbuf_direct(const std::vector<u8>& data) {
  if (data.size() != 32) {
    return false;
  }
  const GifTag tag(data.data());
  u64 address = 0;
  std::memcpy(&address, data.data() + 24, sizeof(address));
  return tag.nloop() == 1 && tag.eop() && tag.nreg() == 1 &&
         tag.reg(0) == GifTag::RegisterDescriptor::AD &&
         address == static_cast<u64>(GsRegisterAddress::ZBUF_1);
}

std::vector<u8> make_fragment(bool filtered, bool blended) {
  std::vector<u8> data(112, 0);

  float matrix[16] = {};
  matrix[0] = 1.f;
  matrix[5] = 1.f;
  matrix[10] = 1.f;
  matrix[11] = 1.f;
  std::memcpy(data.data(), matrix, sizeof(matrix));

  const u64 giftag = 1ull << 46;
  write_u64(&data, 64, giftag);
  write_u64(&data, 80, blended ? kSourceOverDestinationAlpha : 0);
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
  constexpr s16 s[4] = {2048, 2048, 2048, 2048};
  constexpr s16 t[4] = {2048, 2048, 2048, 2048};
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

SyntheticChain make_jak2_chain(bool masked, bool malformed, bool filtered) {
  LinearChain chain;
  chain.transfer(vif(VifCode::Kind::MARK), 0);

  chain.transfer(0, vif(VifCode::Kind::DIRECT, 2), make_zbuf_direct(masked));

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

  auto fragment = make_fragment(filtered, true);
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

  chain.transfer(0, vif(VifCode::Kind::DIRECT, 2), make_zbuf_direct(false));

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

  auto fragment = make_fragment(false, false);
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
  pass.colorAttachments[0].clearColor =
      MTLClearColorMake(kClearR / 255.0, kClearG / 255.0, kClearB / 255.0, kClearA / 255.0);
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

struct Pixel {
  u8 r = 0;
  u8 g = 0;
  u8 b = 0;
  u8 a = 0;

  bool operator==(const Pixel& other) const {
    return r == other.r && g == other.g && b == other.b && a == other.a;
  }

  bool operator!=(const Pixel& other) const { return !(*this == other); }
};

constexpr Pixel kClearPixel = {kClearR, kClearG, kClearB, kClearA};

Pixel pixel_at(const std::vector<u8>& pixels, int x, int y) {
  const std::size_t offset = (static_cast<std::size_t>(y) * kTargetSize + x) * 4;
  return {pixels.at(offset), pixels.at(offset + 1), pixels.at(offset + 2), pixels.at(offset + 3)};
}

bool near(Pixel actual, Pixel expected, int tolerance) {
  const auto close = [tolerance](u8 a, u8 b) {
    const int delta = static_cast<int>(a) - static_cast<int>(b);
    return delta >= -tolerance && delta <= tolerance;
  };
  return close(actual.r, expected.r) && close(actual.g, expected.g) &&
         close(actual.b, expected.b) && close(actual.a, expected.a);
}

struct PixelBounds {
  int min_x = kTargetSize;
  int min_y = kTargetSize;
  int max_x = -1;
  int max_y = -1;
  int count = 0;

  bool operator==(const PixelBounds& other) const {
    return min_x == other.min_x && min_y == other.min_y && max_x == other.max_x &&
           max_y == other.max_y && count == other.count;
  }
};

PixelBounds changed_pixel_bounds(const std::vector<u8>& pixels) {
  PixelBounds bounds;
  for (int y = 0; y < kTargetSize; y++) {
    for (int x = 0; x < kTargetSize; x++) {
      if (pixel_at(pixels, x, y) == kClearPixel) {
        continue;
      }
      bounds.min_x = std::min(bounds.min_x, x);
      bounds.min_y = std::min(bounds.min_y, y);
      bounds.max_x = std::max(bounds.max_x, x);
      bounds.max_y = std::max(bounds.max_y, y);
      bounds.count++;
    }
  }
  return bounds;
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
    const std::size_t initial_live_textures = metal_texture_live_count();
    TexturePool texture_pool(GameVersion::Jak2);
    check(metal_setup_placeholder(device, queue, texture_pool),
          "published the synthetic-safe Metal placeholder texture");
    const u64 placeholder = texture_pool.get_placeholder_texture();

    std::array<u32, 16 * 16> pattern_pixels = {};
    for (int y = 0; y < 16; y++) {
      for (int x = 0; x < 16; x++) {
        const bool right = x >= 8;
        const bool bottom = y >= 8;
        Pixel source;
        if (!right && !bottom) {
          source = {64, 16, 16, 64};
        } else if (right && !bottom) {
          source = {16, 64, 16, 64};
        } else if (!right && bottom) {
          source = {16, 16, 64, 64};
        } else {
          source = {96, 96, 16, 64};
        }
        pattern_pixels[y * 16 + x] = rgba(source.r, source.g, source.b, source.a);
      }
    }
    const u64 pattern_handle = metal_upload_texture_rgba8(
        device, queue, reinterpret_cast<const u8*>(pattern_pixels.data()), 16, 16);
    PcTextureId pattern_id;
    bool pattern_registered = false;
    if (pattern_handle) {
      std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
      TextureInput input;
      input.debug_page_name = "SYNTHETIC";
      input.debug_name = "jak2-generic2-filter-alpha";
      input.id = texture_pool.allocate_pc_port_texture(GameVersion::Jak2);
      pattern_id = input.id;
      input.gpu_texture = pattern_handle;
      input.src_data = reinterpret_cast<const u8*>(pattern_pixels.data());
      input.w = 16;
      input.h = 16;
      texture_pool.give_texture_and_load_to_vram(input, kGenericTextureTbp);
      pattern_registered = true;
    }
    check(pattern_handle != 0 &&
              texture_pool.lookup(kGenericTextureTbp).value_or(0) == pattern_handle,
          "published a patterned partial-alpha texture at the Generic2 source TBP");
    check(source_shaped_zbuf_direct(make_zbuf_direct(false)),
          "the Jak 2 DIRECT fixture carries its source GIF tag and ZBUF_1 address");

    auto shared = std::make_shared<MetalGeneric2>();
    MetalGeneric2BucketRenderer alpha_renderer(
        "gmerc-l0-alpha", static_cast<int>(jak2::BucketId::GMERC_L0_ALPHA), shared);
    MetalGeneric2BucketRenderer water_renderer(
        "gmerc-l0-water", static_cast<int>(jak2::BucketId::GMERC_L0_WATER), shared);
    MetalGeneric2BucketRenderer jak1_renderer(
        "generic-pris-l0", static_cast<int>(jak1::BucketId::GENERIC_PRIS_LEVEL0), shared);

    auto alpha_chain = make_jak2_chain(false, false, true);
    auto water_chain = make_jak2_chain(true, false, true);
    auto nearest_chain = make_jak2_chain(false, false, false);
    const auto alpha = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                              &alpha_renderer, &alpha_chain, GameVersion::Jak2);
    const auto water = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                              &water_renderer, &water_chain, GameVersion::Jak2);
    const auto nearest = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                &alpha_renderer, &nearest_chain, GameVersion::Jak2);
    check(is_one_quad(alpha) && alpha.final_offset == alpha_chain.next_bucket,
          "source-shaped Jak 2 alpha Generic2 DMA renders one two-triangle quad");
    check(is_one_quad(water) && water.final_offset == water_chain.next_bucket,
          "source-shaped Jak 2 water Generic2 DMA renders one two-triangle quad");
    check(is_one_quad(nearest) && nearest.final_offset == nearest_chain.next_bucket,
          "the nearest-filter discriminator uses the same source-shaped Jak 2 draw");
    const Pixel filtered_pixel = pixel_at(alpha.pixels, kTargetSize / 2, kTargetSize / 2);
    const Pixel water_pixel = pixel_at(water.pixels, kTargetSize / 2, kTargetSize / 2);
    const Pixel nearest_pixel = pixel_at(nearest.pixels, kTargetSize / 2, kTargetSize / 2);
    check(near(filtered_pixel, {40, 56, 62, 129}, 2) && water_pixel == filtered_pixel,
          "partial texture alpha blends filtered alpha/water color over the known destination");
    check(near(nearest_pixel, {64, 80, 56, 129}, 2) && nearest_pixel != filtered_pixel,
          "the patterned source visibly distinguishes nearest from linear Generic2 sampling");
    const PixelBounds jak2_bounds = {16, 12, 47, 51, 1280};
    check(changed_pixel_bounds(alpha.pixels) == jak2_bounds &&
              changed_pixel_bounds(water.pixels) == jak2_bounds &&
              changed_pixel_bounds(nearest.pixels) == jak2_bounds,
          "Jak 2 Generic2 applies height 0.5 and 512/416 scissor adjustment to exact bounds");
    check(count_written_depth(alpha.depths) == jak2_bounds.count &&
              count_written_depth(nearest.depths) == jak2_bounds.count &&
              count_written_depth(water.depths) == 0,
          "alpha writes covered depth while source-masked water preserves cleared depth");

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
              changed_pixel_bounds(jak1.pixels).count > 0,
          "the existing Jak 1 NORMAL Generic2 walk remains renderable");

    auto malformed_chain = make_jak2_chain(false, true, true);
    const auto malformed = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                  &alpha_renderer, &malformed_chain, GameVersion::Jak2);
    check(malformed.completed && malformed.final_offset == malformed_chain.next_bucket &&
              malformed.stats.unexpected_dma == 1 && malformed.stats.draw_calls == 0 &&
              malformed.stats.triangles == 0 && malformed.draw_calls == 0 &&
              malformed.triangles == 0,
          "malformed Jak 2 Generic2 DMA reaches the boundary without publishing draws");
    check(changed_pixel_bounds(malformed.pixels).count == 0 &&
              count_written_depth(malformed.depths) == 0,
          "malformed Jak 2 Generic2 DMA leaves color and depth untouched");

    if (pattern_registered) {
      std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
      texture_pool.unload_texture(pattern_id, pattern_handle);
    }
    if (pattern_handle) {
      metal_texture_release(pattern_handle);
    }
    if (placeholder) {
      metal_texture_release(placeholder);
    }
    texture_pool.set_placeholder(0);
    check(metal_texture_live_count() == initial_live_textures,
          "released all public synthetic Generic2 proof textures");
    if (failures) {
      std::printf("FAIL: %d Jak 2 Generic2 renderer checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak 2 alpha/water Generic2 rendered asset-free source-shaped DMA\n");
    return 0;
  }
}
