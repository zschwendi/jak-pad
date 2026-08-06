#include <array>
#include <cstdio>
#include <vector>

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_glow_renderer.h"
#include "game/graphics/pipelines/metal/metal_pool_texture.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>
#import <TargetConditionals.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kTargetSize = 64;
constexpr u32 kFlareTbp = 0x2a0;
constexpr u32 kMissingFlareTbp = 0x2a1;

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    failures++;
  }
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

SpriteGlowOutput make_center_flare() {
  SpriteGlowOutput flare = {};
  constexpr float kZ = 16777215.f;
  flare.flare_xyzw[0] = math::Vector4f{1984.f, 1996.f, kZ, 7.f};
  flare.flare_xyzw[1] = math::Vector4f{2112.f, 1996.f, kZ, 8.f};
  flare.flare_xyzw[2] = math::Vector4f{2112.f, 2100.f, kZ, 9.f};
  flare.flare_xyzw[3] = math::Vector4f{1984.f, 2100.f, kZ, 10.f};
  flare.flare_draw_color = math::Vector4f{64.f, 0.f, 0.f, 128.f};
  flare.adgif.tex0_data = kFlareTbp | (1ull << 34);
  flare.adgif.tex0_addr = (u32)GsRegisterAddress::TEX0_1;
  flare.adgif.tex1_data = 1ull << 5;
  flare.adgif.tex1_addr = (u32)GsRegisterAddress::TEX1_1;
  flare.adgif.mip_addr = (u32)GsRegisterAddress::MIPTBP1_1;
  flare.adgif.clamp_data = 0b101;
  flare.adgif.clamp_addr = (u32)GsRegisterAddress::CLAMP_1;
  flare.adgif.alpha_addr = (u32)GsRegisterAddress::ALPHA_1;
  return flare;
}

void check_invalid_record(MetalGlowRenderer& renderer,
                          const SpriteGlowOutput& flare,
                          MetalSharedRenderState* state,
                          MetalFrameContext& context,
                          const char* what) {
  const int draw_calls_before = context.draw_calls;
  const int triangles_before = context.triangles;
  renderer.draw_force_visible(&flare, 1, state, context);
  check(renderer.stats().sprites_submitted == 1 && renderer.stats().invalid_records == 1 &&
            renderer.stats().sprites_drawn == 0 && renderer.stats().draw_calls == 0 &&
            renderer.stats().triangles == 0 && renderer.stats().missing_textures == 0 &&
            context.draw_calls == draw_calls_before && context.triangles == triangles_before,
        what);
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

    TexturePool texture_pool(GameVersion::Jak2);
    check(metal_setup_placeholder(device, queue, texture_pool),
          "published the Metal placeholder texture");
    const u64 placeholder_handle = texture_pool.get_placeholder_texture();
    constexpr std::array<u32, 4> kOpaqueWhite = {
        0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    MetalPoolTexture flare_texture(device, queue, &texture_pool, 2, 2, kFlareTbp,
                                   "synthetic-glow-final-flare");
    check(flare_texture.publish(kOpaqueWhite.data(), kOpaqueWhite.size()),
          "published a known opaque texture at the flare TBP");
    check(texture_pool.lookup(kFlareTbp).value_or(0) == flare_texture.handle(),
          "TexturePool resolves the exact flare TBP to the published texture");
    if (failures) {
      flare_texture.detach_pool();
      metal_texture_release(placeholder_handle);
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

    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    check(commands != nil && encoder != nil, "created an offscreen Metal command buffer and pass");
    if (!commands || !encoder || !color || !depth) {
      flare_texture.detach_pool();
      metal_texture_release(placeholder_handle);
      return 1;
    }

    MetalFrameContext context;
    context.enc = encoder;
    context.pso_cache = &pso_cache;
    context.sampler_cache = &sampler_cache;
    context.stream = &stream;
    context.color_format = MTLPixelFormatBGRA8Unorm;
    context.depth_format = MTLPixelFormatDepth32Float_Stencil8;

    MetalSharedRenderState state;
    state.version = GameVersion::Jak2;
    state.texture_pool = &texture_pool;
    state.game_res_w = kTargetSize;
    state.game_res_h = kTargetSize;

    MetalGlowRenderer renderer;
    renderer.draw_force_visible(nullptr, 0, &state, context);
    check(renderer.stats().sprites_submitted == 0 && renderer.stats().sprites_drawn == 0 &&
              renderer.stats().draw_calls == 0 && renderer.stats().triangles == 0 &&
              renderer.stats().missing_textures == 0 && renderer.stats().invalid_records == 0 &&
              context.draw_calls == 0 && context.triangles == 0,
          "an empty force-visible batch encodes no draw and reports exact zero stats");

    SpriteGlowOutput invalid = make_center_flare();
    invalid.adgif.tex0_addr = (u32)GsRegisterAddress::TEX0_2;
    check_invalid_record(renderer, invalid, &state, context,
                         "an invalid TEX0 address is counted and encodes no draw");

    invalid = make_center_flare();
    invalid.adgif.tex0_data &= ~(1ull << 34);
    check_invalid_record(renderer, invalid, &state, context,
                         "a disabled TEX0 TCC is counted and encodes no draw");

    invalid = make_center_flare();
    invalid.adgif.tex0_data |= 1ull << 35;
    check_invalid_record(renderer, invalid, &state, context,
                         "a non-modulate TEX0 TFX is counted and encodes no draw");

    invalid = make_center_flare();
    invalid.adgif.tex1_addr = (u32)GsRegisterAddress::TEX1_2;
    check_invalid_record(renderer, invalid, &state, context,
                         "an invalid TEX1 address is counted and encodes no draw");

    invalid = make_center_flare();
    invalid.adgif.mip_addr = (u32)GsRegisterAddress::MIPTBP1_2;
    check_invalid_record(renderer, invalid, &state, context,
                         "an invalid MIP address is counted and encodes no draw");

    invalid = make_center_flare();
    invalid.adgif.alpha_addr = (u32)GsRegisterAddress::ALPHA_2;
    check_invalid_record(renderer, invalid, &state, context,
                         "an invalid ALPHA address is counted and encodes no draw");

    invalid = make_center_flare();
    invalid.adgif.clamp_addr = (u32)GsRegisterAddress::FRAME_1;
    check_invalid_record(renderer, invalid, &state, context,
                         "an unsupported CLAMP address is counted and encodes no draw");

    invalid = make_center_flare();
    invalid.adgif.clamp_data = 0b010;
    check_invalid_record(renderer, invalid, &state, context,
                         "an unsupported CLAMP value is counted and encodes no draw");
    check(pso_cache.pipeline_count() == 0 && sampler_cache.count() == 0,
          "all rejected records leave the Metal pipeline and sampler caches untouched");

    SpriteGlowOutput missing = make_center_flare();
    missing.adgif.tex0_data =
        (missing.adgif.tex0_data & ~0x3fffull) | static_cast<u64>(kMissingFlareTbp);
    for (auto& position : missing.flare_xyzw) {
      position.x() += 1024.f;
    }
    renderer.draw_force_visible(&missing, 1, &state, context);
    check(renderer.stats().sprites_submitted == 1 && renderer.stats().invalid_records == 0 &&
              renderer.stats().sprites_drawn == 1 && renderer.stats().draw_calls == 1 &&
              renderer.stats().triangles == 2 && renderer.stats().missing_textures == 1 &&
              context.draw_calls == 1 && context.triangles == 2 &&
              pso_cache.pipeline_count() == 1 && sampler_cache.count() == 1,
          "a valid missing TBP uses the placeholder and reports one missing texture draw");
    context.draw_calls = 0;
    context.triangles = 0;

    const SpriteGlowOutput flare = make_center_flare();
    renderer.draw_force_visible(&flare, 1, &state, context);
    check(renderer.stats().sprites_submitted == 1 && renderer.stats().sprites_drawn == 1 &&
              renderer.stats().draw_calls == 1 && renderer.stats().triangles == 2 &&
              renderer.stats().missing_textures == 0 && renderer.stats().invalid_records == 0 &&
              context.draw_calls == 1 && context.triangles == 2,
          "one force-visible flare encodes one two-triangle draw with an exact texture hit");
    [encoder endEncoding];

#if TARGET_OS_OSX
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit synchronizeResource:color];
    [blit endEncoding];
#endif

    [commands commit];
    [commands waitUntilCompleted];
    check(commands.status == MTLCommandBufferStatusCompleted,
          "the glow flare command buffer completed");
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
    int clear_pixels = 0;
    int unexpected_pixels = 0;
    for (int y = 0; y < kTargetSize; y++) {
      for (int x = 0; x < kTargetSize; x++) {
        if (is_bgra(pixels, x, y, 0, 0, 255, 255)) {
          red_pixels++;
        } else if (is_bgra(pixels, x, y, 0, 0, 0, 0)) {
          clear_pixels++;
        } else {
          unexpected_pixels++;
        }
      }
    }
    check(is_bgra(pixels, kTargetSize / 2, kTargetSize / 2, 0, 0, 255, 255),
          "readback contains the exact force-visible flare color at target center");
    check(is_bgra(pixels, 2, 2, 0, 0, 0, 0),
          "readback retains the exact clear color outside the flare");
    check(red_pixels > 100 && clear_pixels > 100 && unexpected_pixels == 0,
          "readback contains only the synthetic final flare and unchanged outside pixels");

    flare_texture.detach_pool();
    metal_texture_release(placeholder_handle);

    if (failures) {
      std::printf("FAIL: %d Jak II final glow flare checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak II force-visible final glow flare submitted and read back from Metal "
                "offscreen\n");
    return 0;
  }
}
