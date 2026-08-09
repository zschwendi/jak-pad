#include <cstdio>
#include <vector>

#include "game/graphics/pipelines/metal/metal_direct_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_jak2_raw_image_upload_executor.h"
#include "game/graphics/pipelines/metal/metal_jak2_raw_image_upload_fixture.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>
#import <TargetConditionals.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kTargetSize = 64;
int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  failures += !condition;
}

bool is_bgra(const std::vector<u8>& pixels,
             int x,
             int y,
             u8 blue,
             u8 green,
             u8 red,
             u8 alpha) {
  const std::size_t offset = static_cast<std::size_t>(y * kTargetSize + x) * 4;
  return pixels[offset] == blue && pixels[offset + 1] == green &&
         pixels[offset + 2] == red && pixels[offset + 3] == alpha;
}

struct UploadCallback {
  metal_renderer::Jak2RawImageUploadExecutor* executor = nullptr;
  const metal_renderer::Jak2RawImageUploadPlan* plan = nullptr;
  MetalDirectRenderer* renderer = nullptr;
  int calls = 0;
  int draw_calls_at_publication = -1;
  bool executed = false;
};

void publish_raw_image(void* opaque, u32 bucket_id) {
  auto* callback = static_cast<UploadCallback*>(opaque);
  callback->calls++;
  callback->draw_calls_at_publication =
      callback->renderer ? callback->renderer->stats().draw_calls : -1;
  callback->executed =
      bucket_id == metal_renderer::kJak2RawImageUploadBucket && callback->executor &&
      callback->plan && callback->executor->execute(*callback->plan);
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
        g_goalpad_metallib, g_goalpad_metallib_size, nullptr,
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError* library_error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:library_data error:&library_error];
    check(queue != nil && library != nil, "loaded the Metal queue and embedded shader library");
    if (!queue || !library) {
      if (library_error) {
        std::printf("Metal library error: %s\n", library_error.localizedDescription.UTF8String);
      }
      return 1;
    }

    MetalPsoCache pso_cache;
    MetalSamplerCache sampler_cache;
    MetalStreamBuffer stream;
    check(pso_cache.init(device, library), "initialized the Metal pipeline cache");
    sampler_cache.init(device);
    stream.init(device);
    stream.reset();

    TexturePool texture_pool(GameVersion::Jak2);
    check(metal_setup_placeholder(device, queue, texture_pool),
          "published the Metal placeholder texture");
    const u64 placeholder_handle = texture_pool.get_placeholder_texture();

    auto fixture = metal_renderer::make_jak2_raw_image_upload_fixture();
    const auto plan = metal_renderer::plan_jak2_raw_image_upload(
        fixture.ee_memory.data(), fixture.ee_memory.size(), fixture.chain_offset,
        fixture.ee_memory.data(), fixture.ee_memory.size());
    check(plan && plan->present, "planned the exact public bucket-318 raw-image grammar");
    if (!plan || failures) {
      metal_texture_release(placeholder_handle);
      texture_pool.set_placeholder(0);
      return 1;
    }

    metal_renderer::Jak2RawImageUploadExecutor executor(device, queue, &texture_pool);
    const int batch_size = metal_renderer::jak2_metal_direct_batch_size(
        metal_renderer::kJak2RawImageUploadBucket);
    check(batch_size == 1024 * 6, "bucket 318 retains the OpenGL Direct batch capacity");
    MetalHostTextureUploadDirectRenderer renderer(
        "debug-no-zbuf1", metal_renderer::kJak2RawImageUploadBucket, batch_size,
        MetalHostTextureUploadDirectRenderer::CallbackPoint::PcPort12);
    UploadCallback callback{&executor, &*plan, &renderer};

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

    id<MTLCommandBuffer> commands = [queue commandBuffer];
    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
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
    check(commands != nil && encoder != nil, "created an offscreen command buffer and pass");
    if (!commands || !encoder || !color || !depth) {
      executor.detach_pool();
      metal_texture_release(placeholder_handle);
      texture_pool.set_placeholder(0);
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
    state.texture_pool = &texture_pool;
    state.game_res_w = kTargetSize;
    state.game_res_h = kTargetSize;
    state.next_bucket = fixture.bucket_offset + 16;
    state.host_bucket_context = &callback;
    state.host_bucket_callback = publish_raw_image;

    DmaFollower dma(fixture.ee_memory.data(), fixture.bucket_offset,
                    fixture.ee_memory.size());
    renderer.render(dma, &state, context);

    const auto& stats = renderer.stats();
    check(dma.current_tag_offset() == state.next_bucket && callback.calls == 1 &&
              callback.draw_calls_at_publication == 1 && callback.executed &&
              executor.stats().publications == 1,
          "Direct-before work flushes before publication at the source PC_PORT 12 marker");
    check(texture_pool.lookup(metal_renderer::kJak2RawImageDestination).value_or(0) ==
              executor.stats().texture_handle,
          "the published synthetic raw image resolves at TEX0 TBP0");
    check(stats.draw_calls == 3 && stats.triangles == 4 &&
              stats.textured_draw_calls == 2 && stats.missing_texture_draw_calls == 0 &&
              context.draw_calls == 3 && context.triangles == 4,
          "the source black clear and RGB_ONLY fullscreen quad encode four triangles");
    check(stats.last_batch.valid && stats.last_batch.textured &&
              stats.last_batch.tex0_tbp == metal_renderer::kJak2RawImageDestination &&
              stats.last_batch.texture_lookup_hit && !stats.last_batch.used_placeholder,
          "the textured Direct batch records an exact host texture lookup");
    [encoder endEncoding];

#if TARGET_OS_OSX
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit synchronizeResource:color];
    [blit endEncoding];
#endif

    [commands commit];
    [commands waitUntilCompleted];
    check(commands.status == MTLCommandBufferStatusCompleted,
          "the offscreen raw-image command buffer completed");
    if (commands.status != MTLCommandBufferStatusCompleted && commands.error) {
      std::printf("Metal command-buffer error: %s\n",
                  commands.error.localizedDescription.UTF8String);
    }

    std::vector<u8> pixels(kTargetSize * kTargetSize * 4);
    [color getBytes:pixels.data()
        bytesPerRow:kTargetSize * 4
         fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
        mipmapLevel:0];
    int red_pixels = 0;
    int unexpected_pixels = 0;
    for (int y = 0; y < kTargetSize; ++y) {
      for (int x = 0; x < kTargetSize; ++x) {
        if (is_bgra(pixels, x, y, 0, 0, 255, 255)) {
          red_pixels++;
        } else {
          unexpected_pixels++;
        }
      }
    }
    check(is_bgra(pixels, kTargetSize / 2, kTargetSize / 2, 0, 0, 255, 255),
          "frame readback contains the exact synthetic opaque red image at center");
    check(is_bgra(pixels, 2, 2, 0, 0, 255, 255),
          "frame readback contains the exact synthetic image at the fullscreen corner");
    check(red_pixels == kTargetSize * kTargetSize && unexpected_pixels == 0,
          "the source-ordered black background is fully covered by the synthetic image");

    executor.detach_pool();
    metal_texture_release(placeholder_handle);
    texture_pool.set_placeholder(0);

    if (failures) {
      std::printf("FAIL: %d Jak II raw-image upload/readback checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak II bucket 318 host upload rendered its synthetic raw image\n");
    return 0;
  }
}
