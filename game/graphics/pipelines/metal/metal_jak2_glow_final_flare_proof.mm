#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/dma/dma.h"

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_glow_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_sprite_texture_upload_plan.h"
#include "game/graphics/pipelines/metal/metal_renderer.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>
#import <TargetConditionals.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kTargetSize = 64;
constexpr u32 kFlareTbp = 0x2a0;
constexpr u32 kPlaceholderFlareTbp = 0x2a1;
constexpr u16 kFlareTexturePage = 11;
constexpr u32 kChainOffset = 0x100;
constexpr u32 kUploadGroupOffset = 0x2000;
constexpr u32 kUploadTailOffset = 0x3000;
constexpr u32 kTexturePageOffset = 0x6000;
constexpr u32 kTextureObjectOffset = 0x6100;
constexpr u32 kPlaceholderTextureObjectOffset = 0x6200;
constexpr u32 kSyntheticS7 = 0x7f00;
constexpr std::size_t kFixtureMemorySize = 0x10000;

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

bool is_black_rgb(const std::vector<u8>& pixels, int x, int y) {
  const std::size_t offset = static_cast<std::size_t>(y * kTargetSize + x) * 4;
  return pixels[offset] == 0 && pixels[offset + 1] == 0 && pixels[offset + 2] == 0;
}

bool is_half_red(const std::vector<u8>& pixels, int x, int y) {
  const std::size_t offset = static_cast<std::size_t>(y * kTargetSize + x) * 4;
  return pixels[offset] == 0 && pixels[offset + 1] == 0 &&
         (pixels[offset + 2] == 127 || pixels[offset + 2] == 128) && pixels[offset + 3] == 255;
}

u8 red_at(const std::vector<u8>& pixels, int x, int y) {
  const std::size_t offset = static_cast<std::size_t>(y * kTargetSize + x) * 4;
  return pixels[offset + 2];
}

void put_u32(std::array<u8, kFixtureMemorySize>* memory, u32 offset, u32 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_u64(std::array<u8, kFixtureMemorySize>* memory, u32 offset, u64 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_tag(std::array<u8, kFixtureMemorySize>* memory,
             u32 offset,
             DmaTag::Kind kind,
             u16 qwc,
             u32 address,
             u32 vif0,
             u32 vif1) {
  const u64 tag = static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                  (static_cast<u64>(address) << 32);
  put_u64(memory, offset, tag);
  put_u32(memory, offset + 8, vif0);
  put_u32(memory, offset + 12, vif1);
}

struct SpriteTextureUploadFixture {
  std::array<u8, kFixtureMemorySize> packet = {};
  std::array<u8, kFixtureMemorySize> live = {};
};

SpriteTextureUploadFixture make_sprite_texture_upload_fixture() {
  SpriteTextureUploadFixture fixture;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr s64 kMode = -1;
  const u32 bucket_offset =
      kChainOffset + metal_renderer::kJak2SpriteTextureUploadBucket * 16;

  put_tag(&fixture.packet, bucket_offset, DmaTag::Kind::NEXT, 0, kUploadGroupOffset, 0, 0);
  put_tag(&fixture.packet, kUploadGroupOffset, DmaTag::Kind::CNT, 2, 0, 0, kDirect | 2);
  const u32 descriptor_offset = kUploadGroupOffset + 48;
  put_tag(&fixture.packet, descriptor_offset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
  put_u64(&fixture.packet, descriptor_offset + 16, kTexturePageOffset);
  put_u64(&fixture.packet, descriptor_offset + 24, static_cast<u64>(kMode));
  put_tag(&fixture.packet, descriptor_offset + 32, DmaTag::Kind::NEXT, 0,
          kUploadTailOffset, 0, 0);
  put_tag(&fixture.packet, kUploadTailOffset, DmaTag::Kind::CNT, 10, 0, kFlusha,
          kDirect | 10);
  put_tag(&fixture.packet, kUploadTailOffset + 176, DmaTag::Kind::NEXT, 0,
          bucket_offset + 16, 0, 0);

  GoalTexturePage page = {};
  page.id = kFlareTexturePage;
  page.length = 2;
  std::memcpy(fixture.live.data() + kTexturePageOffset, &page, sizeof(page));
  put_u32(&fixture.live, kTexturePageOffset + sizeof(page), kTextureObjectOffset);
  put_u32(&fixture.live, kTexturePageOffset + sizeof(page) + sizeof(u32),
          kPlaceholderTextureObjectOffset);
  GoalTexture texture = {};
  texture.w = 8;
  texture.h = 8;
  texture.num_mips = 1;
  texture.dest[0] = kFlareTbp;
  std::memcpy(fixture.live.data() + kTextureObjectOffset, &texture, sizeof(texture));
  texture.dest[0] = kPlaceholderFlareTbp;
  std::memcpy(fixture.live.data() + kPlaceholderTextureObjectOffset, &texture, sizeof(texture));
  return fixture;
}

void encode_depth_rect(id<MTLRenderCommandEncoder> encoder,
                       MetalPsoCache* pso_cache,
                       id<MTLTexture> bound_texture,
                       float u0,
                       float u1,
                       float depth) {
  const float x0 = u0 * 2.f - 1.f;
  const float x1 = u1 * 2.f - 1.f;
  const auto vertex = [depth](float x, float y) {
    ScaffoldVertex result = {};
    result.pos[0] = x;
    result.pos[1] = y;
    result.pos[2] = depth;
    return result;
  };
  const ScaffoldVertex vertices[6] = {
      vertex(x0, 1.f),  vertex(x1, 1.f),  vertex(x1, -1.f),
      vertex(x0, 1.f),  vertex(x1, -1.f), vertex(x0, -1.f),
  };

  MetalPsoKey pso_key;
  pso_key.shader = MetalShaderId::SCAFFOLD;
  pso_key.color_format = MTLPixelFormatBGRA8Unorm;
  pso_key.depth_format = MTLPixelFormatDepth32Float_Stencil8;
  pso_key.color_write_mask = MTLColorWriteMaskNone;
  MetalDepthStencilKey depth_key;
  depth_key.depth_test = true;
  depth_key.compare = MTLCompareFunctionGreater;
  depth_key.depth_write = true;
  [encoder setRenderPipelineState:pso_cache->get_pipeline(pso_key)];
  [encoder setDepthStencilState:pso_cache->get_depth_stencil(depth_key)];
  [encoder setCullMode:MTLCullModeNone];
  [encoder setVertexBytes:vertices length:sizeof(vertices) atIndex:0];
  [encoder setFragmentTexture:bound_texture atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
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
    constexpr int kFlareTextureSize = 8;
    std::array<u32, kFlareTextureSize * kFlareTextureSize> soft_flare = {};
    for (int y = 0; y < kFlareTextureSize; ++y) {
      for (int x = 0; x < kFlareTextureSize; ++x) {
        constexpr float kCenter = (kFlareTextureSize - 1) / 2.f;
        const float dx = x - kCenter;
        const float dy = y - kCenter;
        const float radius = std::sqrt(dx * dx + dy * dy) / kCenter;
        const u8 intensity =
            static_cast<u8>(std::clamp((1.f - radius) * 384.f, 0.f, 255.f));
        // RGBA8888 in host byte order: only red contributes to the final glow;
        // alpha follows the same falloff so the readback also catches a solid
        // placeholder in the additive alpha channel.
        soft_flare[y * kFlareTextureSize + x] =
            static_cast<u32>(intensity) | (static_cast<u32>(intensity) << 24);
      }
    }
    const u64 flare_handle = metal_upload_texture_rgba8(
        device, queue, reinterpret_cast<const u8*>(soft_flare.data()), kFlareTextureSize,
        kFlareTextureSize);
    const PcTextureId flare_id(kFlareTexturePage, 0);
    bool flare_registered = false;
    if (flare_handle) {
      TextureInput input;
      input.debug_page_name = "synthetic-sprite-page";
      input.debug_name = "synthetic-glow-final-flare";
      input.id = flare_id;
      input.gpu_texture = flare_handle;
      input.src_data = reinterpret_cast<const u8*>(soft_flare.data());
      input.w = kFlareTextureSize;
      input.h = kFlareTextureSize;
      std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
      texture_pool.give_texture(input);
      flare_registered = true;
    }
    check(flare_registered,
          "registered a synthetic transparent-falloff source texture by page and index");

    const auto upload_fixture = make_sprite_texture_upload_fixture();
    const auto upload_plan = metal_renderer::plan_jak2_sprite_texture_upload(
        upload_fixture.packet.data(), upload_fixture.packet.size(), kChainOffset,
        upload_fixture.live.data(), upload_fixture.live.size());
    check(upload_plan && upload_plan->present && upload_plan->upload_count == 1 &&
              upload_plan->uploads[0].page_offset == kTexturePageOffset &&
              upload_plan->uploads[0].mode == -1,
          "planned one exact source-shaped TEX_ALL_SPRITE ordinary upload");
    if (upload_plan) {
      for (std::size_t i = 0; i < upload_plan->upload_count; ++i) {
        texture_pool.handle_upload_now(
            upload_fixture.live.data() + upload_plan->uploads[i].page_offset,
            static_cast<int>(upload_plan->uploads[i].mode), upload_fixture.live.data(),
            kSyntheticS7, false);
      }
    }
    check(texture_pool.lookup(kFlareTbp).value_or(0) == flare_handle,
          "bucket-312 upload publishes the transparent-falloff source at the glow TBP");
    check(texture_pool.lookup(kPlaceholderFlareTbp).value_or(0) == placeholder_handle,
          "bucket-312 upload publishes the pool placeholder while converted flare data is absent");
    const auto release_textures = [&]() {
      if (flare_registered) {
        std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
        texture_pool.unload_texture(flare_id, flare_handle);
      }
      if (flare_handle) {
        metal_texture_release(flare_handle);
      }
      metal_texture_release(placeholder_handle);
    };
    if (failures) {
      release_textures();
      return 1;
    }

    auto* color_desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:kTargetSize
                                    height:kTargetSize
                                 mipmapped:NO];
    color_desc.textureType = MTLTextureType2DArray;
    color_desc.arrayLength = 2;
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
    depth_desc.textureType = MTLTextureType2DArray;
    depth_desc.arrayLength = 2;
    depth_desc.usage = MTLTextureUsageRenderTarget;
    depth_desc.storageMode = MTLStorageModePrivate;
    id<MTLTexture> depth = [device newTextureWithDescriptor:depth_desc];
    check(color != nil && depth != nil,
          "created two-slice render-target-only color and depth targets");

    constexpr u8 kSliceZeroBgra[4] = {0x3d, 0x7a, 0xc1, 0xff};
    std::vector<u8> slice_zero_sentinel(kTargetSize * kTargetSize * 4);
    for (std::size_t offset = 0; offset < slice_zero_sentinel.size(); offset += 4) {
      std::copy(kSliceZeroBgra, kSliceZeroBgra + 4, slice_zero_sentinel.data() + offset);
    }
    [color replaceRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
             mipmapLevel:0
                   slice:0
               withBytes:slice_zero_sentinel.data()
             bytesPerRow:kTargetSize * 4
           bytesPerImage:slice_zero_sentinel.size()];

    id<MTLCommandBuffer> commands = [queue commandBuffer];
    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].slice = 1;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.slice = 1;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 0.0;
    pass.stencilAttachment.texture = depth;
    pass.stencilAttachment.slice = 1;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.clearStencil = 0;

    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    check(commands != nil && encoder != nil, "created an offscreen Metal command buffer and pass");
    if (!commands || !encoder || !color || !depth) {
      release_textures();
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
    context.game_color_slice = 1;
    context.game_depth = depth;
    context.game_depth_slice = 1;
    context.game_viewport = {0.0, 0.0, kTargetSize, kTargetSize, 0.0, 1.0};
    [context.enc setViewport:context.game_viewport];

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
      bytesPerImage:pixels.size()
         fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
        mipmapLevel:0
              slice:1];

    int bright_red_pixels = 0;
    int falloff_red_pixels = 0;
    int clear_pixels = 0;
    for (int y = 0; y < kTargetSize; y++) {
      for (int x = 0; x < kTargetSize; x++) {
        if (is_bgra(pixels, x, y, 0, 0, 255, 255)) {
          bright_red_pixels++;
        } else if (is_bgra(pixels, x, y, 0, 0, 0, 0)) {
          clear_pixels++;
        } else if (red_at(pixels, x, y) > 0) {
          falloff_red_pixels++;
        }
      }
    }
    check(is_bgra(pixels, kTargetSize / 2, kTargetSize / 2, 0, 0, 255, 255),
          "an unoccluded opaque flare has the exact final color at target center");
    check(red_at(pixels, 25, 25) < 16,
          "a point inside the flare quad near its corner retains the texture's dark falloff");
    check(is_bgra(pixels, 2, 2, 0, 0, 0, 0),
          "readback retains the exact clear color outside the flare");
    check(bright_red_pixels > 0 && falloff_red_pixels > 0 && clear_pixels > 100,
          "readback distinguishes a bright center, textured falloff, and unchanged outside pixels");

    stream.reset();
    id<MTLCommandBuffer> missing_commands = [queue commandBuffer];
    auto* missing_pass = [MTLRenderPassDescriptor renderPassDescriptor];
    missing_pass.colorAttachments[0].texture = color;
    missing_pass.colorAttachments[0].slice = 1;
    missing_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    missing_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    missing_pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    missing_pass.depthAttachment.texture = depth;
    missing_pass.depthAttachment.slice = 1;
    missing_pass.depthAttachment.loadAction = MTLLoadActionClear;
    missing_pass.depthAttachment.storeAction = MTLStoreActionStore;
    missing_pass.depthAttachment.clearDepth = 0.0;
    missing_pass.stencilAttachment.texture = depth;
    missing_pass.stencilAttachment.slice = 1;
    missing_pass.stencilAttachment.loadAction = MTLLoadActionClear;
    missing_pass.stencilAttachment.storeAction = MTLStoreActionStore;
    missing_pass.stencilAttachment.clearStencil = 0;
    context.enc = [missing_commands renderCommandEncoderWithDescriptor:missing_pass];
    context.cmds = missing_commands;
    [context.enc setViewport:context.game_viewport];
    context.draw_calls = 0;
    context.triangles = 0;

    SpriteGlowOutput placeholder_backed = make_center_flare();
    placeholder_backed.adgif.tex0_data =
        (placeholder_backed.adgif.tex0_data & ~0x3fffull) |
        static_cast<u64>(kPlaceholderFlareTbp);
    renderer.draw(&placeholder_backed, 1, &state, context);
    check(renderer.stats().sprites_submitted == 1 && renderer.stats().invalid_records == 0 &&
              renderer.stats().sprites_drawn == 1 && renderer.stats().draw_calls == 1 &&
              renderer.stats().triangles == 2 && renderer.stats().visibility_draw_calls == 6 &&
              renderer.stats().visibility_triangles == 12 &&
              renderer.stats().missing_textures == 1 &&
              context.draw_calls == 7 && context.triangles == 14 &&
              pso_cache.pipeline_count() == 4 && sampler_cache.count() == 1,
          "a placeholder-backed TBP runs visibility, uses the fail-soft radial fallback, and "
          "reports one draw");
    [context.enc endEncoding];
#if TARGET_OS_OSX
    id<MTLBlitCommandEncoder> missing_blit = [missing_commands blitCommandEncoder];
    [missing_blit synchronizeResource:color];
    [missing_blit endEncoding];
#endif
    [missing_commands commit];
    [missing_commands waitUntilCompleted];
    check(missing_commands.status == MTLCommandBufferStatusCompleted,
          "the placeholder-backed glow command buffer completed");
    if (missing_commands.status != MTLCommandBufferStatusCompleted && missing_commands.error) {
      std::printf("Metal command-buffer error: %s\n",
                  missing_commands.error.localizedDescription.UTF8String);
    }
    [color getBytes:pixels.data()
        bytesPerRow:kTargetSize * 4
      bytesPerImage:pixels.size()
         fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
        mipmapLevel:0
              slice:1];
    check(is_bgra(pixels, kTargetSize / 2, kTargetSize / 2, 0, 0, 255, 255),
          "the placeholder-backed fallback keeps the flare center bright");
    check(red_at(pixels, 25, 25) < 16,
          "the placeholder-backed fallback fades near the flare quad corner");
    check(is_bgra(pixels, 2, 2, 0, 0, 0, 0),
          "the placeholder-backed fallback leaves pixels outside the flare unchanged");

    stream.reset();
    id<MTLCommandBuffer> boosted_commands = [queue commandBuffer];
    auto* boosted_pass = [MTLRenderPassDescriptor renderPassDescriptor];
    boosted_pass.colorAttachments[0].texture = color;
    boosted_pass.colorAttachments[0].slice = 1;
    boosted_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    boosted_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    boosted_pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    boosted_pass.depthAttachment.texture = depth;
    boosted_pass.depthAttachment.slice = 1;
    boosted_pass.depthAttachment.loadAction = MTLLoadActionClear;
    boosted_pass.depthAttachment.storeAction = MTLStoreActionStore;
    boosted_pass.depthAttachment.clearDepth = 0.0;
    boosted_pass.stencilAttachment.texture = depth;
    boosted_pass.stencilAttachment.slice = 1;
    boosted_pass.stencilAttachment.loadAction = MTLLoadActionClear;
    boosted_pass.stencilAttachment.storeAction = MTLStoreActionStore;
    boosted_pass.stencilAttachment.clearStencil = 0;
    context.enc = [boosted_commands renderCommandEncoderWithDescriptor:boosted_pass];
    context.cmds = boosted_commands;
    [context.enc setViewport:context.game_viewport];
    context.draw_calls = 0;
    context.triangles = 0;
    state.target_fps = 120.f;

    renderer.draw(&flare, 1, &state, context);
    check(renderer.stats().sprites_submitted == 1 && renderer.stats().sprites_drawn == 1 &&
              renderer.stats().draw_calls == 1 && renderer.stats().triangles == 2 &&
              renderer.stats().visibility_draw_calls == 6 &&
              renderer.stats().visibility_triangles == 12 &&
              renderer.stats().missing_textures == 0 && renderer.stats().invalid_records == 0 &&
              context.draw_calls == 7 && context.triangles == 14,
          "one 120 Hz flare preserves the exact visibility and draw accounting");
    [context.enc endEncoding];
#if TARGET_OS_OSX
    id<MTLBlitCommandEncoder> boosted_blit = [boosted_commands blitCommandEncoder];
    [boosted_blit synchronizeResource:color];
    [boosted_blit endEncoding];
#endif
    [boosted_commands commit];
    [boosted_commands waitUntilCompleted];
    check(boosted_commands.status == MTLCommandBufferStatusCompleted,
          "the 120 Hz glow command buffer completed");
    if (boosted_commands.status != MTLCommandBufferStatusCompleted && boosted_commands.error) {
      std::printf("Metal command-buffer error: %s\n",
                  boosted_commands.error.localizedDescription.UTF8String);
    }
    [color getBytes:pixels.data()
        bytesPerRow:kTargetSize * 4
      bytesPerImage:pixels.size()
         fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
        mipmapLevel:0
              slice:1];
    check(is_half_red(pixels, kTargetSize / 2, kTargetSize / 2),
          "a 120 Hz flare is half the 60 Hz RGB intensity within UNORM quantization");
    check(is_bgra(pixels, 2, 2, 0, 0, 0, 0),
          "120 Hz scaling leaves pixels outside the flare unchanged");
    state.target_fps = 60.f;

    stream.reset();
    id<MTLCommandBuffer> cell_commands = [queue commandBuffer];
    auto* cell_pass = [MTLRenderPassDescriptor renderPassDescriptor];
    cell_pass.colorAttachments[0].texture = color;
    cell_pass.colorAttachments[0].slice = 1;
    cell_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    cell_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    cell_pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    cell_pass.depthAttachment.texture = depth;
    cell_pass.depthAttachment.slice = 1;
    cell_pass.depthAttachment.loadAction = MTLLoadActionClear;
    cell_pass.depthAttachment.storeAction = MTLStoreActionStore;
    cell_pass.depthAttachment.clearDepth = 0.0;
    cell_pass.stencilAttachment.texture = depth;
    cell_pass.stencilAttachment.slice = 1;
    cell_pass.stencilAttachment.loadAction = MTLLoadActionClear;
    cell_pass.stencilAttachment.storeAction = MTLStoreActionStore;
    cell_pass.stencilAttachment.clearStencil = 0;
    context.enc = [cell_commands renderCommandEncoderWithDescriptor:cell_pass];
    context.cmds = cell_commands;
    [context.enc setViewport:context.game_viewport];
    context.draw_calls = 0;
    context.triangles = 0;

    id<MTLTexture> flare_metal_texture = metal_texture_lookup(flare_handle);
    check(flare_metal_texture != nil, "resolved the synthetic flare texture for depth setup");
    // Cell 0 samples clear depth, cell 1 samples the full first rectangle, and
    // cell 2 straddles the second rectangle's right edge at exactly half width.
    encode_depth_rect(context.enc, &pso_cache, flare_metal_texture, 0.375f, 0.625f, 0.75f);
    encode_depth_rect(context.enc, &pso_cache, flare_metal_texture, 0.75f, 0.875f, 0.75f);

    const std::array<SpriteGlowOutput, 3> cell_flares = {
        make_flare(1824.f, 1936.f, 0.f, 128.f),
        make_flare(1992.f, 2104.f, 192.f, 320.f),
        make_flare(2160.f, 2272.f, 384.f, 512.f),
    };
    renderer.draw(cell_flares.data(), cell_flares.size(), &state, context);
    check(renderer.stats().sprites_submitted == 3 && renderer.stats().sprites_drawn == 3 &&
              renderer.stats().draw_calls == 3 && renderer.stats().triangles == 6 &&
              renderer.stats().visibility_draw_calls == 6 &&
              renderer.stats().visibility_triangles == 36 &&
              renderer.stats().missing_textures == 0 && renderer.stats().invalid_records == 0 &&
              context.draw_calls == 9 && context.triangles == 42,
          "three visibility cells traverse all four downsamples and three final draws");
    [context.enc endEncoding];
#if TARGET_OS_OSX
    id<MTLBlitCommandEncoder> cell_blit = [cell_commands blitCommandEncoder];
    [cell_blit synchronizeResource:color];
    [cell_blit endEncoding];
#endif
    [cell_commands commit];
    [cell_commands waitUntilCompleted];
    check(cell_commands.status == MTLCommandBufferStatusCompleted,
          "the nonuniform multi-flare glow command buffer completed");
    if (cell_commands.status != MTLCommandBufferStatusCompleted && cell_commands.error) {
      std::printf("Metal command-buffer error: %s\n",
                  cell_commands.error.localizedDescription.UTF8String);
    }
    [color getBytes:pixels.data()
        bytesPerRow:kTargetSize * 4
      bytesPerImage:pixels.size()
         fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
        mipmapLevel:0
              slice:1];
    check(is_bgra(pixels, 11, kTargetSize / 2, 0, 0, 255, 255),
          "the unoccluded cell remains fully visible beside an occluded cell");
    check(is_black_rgb(pixels, 32, kTargetSize / 2),
          "the occluded cell receives no RGB bleed from either visible neighbor");
    check(is_half_red(pixels, 53, kTargetSize / 2),
          "the half-occluded cell averages to half-intensity visibility");
    check(is_bgra(pixels, 2, 2, 0, 0, 0, 0),
          "the nonuniform visibility test leaves outside pixels unchanged");

    stream.reset();
    id<MTLCommandBuffer> occluded_commands = [queue commandBuffer];
    auto* occluded_pass = [MTLRenderPassDescriptor renderPassDescriptor];
    occluded_pass.colorAttachments[0].texture = color;
    occluded_pass.colorAttachments[0].slice = 1;
    occluded_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    occluded_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    occluded_pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    occluded_pass.depthAttachment.texture = depth;
    occluded_pass.depthAttachment.slice = 1;
    occluded_pass.depthAttachment.loadAction = MTLLoadActionClear;
    occluded_pass.depthAttachment.storeAction = MTLStoreActionStore;
    occluded_pass.depthAttachment.clearDepth = 1.0;
    occluded_pass.stencilAttachment.texture = depth;
    occluded_pass.stencilAttachment.slice = 1;
    occluded_pass.stencilAttachment.loadAction = MTLLoadActionClear;
    occluded_pass.stencilAttachment.storeAction = MTLStoreActionStore;
    occluded_pass.stencilAttachment.clearStencil = 0;
    context.enc = [occluded_commands renderCommandEncoderWithDescriptor:occluded_pass];
    context.cmds = occluded_commands;
    [context.enc setViewport:context.game_viewport];
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
      bytesPerImage:pixels.size()
         fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
        mipmapLevel:0
              slice:1];
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

    std::vector<u8> slice_zero(slice_zero_sentinel.size());
    [color getBytes:slice_zero.data()
         bytesPerRow:kTargetSize * 4
       bytesPerImage:slice_zero.size()
          fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
         mipmapLevel:0
               slice:0];
    check(slice_zero == slice_zero_sentinel,
          "Glow depth snapshot and game-pass restarts preserve unselected slice 0");

    release_textures();

    if (failures) {
      std::printf("FAIL: %d Jak II final glow flare checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak II bucket-312 publication preserves Metal glow texture falloff, "
                "matches 60/120 Hz intensity, and isolates averaged visibility cells\n");
    return 0;
  }
}
