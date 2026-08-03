#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/dma/gs.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_direct_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"

#import <Metal/Metal.h>
#import <TargetConditionals.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kTargetSize = 64;
constexpr std::size_t kGifQwords = 7;
constexpr std::size_t kGifBytes = kGifQwords * 16;

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    failures++;
  }
}

void put_u64(std::array<u8, kGifBytes>& payload, std::size_t offset, u64 value) {
  std::memcpy(payload.data() + offset, &value, sizeof(value));
}

void put_rgbaq(std::array<u8, kGifBytes>& payload, std::size_t offset) {
  constexpr std::array<u32, 4> kGreen = {0, 255, 0, 128};
  std::memcpy(payload.data() + offset, kGreen.data(), 16);
}

void put_xyzf2(std::array<u8, kGifBytes>& payload,
               std::size_t offset,
               u32 x,
               u32 y) {
  constexpr u64 kZ = 0xffffff;
  std::memcpy(payload.data() + offset, &x, sizeof(x));
  std::memcpy(payload.data() + offset + 4, &y, sizeof(y));
  put_u64(payload, offset + 8, kZ << 4);
}

std::array<u8, kGifBytes> make_debug_triangle_payload() {
  std::array<u8, kGifBytes> payload = {};

  // Mirrors Jak II's public add-debug-tri Direct packet shape in
  // goal_src/jak2/engine/debug/debug.gc: PACKED, PRE TRI with IIP+ABE, and
  // three RGBAQ/XYZF2 pairs. All colors and coordinates here are synthetic.
  constexpr u64 kNloop = 1;
  constexpr u64 kEop = 1ull << 15;
  constexpr u64 kPre = 1ull << 46;
  constexpr u64 kPrim = static_cast<u64>(GsPrim::Kind::TRI) | (1ull << 3) | (1ull << 6);
  constexpr u64 kNreg = 6ull << 60;
  put_u64(payload, 0, kNloop | kEop | kPre | (kPrim << 47) | kNreg);

  constexpr u64 kRgbaq = static_cast<u64>(GifTag::RegisterDescriptor::RGBAQ);
  constexpr u64 kXyzf2 = static_cast<u64>(GifTag::RegisterDescriptor::XYZF2);
  constexpr u64 kRegisters = kRgbaq | (kXyzf2 << 4) | (kRgbaq << 8) |
                             (kXyzf2 << 12) | (kRgbaq << 16) | (kXyzf2 << 20);
  put_u64(payload, 8, kRegisters);

  put_rgbaq(payload, 16);
  put_xyzf2(payload, 32, 0x8000, 0x7800);
  put_rgbaq(payload, 48);
  put_xyzf2(payload, 64, 0x7800, 0x8800);
  put_rgbaq(payload, 80);
  put_xyzf2(payload, 96, 0x8800, 0x8800);
  return payload;
}

bool is_bgra(const std::vector<u8>& pixels,
             int x,
             int y,
             u8 blue,
             u8 green,
             u8 red,
             u8 alpha) {
  const std::size_t offset = static_cast<std::size_t>(y * kTargetSize + x) * 4;
  return pixels[offset] == blue && pixels[offset + 1] == green && pixels[offset + 2] == red &&
         pixels[offset + 3] == alpha;
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
    check(queue != nil, "created a Metal command queue");

    dispatch_data_t library_data = dispatch_data_create(
        g_goalpad_metallib, g_goalpad_metallib_size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError* library_error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:library_data error:&library_error];
    check(library != nil, "loaded the product's embedded Metal shader library");
    if (!queue || !library) {
      if (library_error) {
        std::printf("Metal library error: %s\n", library_error.localizedDescription.UTF8String);
      }
      return 1;
    }

    MetalPsoCache pso_cache;
    MetalSamplerCache sampler_cache;
    MetalStreamBuffer stream;
    check(pso_cache.init(device, library), "initialized the product Metal pipeline cache");
    sampler_cache.init(device);
    stream.init(device);
    stream.reset();
    if (failures) {
      return 1;
    }

    auto* color_desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
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
    check(color != nil && depth != nil, "created offscreen color and depth targets");
    if (!color || !depth) {
      return 1;
    }

    id<MTLCommandBuffer> commands = [queue commandBuffer];
    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    pass.depthAttachment.clearDepth = 0.0;
    pass.stencilAttachment.texture = depth;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
    pass.stencilAttachment.clearStencil = 0;

    id<MTLRenderCommandEncoder> encoder =
        [commands renderCommandEncoderWithDescriptor:pass];
    check(commands != nil && encoder != nil, "created an offscreen Metal command buffer and pass");
    if (!commands || !encoder) {
      return 1;
    }
    [encoder setCullMode:MTLCullModeNone];

    MetalFrameContext context;
    context.enc = encoder;
    context.pso_cache = &pso_cache;
    context.sampler_cache = &sampler_cache;
    context.stream = &stream;
    context.color_format = MTLPixelFormatBGRA8Unorm;
    context.depth_format = MTLPixelFormatDepth32Float_Stencil8;
    context.cmds = commands;
    context.game_color = color;
    context.game_depth = depth;

    MetalSharedRenderState state;
    state.version = GameVersion::Jak2;
    state.game_res_w = kTargetSize;
    state.game_res_h = kTargetSize;

    constexpr std::size_t kDebug3 = static_cast<std::size_t>(jak2::BucketId::DEBUG3);
    const int batch_size = metal_renderer::jak2_metal_direct_batch_size(kDebug3);
    check(batch_size == 0x2000, "DEBUG3 retains its audited Jak II Direct batch size");
    if (batch_size != 0x2000) {
      return 1;
    }

    MetalDirectRenderer direct("jak2-submit-readback", static_cast<int>(kDebug3), batch_size);
    direct.reset_state();
    const auto payload = make_debug_triangle_payload();
    direct.render_gif(payload.data(), static_cast<u32>(payload.size()), &state, context);
    direct.flush_pending(&state, context);
    [encoder endEncoding];

#if TARGET_OS_OSX
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit synchronizeResource:color];
    [blit endEncoding];
#endif

    check(context.draw_calls == 1 && context.triangles == 1 && direct.stats().draw_calls == 1 &&
              direct.stats().triangles == 1 && direct.stats().unsupported_blends == 0,
          "one audited Direct payload encoded one supported triangle draw");

    [commands commit];
    [commands waitUntilCompleted];
    check(commands.status == MTLCommandBufferStatusCompleted,
          "the offscreen Metal command buffer completed");
    if (commands.status != MTLCommandBufferStatusCompleted && commands.error) {
      std::printf("Metal command-buffer error: %s\n",
                  commands.error.localizedDescription.UTF8String);
    }

    std::vector<u8> pixels(kTargetSize * kTargetSize * 4);
    [color getBytes:pixels.data()
        bytesPerRow:kTargetSize * 4
         fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
        mipmapLevel:0];

    int green_pixels = 0;
    int black_pixels = 0;
    int unexpected_pixels = 0;
    for (int y = 0; y < kTargetSize; y++) {
      for (int x = 0; x < kTargetSize; x++) {
        if (is_bgra(pixels, x, y, 0, 255, 0, 255)) {
          green_pixels++;
        } else if (is_bgra(pixels, x, y, 0, 0, 0, 255)) {
          black_pixels++;
        } else {
          unexpected_pixels++;
        }
      }
    }
    check(is_bgra(pixels, kTargetSize / 2, kTargetSize / 2, 0, 255, 0, 255),
          "readback contains the exact green triangle at target center");
    check(is_bgra(pixels, 2, 2, 0, 0, 0, 255),
          "readback retains the exact black clear color outside the triangle");
    check(green_pixels > 100 && black_pixels > 100 && unexpected_pixels == 0,
          "readback contains only the synthetic draw and clear colors");

    if (failures) {
      std::printf("FAIL: %d Jak II offscreen submit/readback checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak II Direct GIF payload submitted and read back from Metal offscreen\n");
    return 0;
  }
}
