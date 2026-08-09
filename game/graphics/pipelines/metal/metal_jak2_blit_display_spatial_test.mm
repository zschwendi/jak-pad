#import <TargetConditionals.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/dma/dma.h"
#include "common/dma/gs.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_direct_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_blit_display_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"
#import <Metal/Metal.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kTargetSize = 64;
constexpr u32 kBlitBucket = static_cast<u32>(jak2::BucketId::BUCKET_3);
constexpr u32 kSkyDrawBucket = static_cast<u32>(jak2::BucketId::SKY_DRAW);
constexpr u32 kProgressBucket = static_cast<u32>(jak2::BucketId::PROGRESS);

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  failures += !condition;
}

constexpr u32 vif(VifCode::Kind kind, u16 immediate = 0) {
  return (static_cast<u32>(kind) << 24) | immediate;
}

void put_tag(std::vector<u8>* chain,
             u32 offset,
             DmaTag::Kind kind,
             u16 qwc = 0,
             u32 address = 0,
             u32 vif0 = 0,
             u32 vif1 = 0) {
  if (chain->size() < offset + 16) {
    chain->resize(offset + 16);
  }
  const u64 value =
      static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) | (static_cast<u64>(address) << 32);
  std::memcpy(chain->data() + offset, &value, sizeof(value));
  std::memcpy(chain->data() + offset + 8, &vif0, sizeof(vif0));
  std::memcpy(chain->data() + offset + 12, &vif1, sizeof(vif1));
}

std::vector<u8> make_blit_chain(metal_renderer::Jak2BlitDisplayCommand command) {
  std::vector<u8> chain(128);
  if (command == metal_renderer::Jak2BlitDisplayCommand::None) {
    put_tag(&chain, 0, DmaTag::Kind::CNT);
    put_tag(&chain, 16, DmaTag::Kind::END);
    return chain;
  }

  constexpr u32 kPayloadOffset = 64;
  put_tag(&chain, 0, DmaTag::Kind::NEXT, 0, kPayloadOffset);
  if (command == metal_renderer::Jak2BlitDisplayCommand::Snapshot) {
    put_tag(&chain, kPayloadOffset, DmaTag::Kind::CNT, 1, 0, vif(VifCode::Kind::PC_PORT, 0x10),
            vif(VifCode::Kind::PC_PORT, metal_renderer::kJak2BlitDisplayTbp));
    put_tag(&chain, kPayloadOffset + 32, DmaTag::Kind::NEXT, 0, 16);
  } else {
    put_tag(&chain, kPayloadOffset, DmaTag::Kind::CNT, 0, 0, vif(VifCode::Kind::PC_PORT, 0x11),
            vif(VifCode::Kind::PC_PORT));
    put_tag(&chain, kPayloadOffset + 16, DmaTag::Kind::NEXT, 0, 16);
  }
  put_tag(&chain, 16, DmaTag::Kind::END);
  return chain;
}

void append_qword(std::vector<u8>* data, u64 low, u64 high = 0) {
  const std::size_t offset = data->size();
  data->resize(offset + 16);
  std::memcpy(data->data() + offset, &low, sizeof(low));
  std::memcpy(data->data() + offset + 8, &high, sizeof(high));
}

void append_gif_tag(std::vector<u8>* data,
                    u16 nloop,
                    u64 registers,
                    u8 nreg,
                    bool pre,
                    u64 prim,
                    bool eop = true) {
  const u64 low = static_cast<u64>(nloop) | (static_cast<u64>(eop) << 15) |
                  (pre ? (1ull << 46) : 0) | (prim << 47) | (static_cast<u64>(nreg) << 60);
  append_qword(data, low, registers);
}

void append_rgbaq(std::vector<u8>* data, const std::array<u8, 4>& color) {
  std::array<u32, 4> packed = {color[0], color[1], color[2], color[3]};
  const std::size_t offset = data->size();
  data->resize(offset + 16);
  std::memcpy(data->data() + offset, packed.data(), 16);
}

void append_xyzf2(std::vector<u8>* data, u32 x, u32 y) {
  constexpr u64 kZ = 0xffffff;
  append_qword(data, static_cast<u64>(x) | (static_cast<u64>(y) << 32), kZ << 4);
}

void append_st(std::vector<u8>* data, float s, float t) {
  constexpr float kQ = 1.f;
  const std::size_t offset = data->size();
  data->resize(offset + 16);
  std::memcpy(data->data() + offset, &s, sizeof(s));
  std::memcpy(data->data() + offset + 4, &t, sizeof(t));
  std::memcpy(data->data() + offset + 8, &kQ, sizeof(kQ));
}

void append_basic_sprite(std::vector<u8>* payload,
                         u32 x0,
                         u32 y0,
                         u32 x1,
                         u32 y1,
                         const std::array<u8, 4>& color,
                         bool eop = true) {
  constexpr u64 kRgbaq = static_cast<u64>(GifTag::RegisterDescriptor::RGBAQ);
  constexpr u64 kXyzf2 = static_cast<u64>(GifTag::RegisterDescriptor::XYZF2);
  constexpr u64 kRegisters = kRgbaq | (kXyzf2 << 4) | (kRgbaq << 8) | (kXyzf2 << 12);
  constexpr u64 kPrim = static_cast<u64>(GsPrim::Kind::SPRITE);
  append_gif_tag(payload, 1, kRegisters, 4, true, kPrim, eop);
  append_rgbaq(payload, color);
  append_xyzf2(payload, x0, y0);
  append_rgbaq(payload, color);
  append_xyzf2(payload, x1, y1);
}

std::vector<u8> make_spatial_frame_a() {
  std::vector<u8> payload;
  append_basic_sprite(&payload, 0x7000, 0x7300, 0x8000, 0x8000, {180, 40, 20, 128}, false);
  append_basic_sprite(&payload, 0x8000, 0x7300, 0x9000, 0x8000, {40, 180, 40, 128}, false);
  append_basic_sprite(&payload, 0x7000, 0x8000, 0x8000, 0x8d00, {20, 40, 180, 128}, false);
  append_basic_sprite(&payload, 0x8000, 0x8000, 0x9000, 0x8d00, {160, 120, 60, 128});
  return payload;
}

std::vector<u8> make_snapshot_sky_draw() {
  std::vector<u8> payload;
  constexpr u64 kAd = static_cast<u64>(GifTag::RegisterDescriptor::AD);
  append_gif_tag(&payload, 3, kAd, 1, false, 0, false);
  constexpr u64 kTex0 = metal_renderer::kJak2BlitDisplayTbp | (1ull << 14) | (6ull << 26) |
                        (6ull << 30) | (1ull << 34);
  append_qword(&payload, kTex0, static_cast<u64>(GsRegisterAddress::TEX0_1));
  append_qword(&payload, (1ull << 5) | (1ull << 6), static_cast<u64>(GsRegisterAddress::TEX1_1));
  append_qword(&payload, 0b101, static_cast<u64>(GsRegisterAddress::CLAMP_1));

  constexpr u64 kSt = static_cast<u64>(GifTag::RegisterDescriptor::ST);
  constexpr u64 kRgbaq = static_cast<u64>(GifTag::RegisterDescriptor::RGBAQ);
  constexpr u64 kXyzf2 = static_cast<u64>(GifTag::RegisterDescriptor::XYZF2);
  constexpr u64 kRegisters =
      kSt | (kRgbaq << 4) | (kXyzf2 << 8) | (kSt << 12) | (kRgbaq << 16) | (kXyzf2 << 20);
  constexpr u64 kPrim = static_cast<u64>(GsPrim::Kind::SPRITE) | (1ull << 4);
  constexpr std::array<u8, 4> kSepiaTint = {128, 96, 64, 64};
  append_gif_tag(&payload, 1, kRegisters, 6, true, kPrim);
  append_st(&payload, 0.f, 0.f);
  append_rgbaq(&payload, kSepiaTint);
  append_xyzf2(&payload, 0x7000, 0x7300);
  append_st(&payload, 1.f, 1.f);
  append_rgbaq(&payload, kSepiaTint);
  append_xyzf2(&payload, 0x9000, 0x8d00);
  return payload;
}

std::vector<u8> make_progress_overlay() {
  std::vector<u8> payload;
  append_basic_sprite(&payload, 0x7c00, 0x7b00, 0x8400, 0x8500, {240, 200, 20, 128});
  return payload;
}

std::vector<u8> make_copy_back_decoy() {
  std::vector<u8> payload;
  append_basic_sprite(&payload, 0x7000, 0x7300, 0x9000, 0x8d00, {200, 20, 200, 128});
  return payload;
}

struct Pixel {
  u8 r = 0;
  u8 g = 0;
  u8 b = 0;
  u8 a = 0;
};

Pixel pixel_at(const std::vector<u8>& bgra, int x, int y) {
  const std::size_t offset = static_cast<std::size_t>(y * kTargetSize + x) * 4;
  return {bgra[offset + 2], bgra[offset + 1], bgra[offset], bgra[offset + 3]};
}

bool near(u8 actual, int expected, int tolerance = 3) {
  return std::abs(static_cast<int>(actual) - expected) <= tolerance;
}

bool near_pixel(const Pixel& actual, const Pixel& expected, int tolerance = 3) {
  return near(actual.r, expected.r, tolerance) && near(actual.g, expected.g, tolerance) &&
         near(actual.b, expected.b, tolerance) && near(actual.a, expected.a, tolerance);
}

Pixel tinted(const Pixel& input) {
  const auto scale = [](u8 value, int numerator) {
    return std::min(255, static_cast<int>(std::lround(value * numerator / 255.f)));
  };
  return {static_cast<u8>(scale(input.r, 256)), static_cast<u8>(scale(input.g, 192)),
          static_cast<u8>(scale(input.b, 128)), 255};
}

id<MTLTexture> make_color_target(id<MTLDevice> device) {
  auto* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:kTargetSize
                                                        height:kTargetSize
                                                     mipmapped:NO];
  descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
#if TARGET_OS_OSX
  descriptor.storageMode = MTLStorageModeManaged;
#else
  descriptor.storageMode = MTLStorageModeShared;
#endif
  return [device newTextureWithDescriptor:descriptor];
}

id<MTLTexture> make_depth_target(id<MTLDevice> device) {
  auto* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                         width:kTargetSize
                                                        height:kTargetSize
                                                     mipmapped:NO];
  descriptor.usage = MTLTextureUsageRenderTarget;
  descriptor.storageMode = MTLStorageModePrivate;
  return [device newTextureWithDescriptor:descriptor];
}

MetalFrameContext begin_frame(id<MTLCommandQueue> queue,
                              id<MTLTexture> color,
                              id<MTLTexture> depth,
                              MetalPsoCache* pso_cache,
                              MetalSamplerCache* sampler_cache,
                              MetalStreamBuffer* stream,
                              bool load_previous) {
  MetalFrameContext ctx;
  ctx.cmds = [queue commandBuffer];
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = color;
  pass.colorAttachments[0].loadAction = load_previous ? MTLLoadActionLoad : MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
  pass.depthAttachment.texture = depth;
  pass.depthAttachment.loadAction = MTLLoadActionClear;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = 0;
  pass.stencilAttachment.texture = depth;
  pass.stencilAttachment.loadAction = MTLLoadActionClear;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;
  pass.stencilAttachment.clearStencil = 0;
  ctx.enc = [ctx.cmds renderCommandEncoderWithDescriptor:pass];
  [ctx.enc setCullMode:MTLCullModeNone];
  ctx.pso_cache = pso_cache;
  ctx.sampler_cache = sampler_cache;
  ctx.stream = stream;
  ctx.color_format = MTLPixelFormatBGRA8Unorm;
  ctx.depth_format = MTLPixelFormatDepth32Float_Stencil8;
  ctx.game_color = color;
  ctx.game_depth = depth;
  return ctx;
}

bool finish_frame(MetalFrameContext* ctx,
                  MetalJak2BlitDisplayRenderer* blit,
                  id<MTLTexture> color,
                  std::vector<u8>* pixels) {
  blit->finish_frame(*ctx);
  [ctx->enc endEncoding];
#if TARGET_OS_OSX
  id<MTLBlitCommandEncoder> sync = [ctx->cmds blitCommandEncoder];
  [sync synchronizeResource:color];
  [sync endEncoding];
#endif
  [ctx->cmds commit];
  [ctx->cmds waitUntilCompleted];
  if (ctx->cmds.status != MTLCommandBufferStatusCompleted) {
    if (ctx->cmds.error) {
      std::printf("Metal command-buffer error: %s\n",
                  ctx->cmds.error.localizedDescription.UTF8String);
    }
    return false;
  }
  pixels->resize(kTargetSize * kTargetSize * 4);
  [color getBytes:pixels->data()
      bytesPerRow:kTargetSize * 4
       fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
      mipmapLevel:0];
  return true;
}

void render_blit(MetalJak2BlitDisplayRenderer* blit,
                 metal_renderer::Jak2BlitDisplayCommand command,
                 MetalSharedRenderState* state,
                 MetalFrameContext* ctx) {
  auto chain = make_blit_chain(command);
  state->next_bucket = 16;
  DmaFollower dma(chain.data(), 0, chain.size());
  blit->render(dma, state, *ctx);
}

void render_direct(MetalDirectRenderer* renderer,
                   const std::vector<u8>& payload,
                   MetalSharedRenderState* state,
                   MetalFrameContext* ctx) {
  renderer->reset_state();
  renderer->render_gif(payload.data(), static_cast<u32>(payload.size()), state, *ctx);
  renderer->flush_pending(state, *ctx);
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
    check(queue != nil && library != nil, "the focused test loaded the embedded Metal product");
    if (!queue || !library) {
      return 1;
    }

    MetalPsoCache pso_cache;
    MetalSamplerCache sampler_cache;
    MetalStreamBuffer stream;
    check(pso_cache.init(device, library), "initialized the Direct renderer pipeline cache");
    sampler_cache.init(device);
    stream.init(device);
    if (failures) {
      return 1;
    }

    const std::size_t initial_texture_count = metal_texture_live_count();
    TexturePool texture_pool(GameVersion::Jak2);
    check(metal_setup_placeholder(device, queue, texture_pool),
          "published a placeholder so snapshot lookup failures stay observable");
    const u64 placeholder = texture_pool.get_placeholder_texture();

    id<MTLTexture> color = make_color_target(device);
    id<MTLTexture> depth = make_depth_target(device);
    check(color != nil && depth != nil, "created the deterministic 64x64 game targets");
    if (!color || !depth) {
      return 1;
    }

    {
      MetalJak2BlitDisplayRenderer blit("blit-display", kBlitBucket, &texture_pool);
      MetalDirectRenderer sky("sky-draw", kSkyDrawBucket,
                              metal_renderer::jak2_metal_direct_batch_size(kSkyDrawBucket));
      MetalDirectRenderer progress("progress", kProgressBucket,
                                   metal_renderer::jak2_metal_direct_batch_size(kProgressBucket));

      MetalSharedRenderState state;
      state.version = GameVersion::Jak2;
      state.texture_pool = &texture_pool;
      state.game_res_w = kTargetSize;
      state.game_res_h = kTargetSize;

      stream.reset();
      auto frame_a_ctx =
          begin_frame(queue, color, depth, &pso_cache, &sampler_cache, &stream, false);
      render_blit(&blit, metal_renderer::Jak2BlitDisplayCommand::None, &state, &frame_a_ctx);
      render_direct(&sky, make_spatial_frame_a(), &state, &frame_a_ctx);
      std::vector<u8> frame_a;
      check(finish_frame(&frame_a_ctx, &blit, color, &frame_a),
            "frame A completed with four nonuniform spatial regions");
      check(near_pixel(pixel_at(frame_a, 16, 16), {180, 40, 20, 255}) &&
                near_pixel(pixel_at(frame_a, 48, 16), {40, 180, 40, 255}) &&
                near_pixel(pixel_at(frame_a, 16, 48), {20, 40, 180, 255}) &&
                near_pixel(pixel_at(frame_a, 48, 48), {160, 120, 60, 255}),
            "frame A readback preserves all four authored positions and colors");

      stream.reset();
      auto frame_b_ctx =
          begin_frame(queue, color, depth, &pso_cache, &sampler_cache, &stream, true);
      render_blit(&blit, metal_renderer::Jak2BlitDisplayCommand::Snapshot, &state, &frame_b_ctx);
      render_direct(&sky, make_snapshot_sky_draw(), &state, &frame_b_ctx);
      const auto sky_stats = sky.stats();
      render_direct(&progress, make_progress_overlay(), &state, &frame_b_ctx);
      const auto progress_stats = progress.stats();
      std::vector<u8> frame_b;
      check(finish_frame(&frame_b_ctx, &blit, color, &frame_b),
            "frame B completed after bucket-3 snapshot, SKY_DRAW, and PROGRESS");

      const auto& blit_stats = blit.stats();
      const auto lookup = texture_pool.lookup(metal_renderer::kJak2BlitDisplayTbp);
      check(blit_stats.plan_valid && blit_stats.snapshot_requested &&
                blit_stats.texture_tbp == metal_renderer::kJak2BlitDisplayTbp &&
                blit_stats.texture_lookup_hit && !blit_stats.used_placeholder && lookup &&
                *lookup == blit_stats.texture_handle && *lookup != placeholder,
            "bucket 3 publishes the captured frame at TBP 0x3300 without a placeholder");
      check(sky_stats.draw_calls == 1 && sky_stats.triangles == 2 &&
                sky_stats.textured_draw_calls == 1 && sky_stats.missing_texture_draw_calls == 0 &&
                sky_stats.last_batch.textured &&
                sky_stats.last_batch.tex0_tbp == metal_renderer::kJak2BlitDisplayTbp &&
                sky_stats.last_batch.texture_lookup_hit && !sky_stats.last_batch.used_placeholder,
            "the real SKY_DRAW Direct renderer samples the exact bucket-3 texture");
      check(progress_stats.draw_calls == 1 && progress_stats.triangles == 2 &&
                progress_stats.textured_draw_calls == 0,
            "the real PROGRESS Direct renderer contributes the ordered overlay");

      const std::array<std::array<int, 2>, 4> samples = {
          std::array<int, 2>{16, 16}, {48, 16}, {16, 48}, {48, 48}};
      bool tint_and_space_match = true;
      for (const auto& sample : samples) {
        tint_and_space_match &= near_pixel(pixel_at(frame_b, sample[0], sample[1]),
                                           tinted(pixel_at(frame_a, sample[0], sample[1])), 4);
      }
      check(tint_and_space_match,
            "frame B retains frame A's four spatial regions with the authored sepia tint");
      check(near_pixel(pixel_at(frame_b, 32, 32), {240, 200, 20, 255}) &&
                !near_pixel(pixel_at(frame_b, 32, 32), tinted(pixel_at(frame_a, 32, 32)), 4),
            "PROGRESS lands after SKY_DRAW and visibly replaces the sampled center");

      stream.reset();
      auto copy_back_ctx =
          begin_frame(queue, color, depth, &pso_cache, &sampler_cache, &stream, true);
      render_blit(&blit, metal_renderer::Jak2BlitDisplayCommand::CopyBack, &state, &copy_back_ctx);
      render_direct(&sky, make_copy_back_decoy(), &state, &copy_back_ctx);
      std::vector<u8> copy_back_frame;
      check(finish_frame(&copy_back_ctx, &blit, color, &copy_back_frame),
            "the bounded copy-back command completed after a decoy draw");
      bool copy_back_matches = true;
      for (const auto& sample : samples) {
        copy_back_matches &= pixel_at(copy_back_frame, sample[0], sample[1]).r ==
                                 pixel_at(frame_a, sample[0], sample[1]).r &&
                             pixel_at(copy_back_frame, sample[0], sample[1]).g ==
                                 pixel_at(frame_a, sample[0], sample[1]).g &&
                             pixel_at(copy_back_frame, sample[0], sample[1]).b ==
                                 pixel_at(frame_a, sample[0], sample[1]).b;
      }
      check(
          blit.stats().copy_back_requested && blit.stats().copy_back_performed && copy_back_matches,
          "opcode 0x11 restores only the dimension-matched retained snapshot");
    }

    metal_texture_release(placeholder);
    texture_pool.set_placeholder(0);
    check(metal_texture_live_count() == initial_texture_count,
          "the focused test releases both placeholder and bucket-3 snapshot handles");

    if (failures) {
      std::printf("FAIL: %d Jak II BlitDisplays spatial checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak II bucket 3 snapshots frame A for spatially correct SKY_DRAW and "
                "PROGRESS composition\n");
    return 0;
  }
}
