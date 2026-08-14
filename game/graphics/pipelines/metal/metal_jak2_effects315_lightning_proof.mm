#import <TargetConditionals.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "common/util/Assert.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_generic2.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"
#import <Metal/Metal.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kTargetSize = 64;
constexpr u32 kLightningTextureTbp = 0x7e0;
constexpr u32 kMissingTextureTbp = 0x7e1;
constexpr u8 kClearR = 16;
constexpr u8 kClearG = 32;
constexpr u8 kClearB = 48;
constexpr u8 kClearA = 255;
constexpr u8 kFogR = 200;
constexpr u8 kFogG = 10;
constexpr u8 kFogB = 150;
constexpr u8 kLightningVertexAlpha = 32;
constexpr u8 kLightningTextureAlpha = 128;
constexpr u8 kGenericAlphaReference = 0x26;
constexpr float kLightningFragmentAlpha =
    4.f * kLightningVertexAlpha * kLightningTextureAlpha / (255.f * 255.f);
constexpr float kGenericAlphaReject = kGenericAlphaReference / 128.f;
static_assert(kLightningFragmentAlpha < kGenericAlphaReject);
constexpr float kPassingDepth = 0.25f;
constexpr float kRejectingDepth = 0.75f;

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

u32 vif(VifCode::Kind kind, u16 immediate = 0, u8 num = 0, bool interrupt = false) {
  return (static_cast<u32>(interrupt) << 31) | (static_cast<u32>(kind) << 24) |
         (static_cast<u32>(num) << 16) | immediate;
}

u32 vif_unpack_v4_32(u16 address, u8 qwc) {
  return vif(VifCode::Kind::UNPACK_V4_32, address, qwc);
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

void put_tag(std::vector<u8>* data,
             std::size_t offset,
             DmaTag::Kind kind,
             u16 qwc = 0,
             u32 address = 0,
             u32 vif0 = 0,
             u32 vif1 = 0) {
  ASSERT(offset + 16 <= data->size());
  const u64 tag =
      static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) | (static_cast<u64>(address) << 32);
  write_u64(data, offset, tag);
  write_u32(data, offset + 8, vif0);
  write_u32(data, offset + 12, vif1);
}

struct LinearChain {
  std::vector<u8> bytes;

  u32 transfer(u32 vif0, u32 vif1, const std::vector<u8>& payload = {}) {
    ASSERT((payload.size() & 0xf) == 0);
    const auto offset = bytes.size();
    bytes.resize(offset + 16 + payload.size(), 0);
    put_tag(&bytes, offset, DmaTag::Kind::CNT, static_cast<u16>(payload.size() / 16), 0, vif0,
            vif1);
    if (!payload.empty()) {
      std::memcpy(bytes.data() + offset + 16, payload.data(), payload.size());
    }
    return static_cast<u32>(offset);
  }
};

u64 zbuf() {
  return 0x130ull | (1ull << 24) | (1ull << 32);
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

constexpr u32 gcf_prim_control(GsPrim::Kind kind) {
  const u32 prim = static_cast<u32>(kind) | (1u << 3) | (1u << 4) | (1u << 6);
  return (1u << 14) | (prim << 15) | (3u << 28);
}

u64 tex0(u32 tbp) {
  return tbp | (1ull << 14) | (2ull << 26) | (2ull << 30) | (1ull << 34) | (1ull << 61);
}

std::vector<u8> make_lightning_header(u32 tbp, u32 vertex_count) {
  std::vector<u8> data(192, 0);

  float matrix[16] = {};
  matrix[0] = 1.f;
  matrix[5] = 1.f;
  matrix[10] = 1.f;
  matrix[11] = 1.f;
  std::memcpy(data.data(), matrix, sizeof(matrix));

  write_u32(&data, 64, gcf_prim_control(GsPrim::Kind::TRI_FAN));
  write_u32(&data, 68, gcf_prim_control(GsPrim::Kind::TRI_STRIP));
  write_u32(&data, 72,
            static_cast<u32>(GifTag::RegisterDescriptor::ST) |
                (static_cast<u32>(GifTag::RegisterDescriptor::RGBAQ) << 4) |
                (static_cast<u32>(GifTag::RegisterDescriptor::XYZF2) << 8));
  write_u32(&data, 76, 1);
  write_u32(&data, 88, 0x7f);
  write_u32(&data, 92, vertex_count);
  write_u32(&data, 104, 0x7f);
  write_u32(&data, 108, 0);

  AdGifData adgif = {};
  adgif.tex0_data = tex0(tbp);
  adgif.tex0_addr = static_cast<u64>(GsRegisterAddress::TEX0_1);
  adgif.tex1_data = (1ull << 5) | (1ull << 6);
  adgif.tex1_addr = static_cast<u64>(GsRegisterAddress::TEX1_1) |
                    (static_cast<u64>(0x8000u | vertex_count) << 32);
  adgif.mip_addr = static_cast<u64>(GsRegisterAddress::MIPTBP1_1);
  adgif.clamp_data = 0b0101;
  adgif.clamp_addr = static_cast<u64>(GsRegisterAddress::CLAMP_1);
  adgif.alpha_data = (2ull << 2) | (1ull << 6) | (0x80ull << 32);
  adgif.alpha_addr = static_cast<u64>(GsRegisterAddress::ALPHA_1);
  std::memcpy(data.data() + 112, &adgif, sizeof(adgif));
  return data;
}

std::vector<u8> make_lightning_vertices(std::size_t byte_count = 192) {
  std::vector<u8> data(192, 0);
  constexpr s32 s[4] = {0, 4096, 0, 4096};
  constexpr s32 t[4] = {2048, 2048, 2048, 2048};
  constexpr float x[4] = {128.f, -128.f, 128.f, -128.f};
  constexpr float y[4] = {128.f, 128.f, -128.f, -128.f};
  for (int i = 0; i < 4; i++) {
    const std::size_t offset = i * 48;
    std::memcpy(data.data() + offset, &s[i], sizeof(s[i]));
    std::memcpy(data.data() + offset + 4, &t[i], sizeof(t[i]));
    write_u32(&data, offset + 16, 128);
    write_u32(&data, offset + 20, 128);
    write_u32(&data, offset + 24, 128);
    write_u32(&data, offset + 28, kLightningVertexAlpha);
    write_float(&data, offset + 32, x[i]);
    write_float(&data, offset + 36, y[i]);
    write_float(&data, offset + 40, -1.f);
    write_float(&data, offset + 44, 1.f);
  }
  data.resize(byte_count);
  return data;
}

enum class Malformation {
  NONE,
  DIRECT,
  CONSTANTS_ADDRESS,
  HEADER_ADDRESS,
  HEADER_COUNT,
  GCF_FOG,
  GCF_COUNT,
  ADGIF_ADDRESS,
  ADGIF_VERTEX_COUNT,
  TEX0_STATE,
  TEX1_STATE,
  CLAMP_STATE,
  UNSUPPORTED_ALPHA,
  VERTEX,
  MSCAL,
  LINKER,
  TRAILER,
  FINAL_NOP,
};

enum class TransferSlot : std::size_t {
  MARKER,
  DIRECT,
  CONSTANTS,
  VU_SETUP,
  SETUP_NOP,
  HEADER,
  VERTICES,
  MSCAL,
  LINKER,
  TRAILER,
  FINAL_NOP,
  COUNT,
};

struct SyntheticChain {
  std::vector<u8> bytes;
  std::array<u32, static_cast<std::size_t>(TransferSlot::COUNT)> offsets = {};
  u32 next_bucket = 0;
};

SyntheticChain make_lightning_chain(u32 tbp, Malformation malformation = Malformation::NONE) {
  LinearChain chain;
  SyntheticChain result;
  const auto record = [&](TransferSlot slot, u32 offset) {
    result.offsets.at(static_cast<std::size_t>(slot)) = offset;
  };

  record(TransferSlot::MARKER, chain.transfer(vif(VifCode::Kind::MARK), 0));
  record(TransferSlot::DIRECT,
         chain.transfer(0, vif(VifCode::Kind::DIRECT, 2, 0, true), make_zbuf_direct()));

  std::vector<u8> constants(128, 0);
  write_float(&constants, 0, 1.f);
  write_float(&constants, 4, 0.f);
  write_float(&constants, 8, 255.f);
  constexpr float hvdf[4] = {2048.f, 2048.f, 8388607.f, 0.f};
  std::memcpy(constants.data() + 32, hvdf, sizeof(hvdf));
  record(TransferSlot::CONSTANTS,
         chain.transfer(vif(VifCode::Kind::STCYCL, 0x404), vif_unpack_v4_32(897, 8), constants));
  record(TransferSlot::VU_SETUP, chain.transfer(vif(VifCode::Kind::MSCALF, 0, 0, true),
                                                vif(VifCode::Kind::STMOD), std::vector<u8>(32, 0)));
  record(TransferSlot::SETUP_NOP, chain.transfer(0, 0));

  const u32 header_tag =
      chain.transfer(0, vif_unpack_v4_32(837, 12), make_lightning_header(tbp, 4));
  record(TransferSlot::HEADER, header_tag);
  const std::size_t vertex_bytes = malformation == Malformation::VERTEX ? 176 : 192;
  record(TransferSlot::VERTICES,
         chain.transfer(0, vif_unpack_v4_32(9, static_cast<u8>(vertex_bytes / 16)),
                        make_lightning_vertices(vertex_bytes)));
  const u32 mscal_tag = chain.transfer(0, vif(VifCode::Kind::MSCAL, 6, 0, true));
  record(TransferSlot::MSCAL, mscal_tag);
  const u32 linker_tag = chain.transfer(0, 0);
  record(TransferSlot::LINKER, linker_tag);
  const u32 trailer_tag =
      chain.transfer(vif(VifCode::Kind::FLUSHA, 0, 0, true),
                     vif(VifCode::Kind::DIRECT, 10, 0, true), std::vector<u8>(160, 0));
  record(TransferSlot::TRAILER, trailer_tag);
  const u32 final_tag = chain.transfer(0, 0);
  record(TransferSlot::FINAL_NOP, final_tag);

  const u32 direct_tag = result.offsets.at(static_cast<std::size_t>(TransferSlot::DIRECT));
  const u32 constants_tag = result.offsets.at(static_cast<std::size_t>(TransferSlot::CONSTANTS));
  if (malformation == Malformation::DIRECT) {
    write_u32(&chain.bytes, direct_tag + 12, vif(VifCode::Kind::NOP));
  } else if (malformation == Malformation::CONSTANTS_ADDRESS) {
    write_u32(&chain.bytes, constants_tag + 12, vif_unpack_v4_32(896, 8));
  } else if (malformation == Malformation::HEADER_ADDRESS) {
    write_u32(&chain.bytes, header_tag + 12, vif_unpack_v4_32(836, 12));
  } else if (malformation == Malformation::HEADER_COUNT) {
    write_u32(&chain.bytes, header_tag + 12, vif_unpack_v4_32(837, 11));
  } else if (malformation == Malformation::GCF_FOG) {
    write_u32(&chain.bytes, header_tag + 16 + 68,
              gcf_prim_control(GsPrim::Kind::TRI_STRIP) | (1u << 20));
  } else if (malformation == Malformation::GCF_COUNT) {
    write_u32(&chain.bytes, header_tag + 16 + 92, 6);
  } else if (malformation == Malformation::ADGIF_ADDRESS) {
    write_u64(&chain.bytes, header_tag + 16 + 112 + 8, static_cast<u64>(GsRegisterAddress::TEX0_2));
  } else if (malformation == Malformation::ADGIF_VERTEX_COUNT) {
    write_u64(&chain.bytes, header_tag + 16 + 112 + 24,
              static_cast<u64>(GsRegisterAddress::TEX1_1) | (0x8006ull << 32));
  } else if (malformation == Malformation::TEX0_STATE) {
    write_u64(&chain.bytes, header_tag + 16 + 112, tex0(tbp) & ~(1ull << 34));
  } else if (malformation == Malformation::TEX1_STATE) {
    write_u64(&chain.bytes, header_tag + 16 + 112 + 16, 1ull << 5);
  } else if (malformation == Malformation::CLAMP_STATE) {
    write_u64(&chain.bytes, header_tag + 16 + 112 + 48, 0b0100);
  } else if (malformation == Malformation::UNSUPPORTED_ALPHA) {
    write_u64(&chain.bytes, header_tag + 16 + 112 + 64, (3ull << 2) | (1ull << 6));
  } else if (malformation == Malformation::MSCAL) {
    write_u32(&chain.bytes, mscal_tag + 12, vif(VifCode::Kind::MSCAL, 5, 0, true));
  } else if (malformation == Malformation::LINKER) {
    write_u32(&chain.bytes, linker_tag + 8, vif(VifCode::Kind::MARK));
  } else if (malformation == Malformation::TRAILER) {
    write_u32(&chain.bytes, trailer_tag + 12, vif(VifCode::Kind::DIRECT, 9, 0, true));
  } else if (malformation == Malformation::FINAL_NOP) {
    write_u32(&chain.bytes, final_tag + 8, vif(VifCode::Kind::MARK));
  }

  result.next_bucket = static_cast<u32>(chain.bytes.size());
  chain.bytes.resize(chain.bytes.size() + 16, 0);
  put_tag(&chain.bytes, result.next_bucket, DmaTag::Kind::END);
  result.bytes = std::move(chain.bytes);
  return result;
}

SyntheticChain truncate_before(TransferSlot slot) {
  auto chain = make_lightning_chain(kLightningTextureTbp);
  chain.next_bucket = chain.offsets.at(static_cast<std::size_t>(slot));
  return chain;
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
                    double clear_depth) {
  RenderResult result;
  auto* color_desc =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
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

  auto* depth_desc =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
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
  pass.depthAttachment.clearDepth = clear_depth;
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
  state.version = GameVersion::Jak2;
  state.texture_pool = texture_pool;
  state.next_bucket = chain->next_bucket;
  state.game_res_w = kTargetSize;
  state.game_res_h = kTargetSize;
  state.fog_color = math::Vector<u8, 4>{kFogR, kFogG, kFogB, 0};
  state.fog_intensity = 255.f;

  DmaFollower dma(chain->bytes.data(), 0, chain->bytes.size());
  renderer->render(dma, &state, context);
  result.stats = renderer->stats();
  result.final_offset = dma.current_tag_offset();
  result.draw_calls = context.draw_calls;
  result.triangles = context.triangles;

  [encoder endEncoding];
  constexpr std::size_t kDepthBytesPerRow = kTargetSize * sizeof(float);
  id<MTLBuffer> depth_readback = [device newBufferWithLength:kDepthBytesPerRow * kTargetSize
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

int changed_pixel_count(const std::vector<u8>& pixels) {
  int count = 0;
  for (int y = 0; y < kTargetSize; y++) {
    for (int x = 0; x < kTargetSize; x++) {
      count += pixel_at(pixels, x, y) != kClearPixel;
    }
  }
  return count;
}

bool all_depths_equal(const std::vector<float>& depths, float expected) {
  return !depths.empty() && std::all_of(depths.begin(), depths.end(), [expected](float value) {
    return std::abs(value - expected) < 0.00001f;
  });
}

bool is_exact_lightning_draw(const RenderResult& result) {
  return result.completed && result.stats.fragments == 1 && result.stats.vertices == 4 &&
         result.stats.adgifs == 1 && result.stats.draw_buckets == 1 &&
         result.stats.draw_calls == 1 && result.stats.triangles == 2 &&
         result.stats.missing_textures == 0 && result.stats.placeholder_draws == 0 &&
         result.stats.unsupported_blends == 0 && result.stats.unexpected_dma == 0 &&
         result.stats.overflow == 0 && result.draw_calls == 1 && result.triangles == 2;
}

bool is_clean_rejection(const RenderResult& result, const SyntheticChain& chain, float depth) {
  return result.completed && result.final_offset == chain.next_bucket &&
         result.stats.unexpected_dma == 1 && result.stats.missing_textures == 0 &&
         result.stats.placeholder_draws == 0 && result.stats.unsupported_blends == 0 &&
         result.stats.overflow == 0 && result.draw_calls == 0 && result.triangles == 0 &&
         changed_pixel_count(result.pixels) == 0 && all_depths_equal(result.depths, depth);
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

    std::array<u32, 4 * 4> texture_pixels = {};
    texture_pixels.fill(rgba(128, 128, 128, kLightningTextureAlpha));
    const u64 texture_handle = metal_upload_texture_rgba8(
        device, queue, reinterpret_cast<const u8*>(texture_pixels.data()), 4, 4);
    PcTextureId texture_id;
    bool texture_registered = false;
    if (texture_handle) {
      std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
      TextureInput input;
      input.debug_page_name = "SYNTHETIC";
      input.debug_name = "jak2-effects315-lightning-proof";
      input.id = texture_pool.allocate_pc_port_texture(GameVersion::Jak2);
      texture_id = input.id;
      input.gpu_texture = texture_handle;
      input.src_data = reinterpret_cast<const u8*>(texture_pixels.data());
      input.w = 4;
      input.h = 4;
      texture_pool.give_texture_and_load_to_vram(input, kLightningTextureTbp);
      texture_registered = true;
    }
    check(texture_handle != 0 &&
              texture_pool.lookup(kLightningTextureTbp).value_or(0) == texture_handle,
          "published the synthetic Lightning texture at the source TBP");

    const auto& effects_policy = metal_renderer::jak2_metal_bucket_table().at(
        static_cast<std::size_t>(jak2::BucketId::EFFECTS));
    check(effects_policy.behavior == metal_renderer::Jak2MetalBucketBehavior::DeferredSkip,
          "bucket 315 remains deferred while the private Lightning proof runs");

    auto shared = std::make_shared<MetalGeneric2>();
    MetalGeneric2BucketRenderer renderer("effects-lightning-proof",
                                         static_cast<int>(jak2::BucketId::EFFECTS), shared,
                                         MetalGeneric2::Mode::LIGHTNING);
    auto chain = make_lightning_chain(kLightningTextureTbp);
    const auto result = render(device, queue, &pso_cache, &sampler_cache, &texture_pool, &renderer,
                               &chain, kPassingDepth);
    check(is_exact_lightning_draw(result) && result.final_offset == chain.next_bucket,
          "one source-exact Lightning fragment reaches the boundary as one four-vertex draw");
    check(changed_pixel_count(result.pixels) == 1280,
          "the exact four-vertex Lightning strip covers the deterministic Jak 2 bounds");
    const Pixel center = pixel_at(result.pixels, kTargetSize / 2, kTargetSize / 2);
    check(near(center, {48, 64, 80, 255}, 2) && !near(center, {66, 35, 86, 255}, 2) &&
              !near(center, {128, 128, 128, 255}, 2),
          "the no-FGE source PRIM produces unfogged additive color over a nonblack target");
    check(kLightningFragmentAlpha < kGenericAlphaReject && center != kClearPixel,
          "source-disabled alpha testing keeps the below-GEQUAL low-alpha Lightning visible");
    check(all_depths_equal(result.depths, kPassingDepth),
          "source ZMSK preserves passing destination depth across the visible strip");

    auto depth_reject_chain = make_lightning_chain(kLightningTextureTbp);
    const auto depth_reject = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                     &renderer, &depth_reject_chain, kRejectingDepth);
    check(is_exact_lightning_draw(depth_reject) &&
              depth_reject.final_offset == depth_reject_chain.next_bucket &&
              changed_pixel_count(depth_reject.pixels) == 0 &&
              all_depths_equal(depth_reject.depths, kRejectingDepth),
          "Lightning GEQUAL rejects depth 0.5 behind 0.75 while ZMSK preserves the target");

    struct MalformedCase {
      Malformation kind;
      const char* description;
    };
    constexpr std::array<MalformedCase, 17> malformed_cases = {{
        {Malformation::DIRECT, "a malformed Lightning DIRECT setup fails closed"},
        {Malformation::CONSTANTS_ADDRESS, "a malformed constants address fails closed"},
        {Malformation::HEADER_ADDRESS, "a malformed GCF unpack address fails closed"},
        {Malformation::HEADER_COUNT, "a malformed GCF unpack count fails closed"},
        {Malformation::GCF_FOG, "an FGE bit absent from the source Lightning PRIM fails closed"},
        {Malformation::GCF_COUNT, "a mismatched GCF vertex count fails closed"},
        {Malformation::ADGIF_ADDRESS, "a malformed source TEX0 address fails closed"},
        {Malformation::ADGIF_VERTEX_COUNT, "a mismatched adgif vertex count fails closed"},
        {Malformation::TEX0_STATE, "a non-source Lightning TEX0 state fails closed"},
        {Malformation::TEX1_STATE, "a non-source Lightning TEX1 state fails closed"},
        {Malformation::CLAMP_STATE, "a non-source Lightning clamp state fails closed"},
        {Malformation::UNSUPPORTED_ALPHA, "an unsupported Lightning ALPHA tuple fails closed"},
        {Malformation::VERTEX, "a truncated Lightning vertex record fails closed"},
        {Malformation::MSCAL, "a non-MSCAL-6 Lightning call fails closed"},
        {Malformation::LINKER, "a missing NOP linker before the trailer fails closed"},
        {Malformation::TRAILER, "a malformed FLUSHA/DIRECT trailer fails closed"},
        {Malformation::FINAL_NOP, "a malformed final Lightning NOP fails closed"},
    }};
    for (const auto& malformed_case : malformed_cases) {
      auto malformed_chain = make_lightning_chain(kLightningTextureTbp, malformed_case.kind);
      const auto malformed = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                    &renderer, &malformed_chain, kPassingDepth);
      check(is_clean_rejection(malformed, malformed_chain, kPassingDepth),
            malformed_case.description);
    }

    struct TruncationCase {
      TransferSlot slot;
      const char* description;
    };
    constexpr std::array<TruncationCase, 11> truncation_cases = {{
        {TransferSlot::MARKER, "truncation before the marker fails at the boundary"},
        {TransferSlot::DIRECT, "truncation before DIRECT fails at the boundary"},
        {TransferSlot::CONSTANTS, "truncation before constants fails at the boundary"},
        {TransferSlot::VU_SETUP, "truncation before VU setup fails at the boundary"},
        {TransferSlot::SETUP_NOP, "truncation before setup NOP fails at the boundary"},
        {TransferSlot::HEADER, "truncation before the GCF header fails at the boundary"},
        {TransferSlot::VERTICES, "truncation before vertices fails at the boundary"},
        {TransferSlot::MSCAL, "truncation before MSCAL fails at the boundary"},
        {TransferSlot::LINKER, "truncation before the NOP linker fails at the boundary"},
        {TransferSlot::TRAILER, "truncation before the trailer fails at the boundary"},
        {TransferSlot::FINAL_NOP, "truncation before the final NOP fails at the boundary"},
    }};
    for (const auto& truncation_case : truncation_cases) {
      auto truncated_chain = truncate_before(truncation_case.slot);
      const auto truncated = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                    &renderer, &truncated_chain, kPassingDepth);
      check(is_clean_rejection(truncated, truncated_chain, kPassingDepth),
            truncation_case.description);
    }

    auto missing_chain = make_lightning_chain(kMissingTextureTbp);
    const auto missing = render(device, queue, &pso_cache, &sampler_cache, &texture_pool, &renderer,
                                &missing_chain, kPassingDepth);
    check(missing.completed && missing.final_offset == missing_chain.next_bucket &&
              missing.stats.fragments == 1 && missing.stats.vertices == 4 &&
              missing.stats.adgifs == 1 && missing.stats.missing_textures == 1 &&
              missing.stats.placeholder_draws == 1 && missing.stats.unexpected_dma == 0 &&
              missing.stats.unsupported_blends == 0 && missing.stats.overflow == 0 &&
              missing.draw_calls == 1 && missing.triangles == 2 &&
              all_depths_equal(missing.depths, kPassingDepth),
          "a missing Lightning TBP is explicit and uses one depth-masked placeholder draw");

    if (texture_registered) {
      std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
      texture_pool.unload_texture(texture_id, texture_handle);
    }
    if (texture_handle) {
      metal_texture_release(texture_handle);
    }
    if (placeholder) {
      metal_texture_release(placeholder);
    }
    texture_pool.set_placeholder(0);
    check(metal_texture_live_count() == initial_live_textures,
          "released every public synthetic Lightning proof texture");
    if (failures) {
      std::printf("FAIL: %d Jak 2 effects315 Lightning checks failed\n", failures);
      return 1;
    }
    std::printf(
        "PASS: Jak 2 bucket-315 Lightning rendered source-exact DMA while remaining deferred\n");
    return 0;
  }
}
