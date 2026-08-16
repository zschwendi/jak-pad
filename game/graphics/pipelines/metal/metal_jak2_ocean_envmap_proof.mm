#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
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

  void st(float s, float t) {
    const std::array<float, 4> value = {s, t, 1.f, 0.f};
    const std::size_t offset = data.size();
    data.resize(offset + sizeof(value));
    std::memcpy(data.data() + offset, value.data(), sizeof(value));
  }

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

u32 vif(VifCode::Kind kind, u16 immediate = 0, u8 num = 0) {
  return (static_cast<u32>(kind) << 24) | (static_cast<u32>(num) << 16) | immediate;
}

u32 append_vif_transfer(std::vector<u8>* chain,
                        const std::vector<u8>& payload,
                        u32 vif0,
                        u32 vif1,
                        DmaTag::Kind kind = DmaTag::Kind::CNT,
                        u32 address = 0) {
  if ((payload.size() & 15) != 0 || payload.size() / 16 > UINT16_MAX) {
    check(false, "synthetic transfer is qword-aligned and bounded");
    return static_cast<u32>(chain->size());
  }
  const u16 qwc = static_cast<u16>(payload.size() / 16);
  const u64 tag = static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                  (static_cast<u64>(address) << 32);
  const u64 transferred_tag = static_cast<u64>(vif0) | (static_cast<u64>(vif1) << 32);
  const std::size_t offset = chain->size();
  chain->resize(offset + 16 + payload.size());
  std::memcpy(chain->data() + offset, &tag, sizeof(tag));
  std::memcpy(chain->data() + offset + 8, &transferred_tag, sizeof(transferred_tag));
  if (!payload.empty()) {
    std::memcpy(chain->data() + offset + 16, payload.data(), payload.size());
  }
  return static_cast<u32>(offset);
}

void append_transfer(std::vector<u8>* chain, const std::vector<u8>& payload) {
  append_vif_transfer(chain, payload, 0, vif_direct(static_cast<u32>(payload.size() / 16)));
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

GifBuilder make_direct_state(u32 source_tbp, bool unsupported_blend = false) {
  GifBuilder gif;
  gif.tag(unsupported_blend ? 7 : 6, true, {GifTag::RegisterDescriptor::AD});
  gif.ad(GsRegisterAddress::XYOFFSET_1, 0x200ull | (0x200ull << 32));
  gif.ad(GsRegisterAddress::TEST_1, test_always());
  gif.ad(GsRegisterAddress::ZBUF_1, zbuf_no_write());
  gif.ad(GsRegisterAddress::TEX0_1, tex0(source_tbp));
  gif.ad(GsRegisterAddress::TEX1_1, (1ull << 5) | (1ull << 6));
  gif.ad(GsRegisterAddress::CLAMP_1, 0b101);
  if (unsupported_blend) {
    gif.ad(GsRegisterAddress::ALPHA_1, 0x55);
  }
  return gif;
}

GifBuilder make_textured_offscreen_sprite(bool alpha_blend = false) {
  GifBuilder gif;
  const u16 sprite_prim = prim(GsPrim::Kind::SPRITE, true, alpha_blend, true);
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

GifBuilder make_ocean_adgif(u32 source_tbp) {
  GifBuilder gif;
  gif.tag(5, true, {GifTag::RegisterDescriptor::AD});
  gif.ad(GsRegisterAddress::TEX0_1, tex0(source_tbp));
  gif.ad(GsRegisterAddress::TEX1_1, 0);
  gif.ad(GsRegisterAddress::MIPTBP1_1, 0);
  gif.ad(GsRegisterAddress::CLAMP_1, 0b101);
  gif.ad(GsRegisterAddress::ALPHA_1, 0);
  return gif;
}

GifBuilder make_ocean_far_setup() {
  GifBuilder gif;
  gif.tag(9, true, {GifTag::RegisterDescriptor::AD});
  for (int i = 0; i < 9; i++) {
    gif.ad(GsRegisterAddress::PRIM, 0);
  }
  return gif;
}

GifBuilder make_debug_triangle() {
  GifBuilder gif;
  gif.tag(1, true,
          {GifTag::RegisterDescriptor::RGBAQ, GifTag::RegisterDescriptor::XYZF2,
           GifTag::RegisterDescriptor::RGBAQ, GifTag::RegisterDescriptor::XYZF2,
           GifTag::RegisterDescriptor::RGBAQ, GifTag::RegisterDescriptor::XYZF2},
          true, prim(GsPrim::Kind::TRI, false, true, false) | (1u << 3));
  gif.rgbaq(0, 255, 0, 128);
  gif.xyzf2(0x8000, 0x7800);
  gif.rgbaq(0, 255, 0, 128);
  gif.xyzf2(0x7800, 0x8800);
  gif.rgbaq(0, 255, 0, 128);
  gif.xyzf2(0x8800, 0x8800);
  return gif;
}

GifBuilder make_common_ocean_xgkick(bool mid) {
  GifBuilder gif;
  if (mid) {
    gif.tag(5, false, {GifTag::RegisterDescriptor::AD});
    gif.ad(GsRegisterAddress::TEX0_1, tex0(MetalOceanTexture::vram_slot(GameVersion::Jak2)));
    gif.ad(GsRegisterAddress::TEX1_1, 1ull << 5);
    gif.ad(GsRegisterAddress::MIPTBP1_1, 0);
    gif.ad(GsRegisterAddress::CLAMP_1, 0b101);
    gif.ad(GsRegisterAddress::ALPHA_1, 0);
  }
  gif.tag(4, true, {GifTag::RegisterDescriptor::ST, GifTag::RegisterDescriptor::RGBAQ,
                     GifTag::RegisterDescriptor::XYZF2}, true,
          prim(GsPrim::Kind::TRI_STRIP, true, false, false));
  constexpr std::array<std::array<u32, 2>, 4> pos =
      {{{29400, 31800}, {31400, 31800}, {29400, 33800}, {31400, 33800}}};
  constexpr std::array<std::array<float, 2>, 4> st =
      {{{0.f, 0.f}, {1.f, 0.f}, {0.f, 1.f}, {1.f, 1.f}}};
  for (std::size_t i = 0; i < pos.size(); ++i) {
    gif.st(st[i][0], st[i][1]);
    gif.rgbaq(128, 128, 128, 128);
    gif.xyzf2(pos[i][0], pos[i][1], 0x7fffff);
  }
  return gif;
}

std::vector<u8> make_texture_vertices(std::size_t qwords, std::size_t qword_base = 0) {
  std::vector<u8> result(qwords * 16);
  for (std::size_t qword = 0; qword < qwords; qword++) {
    const std::size_t source_qword = qword_base + qword;
    const std::array<float, 4> value = source_qword % 2 == 0
                                           ? std::array<float, 4>{96.f, 128.f, 160.f, 128.f}
                                           : std::array<float, 4>{0.25f, 0.5f, 0.75f, 1.f};
    std::memcpy(result.data() + qword * 16, value.data(), 16);
  }
  return result;
}

void append_ocean_texture(std::vector<u8>* chain,
                          u32 source_tbp,
                          bool valid_vu_buffer_setup = true,
                          bool short_initial_upload = false) {
  append_transfer(chain, make_display_setup(128, 128, 0x40).data);
  append_transfer(chain, make_ocean_adgif(source_tbp).data);
  append_vif_transfer(chain, std::vector<u8>(64), 0, vif_direct(4));
  append_vif_transfer(chain, {}, vif(VifCode::Kind::BASE, 0),
                      vif(VifCode::Kind::OFFSET, valid_vu_buffer_setup ? 0xc0 : 0xbf));
  append_vif_transfer(chain, std::vector<u8>(112), vif(VifCode::Kind::STCYCL, 0x404),
                      vif(VifCode::Kind::UNPACK_V4_32, 985, 7));
  append_vif_transfer(chain, make_texture_vertices(short_initial_upload ? 1 : 192),
                      vif(VifCode::Kind::STCYCL, 0x404),
                      vif(VifCode::Kind::UNPACK_V4_32, 0x8000, 192));
  append_vif_transfer(chain, {}, vif(VifCode::Kind::MSCALF, 0),
                      vif(VifCode::Kind::STMOD, 0));
  for (int loop = 0; loop < 9; loop++) {
    append_vif_transfer(chain, make_texture_vertices(192), vif(VifCode::Kind::STCYCL, 0x404),
                        vif(VifCode::Kind::UNPACK_V4_32, 0x8000, 192));
    append_vif_transfer(chain, {}, vif(VifCode::Kind::MSCALF, 2),
                        vif(VifCode::Kind::STMOD, 0));
  }
  append_vif_transfer(chain, make_texture_vertices(128), vif(VifCode::Kind::STCYCL, 0x404),
                      vif(VifCode::Kind::UNPACK_V4_32, 0x8000, 128));
  append_vif_transfer(chain, make_texture_vertices(64, 128),
                      vif(VifCode::Kind::STCYCL, 0x404),
                      vif(VifCode::Kind::UNPACK_V4_32, 0x8080, 64));
  append_vif_transfer(chain, {}, vif(VifCode::Kind::MSCALF, 2),
                      vif(VifCode::Kind::STMOD, 0));
  append_vif_transfer(chain, {}, vif(VifCode::Kind::MSCALF, 4),
                      vif(VifCode::Kind::STMOD, 0));
}

struct Fixture {
  std::vector<u8> chain;
  u32 ocean_texture_offset = 0;
  u32 end_offset = 0;
};

void append_envmap_prefix(std::vector<u8>* chain,
                          u32 source_tbp,
                          bool unsupported_blend = false) {
  constexpr std::array<u8, 4> kSky = {20, 40, 80, 128};
  const auto first_setup = make_display_setup(64, 64, 0x58);
  const auto sky = make_sky_color_packet(kSky);
  const auto state = make_direct_state(source_tbp, unsupported_blend);
  const auto sprite = make_textured_offscreen_sprite(unsupported_blend);
  const auto haze = make_additive_haze();
  const auto second_setup = make_display_setup(64, 64, MetalOceanEnvmap::kVramSlot >> 5);
  for (const GifBuilder* gif : {&first_setup, &sky, &state, &sprite, &haze, &second_setup}) {
    append_transfer(chain, gif->data);
  }
}

Fixture make_fixture(u32 source_tbp) {
  Fixture fixture;
  append_envmap_prefix(&fixture.chain, source_tbp);

  fixture.ocean_texture_offset = static_cast<u32>(fixture.chain.size());
  const auto ocean_texture_setup = make_display_setup(128, 128, 0x40);
  append_transfer(&fixture.chain, ocean_texture_setup.data);
  fixture.end_offset = static_cast<u32>(fixture.chain.size());

  const u64 sentinel = static_cast<u64>(DmaTag::Kind::END) << 28;
  fixture.chain.resize(fixture.chain.size() + 16);
  std::memcpy(fixture.chain.data() + fixture.end_offset, &sentinel, sizeof(sentinel));
  return fixture;
}

std::vector<u8> make_noop_direct(std::size_t qwords) {
  GifBuilder gif;
  gif.tag(static_cast<u32>(qwords - 1), true, {GifTag::RegisterDescriptor::AD});
  for (std::size_t i = 1; i < qwords; i++) {
    gif.ad(GsRegisterAddress::PRIM, 0);
  }
  return gif.data;
}

struct FullOceanFixture {
  std::vector<u8> chain;
  u32 texture_offset = 0;
  u32 after_texture_offset = 0;
  u32 next_bucket = 0;
};

void set_qword_u32(std::vector<u8>* data,
                   std::size_t qword,
                   const std::array<u32, 4>& value) {
  std::memcpy(data->data() + qword * 16, value.data(), 16);
}

void append_mid_call(std::vector<u8>* chain,
                     u16 call,
                     std::vector<u8> upload) {
  append_vif_transfer(chain, upload, vif(VifCode::Kind::STCYCL, 0x404),
                      vif(VifCode::Kind::UNPACK_V4_32, 0x8000,
                          static_cast<u8>(upload.size() / 16)));
  append_vif_transfer(chain, {}, vif(VifCode::Kind::STCYCL, 0x404),
                      vif(VifCode::Kind::MSCALF, call));
}

FullOceanFixture make_mid_far_fixture(bool sky_active,
                                      u32 envmap_source_tbp,
                                      bool unsupported_envmap_blend,
                                      bool draw_far_triangle,
                                      bool all_mid_calls) {
  FullOceanFixture fixture;
  append_vif_transfer(&fixture.chain, {}, 0, 0);
  if (sky_active) {
    append_envmap_prefix(&fixture.chain, envmap_source_tbp, unsupported_envmap_blend);
  }
  fixture.texture_offset = static_cast<u32>(fixture.chain.size());
  append_ocean_texture(&fixture.chain, MetalOceanEnvmap::kVramSlot);
  fixture.after_texture_offset = static_cast<u32>(fixture.chain.size());

  append_transfer(&fixture.chain, make_ocean_far_setup().data);
  if (draw_far_triangle) {
    append_transfer(&fixture.chain, make_debug_triangle().data);
  }
  append_vif_transfer(&fixture.chain, {}, vif(VifCode::Kind::BASE, 0),
                      vif(VifCode::Kind::OFFSET, 0x76));
  append_vif_transfer(&fixture.chain, std::vector<u8>(0x240),
                      vif(VifCode::Kind::STCYCL, 0x404),
                      vif(VifCode::Kind::UNPACK_V4_32, 0x2dd, 0x24));
  append_vif_transfer(&fixture.chain, {}, vif(VifCode::Kind::STCYCL, 0x404),
                      vif(VifCode::Kind::MSCALF, 0));

  if (all_mid_calls) {
    std::vector<u8> call73(118 * 16);
    set_qword_u32(&call73, 116, {0xff, 0xff, 0xff, 0xff});
    set_qword_u32(&call73, 117, {0xff, 0xff, 0xff, 0xff});
    append_mid_call(&fixture.chain, 73, std::move(call73));

    std::vector<u8> call107(18 * 16);
    set_qword_u32(&call107, 8, {0, 0xff, 0xff, 0xff});
    append_mid_call(&fixture.chain, 107, std::move(call107));
  }
  append_vif_transfer(&fixture.chain, make_noop_direct(2), 0, vif_direct(2));

  const u32 next_offset = static_cast<u32>(fixture.chain.size());
  append_vif_transfer(&fixture.chain, {}, 0, 0, DmaTag::Kind::NEXT, next_offset + 16);
  append_vif_transfer(&fixture.chain, {}, 0, 0);
  append_vif_transfer(&fixture.chain, {}, 0, 0);
  fixture.next_bucket = static_cast<u32>(fixture.chain.size());
  append_vif_transfer(&fixture.chain, {}, 0, 0, DmaTag::Kind::END);
  return fixture;
}

FullOceanFixture make_near_fixture(bool valid_vu_buffer_setup = true) {
  FullOceanFixture fixture;
  append_vif_transfer(&fixture.chain, {}, 0, 0);
  fixture.texture_offset = static_cast<u32>(fixture.chain.size());
  append_ocean_texture(&fixture.chain, MetalOceanEnvmap::kVramSlot, valid_vu_buffer_setup);
  fixture.after_texture_offset = static_cast<u32>(fixture.chain.size());
  append_vif_transfer(&fixture.chain, make_noop_direct(2), 0, vif_direct(2));
  append_vif_transfer(&fixture.chain, {}, vif(VifCode::Kind::BASE, 0),
                      vif(VifCode::Kind::OFFSET, 0x10));
  append_vif_transfer(&fixture.chain, {}, vif(VifCode::Kind::MSCALF, 0),
                      vif(VifCode::Kind::STMOD, 0));
  std::vector<u8> call39(16 * 16);
  set_qword_u32(&call39, 8, {0xff, 0xff, 0xff, 0xff});
  set_qword_u32(&call39, 9, {0xff, 0xff, 0xff, 0xff});
  append_vif_transfer(&fixture.chain, call39, vif(VifCode::Kind::STCYCL, 0x404),
                      vif(VifCode::Kind::UNPACK_V4_32, 0x8000, 16));
  append_vif_transfer(&fixture.chain, {}, vif(VifCode::Kind::MSCALF, 39),
                      vif(VifCode::Kind::STMOD, 0));
  append_vif_transfer(&fixture.chain, make_noop_direct(2), 0, vif_direct(2));
  fixture.next_bucket = static_cast<u32>(fixture.chain.size());
  append_vif_transfer(&fixture.chain, {}, 0, 0, DmaTag::Kind::END);
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

std::vector<u8> read_rgba8(id<MTLCommandQueue> queue,
                           id<MTLTexture> texture,
                           NSUInteger width = MetalOceanEnvmap::kWidth,
                           NSUInteger height = MetalOceanEnvmap::kHeight) {
  const NSUInteger bytes_per_row = width * 4;
  const NSUInteger byte_count = bytes_per_row * height;
  id<MTLBuffer> readback = [queue.device newBufferWithLength:byte_count
                                                    options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> commands = [queue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
  [blit copyFromTexture:texture
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(width, height, 1)
               toBuffer:readback
      destinationOffset:0
 destinationBytesPerRow:bytes_per_row
destinationBytesPerImage:byte_count];
  [blit endEncoding];
  [commands commit];
  [commands waitUntilCompleted];
  if (commands.status != MTLCommandBufferStatusCompleted) {
    return {};
  }
  std::vector<u8> pixels(byte_count);
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
              radial_sampler.mip_filter == MTLSamplerMipFilterNotMipmapped &&
              radial_sampler.wrap_s == MTLSamplerAddressModeRepeat &&
              radial_sampler.wrap_t == MTLSamplerAddressModeRepeat,
          "matched the GL radial sampler's linear-min, nearest-mag, no-mip, repeat state");
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
      check(stats.prefix_present && stats.found_sky_color &&
                std::equal(std::begin(stats.sky_color), std::end(stats.sky_color),
                           std::array<u8, 4>{20, 40, 80, 128}.begin()) &&
                stats.setup_64_count == 2,
            "found the sky clear and exactly two 64x64 setups");
      check(stats.direct_draw_calls == 1 && stats.haze_draw_calls == 1 &&
                stats.radial_draw_calls == 1 && stats.command_buffers_committed == 1 &&
                stats.command_buffers_completed == 1 && stats.command_buffer_errors == 0 &&
                stats.last_command_buffer_status == MTLCommandBufferStatusCompleted,
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
                    metal_renderer::Jak2MetalBucketBehavior::OceanMidFar &&
                bucket_table[static_cast<std::size_t>(jak2::BucketId::OCEAN_NEAR)].behavior ==
                    metal_renderer::Jak2MetalBucketBehavior::OceanNear,
            "the source-shaped mid and near mesh draws clear both Jak II OCEAN routes together");

      const u64 envmap_slot_before_failure =
          texture_pool.lookup(MetalOceanEnvmap::kVramSlot).value_or(0);
      envmap.force_command_buffer_failure_for_testing(true);
      DmaFollower failed_envmap_dma(fixture.chain.data(), 0, fixture.chain.size());
      state.next_bucket = fixture.end_offset;
      stream.reset();
      check(!envmap.handle_ocean_envmap_jak2(failed_envmap_dma, &state, ctx) &&
                envmap.stats().transfers_consumed == 6 &&
                envmap.stats().command_buffers_committed == 0 &&
                envmap.stats().command_buffers_completed == 0 &&
                envmap.stats().command_buffer_errors == 1 &&
                envmap.stats().last_command_buffer_status == MTLCommandBufferStatusError &&
                !envmap.stats().published &&
                texture_pool.lookup(MetalOceanEnvmap::kVramSlot).value_or(0) ==
                    envmap_slot_before_failure,
            "tracked a deterministic envmap private-command failure without republishing VRAM 0xf80");
      envmap.force_command_buffer_failure_for_testing(false);

      std::vector<u8> malformed_prefix;
      append_transfer(&malformed_prefix, make_display_setup(64, 64, 0x58).data);
      append_transfer(&malformed_prefix, make_display_setup(128, 128, 0x40).data);
      const u32 malformed_end = static_cast<u32>(malformed_prefix.size());
      append_vif_transfer(&malformed_prefix, {}, 0, 0, DmaTag::Kind::END);
      DmaFollower malformed_dma(malformed_prefix.data(), 0, malformed_prefix.size());
      state.next_bucket = malformed_end;
      check(!envmap.handle_ocean_envmap_jak2(malformed_dma, &state, ctx) &&
                malformed_dma.current_tag_offset() == 0 && envmap.stats().prefix_present &&
                envmap.stats().transfers_consumed == 0,
            "rejected a partial sky prefix without consuming any transfer or publishing partial state");
      envmap.detach_pool();
    }

    {
      MetalOceanMidAndFar mid_far("synthetic-ocean-mid-far",
                                  static_cast<int>(jak2::BucketId::OCEAN_MID_FAR), device, queue);
      mid_far.init_textures(texture_pool, GameVersion::Jak2);

      MetalSharedRenderState state;
      state.version = GameVersion::Jak2;
      state.texture_pool = &texture_pool;
      state.game_res_w = 640;
      state.game_res_h = 416;
      MetalFrameContext ctx;
      ctx.pso_cache = &pso_cache;
      ctx.sampler_cache = &sampler_cache;
      ctx.stream = &stream;

      constexpr NSUInteger kFarTargetSize = 64;
      auto* color_descriptor = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                       width:kFarTargetSize
                                      height:kFarTargetSize
                                   mipmapped:NO];
      color_descriptor.usage = MTLTextureUsageRenderTarget;
      color_descriptor.storageMode = MTLStorageModePrivate;
      id<MTLTexture> far_color = [device newTextureWithDescriptor:color_descriptor];
      auto* depth_descriptor = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                       width:kFarTargetSize
                                      height:kFarTargetSize
                                   mipmapped:NO];
      depth_descriptor.usage = MTLTextureUsageRenderTarget;
      depth_descriptor.storageMode = MTLStorageModePrivate;
      id<MTLTexture> far_depth = [device newTextureWithDescriptor:depth_descriptor];
      id<MTLCommandBuffer> far_commands = [queue commandBuffer];
      auto* far_pass = [MTLRenderPassDescriptor renderPassDescriptor];
      far_pass.colorAttachments[0].texture = far_color;
      far_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      far_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      far_pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
      far_pass.depthAttachment.texture = far_depth;
      far_pass.depthAttachment.loadAction = MTLLoadActionClear;
      far_pass.depthAttachment.storeAction = MTLStoreActionDontCare;
      far_pass.depthAttachment.clearDepth = 0;
      far_pass.stencilAttachment.texture = far_depth;
      far_pass.stencilAttachment.loadAction = MTLLoadActionClear;
      far_pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
      far_pass.stencilAttachment.clearStencil = 0;
      id<MTLRenderCommandEncoder> far_encoder =
          [far_commands renderCommandEncoderWithDescriptor:far_pass];
      check(far_color != nil && far_depth != nil && far_commands != nil && far_encoder != nil,
            "created bounded color/depth targets for the ocean-far output proof");
      if (!far_color || !far_depth || !far_commands || !far_encoder) {
        return 1;
      }
      ctx.enc = far_encoder;
      ctx.cmds = far_commands;
      ctx.color_format = MTLPixelFormatBGRA8Unorm;
      ctx.depth_format = MTLPixelFormatDepth32Float_Stencil8;
      ctx.game_color = far_color;
      ctx.game_depth = far_depth;

      const auto active = make_mid_far_fixture(true, kSourceTbp, true, true, true);
      state.next_bucket = active.next_bucket;
      DmaFollower active_dma(active.chain.data(), 0, active.chain.size());
      stream.reset();
      mid_far.render(active_dma, &state, ctx);
      [far_encoder endEncoding];
      [far_commands commit];
      [far_commands waitUntilCompleted];
      check(far_commands.status == MTLCommandBufferStatusCompleted,
            "completed the ocean-far structural output command buffer");
      check(active_dma.current_tag_offset() == active.next_bucket,
            "sky-active mid/far consumed exactly to the next bucket");
      check(mid_far.envmap_stats().prefix_present &&
                mid_far.envmap_stats().transfers_consumed == 6 &&
                mid_far.envmap_stats().stop_offset == active.texture_offset,
            "sky-active grammar consumed the exact six-transfer envmap prefix boundary");
      check(mid_far.texture_stats().transfers_consumed == 29 &&
                mid_far.texture_stats().vu_buffer_setup_valid &&
                mid_far.texture_stats().vertices == 2112 &&
                mid_far.texture_stats().draw_calls == 9 &&
                mid_far.texture_stats().command_buffers_committed == 1 &&
                mid_far.texture_stats().command_buffers_completed == 1 &&
                mid_far.texture_stats().command_buffer_errors == 0 &&
                mid_far.texture_stats().last_command_buffer_status ==
                    MTLCommandBufferStatusCompleted &&
                mid_far.texture_stats().published_vram_slot == 672 &&
                mid_far.texture_stats().source_tbp == MetalOceanEnvmap::kVramSlot &&
                mid_far.texture_stats().source_handle ==
                    texture_pool.lookup(MetalOceanEnvmap::kVramSlot).value_or(0) &&
                mid_far.texture_stats().source_handle != placeholder,
            "Jak II texture consumed the exact 29-transfer grammar, completed its private GPU work, and published slot 672");
      check(mid_far.phase_order() == 1234 && mid_far.mid_jak2_calls() == 1 &&
                mid_far.mid_stats().draw_calls == 0,
            "sky-active render order is envmap, texture, far, then the bounded mid walker");
      const auto& mid_calls = mid_far.mid_jak2_call_stats();
      // Call 275 consumes authored geometry before it can produce a bounded GIF packet. Exercise
      // the production selector here without claiming a fabricated no-geometry packet ran it.
      check(mid_calls.call0 == 1 && mid_calls.call73 == 1 && mid_calls.call107 == 1 &&
                mid_calls.call275 == 0 &&
                MetalOceanMid::classify_jak2_call(0) == MetalOceanMid::Jak2Call::Call0 &&
                MetalOceanMid::classify_jak2_call(73) == MetalOceanMid::Jak2Call::Call73 &&
                MetalOceanMid::classify_jak2_call(107) == MetalOceanMid::Jak2Call::Call107 &&
                MetalOceanMid::classify_jak2_call(275) == MetalOceanMid::Jak2Call::Call275 &&
                MetalOceanMid::classify_jak2_call(74) == MetalOceanMid::Jak2Call::Unsupported,
            "executed bounded Jak II ocean-mid calls 0, 73, and 107 and structurally routed call 275");
      check(mid_far.envmap_stats().direct_unsupported_blends == 1 &&
                mid_far.direct_stats().unsupported_blends == 1,
            "preserved the nested envmap Direct unsupported blend in outer completeness stats");

      const auto far_pixels = read_rgba8(queue, far_color, kFarTargetSize, kFarTargetSize);
      const auto far_center = far_pixels.size() == kFarTargetSize * kFarTargetSize * 4
                                  ? pixel(far_pixels, kFarTargetSize / 2, kFarTargetSize / 2)
                                  : std::array<u8, 4>{};
      const auto far_corner = far_pixels.size() == kFarTargetSize * kFarTargetSize * 4
                                  ? pixel(far_pixels, 2, 2)
                                  : std::array<u8, 4>{};
      check(far_pixels.size() == kFarTargetSize * kFarTargetSize * 4 &&
                near(far_center, {0, 255, 0, 255}, 0) &&
                near(far_corner, {0, 0, 0, 255}, 0) &&
                mid_far.direct_stats().draw_calls == 1 &&
                mid_far.direct_stats().triangles == 1,
            "ocean-far encoded and read back one synthetic green triangle over the clear target");

      const auto active_pixels = read_rgba8(
          queue, metal_texture_lookup(mid_far.texture_handle()), 128, 128);
      const bool active_nonzero = std::any_of(active_pixels.begin(), active_pixels.end(),
                                              [](u8 value) { return value != 0; });
      check(active_pixels.size() == 128 * 128 * 4 && active_nonzero,
            "read back non-clear pixels from the generated 128x128 Jak II ocean texture");

      PcTextureId xgkick_texture_id;
      {
        std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
        TextureInput input;
        input.debug_page_name = "SYNTHETIC";
        input.debug_name = "ocean-xgkick-source";
        input.id = texture_pool.allocate_pc_port_texture(GameVersion::Jak2);
        input.gpu_texture = source_handle;
        input.w = 4;
        input.h = 4;
        xgkick_texture_id = input.id;
        texture_pool.give_texture_and_load_to_vram(input, MetalOceanTexture::vram_slot(GameVersion::Jak2));
      }
      auto* mesh_color_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:kFarTargetSize height:kFarTargetSize mipmapped:NO];
      mesh_color_desc.usage = MTLTextureUsageRenderTarget;
      mesh_color_desc.storageMode = MTLStorageModePrivate;
      id<MTLTexture> mesh_color = [device newTextureWithDescriptor:mesh_color_desc];
      auto* mesh_depth_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:kFarTargetSize height:kFarTargetSize mipmapped:NO];
      mesh_depth_desc.usage = MTLTextureUsageRenderTarget;
      mesh_depth_desc.storageMode = MTLStorageModePrivate;
      id<MTLTexture> mesh_depth = [device newTextureWithDescriptor:mesh_depth_desc];
      id<MTLCommandBuffer> mesh_commands = [queue commandBuffer];
      auto* mesh_pass = [MTLRenderPassDescriptor renderPassDescriptor];
      mesh_pass.colorAttachments[0].texture = mesh_color;
      mesh_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      mesh_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      mesh_pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
      mesh_pass.depthAttachment.texture = mesh_depth;
      mesh_pass.depthAttachment.loadAction = MTLLoadActionClear;
      mesh_pass.depthAttachment.clearDepth = 0;
      mesh_pass.stencilAttachment.texture = mesh_depth;
      mesh_pass.stencilAttachment.loadAction = MTLLoadActionClear;
      id<MTLRenderCommandEncoder> mesh_encoder = [mesh_commands renderCommandEncoderWithDescriptor:mesh_pass];
      check(mesh_color && mesh_depth && mesh_commands && mesh_encoder,
            "created a dedicated bounded target for source-shaped XGKICK mesh draws");
      if (!mesh_color || !mesh_depth || !mesh_commands || !mesh_encoder) return 1;
      [mesh_encoder setViewport:MTLViewport{0, 0, (double)kFarTargetSize, (double)kFarTargetSize, 0, 1}];
      [mesh_encoder setScissorRect:MTLScissorRect{0, 0, kFarTargetSize, kFarTargetSize}];
      [mesh_encoder setCullMode:MTLCullModeNone];
      MetalFrameContext mesh_ctx = ctx;
      mesh_ctx.enc = mesh_encoder;
      mesh_ctx.cmds = mesh_commands;
      mesh_ctx.game_color = mesh_color;
      mesh_ctx.game_depth = mesh_depth;
      state.fog_color = {64, 128, 192, 0};
      state.fog_intensity = 1.f;
      MetalCommonOceanRenderer xgkick_mid, xgkick_near;
      const auto mid_packet = make_common_ocean_xgkick(true);
      const auto near_packet = make_common_ocean_xgkick(false);
      stream.reset();
      xgkick_mid.init_for_mid();
      xgkick_mid.kick_from_mid(mid_packet.data.data());
      xgkick_mid.flush_mid(&state, mesh_ctx);
      xgkick_near.init_for_near();
      xgkick_near.kick_from_near(near_packet.data.data());
      xgkick_near.flush_near(&state, mesh_ctx);
      [mesh_encoder endEncoding];
      [mesh_commands commit];
      [mesh_commands waitUntilCompleted];
      const auto mesh_pixels = read_rgba8(queue, mesh_color, kFarTargetSize, kFarTargetSize);
      const bool mesh_non_clear = std::any_of(mesh_pixels.begin(), mesh_pixels.end(), [i = std::size_t{0}](u8 x) mutable { return x != (i++ % 4 == 3 ? 255 : 0); });
      check(mesh_commands.status == MTLCommandBufferStatusCompleted &&
                xgkick_mid.stats().vertices == 4 && xgkick_mid.stats().draw_calls == 1 && xgkick_mid.stats().missing_textures == 0 &&
                xgkick_near.stats().vertices == 4 && xgkick_near.stats().draw_calls == 1 && xgkick_near.stats().missing_textures == 0 &&
                mesh_non_clear,
            "source-shaped mid and near XGKICK packets each produced one textured GPU draw and non-clear readback");

      const auto inactive = make_mid_far_fixture(false, kSourceTbp, false, false, false);
      state.next_bucket = inactive.next_bucket;
      DmaFollower inactive_dma(inactive.chain.data(), 0, inactive.chain.size());
      stream.reset();
      mid_far.render(inactive_dma, &state, ctx);
      check(inactive_dma.current_tag_offset() == inactive.next_bucket,
            "sky-inactive mid/far consumed exactly to the next bucket");
      check(!mid_far.envmap_stats().prefix_present &&
                mid_far.envmap_stats().transfers_consumed == 0 &&
                mid_far.envmap_stats().stop_offset == inactive.texture_offset,
            "sky-inactive grammar leaves the first 128x128 texture transfer unconsumed");
      check(mid_far.texture_stats().transfers_consumed == 29 &&
                mid_far.texture_stats().vu_buffer_setup_valid &&
                mid_far.texture_stats().published_vram_slot == 672 &&
                mid_far.phase_order() == 234 && mid_far.mid_jak2_calls() == 2,
            "sky-inactive order begins at texture and still completes far and mid deterministically");

      MetalOceanNear near_renderer("synthetic-ocean-near",
                                   static_cast<int>(jak2::BucketId::OCEAN_NEAR), device, queue);
      near_renderer.init_textures(texture_pool, GameVersion::Jak2);
      const auto near = make_near_fixture();
      state.next_bucket = near.next_bucket;
      DmaFollower near_dma(near.chain.data(), 0, near.chain.size());
      stream.reset();
      near_renderer.render(near_dma, &state, ctx);
      check(near_dma.current_tag_offset() == near.next_bucket && near_renderer.phase_order() == 12,
            "near consumed exactly to its next bucket in texture-then-near order");
      check(near_renderer.texture_stats().transfers_consumed == 29 &&
                near_renderer.texture_stats().vu_buffer_setup_valid &&
                near_renderer.texture_stats().vertices == 2112 &&
                near_renderer.texture_stats().draw_calls == 1 &&
                near_renderer.texture_stats().published_vram_slot == 672 &&
                near_renderer.jak2_calls() == 2 &&
                near_renderer.jak2_call_stats().call0 == 1 &&
                near_renderer.jak2_call_stats().call39 == 1 &&
                near_renderer.near_stats().draw_calls == 0 &&
                MetalOceanNear::classify_jak2_call(0) == MetalOceanNear::Jak2Call::Call0 &&
                MetalOceanNear::classify_jak2_call(39) == MetalOceanNear::Jak2Call::Call39 &&
                MetalOceanNear::classify_jak2_call(40) == MetalOceanNear::Jak2Call::Unsupported,
            "near selected source-specific Jak II calls 0 and 39 with bounded sentinel masks");

      const auto malformed_near = make_near_fixture(false);
      state.next_bucket = malformed_near.next_bucket;
      DmaFollower malformed_near_dma(malformed_near.chain.data(), 0,
                                     malformed_near.chain.size());
      stream.reset();
      near_renderer.render(malformed_near_dma, &state, ctx);
      check(malformed_near_dma.current_tag_offset() == malformed_near.next_bucket &&
                near_renderer.phase_order() == 0 &&
                near_renderer.texture_stats().transfers_consumed == 4 &&
                !near_renderer.texture_stats().vu_buffer_setup_valid &&
                near_renderer.texture_stats().grammar_errors == 1 &&
                near_renderer.texture_stats().command_buffers_committed == 0 &&
                near_renderer.texture_stats().published_vram_slot == 0 &&
                near_renderer.jak2_calls() == 0,
            "rejected malformed Jak II ocean BASE/OFFSET grammar before GPU work or publication");

      MetalOceanTexture malformed_texture(false, device, queue);
      malformed_texture.init_textures(texture_pool, GameVersion::Jak2);
      std::vector<u8> short_upload_chain;
      append_ocean_texture(&short_upload_chain, MetalOceanEnvmap::kVramSlot, true, true);
      append_vif_transfer(&short_upload_chain, {}, 0, 0, DmaTag::Kind::END);
      DmaFollower short_upload_dma(short_upload_chain.data(), 0, short_upload_chain.size());
      check(!malformed_texture.handle_ocean_texture_jak2(short_upload_dma, &state, ctx) &&
                malformed_texture.stats().transfers_consumed == 6 &&
                malformed_texture.stats().grammar_errors == 1 &&
                malformed_texture.stats().command_buffers_committed == 0 &&
                malformed_texture.stats().published_vram_slot == 0,
            "rejected a one-qword payload before the fixed 192-qword ocean upload copy");

      MetalOceanTexture failed_texture(false, device, queue);
      failed_texture.init_textures(texture_pool, GameVersion::Jak2);
      const u64 failure_slot_before = texture_pool.lookup(672).value_or(0);
      failed_texture.force_command_buffer_failure_for_testing(true);
      std::vector<u8> failed_texture_chain;
      append_ocean_texture(&failed_texture_chain, MetalOceanEnvmap::kVramSlot);
      append_vif_transfer(&failed_texture_chain, {}, 0, 0, DmaTag::Kind::END);
      DmaFollower failed_texture_dma(failed_texture_chain.data(), 0,
                                     failed_texture_chain.size());
      check(!failed_texture.handle_ocean_texture_jak2(failed_texture_dma, &state, ctx) &&
                failed_texture.stats().transfers_consumed == 29 &&
                failed_texture.stats().vu_buffer_setup_valid &&
                failed_texture.stats().command_buffers_committed == 0 &&
                failed_texture.stats().command_buffers_completed == 0 &&
                failed_texture.stats().command_buffer_errors == 1 &&
                failed_texture.stats().last_command_buffer_status ==
                    MTLCommandBufferStatusError &&
                failed_texture.stats().published_vram_slot == 0 &&
                texture_pool.lookup(672).value_or(0) == failure_slot_before,
            "failed closed on a deterministic private command-buffer error without republishing slot 672");

      check(MetalOceanTexture::vram_slot(GameVersion::Jak1) == 8160 &&
                MetalOceanTexture::vram_slot(GameVersion::Jak2) == 672 &&
                std::abs(MetalCommonOceanRenderer::effective_scissor_adjust(GameVersion::Jak1) -
                         512.f / 448.f) < 0.00001f &&
                std::abs(MetalCommonOceanRenderer::effective_scissor_adjust(GameVersion::Jak2) -
                         (0.5f * 512.f / 416.f)) < 0.00001f,
            "preserved Jak 1 slot/scissor semantics while selecting Jak II slot 672 and half-height projection");
      {
        std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
        texture_pool.unload_texture(xgkick_texture_id, source_handle);
      }
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

    {
      auto lifecycle_pool = std::make_unique<TexturePool>(GameVersion::Jak1);
      check(metal_setup_placeholder(device, queue, *lifecycle_pool),
            "published the Jak 1 lifecycle placeholder");
      const u64 lifecycle_placeholder = lifecycle_pool->get_placeholder_texture();
      auto lifecycle_ocean = std::make_unique<MetalOceanTexture>(false, device, queue);
      lifecycle_ocean->init_textures(*lifecycle_pool, GameVersion::Jak1);
      check(lifecycle_pool->lookup(MetalOceanTexture::vram_slot(GameVersion::Jak1)).value_or(0) !=
                lifecycle_placeholder,
            "published the renderer-owned Jak 1 Ocean texture");

      lifecycle_ocean.reset();
      check(lifecycle_pool->lookup(MetalOceanTexture::vram_slot(GameVersion::Jak1)).value_or(0) ==
                lifecycle_placeholder,
            "unloaded the Jak 1 Ocean publication while its pool remained live");
      metal_texture_release(lifecycle_pool->get_placeholder_texture());
      lifecycle_pool.reset();
      check(metal_texture_live_count() == initial_live_textures,
            "destroyed the Jak 1 Ocean owner before its texture pool without leaking handles");
    }

    if (failures) {
      std::printf("FAIL: %d Jak II ocean envmap proof check(s) failed\n", failures);
      return 1;
    }
    std::puts("PASS: experimental Jak II Metal ocean envmap, texture, and bounded mesh parser proof");
    return 0;
  }
}
