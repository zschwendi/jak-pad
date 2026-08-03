#import <TargetConditionals.h>
#include <cstdio>
#include <vector>

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_direct_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_jak2_test_packets.h"
#import <Metal/Metal.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kTargetSize = 64;

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    failures++;
  }
}

bool is_bgra(const std::vector<u8>& pixels, int x, int y, u8 blue, u8 green, u8 red, u8 alpha) {
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

    dispatch_data_t library_data = dispatch_data_create(g_goalpad_metallib, g_goalpad_metallib_size,
                                                        nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
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

    auto* color_desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
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

    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
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

    constexpr std::size_t kScreenFilter = static_cast<std::size_t>(jak2::BucketId::SCREEN_FILTER);
    const int batch_size = metal_renderer::jak2_metal_direct_batch_size(kScreenFilter);
    check(batch_size == 256, "SCREEN_FILTER retains its audited Jak II Direct batch size");
    if (batch_size != 256) {
      return 1;
    }

    MetalDirectRenderer direct("jak2-screen-filter-readback", static_cast<int>(kScreenFilter),
                               batch_size);
    direct.reset_state();
    const auto setup = metal_renderer::jak2_test::make_screen_filter_setup();
    const auto sprite = metal_renderer::jak2_test::make_screen_filter_sprite();
    direct.render_gif(setup.data(), static_cast<u32>(setup.size()), &state, context);
    direct.render_gif(sprite.data(), static_cast<u32>(sprite.size()), &state, context);
    direct.flush_pending(&state, context);
    [encoder endEncoding];

#if TARGET_OS_OSX
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit synchronizeResource:color];
    [blit endEncoding];
#endif

    check(context.draw_calls == 1 && context.triangles == 2 && direct.stats().draw_calls == 1 &&
              direct.stats().triangles == 2 && direct.stats().unsupported_blends == 0,
          "the audited SCREEN_FILTER packet encoded one supported sprite draw");

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

    int filtered_pixels = 0;
    int unexpected_pixels = 0;
    for (int y = 0; y < kTargetSize; y++) {
      for (int x = 0; x < kTargetSize; x++) {
        if (is_bgra(pixels, x, y, 0, 48, 128, 128)) {
          filtered_pixels++;
        } else {
          unexpected_pixels++;
        }
      }
    }
    check(is_bgra(pixels, kTargetSize / 2, kTargetSize / 2, 0, 48, 128, 128),
          "readback contains the exact half-alpha orange filter at target center");
    check(is_bgra(pixels, 0, 0, 0, 48, 128, 128),
          "the production full-screen sprite reaches the target corner");
    check(filtered_pixels == kTargetSize * kTargetSize && unexpected_pixels == 0,
          "every readback pixel matches the deterministic SCREEN_FILTER blend");

    if (failures) {
      std::printf("FAIL: %d Jak II offscreen submit/readback checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak II SCREEN_FILTER submitted and read back from Metal offscreen\n");
    return 0;
  }
}
