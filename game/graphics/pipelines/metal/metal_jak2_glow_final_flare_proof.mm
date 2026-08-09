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

SpriteGlowOutput make_flare(float x0, float x1, float sample_x0, float sample_x1) {
  SpriteGlowOutput flare = {};
  constexpr float kZ = 16777215.f;
  flare.flare_xyzw[0] = math::Vector4f{x0, 1996.f, kZ, 7.f};
  flare.flare_xyzw[1] = math::Vector4f{x1, 1996.f, kZ, 8.f};
  flare.flare_xyzw[2] = math::Vector4f{x1, 2100.f, kZ, 9.f};
  flare.flare_xyzw[3] = math::Vector4f{x0, 2100.f, kZ, 10.f};
  flare.flare_draw_color = math::Vector4f{64.f, 0.f, 0.f, 128.f};
  flare.offscreen_uv[0] = math::Vector2f{sample_x0, 0.f};
  flare.offscreen_uv[1] = math::Vector2f{sample_x1, 416.f};
  flare.second_clear_pos[0].z() = 8388608.f;
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

SpriteGlowOutput make_center_flare() {
  return make_flare(1984.f, 2112.f, 0.f, 512.f);
}

void check_invalid_record(MetalGlowRenderer& renderer,
                          const SpriteGlowOutput& flare,
                          MetalSharedRenderState* state,
                          MetalFrameContext& context,
                          const char* what) {
  const int draw_calls_before = context.draw_calls;
  const int triangles_before = context.triangles;
  renderer.draw(&flare, 1, state, context);
    check(renderer.stats().sprites_submitted == 1 && renderer.stats().invalid_records == 1 &&
            renderer.stats().sprites_drawn == 0 && renderer.stats().draw_calls == 0 &&
            renderer.stats().triangles == 0 && renderer.stats().visibility_draw_calls == 0 &&
            renderer.stats().visibility_triangles == 0 &&
            renderer.stats().missing_textures == 0 &&
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
    depth_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
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
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 0.0;
    pass.stencilAttachment.texture = depth;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
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
    context.cmds = commands;
    context.game_color = color;
    context.game_depth = depth;

    MetalSharedRenderState state;
    state.version = GameVersion::Jak2;
    state.texture_pool = &texture_pool;
    state.game_res_w = kTargetSize;
    state.game_res_h = kTargetSize;

    MetalGlowRenderer renderer;
    renderer.draw(nullptr, 0, &state, context);
    check(renderer.stats().sprites_submitted == 0 && renderer.stats().sprites_drawn == 0 &&
              renderer.stats().draw_calls == 0 && renderer.stats().triangles == 0 &&
              renderer.stats().visibility_draw_calls == 0 &&
              renderer.stats().visibility_triangles == 0 &&
              renderer.stats().missing_textures == 0 && renderer.stats().invalid_records == 0 &&
              context.draw_calls == 0 && context.triangles == 0,
          "an empty glow batch encodes no draw and reports exact zero stats");

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
    renderer.draw(&missing, 1, &state, context);
    check(renderer.stats().sprites_submitted == 1 && renderer.stats().invalid_records == 0 &&
              renderer.stats().sprites_drawn == 1 && renderer.stats().draw_calls == 1 &&
              renderer.stats().triangles == 2 && renderer.stats().visibility_draw_calls == 6 &&
              renderer.stats().visibility_triangles == 12 &&
              renderer.stats().missing_textures == 1 &&
              context.draw_calls == 7 && context.triangles == 14 &&
              pso_cache.pipeline_count() == 4 && sampler_cache.count() == 1,
          "a valid missing TBP runs visibility, uses the placeholder, and reports one draw");
    context.draw_calls = 0;
    context.triangles = 0;

    const SpriteGlowOutput flare = make_center_flare();
    renderer.draw(&flare, 1, &state, context);
    check(renderer.stats().sprites_submitted == 1 && renderer.stats().sprites_drawn == 1 &&
              renderer.stats().draw_calls == 1 && renderer.stats().triangles == 2 &&
              renderer.stats().visibility_draw_calls == 6 &&
              renderer.stats().visibility_triangles == 12 &&
              renderer.stats().missing_textures == 0 && renderer.stats().invalid_records == 0 &&
              context.draw_calls == 7 && context.triangles == 14,
          "one visible flare encodes the probe chain and one exact-texture final draw");
    [context.enc endEncoding];

#if TARGET_OS_OSX
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit synchronizeResource:color];
    [blit endEncoding];
#endif

    [commands commit];
    [commands waitUntilCompleted];
    check(commands.status == MTLCommandBufferStatusCompleted,
          "the unoccluded glow command buffer completed");
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
          "an unoccluded opaque flare has the exact final color at target center");
    check(is_bgra(pixels, 2, 2, 0, 0, 0, 0),
          "readback retains the exact clear color outside the flare");
    check(red_pixels > 100 && clear_pixels > 100 && unexpected_pixels == 0,
          "unoccluded readback contains only the flare and unchanged outside pixels");

    stream.reset();
    id<MTLCommandBuffer> occluded_commands = [queue commandBuffer];
    auto* occluded_pass = [MTLRenderPassDescriptor renderPassDescriptor];
    occluded_pass.colorAttachments[0].texture = color;
    occluded_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    occluded_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    occluded_pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    occluded_pass.depthAttachment.texture = depth;
    occluded_pass.depthAttachment.loadAction = MTLLoadActionClear;
    occluded_pass.depthAttachment.storeAction = MTLStoreActionStore;
    occluded_pass.depthAttachment.clearDepth = 1.0;
    occluded_pass.stencilAttachment.texture = depth;
    occluded_pass.stencilAttachment.loadAction = MTLLoadActionClear;
    occluded_pass.stencilAttachment.storeAction = MTLStoreActionStore;
    occluded_pass.stencilAttachment.clearStencil = 0;
    context.enc = [occluded_commands renderCommandEncoderWithDescriptor:occluded_pass];
    context.cmds = occluded_commands;
    context.draw_calls = 0;
    context.triangles = 0;

    renderer.draw(&flare, 1, &state, context);
    check(renderer.stats().sprites_submitted == 1 && renderer.stats().sprites_drawn == 1 &&
              renderer.stats().draw_calls == 1 && renderer.stats().triangles == 2 &&
              renderer.stats().visibility_draw_calls == 6 &&
              renderer.stats().visibility_triangles == 12 &&
              renderer.stats().missing_textures == 0 && renderer.stats().invalid_records == 0 &&
              context.draw_calls == 7 && context.triangles == 14,
          "one occluded flare still encodes the source-exact visibility and final passes");
    [context.enc endEncoding];
#if TARGET_OS_OSX
    id<MTLBlitCommandEncoder> occluded_blit = [occluded_commands blitCommandEncoder];
    [occluded_blit synchronizeResource:color];
    [occluded_blit endEncoding];
#endif
    [occluded_commands commit];
    [occluded_commands waitUntilCompleted];
    check(occluded_commands.status == MTLCommandBufferStatusCompleted,
          "the occluded glow command buffer completed");
    if (occluded_commands.status != MTLCommandBufferStatusCompleted && occluded_commands.error) {
      std::printf("Metal command-buffer error: %s\n",
                  occluded_commands.error.localizedDescription.UTF8String);
    }

    [color getBytes:pixels.data()
        bytesPerRow:kTargetSize * 4
         fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
        mipmapLevel:0];
    int non_black_occluded_pixels = 0;
    for (int y = 0; y < kTargetSize; y++) {
      for (int x = 0; x < kTargetSize; x++) {
        const std::size_t offset = static_cast<std::size_t>(y * kTargetSize + x) * 4;
        if (pixels[offset] != 0 || pixels[offset + 1] != 0 || pixels[offset + 2] != 0) {
          non_black_occluded_pixels++;
        }
      }
    }
    check(non_black_occluded_pixels == 0,
          "a fully occluded opaque flare contributes no visible color to the final target");

    flare_texture.detach_pool();
    metal_texture_release(placeholder_handle);

    if (failures) {
      std::printf("FAIL: %d Jak II final glow flare checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak II Metal glow visibility rejects occluded flares and preserves visible "
                "flares\n");
    return 0;
  }
}
