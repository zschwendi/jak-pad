#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

#include "common/dma/gs.h"

#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_ocean_renderer.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
  if (!condition) {
    failures++;
  }
}

struct GifBuilder {
  std::vector<u8> data;

  void qword(u64 lo, u64 hi) {
    const std::size_t offset = data.size();
    data.resize(offset + 16);
    std::memcpy(data.data() + offset, &lo, sizeof(lo));
    std::memcpy(data.data() + offset + 8, &hi, sizeof(hi));
  }

  void tag(u32 nloop,
           bool eop,
           const std::vector<GifTag::RegisterDescriptor>& regs,
           bool pre = false,
           u16 prim = 0) {
    const u64 lo = (nloop & 0x7fff) | (static_cast<u64>(eop) << 15) |
                   (static_cast<u64>(pre) << 46) | (static_cast<u64>(prim & 0x7ff) << 47) |
                   (static_cast<u64>(regs.size() & 0xf) << 60);
    u64 hi = 0;
    for (std::size_t i = 0; i < regs.size(); i++) {
      hi |= static_cast<u64>(regs[i]) << (4 * i);
    }
    qword(lo, hi);
  }

  void ad(GsRegisterAddress address, u64 value) {
    qword(value, static_cast<u64>(address));
  }

  void rgbaq(u8 r, u8 g, u8 b, u8 a) {
    std::array<u8, 16> packed = {};
    packed[0] = r;
    packed[4] = g;
    packed[8] = b;
    packed[12] = a;
    const std::size_t offset = data.size();
    data.resize(offset + packed.size());
    std::memcpy(data.data() + offset, packed.data(), packed.size());
  }

  void uv(u32 u, u32 v) { qword(static_cast<u64>(u) | (static_cast<u64>(v) << 32), 0); }

  void xyzf2(u32 x, u32 y, u32 z = 0xffffff) {
    qword(static_cast<u64>(x) | (static_cast<u64>(y) << 32), static_cast<u64>(z) << 4);
  }
};

u16 prim(GsPrim::Kind kind, bool textured, bool alpha_blend, bool fixed_uv) {
  return static_cast<u16>(kind) | (static_cast<u16>(textured) << 4) |
         (static_cast<u16>(alpha_blend) << 6) | (static_cast<u16>(fixed_uv) << 8);
}

u64 scissor(u32 width, u32 height) {
  return static_cast<u64>(width - 1) << 16 | static_cast<u64>(height - 1) << 48;
}

u64 frame(u32 fbp) {
  return fbp | (1ull << 16);
}

u64 tex0(u32 tbp) {
  return tbp | (1ull << 14) | (2ull << 26) | (2ull << 30) | (1ull << 34);
}

u64 test_always() {
  return (1ull << 16) | (static_cast<u64>(GsTest::ZTest::ALWAYS) << 17);
}

u64 zbuf_no_write() {
  return 304ull | (1ull << 24) | (1ull << 32);
}

u32 vif_direct(u32 qwc) {
  return (static_cast<u32>(VifCode::Kind::DIRECT) << 24) | qwc;
}

void append_transfer(std::vector<u8>* chain, const std::vector<u8>& payload) {
  check((payload.size() & 15) == 0 && payload.size() / 16 <= UINT16_MAX,
        "synthetic transfer is qword-aligned and bounded");
  const u16 qwc = static_cast<u16>(payload.size() / 16);
  const u64 tag = static_cast<u64>(qwc) | (static_cast<u64>(DmaTag::Kind::CNT) << 28);
  const u64 vif = static_cast<u64>(vif_direct(qwc)) << 32;
  const std::size_t offset = chain->size();
  chain->resize(offset + 16 + payload.size());
  std::memcpy(chain->data() + offset, &tag, sizeof(tag));
  std::memcpy(chain->data() + offset + 8, &vif, sizeof(vif));
  std::memcpy(chain->data() + offset + 16, payload.data(), payload.size());
}

GifBuilder make_display_setup(u32 width, u32 height, u32 fbp) {
  GifBuilder gif;
  gif.tag(2, true, {GifTag::RegisterDescriptor::AD});
  gif.ad(GsRegisterAddress::SCISSOR_1, scissor(width, height));
  gif.ad(GsRegisterAddress::FRAME_1, frame(fbp));
  return gif;
}

GifBuilder make_sky_color_packet(const std::array<u8, 4>& color) {
  GifBuilder gif;
  gif.tag(2, true,
          {GifTag::RegisterDescriptor::RGBAQ, GifTag::RegisterDescriptor::XYZF2}, true,
          prim(GsPrim::Kind::SPRITE, false, false, false));
  gif.rgbaq(color[0], color[1], color[2], color[3]);
  gif.xyzf2(0, 0);
  gif.rgbaq(color[0], color[1], color[2], color[3]);
  gif.xyzf2(1024, 1024);
  return gif;
}

GifBuilder make_direct_state(u32 source_tbp) {
  GifBuilder gif;
  gif.tag(6, true, {GifTag::RegisterDescriptor::AD});
  gif.ad(GsRegisterAddress::XYOFFSET_1, 0x200ull | (0x200ull << 32));
  gif.ad(GsRegisterAddress::TEST_1, test_always());
  gif.ad(GsRegisterAddress::ZBUF_1, zbuf_no_write());
  gif.ad(GsRegisterAddress::TEX0_1, tex0(source_tbp));
  gif.ad(GsRegisterAddress::TEX1_1, (1ull << 5) | (1ull << 6));
  gif.ad(GsRegisterAddress::CLAMP_1, 0b101);
  return gif;
}

GifBuilder make_textured_offscreen_sprite() {
  GifBuilder gif;
  const u16 sprite_prim = prim(GsPrim::Kind::SPRITE, true, false, true);
  gif.tag(2, true,
          {GifTag::RegisterDescriptor::RGBAQ, GifTag::RegisterDescriptor::UV,
           GifTag::RegisterDescriptor::XYZF2},
          true, sprite_prim);
  gif.rgbaq(128, 128, 128, 128);
  gif.uv(0, 0);
  gif.xyzf2(1150, 2000);
  gif.rgbaq(128, 128, 128, 128);
  gif.uv(4 * 16, 4 * 16);
  gif.xyzf2(1400, 1800);
  return gif;
}

GifBuilder make_additive_haze() {
  GifBuilder gif;
  const u16 haze_prim = prim(GsPrim::Kind::TRI_STRIP, false, true, false);
  gif.tag(4, true,
          {GifTag::RegisterDescriptor::RGBAQ, GifTag::RegisterDescriptor::XYZF2}, true,
          haze_prim);
  const std::array<std::array<u16, 2>, 4> positions = {{{600, 2400},
                                                        {850, 2400},
                                                        {600, 2160},
                                                        {850, 2160}}};
  for (const auto& position : positions) {
    gif.rgbaq(255, 0, 0, 32);
    gif.xyzf2(position[0], position[1]);
  }
  return gif;
}

struct Fixture {
  std::vector<u8> chain;
  u32 ocean_texture_offset = 0;
  u32 end_offset = 0;
};

Fixture make_fixture(u32 source_tbp) {
  Fixture fixture;
  constexpr std::array<u8, 4> kSky = {20, 40, 80, 128};
  const auto first_setup = make_display_setup(64, 64, 0x58);
  const auto sky = make_sky_color_packet(kSky);
  const auto state = make_direct_state(source_tbp);
  const auto sprite = make_textured_offscreen_sprite();
  const auto haze = make_additive_haze();
  const auto second_setup = make_display_setup(64, 64, MetalOceanEnvmap::kVramSlot >> 5);
  for (const GifBuilder* gif : {&first_setup, &sky, &state, &sprite, &haze, &second_setup}) {
    append_transfer(&fixture.chain, gif->data);
  }

  fixture.ocean_texture_offset = static_cast<u32>(fixture.chain.size());
  const auto ocean_texture_setup = make_display_setup(128, 128, 0x40);
  append_transfer(&fixture.chain, ocean_texture_setup.data);
  fixture.end_offset = static_cast<u32>(fixture.chain.size());

  const u64 sentinel = static_cast<u64>(DmaTag::Kind::END) << 28;
  fixture.chain.resize(fixture.chain.size() + 16);
  std::memcpy(fixture.chain.data() + fixture.end_offset, &sentinel, sizeof(sentinel));
  return fixture;
}

std::vector<u8> render_sampler_oracle(id<MTLDevice> device,
                                      id<MTLCommandQueue> queue,
                                      MetalPsoCache& pso_cache,
                                      MetalSamplerCache& sampler_cache,
                                      id<MTLTexture> source) {
  struct SampleVertex {
    float pos[3];
    float uv[2];
    float color[4];
    float use_texture;
  };
  static_assert(sizeof(SampleVertex) == 40);

  constexpr std::array<std::array<float, 2>, 5> kOracleUv = {{
      {0.5f, 0.5f},
      {0.f, 0.5f},
      {1.f, 0.5f},
      {0.5f, 0.f},
      {0.5f, 1.f},
  }};
  std::array<SampleVertex, 30> vertices = {};
  const auto make_vertex = [](float x, float y, const std::array<float, 2>& uv) {
    SampleVertex result = {};
    result.pos[0] = x;
    result.pos[1] = y;
    result.uv[0] = uv[0];
    result.uv[1] = uv[1];
    result.use_texture = 1.f;
    return result;
  };
  for (int cell = 0; cell < 5; cell++) {
    const float x0 = -1.f + 2.f * cell / 5.f;
    const float x1 = -1.f + 2.f * (cell + 1) / 5.f;
    const auto& uv = kOracleUv[cell];
    vertices[cell * 6 + 0] = make_vertex(x0, 1.f, uv);
    vertices[cell * 6 + 1] = make_vertex(x1, 1.f, uv);
    vertices[cell * 6 + 2] = make_vertex(x1, -1.f, uv);
    vertices[cell * 6 + 3] = make_vertex(x0, 1.f, uv);
    vertices[cell * 6 + 4] = make_vertex(x1, -1.f, uv);
    vertices[cell * 6 + 5] = make_vertex(x0, -1.f, uv);
  }

  auto* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:5
                                  height:1
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModePrivate;
  id<MTLTexture> target = [device newTextureWithDescriptor:descriptor];
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

  id<MTLCommandBuffer> commands = [queue commandBuffer];
  id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
  MetalPsoKey key;
  key.shader = MetalShaderId::SAMPLE;
  key.color_format = MTLPixelFormatRGBA8Unorm;
  [encoder setRenderPipelineState:pso_cache.get_pipeline(key)];
  [encoder setVertexBytes:vertices.data() length:sizeof(vertices) atIndex:0];
  [encoder setFragmentTexture:source atIndex:0];
  [encoder setFragmentSamplerState:sampler_cache.get(MetalOceanEnvmap::radial_sampler_key())
                           atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertices.size()];
  [encoder endEncoding];
  [commands commit];
  [commands waitUntilCompleted];
  if (commands.status != MTLCommandBufferStatusCompleted) {
    return {};
  }

  constexpr NSUInteger kBytesPerRow = 5 * 4;
  id<MTLBuffer> readback = [device newBufferWithLength:kBytesPerRow
                                               options:MTLResourceStorageModeShared];
  commands = [queue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
  [blit copyFromTexture:target
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(5, 1, 1)
               toBuffer:readback
      destinationOffset:0
 destinationBytesPerRow:kBytesPerRow
destinationBytesPerImage:kBytesPerRow];
  [blit endEncoding];
  [commands commit];
  [commands waitUntilCompleted];
  if (commands.status != MTLCommandBufferStatusCompleted) {
    return {};
  }
  std::vector<u8> pixels(kBytesPerRow);
  std::memcpy(pixels.data(), readback.contents, pixels.size());
  return pixels;
}

std::vector<u8> read_rgba8(id<MTLCommandQueue> queue, id<MTLTexture> texture) {
  constexpr NSUInteger kBytesPerRow = MetalOceanEnvmap::kWidth * 4;
  constexpr NSUInteger kByteCount = kBytesPerRow * MetalOceanEnvmap::kHeight;
  id<MTLBuffer> readback = [queue.device newBufferWithLength:kByteCount
                                                    options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> commands = [queue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
  [blit copyFromTexture:texture
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(MetalOceanEnvmap::kWidth, MetalOceanEnvmap::kHeight, 1)
               toBuffer:readback
      destinationOffset:0
 destinationBytesPerRow:kBytesPerRow
destinationBytesPerImage:kByteCount];
  [blit endEncoding];
  [commands commit];
  [commands waitUntilCompleted];
  if (commands.status != MTLCommandBufferStatusCompleted) {
    return {};
  }
  std::vector<u8> pixels(kByteCount);
  std::memcpy(pixels.data(), readback.contents, pixels.size());
  return pixels;
}

std::array<u8, 4> pixel(const std::vector<u8>& rgba, int x, int y) {
  const std::size_t offset = static_cast<std::size_t>(y * MetalOceanEnvmap::kWidth + x) * 4;
  return {rgba[offset], rgba[offset + 1], rgba[offset + 2], rgba[offset + 3]};
}

bool near(const std::array<u8, 4>& value, const std::array<u8, 4>& expected, int tolerance) {
  for (int i = 0; i < 4; i++) {
    if (std::abs(static_cast<int>(value[i]) - static_cast<int>(expected[i])) > tolerance) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    check(device != nil, "a Metal device is available");
    if (!device) {
      return 1;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    dispatch_data_t library_data = dispatch_data_create(g_goalpad_metallib, g_goalpad_metallib_size,
                                                        nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError* library_error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:library_data error:&library_error];
    check(queue != nil && library != nil, "loaded the embedded Metal product");
    if (!queue || !library) {
      return 1;
    }

    MetalPsoCache pso_cache;
    MetalSamplerCache sampler_cache;
    MetalStreamBuffer stream;
    check(pso_cache.init(device, library), "initialized envmap shader pipelines");
    sampler_cache.init(device);
    stream.init(device);
    if (failures) {
      return 1;
    }

    constexpr u32 kSourceTbp = 0x180;
    const std::size_t initial_live_textures = metal_texture_live_count();
    TexturePool texture_pool(GameVersion::Jak2);
    check(metal_setup_placeholder(device, queue, texture_pool),
          "published a placeholder for observable lookup failures");
    const u64 placeholder = texture_pool.get_placeholder_texture();

    constexpr std::array<u32, 16> kSourcePixels = {
        0xff0000ff, 0xff0000ff, 0xff00ff00, 0xff00ff00,
        0xff0000ff, 0xff0000ff, 0xff00ff00, 0xff00ff00,
        0xffff0000, 0xffff0000, 0xff00ffff, 0xff00ffff,
        0xffff0000, 0xffff0000, 0xff00ffff, 0xff00ffff,
    };
    const u64 source_handle = metal_upload_texture_rgba8(
        device, queue, reinterpret_cast<const u8*>(kSourcePixels.data()), 4, 4);
    PcTextureId source_id;
    {
      std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
      TextureInput input;
      input.debug_page_name = "SYNTHETIC";
      input.debug_name = "ocean-envmap-source";
      input.id = texture_pool.allocate_pc_port_texture(GameVersion::Jak2);
      source_id = input.id;
      input.gpu_texture = source_handle;
      input.w = 4;
      input.h = 4;
      texture_pool.give_texture_and_load_to_vram(input, kSourceTbp);
    }
    check(source_handle != 0 && texture_pool.lookup(kSourceTbp).value_or(0) == source_handle,
          "published a public synthetic source texture");
    const auto radial_sampler = MetalOceanEnvmap::radial_sampler_key();
    check(radial_sampler.min_filter == MTLSamplerMinMagFilterLinear &&
              radial_sampler.mag_filter == MTLSamplerMinMagFilterNearest &&
              radial_sampler.wrap_s == MTLSamplerAddressModeRepeat &&
              radial_sampler.wrap_t == MTLSamplerAddressModeRepeat,
          "matched the GL radial sampler's linear-min, nearest-mag, repeat state");
    const auto sampler_oracle = render_sampler_oracle(
        device, queue, pso_cache, sampler_cache, metal_texture_lookup(source_handle));
    check(sampler_oracle.size() == 5 * 4, "read back the source-sampler oracle pixels");
    if (sampler_oracle.size() == 5 * 4) {
      const auto oracle_pixel = [&sampler_oracle](int x) {
        const std::size_t offset = static_cast<std::size_t>(x) * 4;
        return std::array<u8, 4>{sampler_oracle[offset], sampler_oracle[offset + 1],
                                 sampler_oracle[offset + 2], sampler_oracle[offset + 3]};
      };
      check(near(oracle_pixel(0), {255, 255, 0, 255}, 0),
            "the center oracle uses nearest magnification, not a linear four-texel blend");
      check(near(oracle_pixel(1), {0, 0, 255, 255}, 0) &&
                near(oracle_pixel(2), {0, 0, 255, 255}, 0) &&
                near(oracle_pixel(3), {0, 255, 0, 255}, 0) &&
                near(oracle_pixel(4), {0, 255, 0, 255}, 0),
            "the west/east/north/south oracles wrap at both cardinal seams");
    }

    {
      MetalOceanEnvmap envmap(device, queue);
      check(envmap.init_textures(texture_pool, GameVersion::Jak2),
            "initialized the standalone Jak II envmap target");
      check(texture_pool.lookup(MetalOceanEnvmap::kVramSlot).value_or(0) ==
                envmap.result_handle(),
            "reserved the source-exact 0xf80 publication slot");

      const auto fixture = make_fixture(kSourceTbp);
      DmaFollower dma(fixture.chain.data(), 0, fixture.chain.size());
      MetalSharedRenderState state;
      state.version = GameVersion::Jak2;
      state.texture_pool = &texture_pool;
      state.next_bucket = fixture.end_offset;
      state.game_res_w = 64;
      state.game_res_h = 64;
      MetalFrameContext ctx;
      ctx.pso_cache = &pso_cache;
      ctx.sampler_cache = &sampler_cache;
      ctx.stream = &stream;
      stream.reset();

      const auto scissor_before = envmap.direct_renderer().capture_scissor();
      check(envmap.handle_ocean_envmap_jak2(dma, &state, ctx),
            "executed the bounded ocean-method-89 prefix on the GPU");
      const auto& stats = envmap.stats();
      const auto scissor_after = envmap.direct_renderer().capture_scissor();
      check(stats.found_sky_color &&
                std::equal(std::begin(stats.sky_color), std::end(stats.sky_color),
                           std::array<u8, 4>{20, 40, 80, 128}.begin()) &&
                stats.setup_64_count == 2,
            "found the sky clear and exactly two 64x64 setups");
      check(stats.direct_draw_calls == 1 && stats.haze_draw_calls == 1 &&
                stats.radial_draw_calls == 1,
            "encoded selectable Direct, additive haze, and radial remap once each");
      check(stats.direct_batch.valid && stats.direct_batch.textured &&
                stats.direct_batch.tex0_tbp == kSourceTbp &&
                stats.direct_batch.texture_lookup_hit && !stats.direct_batch.used_placeholder,
            "the Direct pass selected the published synthetic source texture");
      check(stats.published && stats.published_vram_slot == MetalOceanEnvmap::kVramSlot &&
                texture_pool.lookup(MetalOceanEnvmap::kVramSlot).value_or(0) ==
                    envmap.result_handle(),
            "published the remapped Metal texture at VRAM 0xf80");
      check(stats.scissor_restored && scissor_before == scissor_after &&
                !envmap.direct_renderer().offscreen_mode(),
            "restored shared scissor state and disabled the Direct offscreen transform");
      check(stats.stopped_before_ocean_texture &&
                stats.stop_offset == fixture.ocean_texture_offset &&
                dma.current_tag_offset() == fixture.ocean_texture_offset &&
                stats.transfers_consumed == 6,
            "stopped with the 128x128 ocean-texture setup still unconsumed");
      DmaFollower marker = dma;
      const auto marker_transfer = marker.read_and_advance();
      u64 marker_scissor = 0;
      std::memcpy(&marker_scissor, marker_transfer.data + 16, sizeof(marker_scissor));
      check(GsScissor(marker_scissor).x1() == 127 && GsScissor(marker_scissor).y1() == 127,
            "the unconsumed marker is exactly the 128x128 setup boundary");

      const auto first_pass = read_rgba8(queue, envmap.first_pass_texture());
      const auto result = read_rgba8(queue, envmap.result_texture());
      check(first_pass.size() == 64 * 64 * 4 && result.size() == first_pass.size(),
            "read back both 64x64 GPU targets");
      check(near(pixel(first_pass, 63, 63), {20, 40, 80, 128}, 1),
            "the untouched first-pass corner retains the exact sky clear");
      const auto haze_pixel = pixel(first_pass, 12, 16);
      check(haze_pixel[0] > 60 && haze_pixel[1] <= 42 && haze_pixel[2] <= 82,
            "the authored haze region adds red over the sky clear");
      const auto direct_pixel = pixel(first_pass, 48, 40);
      check(!near(direct_pixel, {20, 40, 80, 128}, 5),
            "the source-positioned sprite lands only through Direct offscreen coordinates");
      std::set<std::array<u8, 4>> result_colors;
      for (int y = 0; y < 64; y += 4) {
        for (int x = 0; x < 64; x += 4) {
          result_colors.insert(pixel(result, x, y));
        }
      }
      check(result_colors.size() >= 6 && result != first_pass,
            "the radial pass publishes varied remapped content, not a copy or clear");

      const auto& bucket_table = metal_renderer::jak2_metal_bucket_table();
      check(bucket_table[static_cast<std::size_t>(jak2::BucketId::OCEAN_MID_FAR)].behavior ==
                    metal_renderer::Jak2MetalBucketBehavior::DeferredSkip &&
                bucket_table[static_cast<std::size_t>(jak2::BucketId::OCEAN_NEAR)].behavior ==
                    metal_renderer::Jak2MetalBucketBehavior::DeferredSkip,
            "left both Jak II OCEAN buckets explicitly deferred");
      envmap.detach_pool();
    }

    {
      std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
      texture_pool.unload_texture(source_id, source_handle);
    }
    metal_texture_release(source_handle);
    metal_texture_release(placeholder);
    texture_pool.set_placeholder(0);
    check(metal_texture_live_count() == initial_live_textures,
          "released all standalone proof textures");

    if (failures) {
      std::printf("FAIL: %d Jak II ocean envmap proof check(s) failed\n", failures);
      return 1;
    }
    std::puts("PASS: standalone non-promoted Jak II Metal ocean envmap prefix");
    return 0;
  }
}
