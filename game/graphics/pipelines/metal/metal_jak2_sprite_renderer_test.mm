#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/util/Assert.h"

#include "game/graphics/pipelines/metal/metal_sprite_renderer.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>
#import <TargetConditionals.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kDistortTargetWidth = 128;
constexpr int kDistortTargetHeight = 96;
constexpr u16 kEffectsTexturePage = 12;
constexpr u16 kMechFlameTextureIndex = 144;
constexpr u32 kMechFlameTextureComboId =
    (static_cast<u32>(kEffectsTexturePage) << 16) | kMechFlameTextureIndex;
constexpr u32 kMechFlameTextureTbp = 325;
constexpr u16 kMechFlameTextureSize = 64;
static_assert(kMechFlameTextureComboId == 786576);

u32 vif_code(VifCode::Kind kind, u16 immediate = 0, u8 num = 0) {
  return (static_cast<u32>(kind) << 24) | (static_cast<u32>(num) << 16) | immediate;
}

u32 vif_stcycl(u16 cl, u16 wl) {
  return vif_code(VifCode::Kind::STCYCL, cl | (wl << 8));
}

u32 vif_unpack_v4_32(u8 qwc, u16 address, bool tops) {
  return vif_code(VifCode::Kind::UNPACK_V4_32, address | (tops ? (1 << 15) : 0), qwc);
}

void write_u64(std::vector<u8>& bytes, std::size_t offset, u64 value) {
  ASSERT(offset + sizeof(value) <= bytes.size());
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void write_u32(std::vector<u8>& bytes, std::size_t offset, u32 value) {
  ASSERT(offset + sizeof(value) <= bytes.size());
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void write_float(std::vector<u8>& bytes, std::size_t offset, float value) {
  ASSERT(offset + sizeof(value) <= bytes.size());
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void write_vec4(std::vector<u8>& bytes,
                std::size_t offset,
                float x,
                float y,
                float z = 0.f,
                float w = 0.f) {
  write_float(bytes, offset + 0, x);
  write_float(bytes, offset + 4, y);
  write_float(bytes, offset + 8, z);
  write_float(bytes, offset + 12, w);
}

u64 gs_zbuf(u32 zbp, bool masked) {
  return zbp | (0b0001ull << 24) | (static_cast<u64>(masked) << 32);
}

u64 gs_tex0(u32 tbp, u32 tbw, u32 psm, u32 tw, u32 th) {
  return tbp | (static_cast<u64>(tbw) << 14) | (static_cast<u64>(psm) << 20) |
         (static_cast<u64>(tw) << 26) | (static_cast<u64>(th) << 30);
}

u64 gs_alpha(u32 a, u32 b, u32 c, u32 d) {
  return a | (b << 2) | (c << 4) | (d << 6);
}

struct SyntheticChain {
  std::vector<u8> bytes;

  void transfer(u32 vif0, u32 vif1, const std::vector<u8>& data = {}) {
    ASSERT((data.size() & 0xf) == 0);
    const std::size_t offset = bytes.size();
    bytes.resize(offset + 16 + data.size(), 0);
    const u64 tag = static_cast<u64>(data.size() / 16) |
                    (static_cast<u64>(DmaTag::Kind::CNT) << 28);
    std::memcpy(bytes.data() + offset, &tag, sizeof(tag));
    std::memcpy(bytes.data() + offset + 8, &vif0, sizeof(vif0));
    std::memcpy(bytes.data() + offset + 12, &vif1, sizeof(vif1));
    if (!data.empty()) {
      std::memcpy(bytes.data() + offset + 16, data.data(), data.size());
    }
  }

  void empty_next() {
    const std::size_t offset = bytes.size();
    bytes.resize(offset + 16, 0);
    const u64 tag = (static_cast<u64>(DmaTag::Kind::NEXT) << 28) |
                    (static_cast<u64>(offset + 16) << 32);
    std::memcpy(bytes.data() + offset, &tag, sizeof(tag));
  }

  u32 finish() {
    const u32 next_bucket = static_cast<u32>(bytes.size());
    bytes.resize(bytes.size() + 16, 0);
    const u64 end = static_cast<u64>(DmaTag::Kind::END) << 28;
    std::memcpy(bytes.data() + next_bucket, &end, sizeof(end));
    return next_bucket;
  }
};

// Jak 2 defaults; Jak 1 uses zbp 0x1c0 and th 8 (MetalSpriteRenderer::distort_dma).
std::vector<u8> make_distorter_setup(u32 zbp = 0x130, u32 th = 9) {
  std::vector<u8> data(7 * 16, 0);
  const u64 gif_tag_lo = 1 | (1ull << 15) | (6ull << 60);
  const u64 gif_tag_hi = static_cast<u64>(GifTag::RegisterDescriptor::AD);
  write_u64(data, 0, gif_tag_lo);
  write_u64(data, 8, gif_tag_hi);
  write_u64(data, 16, gs_zbuf(zbp, true));
  write_u64(data, 32, gs_tex0(0, 8, 0, 9, th));
  write_u64(data, 48, (1ull << 5) | (1ull << 6));
  write_u64(data, 96, gs_alpha(0, 1, 0, 1));
  return data;
}

std::vector<u8> make_sine_tables() {
  std::vector<u8> data(0x8b * 16, 0);

  // Resolution three uses entry pairs [0, 1], [2, 3], [4, 5], then the
  // repeated first vertex of the following table at [6, 7]. The texture
  // deltas are zero so each synthetic distorter samples one exact color field.
  constexpr float kSin60 = 0.8660254037844386f;
  write_vec4(data, 0 * 16, 0.f, -1.f);
  write_vec4(data, 1 * 16, 0.f, 0.f);
  write_vec4(data, 2 * 16, kSin60, 0.5f);
  write_vec4(data, 3 * 16, 0.f, 0.f);
  write_vec4(data, 4 * 16, -kSin60, 0.5f);
  write_vec4(data, 5 * 16, 0.f, 0.f);
  write_vec4(data, 6 * 16, 0.f, -1.f);
  write_vec4(data, 7 * 16, 0.f, 0.f);

  constexpr std::size_t kIentryOffset = 128 * 16;
  write_u32(data, kIentryOffset, 352);
  const u64 gif_tag_lo = static_cast<u64>(GsPrim::Kind::TRI_STRIP) << 47;
  write_u64(data, (128 + 9) * 16, gif_tag_lo);
  constexpr std::size_t kColorOffset = (128 + 9 + 1) * 16;
  for (int lane = 0; lane < 4; lane++) {
    write_u32(data, kColorOffset + lane * sizeof(u32), 128);
  }
  return data;
}

std::vector<u8> make_sprite_direct_setup() {
  std::vector<u8> data(3 * 16, 0);
  constexpr u64 words[] = {
      0x2000000000008001ull, 0xEEEEEEEEEEEEEEEEull, 0x000000000005126Bull,
      0x0000000000000047ull, 0x0000000000000005ull, 0x0000000000000008ull,
  };
  std::memcpy(data.data(), words, sizeof(words));
  return data;
}

template <typename T>
std::vector<u8> bytes_of(const T& value) {
  std::vector<u8> bytes(sizeof(value));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

struct GlowFixture {
  SpriteGlowConsts consts = {};
  SpriteGlowData data = {};
  std::vector<u8> adgif = std::vector<u8>(sizeof(AdGifData), 0);

  GlowFixture() {
    consts.camera[0] = math::Vector4f(1.f, 0.f, 0.f, 0.f);
    consts.camera[1] = math::Vector4f(0.f, 1.f, 0.f, 0.f);
    consts.camera[2] = math::Vector4f(0.f, 0.f, 1.f, 0.f);
    consts.camera[3] = math::Vector4f(0.f, 0.f, 0.f, 1.f);

    consts.perspective[0] = math::Vector4f(1.f, 0.f, 0.f, 0.f);
    consts.perspective[1] = math::Vector4f(0.f, 1.f, 0.f, 0.f);
    consts.perspective[2] = math::Vector4f(0.f, 0.f, 0.25f, 0.f);
    consts.perspective[3] = math::Vector4f(0.f, 0.f, 0.f, 1.f);
    consts.hvdf = math::Vector4f(128.f, 96.f, 0.f, 0.f);
    consts.hmge = math::Vector4f(1.f, 1.f, 1.f, 1.f);
    consts.deg_to_rad = 0.017453292519943295f;
    consts.basis_x = math::Vector4f(1.f, 0.f, 0.f, 0.f);
    consts.basis_y = math::Vector4f(0.f, 1.f, 0.f, 0.f);
    consts.xy_array[0] = math::Vector4f(-1.f, -1.f, 0.f, 0.f);
    consts.xy_array[1] = math::Vector4f(1.f, -1.f, 0.f, 0.f);
    consts.xy_array[2] = math::Vector4f(-1.f, 1.f, 0.f, 0.f);
    consts.xy_array[3] = math::Vector4f(1.f, 1.f, 0.f, 0.f);
    consts.clamp_min = math::Vector4f(0.f, 0.f, 0.f, 0.f);
    consts.clamp_max = math::Vector4f(256.f, 192.f, 64.f, 16.f);

    data.pos[0] = 0.25f;
    data.pos[1] = -0.25f;
    data.pos[2] = 2.f;
    data.size_x = 8.f;
    data.size_probe = 4.f;
    data.z_offset = 0.25f;
    data.rot_angle = 15.f;
    data.size_y = 6.f;
    data.color[0] = 64.f;
    data.color[1] = 32.f;
    data.color[2] = 16.f;
    data.color[3] = 128.f;
    data.fade_b = 1.f;

    for (std::size_t i = 0; i < adgif.size(); i++) {
      adgif[i] = static_cast<u8>((i * 37 + 11) & 0xff);
    }
  }
};

bool output_is_finite(const SpriteGlowOutput& output) {
  const auto vector_is_finite = [](const auto& vector) {
    for (float component : vector) {
      if (!std::isfinite(component)) {
        return false;
      }
    }
    return true;
  };

  for (const auto& position : output.first_clear_pos) {
    if (!vector_is_finite(position)) {
      return false;
    }
  }
  for (const auto& position : output.second_clear_pos) {
    if (!vector_is_finite(position)) {
      return false;
    }
  }
  for (const auto& uv : output.offscreen_uv) {
    if (!vector_is_finite(uv)) {
      return false;
    }
  }
  for (const auto& position : output.flare_xyzw) {
    if (!vector_is_finite(position)) {
      return false;
    }
  }
  return vector_is_finite(output.flare_draw_color) && std::isfinite(output.perspective_q);
}

struct NormalSpriteFixture {
  SpriteFrameData frame = {};
  Sprite3DMatrixData matrix = {};
  SpriteVecData2d vector = {};
  AdGifData adgif = {};

  NormalSpriteFixture(bool tcc, bool filtered, float particle_alpha = 64.f) {
    frame.xy_array[0] = math::Vector4f(-128.f, -64.f, 0.f, 0.f);
    frame.xy_array[1] = math::Vector4f(128.f, -64.f, 0.f, 0.f);
    frame.xy_array[2] = math::Vector4f(-128.f, 64.f, 0.f, 0.f);
    frame.xy_array[3] = math::Vector4f(128.f, 64.f, 0.f, 0.f);
    frame.st_array[0] = math::Vector4f(0.f, 0.f, 0.f, 0.f);
    frame.st_array[1] = math::Vector4f(1.f, 0.f, 0.f, 0.f);
    frame.st_array[2] = math::Vector4f(0.f, 1.f, 0.f, 0.f);
    frame.st_array[3] = math::Vector4f(1.f, 1.f, 0.f, 0.f);
    frame.basis_x = math::Vector4f(1.f, 0.f, 0.f, 0.f);
    frame.basis_y = math::Vector4f(0.f, 1.f, 0.f, 0.f);
    frame.pfog0 = 1.f;
    frame.inv_area = 1.f;
    frame.max_scale = 1024.f;

    matrix.camera = math::Matrix4f::identity();

    vector.xyz_sx = math::Vector4f(2048.f, 2048.f, 8388608.f, 1.f);
    vector.flag_rot_sy = math::Vector4f(0.f, 0.f, 0.f, 1.f);
    vector.rgba = math::Vector4f(128.f, 128.f, 128.f, particle_alpha);

    adgif.tex0_data = gs_tex0(kMechFlameTextureTbp, 1, 0, 6, 6) |
                      (static_cast<u64>(tcc) << 34);
    adgif.tex0_addr = static_cast<u64>(GsRegisterAddress::TEX0_1);
    adgif.tex1_data = static_cast<u64>(filtered) << 5;
    adgif.tex1_addr = static_cast<u64>(GsRegisterAddress::TEX1_1);
    adgif.mip_addr = static_cast<u64>(GsRegisterAddress::MIPTBP1_1);
    adgif.clamp_data = 0b101;
    adgif.clamp_addr = static_cast<u64>(GsRegisterAddress::CLAMP_1);
    adgif.alpha_data = gs_alpha(0, 1, 0, 1);
    adgif.alpha_addr = static_cast<u64>(GsRegisterAddress::ALPHA_1);
  }
};

SyntheticChain make_normal_jak2_chain(bool include_empty_hud_chunk = false,
                                      bool use_chain3_glow_tail = false,
                                      bool include_glow_marked_group0 = false,
                                      const GlowFixture* glow = nullptr,
                                      bool malformed_glow_template = false,
                                      int glow_record_count = 1,
                                      const std::vector<MetalSpriteRenderer::SpriteDistortFrameData>*
                                          distorters = nullptr,
                                      const NormalSpriteFixture* normal_sprite = nullptr) {
  SyntheticChain chain;
  chain.empty_next();
  chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::DIRECT, 7),
                 make_distorter_setup());
  chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::PC_PORT),
                 std::vector<u8>(16, 0));
  chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(0x8b, 0x160, false),
                 make_sine_tables());
  if (distorters && !distorters->empty()) {
    ASSERT(distorters->size() <= 85);
    std::vector<u8> frame_bytes(distorters->size() *
                                sizeof(MetalSpriteRenderer::SpriteDistortFrameData));
    std::memcpy(frame_bytes.data(), distorters->data(), frame_bytes.size());
    const u8 frame_qwc = static_cast<u8>(frame_bytes.size() / 16);
    chain.transfer(vif_code(VifCode::Kind::NOP), vif_unpack_v4_32(frame_qwc, 512, false),
                   frame_bytes);

    std::vector<u8> count(16, 0);
    const u32 sprite_count = static_cast<u32>(distorters->size());
    std::memcpy(count.data(), &sprite_count, sizeof(sprite_count));
    chain.transfer(vif_code(VifCode::Kind::NOP), vif_unpack_v4_32(1, 511, false), count);
    chain.transfer(vif_code(VifCode::Kind::MSCALF), vif_code(VifCode::Kind::FLUSH));
  }
  chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::DIRECT, 3),
                 make_sprite_direct_setup());
  chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(0x2a, SpriteDataMem::FrameData, false),
                 normal_sprite ? bytes_of(normal_sprite->frame)
                               : std::vector<u8>(sizeof(SpriteFrameData), 0));
  chain.transfer(vif_code(VifCode::Kind::MSCALF, SpriteProgMem::Init),
                 vif_code(VifCode::Kind::FLUSHE));
  chain.transfer(vif_code(VifCode::Kind::BASE, SpriteDataMem::Buffer0),
                 vif_code(VifCode::Kind::OFFSET, SpriteDataMem::Buffer1));
  chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(5, SpriteDataMem::Matrix, false),
                 normal_sprite ? bytes_of(normal_sprite->matrix)
                               : std::vector<u8>(sizeof(Sprite3DMatrixData), 0));
  if (include_glow_marked_group0) {
    std::vector<u8> header(16, 0);
    const u32 sprite_count = 1;
    std::memcpy(header.data(), &sprite_count, sizeof(sprite_count));
    chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(1, SpriteDataMem::Header, true), header);
    std::vector<u8> vector_data(sizeof(SpriteVecData2d), 0);
    const s32 glow_matrix = -1;
    std::memcpy(vector_data.data() + offsetof(SpriteVecData2d, flag_rot_sy) + sizeof(float),
                &glow_matrix, sizeof(glow_matrix));
    chain.transfer(vif_code(VifCode::Kind::NOP),
                   vif_unpack_v4_32(3, SpriteDataMem::Vector, true), vector_data);
    chain.transfer(vif_code(VifCode::Kind::NOP),
                   vif_unpack_v4_32(5, SpriteDataMem::Adgif, true),
                   std::vector<u8>(sizeof(AdGifData), 0));
    chain.transfer(vif_code(VifCode::Kind::NOP),
                   vif_code(VifCode::Kind::MSCAL, SpriteProgMem::Sprites2dGrp0));
  } else if (normal_sprite) {
    std::vector<u8> header(16, 0);
    const u32 sprite_count = 1;
    std::memcpy(header.data(), &sprite_count, sizeof(sprite_count));
    chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(1, SpriteDataMem::Header, true), header);
    chain.transfer(vif_code(VifCode::Kind::NOP),
                   vif_unpack_v4_32(3, SpriteDataMem::Vector, true),
                   bytes_of(normal_sprite->vector));
    chain.transfer(vif_code(VifCode::Kind::NOP),
                   vif_unpack_v4_32(5, SpriteDataMem::Adgif, true),
                   bytes_of(normal_sprite->adgif));
    chain.transfer(vif_code(VifCode::Kind::NOP),
                   vif_code(VifCode::Kind::MSCAL, SpriteProgMem::Sprites2dGrp0));
  }
  chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::FLUSHE));
  chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(80, SpriteDataMem::Matrix, false),
                 std::vector<u8>(sizeof(SpriteHudMatrixData), 0));
  if (include_empty_hud_chunk) {
    chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(1, SpriteDataMem::Header, true),
                   std::vector<u8>(16, 0));
    chain.transfer(vif_code(VifCode::Kind::NOP),
                   vif_unpack_v4_32(0, SpriteDataMem::Vector, true));
    chain.transfer(vif_code(VifCode::Kind::NOP),
                   vif_unpack_v4_32(0, SpriteDataMem::Adgif, true));
    chain.transfer(vif_code(VifCode::Kind::NOP),
                   vif_code(VifCode::Kind::MSCAL, SpriteProgMem::Sprites2dHud_Jak2));
  }
  chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::FLUSHE));

  if (use_chain3_glow_tail) {
    for (int i = 0; i < 4; i++) {
      chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(1, 0, true), std::vector<u8>(16, 0));
      chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(4, 1, true), std::vector<u8>(4 * 16, 0));
      chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(5, 145, true), std::vector<u8>(5 * 16, 0));
      chain.transfer(vif_code(VifCode::Kind::MSCALF, 10), vif_code(VifCode::Kind::FLUSHE));
    }
    chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::FLUSHE));
  } else {
    const auto constants =
        glow ? bytes_of(glow->consts) : std::vector<u8>(sizeof(SpriteGlowConsts), 0);
    chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(24, 980, false), constants);
    chain.transfer(vif_stcycl(4, 4),
                   vif_unpack_v4_32(0x54, malformed_glow_template ? 801 : 800, false),
                   std::vector<u8>(0x54 * 16, 0));
    chain.transfer(vif_code(VifCode::Kind::MSCAL, 0), vif_unpack_v4_32(0x54, 884, false),
                   std::vector<u8>(0x54 * 16, 0));
    chain.transfer(vif_code(VifCode::Kind::BASE, 0), vif_code(VifCode::Kind::OFFSET, 400));
    chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::FLUSHE));
    for (int record = 0; glow && record < glow_record_count; record++) {
      std::vector<u8> control(16, 0);
      const u32 sprite_count = 1;
      std::memcpy(control.data(), &sprite_count, sizeof(sprite_count));
      chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(1, 0, true), control);
      chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(4, 1, true), bytes_of(glow->data));
      chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(5, 145, true), glow->adgif);
      chain.transfer(vif_code(VifCode::Kind::MSCALF, 10), vif_code(VifCode::Kind::FLUSHE));
    }
    chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::FLUSHE));
  }

  // Both observed chains finish with the residual NEXT/DIRECT10/NEXT envelope.
  chain.empty_next();
  chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::DIRECT, 10),
                 std::vector<u8>(10 * 16, 0));
  chain.empty_next();
  return chain;
}

void test_empty_jak2_bucket() {
  SyntheticChain chain;
  chain.transfer(0, 0);
  const u32 next_bucket = chain.finish();

  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.next_bucket = next_bucket;
  MetalFrameContext ctx;
  DmaFollower dma(chain.bytes.data(), 0);
  MetalSpriteRenderer renderer("synthetic-jak2-sprite", 313);
  renderer.render(dma, &state, ctx);

  ASSERT(dma.current_tag_offset() == next_bucket);
  ASSERT(renderer.stats().glow_transfers_skipped == 0);
}

void test_normal_jak2_parser_and_residual_accounting() {
  auto chain = make_normal_jak2_chain();
  const u32 next_bucket = chain.finish();

  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.next_bucket = next_bucket;
  MetalFrameContext ctx;
  DmaFollower dma(chain.bytes.data(), 0);
  MetalSpriteRenderer renderer("synthetic-jak2-sprite", 313);
  renderer.render(dma, &state, ctx);

  ASSERT(dma.current_tag_offset() == next_bucket);
  ASSERT(renderer.stats().blocks_2d_grp1 == 0);
  ASSERT(renderer.stats().count_2d_grp1 == 0);
  ASSERT(renderer.stats().draw_calls == 0);
  ASSERT(renderer.stats().glow_sprites_parsed == 0);
  ASSERT(renderer.stats().glow_sprites_accepted == 0);
  ASSERT(renderer.stats().glow_sprites_rejected == 0);
  ASSERT(renderer.pending_glow_outputs().empty());
  ASSERT(renderer.stats().glow_transfers_skipped == 0);
  ASSERT(renderer.stats().glow_bytes_skipped == 0);
  ASSERT(renderer.stats().post_glow_residual_transfers == 3);
  ASSERT(renderer.stats().post_glow_residual_bytes == 10 * 16);
  ASSERT(renderer.stats().unsupported_bytes == 10 * 16);
}

void test_constants_led_glow_retains_finite_output_and_exact_adgif() {
  GlowFixture glow;
  auto chain = make_normal_jak2_chain(false, false, false, &glow);
  const u32 next_bucket = chain.finish();

  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.next_bucket = next_bucket;
  MetalFrameContext ctx;
  DmaFollower dma(chain.bytes.data(), 0);
  MetalSpriteRenderer renderer("synthetic-jak2-sprite", 313);
  renderer.render(dma, &state, ctx);

  ASSERT(dma.current_tag_offset() == next_bucket);
  ASSERT(renderer.stats().glow_sprites_parsed == 1);
  ASSERT(renderer.stats().glow_sprites_accepted == 1);
  ASSERT(renderer.stats().glow_sprites_rejected == 0);
  ASSERT(renderer.stats().glow_invalid_records == 1);
  ASSERT(renderer.stats().glow_force_visible_submitted == 1);
  ASSERT(renderer.stats().glow_force_visible_drawn == 0);
  ASSERT(renderer.stats().glow_sprites_skipped == 1);
  ASSERT(renderer.stats().glow_transfers_skipped == 0);
  ASSERT(renderer.stats().unsupported_bytes == 10 * 16);
  ASSERT(renderer.pending_glow_outputs().size() == 1);
  const auto& output = renderer.pending_glow_outputs().front();
  ASSERT(output_is_finite(output));
  ASSERT(output.flare_draw_color.x() > 0.f);
  ASSERT(output.flare_draw_color.y() > 0.f);
  ASSERT(output.flare_draw_color.z() > 0.f);
  ASSERT(std::memcmp(&output.adgif, glow.adgif.data(), glow.adgif.size()) == 0);
}

void test_constants_led_glow_rejects_clipped_output() {
  GlowFixture glow;
  glow.data.pos[0] = 4.f;
  auto chain = make_normal_jak2_chain(false, false, false, &glow);
  const u32 next_bucket = chain.finish();

  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.next_bucket = next_bucket;
  MetalFrameContext ctx;
  DmaFollower dma(chain.bytes.data(), 0);
  MetalSpriteRenderer renderer("synthetic-jak2-sprite", 313);
  renderer.render(dma, &state, ctx);

  ASSERT(dma.current_tag_offset() == next_bucket);
  ASSERT(renderer.stats().glow_sprites_parsed == 1);
  ASSERT(renderer.stats().glow_sprites_accepted == 0);
  ASSERT(renderer.stats().glow_sprites_rejected == 1);
  ASSERT(renderer.pending_glow_outputs().empty());
  ASSERT(renderer.stats().unsupported_bytes == 10 * 16);
}

void test_malformed_constants_led_glow_fails_closed() {
  GlowFixture glow;
  auto chain = make_normal_jak2_chain(false, false, false, &glow, true);
  const u32 next_bucket = chain.finish();

  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.next_bucket = next_bucket;
  MetalFrameContext ctx;
  DmaFollower dma(chain.bytes.data(), 0);
  MetalSpriteRenderer renderer("synthetic-jak2-sprite", 313);
  renderer.render(dma, &state, ctx);

  ASSERT(dma.current_tag_offset() == next_bucket);
  ASSERT(renderer.stats().glow_sprites_parsed == 0);
  ASSERT(renderer.stats().glow_sprites_accepted == 0);
  ASSERT(renderer.stats().glow_sprites_rejected == 0);
  ASSERT(renderer.pending_glow_outputs().empty());
  ASSERT(renderer.stats().glow_transfers_skipped == 10);
  ASSERT(renderer.stats().glow_bytes_skipped == 202 * 16);
  ASSERT(renderer.stats().post_glow_residual_transfers == 3);
  ASSERT(renderer.stats().post_glow_residual_bytes == 10 * 16);
  ASSERT(renderer.stats().unsupported_bytes == 212 * 16);
}

void test_constants_led_glow_accepts_source_record_limit() {
  GlowFixture glow;
  auto chain = make_normal_jak2_chain(false, false, false, &glow, false, 400);
  const u32 next_bucket = chain.finish();

  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.next_bucket = next_bucket;
  MetalFrameContext ctx;
  DmaFollower dma(chain.bytes.data(), 0);
  MetalSpriteRenderer renderer("synthetic-jak2-sprite", 313);
  renderer.render(dma, &state, ctx);

  ASSERT(dma.current_tag_offset() == next_bucket);
  ASSERT(renderer.stats().glow_sprites_parsed == 400);
  ASSERT(renderer.stats().glow_sprites_accepted == 400);
  ASSERT(renderer.stats().glow_sprites_rejected == 0);
  ASSERT(renderer.stats().glow_invalid_records == 400);
  ASSERT(renderer.stats().glow_force_visible_submitted == 400);
  ASSERT(renderer.stats().glow_force_visible_drawn == 0);
  ASSERT(renderer.stats().glow_sprites_skipped == 400);
  ASSERT(renderer.stats().glow_transfers_skipped == 0);
  ASSERT(renderer.stats().glow_bytes_skipped == 0);
  ASSERT(renderer.stats().post_glow_residual_transfers == 3);
  ASSERT(renderer.stats().post_glow_residual_bytes == 10 * 16);
  ASSERT(renderer.stats().unsupported_bytes == 10 * 16);
  ASSERT(renderer.pending_glow_outputs().size() == 400);
  ASSERT(output_is_finite(renderer.pending_glow_outputs().front()));
  ASSERT(output_is_finite(renderer.pending_glow_outputs().back()));
}

void test_constants_led_glow_record_overflow_fails_closed() {
  GlowFixture glow;
  auto chain = make_normal_jak2_chain(false, false, false, &glow, false, 401);
  const u32 next_bucket = chain.finish();

  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.next_bucket = next_bucket;
  MetalFrameContext ctx;
  DmaFollower dma(chain.bytes.data(), 0);
  MetalSpriteRenderer renderer("synthetic-jak2-sprite", 313);
  renderer.render(dma, &state, ctx);

  ASSERT(dma.current_tag_offset() == next_bucket);
  ASSERT(renderer.stats().glow_sprites_parsed == 0);
  ASSERT(renderer.stats().glow_sprites_accepted == 0);
  ASSERT(renderer.stats().glow_sprites_rejected == 0);
  ASSERT(renderer.stats().glow_sprites_skipped == 0);
  ASSERT(renderer.pending_glow_outputs().empty());
  ASSERT(renderer.stats().glow_transfers_skipped == 1610);
  ASSERT(renderer.stats().glow_bytes_skipped == 4202 * 16);
  ASSERT(renderer.stats().post_glow_residual_transfers == 3);
  ASSERT(renderer.stats().post_glow_residual_bytes == 10 * 16);
  ASSERT(renderer.stats().unsupported_bytes == 4212 * 16);
}

void test_jak2_hud_program() {
  auto chain = make_normal_jak2_chain(true);
  const u32 next_bucket = chain.finish();

  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.next_bucket = next_bucket;
  MetalFrameContext ctx;
  DmaFollower dma(chain.bytes.data(), 0);
  MetalSpriteRenderer renderer("synthetic-jak2-sprite", 313);
  renderer.render(dma, &state, ctx);

  ASSERT(dma.current_tag_offset() == next_bucket);
  ASSERT(renderer.stats().blocks_2d_grp1 == 1);
  ASSERT(renderer.stats().count_2d_grp1 == 0);
  ASSERT(renderer.stats().draw_calls == 0);
}

void test_jak2_glow_marker_is_not_submitted_as_an_ordinary_sprite() {
  auto chain = make_normal_jak2_chain(false, false, true);
  const u32 next_bucket = chain.finish();

  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.next_bucket = next_bucket;
  MetalFrameContext ctx;
  DmaFollower dma(chain.bytes.data(), 0);
  MetalSpriteRenderer renderer("synthetic-jak2-sprite", 313);
  renderer.render(dma, &state, ctx);

  ASSERT(dma.current_tag_offset() == next_bucket);
  ASSERT(renderer.stats().count_2d_grp0 == 1);
  ASSERT(renderer.stats().glow_marked_sprites == 1);
  ASSERT(renderer.stats().normal_sprites_submitted == 0);
  ASSERT(renderer.stats().draw_calls == 0);
}

void test_control_led_glow_without_constants_remains_explicitly_unsupported() {
  auto chain = make_normal_jak2_chain(false, true);
  const u32 next_bucket = chain.finish();

  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.next_bucket = next_bucket;
  MetalFrameContext ctx;
  DmaFollower dma(chain.bytes.data(), 0);
  MetalSpriteRenderer renderer("synthetic-jak2-sprite", 313);
  renderer.render(dma, &state, ctx);

  ASSERT(dma.current_tag_offset() == next_bucket);
  ASSERT(renderer.stats().glow_sprites_parsed == 0);
  ASSERT(renderer.stats().glow_sprites_accepted == 0);
  ASSERT(renderer.stats().glow_sprites_rejected == 0);
  ASSERT(renderer.pending_glow_outputs().empty());
  ASSERT(renderer.stats().glow_transfers_skipped == 17);
  ASSERT(renderer.stats().glow_bytes_skipped == 40 * 16);
  ASSERT(renderer.stats().post_glow_residual_transfers == 3);
  ASSERT(renderer.stats().post_glow_residual_bytes == 10 * 16);
  ASSERT(renderer.stats().unsupported_bytes == 50 * 16);
}

struct Pixel {
  u8 r;
  u8 g;
  u8 b;
  u8 a;
};

Pixel read_bgra_pixel(const std::vector<u8>& pixels, int x, int y) {
  ASSERT(x >= 0 && x < kDistortTargetWidth);
  ASSERT(y >= 0 && y < kDistortTargetHeight);
  const std::size_t offset =
      static_cast<std::size_t>(y * kDistortTargetWidth + x) * sizeof(Pixel);
  return {pixels[offset + 2], pixels[offset + 1], pixels[offset], pixels[offset + 3]};
}

bool pixel_near(Pixel actual, Pixel expected, int tolerance = 2) {
  const auto near = [tolerance](u8 a, u8 b) {
    return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= tolerance;
  };
  return near(actual.r, expected.r) && near(actual.g, expected.g) &&
         near(actual.b, expected.b) && near(actual.a, expected.a);
}

void publish_synthetic_texture(TexturePool& texture_pool, u16 page_id, u16 index, u32 tbp) {
  constexpr u32 kGoalFalse = 0xffffffff;
  const std::size_t texture_pointer_count = index + 1;
  const std::size_t texture_description_offset =
      sizeof(GoalTexturePage) + texture_pointer_count * sizeof(u32);
  std::vector<u8> memory(texture_description_offset + sizeof(GoalTexture), 0);

  GoalTexturePage page = {};
  page.id = page_id;
  page.length = texture_pointer_count;
  std::memcpy(memory.data(), &page, sizeof(page));
  for (std::size_t pointer = 0; pointer < texture_pointer_count; pointer++) {
    write_u32(memory, sizeof(GoalTexturePage) + pointer * sizeof(u32), kGoalFalse);
  }
  write_u32(memory, sizeof(GoalTexturePage) + index * sizeof(u32),
            static_cast<u32>(texture_description_offset));

  GoalTexture texture = {};
  texture.num_mips = 1;
  texture.dest[0] = tbp;
  std::memcpy(memory.data() + texture_description_offset, &texture, sizeof(texture));
  texture_pool.handle_upload_now(memory.data(), -1, memory.data(), kGoalFalse, false);
}

void publish_synthetic_mech_flame(TexturePool& texture_pool) {
  publish_synthetic_texture(texture_pool, kEffectsTexturePage, kMechFlameTextureIndex,
                            kMechFlameTextureTbp);
}

void test_mech_flame_identity_publication_and_alpha() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  ASSERT(device);
  id<MTLCommandQueue> queue = [device newCommandQueue];
  ASSERT(queue);

  dispatch_data_t library_data = dispatch_data_create(
      g_goalpad_metallib, g_goalpad_metallib_size, nullptr,
      DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError* library_error = nil;
  id<MTLLibrary> library = [device newLibraryWithData:library_data error:&library_error];
  if (!library && library_error) {
    std::printf("Metal library error: %s\n", library_error.localizedDescription.UTF8String);
  }
  ASSERT(library);

  MetalPsoCache pso_cache;
  MetalSamplerCache sampler_cache;
  MetalStreamBuffer stream;
  ASSERT(pso_cache.init(device, library));
  sampler_cache.init(device);
  stream.init(device);

  const std::size_t initial_live_textures = metal_texture_live_count();
  TexturePool texture_pool(GameVersion::Jak2);
  ASSERT(metal_setup_placeholder(device, queue, texture_pool));
  const u64 placeholder = texture_pool.get_placeholder_texture();

  std::vector<u8> source(kMechFlameTextureSize * kMechFlameTextureSize * 4, 0);
  for (int y = 0; y < kMechFlameTextureSize; y++) {
    for (int x = 0; x < kMechFlameTextureSize; x++) {
      const std::size_t offset =
          static_cast<std::size_t>(y * kMechFlameTextureSize + x) * 4;
      source[offset + 0] = 200;
      source[offset + 1] = 120;
      source[offset + 2] = 40;
      source[offset + 3] = (x >= 24 && x < 40 && y >= 24 && y < 40) ? 128 : 0;
    }
  }
  const u64 source_handle = metal_upload_texture_rgba8(
      device, queue, source.data(), kMechFlameTextureSize, kMechFlameTextureSize);
  ASSERT(source_handle);
  PcTextureId source_id;
  {
    std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
    TextureInput input;
    input.debug_page_name = "SYNTHETIC";
    input.debug_name = "mech-flame";
    input.id = PcTextureId::from_combo_id(kMechFlameTextureComboId);
    source_id = input.id;
    input.gpu_texture = source_handle;
    input.src_data = source.data();
    input.w = kMechFlameTextureSize;
    input.h = kMechFlameTextureSize;
    texture_pool.give_texture(input);
  }
  publish_synthetic_mech_flame(texture_pool);
  ASSERT(texture_pool.lookup(kMechFlameTextureTbp).value_or(0) == source_handle);
  const GpuTexture* published = texture_pool.lookup_gpu_texture(kMechFlameTextureTbp);
  ASSERT(published);
  ASSERT(published->tex_id == PcTextureId(kEffectsTexturePage, kMechFlameTextureIndex));

  struct Case {
    bool tcc;
    bool filtered;
    float particle_alpha;
    u8 expected_center_alpha;
  };
  constexpr Case cases[] = {
      {true, false, 128.f, 255},
      {true, false, 64.f, 128},
      {false, true, 64.f, 255},
  };
  constexpr Pixel clear = {16, 32, 48, 255};

  for (const auto& test_case : cases) {
    stream.reset();
    NormalSpriteFixture fixture(test_case.tcc, test_case.filtered, test_case.particle_alpha);
    auto chain = make_normal_jak2_chain(false, false, false, nullptr, false, 1, nullptr,
                                        &fixture);
    const u32 next_bucket = chain.finish();

    auto* color_desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:kDistortTargetWidth
                                    height:kDistortTargetHeight
                                 mipmapped:NO];
    color_desc.usage = MTLTextureUsageRenderTarget;
#if TARGET_OS_OSX
    color_desc.storageMode = MTLStorageModeManaged;
#else
    color_desc.storageMode = MTLStorageModeShared;
#endif
    id<MTLTexture> color = [device newTextureWithDescriptor:color_desc];
    ASSERT(color);

    auto* depth_desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                     width:kDistortTargetWidth
                                    height:kDistortTargetHeight
                                 mipmapped:NO];
    depth_desc.usage = MTLTextureUsageRenderTarget;
    depth_desc.storageMode = MTLStorageModePrivate;
    id<MTLTexture> depth = [device newTextureWithDescriptor:depth_desc];
    ASSERT(depth);

    id<MTLCommandBuffer> commands = [queue commandBuffer];
    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor =
        MTLClearColorMake(clear.r / 255.f, clear.g / 255.f, clear.b / 255.f, 1.f);
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 0;
    pass.stencilAttachment.texture = depth;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.clearStencil = 0;

    MetalFrameContext ctx;
    ctx.cmds = commands;
    ctx.enc = [commands renderCommandEncoderWithDescriptor:pass];
    ctx.pso_cache = &pso_cache;
    ctx.sampler_cache = &sampler_cache;
    ctx.stream = &stream;
    ctx.color_format = MTLPixelFormatBGRA8Unorm;
    ctx.depth_format = MTLPixelFormatDepth32Float_Stencil8;
    ctx.game_color = color;
    ctx.game_depth = depth;
    ctx.game_viewport = {0.0, 0.0, kDistortTargetWidth, kDistortTargetHeight, 0.0, 1.0};
    [ctx.enc setCullMode:MTLCullModeNone];
    [ctx.enc setViewport:ctx.game_viewport];

    MetalSharedRenderState state;
    state.version = GameVersion::Jak2;
    state.next_bucket = next_bucket;
    state.texture_pool = &texture_pool;
    state.game_res_w = kDistortTargetWidth;
    state.game_res_h = kDistortTargetHeight;
    DmaFollower dma(chain.bytes.data(), 0, chain.bytes.size());
    MetalSpriteRenderer renderer("synthetic-jak2-normal-sprite", 313);
    renderer.render(dma, &state, ctx);

    ASSERT(dma.current_tag_offset() == next_bucket);
    ASSERT(renderer.stats().normal_sprites_submitted == 1);
    ASSERT(renderer.stats().draw_calls == 1);
    ASSERT(renderer.stats().triangles == 2);
    ASSERT(renderer.stats().missing_textures == 0);
    [ctx.enc endEncoding];
#if TARGET_OS_OSX
    id<MTLBlitCommandEncoder> sync = [commands blitCommandEncoder];
    [sync synchronizeResource:color];
    [sync endEncoding];
#endif
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted && commands.error) {
      std::printf("Metal command-buffer error: %s\n",
                  commands.error.localizedDescription.UTF8String);
    }
    ASSERT(commands.status == MTLCommandBufferStatusCompleted);

    std::vector<u8> pixels(kDistortTargetWidth * kDistortTargetHeight * 4);
    [color getBytes:pixels.data()
        bytesPerRow:kDistortTargetWidth * 4
         fromRegion:MTLRegionMake2D(0, 0, kDistortTargetWidth, kDistortTargetHeight)
        mipmapLevel:0];
    const Pixel center = read_bgra_pixel(pixels, 64, 48);
    const Pixel transparent_texel = read_bgra_pixel(pixels, 90, 48);
    if (test_case.tcc) {
      ASSERT(std::abs(static_cast<int>(center.a) - test_case.expected_center_alpha) <= 2);
      ASSERT(!pixel_near(center, clear));
      ASSERT(pixel_near(transparent_texel, clear));
    } else {
      constexpr Pixel opaque_source = {200, 120, 40, 255};
      ASSERT(pixel_near(center, opaque_source, 3));
      ASSERT(pixel_near(transparent_texel, opaque_source, 3));
    }
  }

  {
    std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
    texture_pool.unload_texture(source_id, source_handle);
  }
  metal_texture_release(source_handle);
  metal_texture_release(placeholder);
  texture_pool.set_placeholder(0);
  ASSERT(metal_texture_live_count() == initial_live_textures);
  std::puts("jak2-metal-sprite-renderer-test: mech-flame identity/alpha PASS");
}

void test_multi_distorter_spatial_sampling_and_alpha() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  ASSERT(device);
  id<MTLCommandQueue> queue = [device newCommandQueue];
  ASSERT(queue);

  dispatch_data_t library_data = dispatch_data_create(
      g_goalpad_metallib, g_goalpad_metallib_size, nullptr,
      DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError* library_error = nil;
  id<MTLLibrary> library = [device newLibraryWithData:library_data error:&library_error];
  if (!library && library_error) {
    std::printf("Metal library error: %s\n", library_error.localizedDescription.UTF8String);
  }
  ASSERT(library);

  MetalPsoCache pso_cache;
  MetalSamplerCache sampler_cache;
  MetalStreamBuffer stream;
  ASSERT(pso_cache.init(device, library));
  sampler_cache.init(device);
  stream.init(device);

  std::vector<MetalSpriteRenderer::SpriteDistortFrameData> distorters(2);
  distorters[0].xyz = math::Vector3f(1888.f, 1968.f, 8388608.f);
  distorters[0].num_255 = 255.f;
  distorters[0].st = math::Vector2f(0.75f, 0.25f);
  distorters[0].num_1 = 1.f;
  distorters[0].flag = 3;
  distorters[0].rgba = math::Vector4f(60.f, 0.f, 0.f, 0.f);
  distorters[1] = distorters[0];
  distorters[1].xyz.x() = 2208.f;
  distorters[1].xyz.y() = 2128.f;
  distorters[1].st = math::Vector2f(0.25f, 0.5625f);

  auto chain = make_normal_jak2_chain(false, false, false, nullptr, false, 1, &distorters);
  const u32 next_bucket = chain.finish();
  constexpr u8 kSourceAlphas[] = {255, 0};
  for (u8 source_alpha : kSourceAlphas) {
    stream.reset();

    auto* color_desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:kDistortTargetWidth
                                    height:kDistortTargetHeight
                                 mipmapped:NO];
    color_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
#if TARGET_OS_OSX
    color_desc.storageMode = MTLStorageModeManaged;
#else
    color_desc.storageMode = MTLStorageModeShared;
#endif
    id<MTLTexture> color = [device newTextureWithDescriptor:color_desc];
    ASSERT(color);

    auto* depth_desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                     width:kDistortTargetWidth
                                    height:kDistortTargetHeight
                                 mipmapped:NO];
    depth_desc.usage = MTLTextureUsageRenderTarget;
    depth_desc.storageMode = MTLStorageModePrivate;
    id<MTLTexture> depth = [device newTextureWithDescriptor:depth_desc];
    ASSERT(depth);

    const Pixel top_left = {24, 72, 208, source_alpha};
    const Pixel top_right = {216, 176, 32, source_alpha};
    const Pixel bottom_left = {32, 200, 96, source_alpha};
    const Pixel bottom_right = {176, 48, 200, source_alpha};
    std::vector<u8> initial(kDistortTargetWidth * kDistortTargetHeight * 4);
    for (int y = 0; y < kDistortTargetHeight; y++) {
      for (int x = 0; x < kDistortTargetWidth; x++) {
        const bool left = x < kDistortTargetWidth / 2;
        const bool top = y < kDistortTargetHeight / 2;
        const Pixel pixel = top ? (left ? top_left : top_right)
                                : (left ? bottom_left : bottom_right);
        const std::size_t offset =
            static_cast<std::size_t>(y * kDistortTargetWidth + x) * 4;
        initial[offset + 0] = pixel.b;
        initial[offset + 1] = pixel.g;
        initial[offset + 2] = pixel.r;
        initial[offset + 3] = pixel.a;
      }
    }
    [color replaceRegion:MTLRegionMake2D(0, 0, kDistortTargetWidth, kDistortTargetHeight)
              mipmapLevel:0
                withBytes:initial.data()
              bytesPerRow:kDistortTargetWidth * 4];

    id<MTLCommandBuffer> commands = [queue commandBuffer];
    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 0;
    pass.stencilAttachment.texture = depth;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.clearStencil = 0;

    MetalFrameContext ctx;
    ctx.cmds = commands;
    ctx.enc = [commands renderCommandEncoderWithDescriptor:pass];
    ctx.pso_cache = &pso_cache;
    ctx.sampler_cache = &sampler_cache;
    ctx.stream = &stream;
    ctx.color_format = MTLPixelFormatBGRA8Unorm;
    ctx.depth_format = MTLPixelFormatDepth32Float_Stencil8;
    ctx.game_color = color;
    ctx.game_depth = depth;
    ctx.game_viewport = {0.0, 0.0, kDistortTargetWidth, kDistortTargetHeight, 0.0, 1.0};
    [ctx.enc setCullMode:MTLCullModeNone];
    [ctx.enc setViewport:ctx.game_viewport];

    MetalSharedRenderState state;
    state.version = GameVersion::Jak2;
    state.next_bucket = next_bucket;
    state.game_res_w = kDistortTargetWidth;
    state.game_res_h = kDistortTargetHeight;
    DmaFollower dma(chain.bytes.data(), 0, chain.bytes.size());
    MetalSpriteRenderer renderer("synthetic-jak2-distort", 313);
    renderer.render(dma, &state, ctx);

    ASSERT(dma.current_tag_offset() == next_bucket);
    ASSERT(renderer.stats().distort_sprites == 2);
    ASSERT(renderer.stats().draw_calls == 1);
    ASSERT(renderer.stats().triangles == 12);
    [ctx.enc endEncoding];
#if TARGET_OS_OSX
    id<MTLBlitCommandEncoder> sync = [commands blitCommandEncoder];
    [sync synchronizeResource:color];
    [sync endEncoding];
#endif
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted && commands.error) {
      std::printf("Metal command-buffer error: %s\n",
                  commands.error.localizedDescription.UTF8String);
    }
    ASSERT(commands.status == MTLCommandBufferStatusCompleted);

    std::vector<u8> pixels(initial.size());
    [color getBytes:pixels.data()
        bytesPerRow:kDistortTargetWidth * 4
         fromRegion:MTLRegionMake2D(0, 0, kDistortTargetWidth, kDistortTargetHeight)
        mipmapLevel:0];

    Pixel expected_left_sample = top_right;
    expected_left_sample.a = 255;
    Pixel expected_right_sample = bottom_left;
    expected_right_sample.a = 255;
    ASSERT(pixel_near(read_bgra_pixel(pixels, 24, 33), expected_left_sample));
    ASSERT(pixel_near(read_bgra_pixel(pixels, 104, 63), expected_right_sample));

    // Checking every pixel outside the two conservative bounds catches an
    // out-of-range vertex or a failed primitive restart, not just one bridge.
    for (int y = 0; y < kDistortTargetHeight; y++) {
      for (int x = 0; x < kDistortTargetWidth; x++) {
        const bool in_left_bounds = x >= 8 && x <= 40 && y >= 19 && y <= 47;
        const bool in_right_bounds = x >= 88 && x <= 120 && y >= 49 && y <= 77;
        if (!in_left_bounds && !in_right_bounds) {
          const bool left = x < kDistortTargetWidth / 2;
          const bool top = y < kDistortTargetHeight / 2;
          ASSERT(pixel_near(read_bgra_pixel(pixels, x, y),
                            top ? (left ? top_left : top_right)
                                : (left ? bottom_left : bottom_right),
                            0));
        }
      }
    }
    ASSERT(pixel_near(read_bgra_pixel(pixels, 64, 48), bottom_right, 0));

    if (source_alpha == 255) {
      std::puts("jak2-metal-sprite-renderer-test: fixed-index restart PASS");
    }
  }
}

// ---------------------------------------------------------------------------
// Jak 1 group-0 2D sprites (world billboards such as particles) under per-eye
// view transforms. The expected eye position is computed from the billboard's
// camera-space corner points with the true depth as w. The sprite VU program
// instead divides by depth and then emits w = depth + hvdf.w, so the GPU result
// only matches when the transform is applied to that whole homogeneous output.
// ---------------------------------------------------------------------------

constexpr u16 kJak1StereoTexturePage = 3;
constexpr u16 kJak1StereoTextureIndex = 5;
constexpr u32 kJak1StereoTextureTbp = 0x1200;
constexpr u16 kJak1StereoTextureSize = 8;
constexpr float kJak1ScissorAdjust = 512.f / 448.f;

// Sprite camera columns: VU position = (256 x, 128 y, kStereoDepthTerm, depth).
constexpr float kStereoCameraX = 256.f;
constexpr float kStereoCameraY = 128.f;
constexpr float kStereoDepthTerm = 33554432.f;
constexpr float kStereoPfog0 = 1.f;
constexpr float kStereoHvdfW = 12.f;  // VU w is depth + 12, never the true depth
constexpr float kStereoBasisX = 128.f;
constexpr float kStereoBasisY = 64.f;

// World billboard, in sprite-camera space.
constexpr float kStereoWorldX = -1.f;
constexpr float kStereoWorldY = 0.5f;
constexpr float kStereoWorldDepth = 4.f;
constexpr float kStereoWorldScale = 1.f;

// HUD sprite, authored through an identity HUD matrix.
constexpr float kStereoHudX = 128.f;
constexpr float kStereoHudY = 64.f;
constexpr float kStereoHudZ = 8388608.f;
constexpr float kStereoHudScale = 0.25f;

AdGifData make_opaque_stereo_adgif() {
  AdGifData adgif = {};
  adgif.tex0_data = gs_tex0(kJak1StereoTextureTbp, 1, 0, 3, 3);
  adgif.tex0_addr = static_cast<u64>(GsRegisterAddress::TEX0_1);
  adgif.tex1_addr = static_cast<u64>(GsRegisterAddress::TEX1_1);
  adgif.mip_addr = static_cast<u64>(GsRegisterAddress::MIPTBP1_1);
  adgif.clamp_data = 0b101;
  adgif.clamp_addr = static_cast<u64>(GsRegisterAddress::CLAMP_1);
  adgif.alpha_data = gs_alpha(0, 1, 0, 1);
  adgif.alpha_addr = static_cast<u64>(GsRegisterAddress::ALPHA_1);
  return adgif;
}

struct Jak1StereoSpriteFixture {
  SpriteFrameDataJak1 frame = {};
  Sprite3DMatrixData matrix = {};
  SpriteHudMatrixData hud_matrix = {};
  SpriteVecData2d world = {};
  SpriteVecData2d hud = {};
  AdGifData adgif = make_opaque_stereo_adgif();

  Jak1StereoSpriteFixture() {
    // Cyclic corner order as in sprite-setup-frame-data; the renderer strips
    // vertex ids 0, 1, 3, 2 into a full quad.
    frame.xy_array[0] = math::Vector4f(-1.f, -1.f, 0.f, 0.f);
    frame.xy_array[1] = math::Vector4f(1.f, -1.f, 0.f, 0.f);
    frame.xy_array[2] = math::Vector4f(1.f, 1.f, 0.f, 0.f);
    frame.xy_array[3] = math::Vector4f(-1.f, 1.f, 0.f, 0.f);
    frame.st_array[0] = math::Vector4f(0.f, 0.f, 0.f, 0.f);
    frame.st_array[1] = math::Vector4f(1.f, 0.f, 0.f, 0.f);
    frame.st_array[2] = math::Vector4f(1.f, 1.f, 0.f, 0.f);
    frame.st_array[3] = math::Vector4f(0.f, 1.f, 0.f, 0.f);
    frame.basis_x = math::Vector4f(kStereoBasisX, 0.f, 0.f, 0.f);
    frame.basis_y = math::Vector4f(0.f, kStereoBasisY, 0.f, 0.f);
    frame.pfog0 = kStereoPfog0;
    frame.deg_to_rad = 0.017453292519943295f;
    frame.inv_area = 1000000.f;  // no distance fade
    frame.min_scale = 0.f;
    frame.max_scale = 1000000000.f;
    frame.fog_min = 0.f;
    frame.fog_max = 1000000000.f;

    const float camera[16] = {
        kStereoCameraX, 0.f, 0.f, 0.f,  0.f, kStereoCameraY, 0.f, 0.f,
        0.f, 0.f, 0.f, 1.f,             0.f, 0.f, kStereoDepthTerm, 0.f,
    };
    static_assert(sizeof(camera) == sizeof(matrix.camera));
    std::memcpy(&matrix.camera, camera, sizeof(camera));
    matrix.hvdf_offset = math::Vector4f(2048.f, 2048.f, 0.f, kStereoHvdfW);

    hud_matrix.matrix = math::Matrix4f::identity();
    hud_matrix.hvdf_offset = math::Vector4f(2048.f, 2048.f, 0.f, 0.f);

    world.xyz_sx =
        math::Vector4f(kStereoWorldX, kStereoWorldY, kStereoWorldDepth, kStereoWorldScale);
    world.flag_rot_sy = math::Vector4f(0.f, 0.f, 0.f, kStereoWorldScale);
    world.rgba = math::Vector4f(128.f, 0.f, 0.f, 128.f);
    hud.xyz_sx = math::Vector4f(kStereoHudX, kStereoHudY, kStereoHudZ, kStereoHudScale);
    hud.flag_rot_sy = math::Vector4f(0.f, 0.f, 0.f, kStereoHudScale);
    hud.rgba = math::Vector4f(0.f, 128.f, 0.f, 128.f);
  }
};

void append_one_sprite_chunk(SyntheticChain& chain,
                             const SpriteVecData2d& vector,
                             const AdGifData& adgif,
                             u16 program) {
  std::vector<u8> header(16, 0);
  const u32 sprite_count = 1;
  std::memcpy(header.data(), &sprite_count, sizeof(sprite_count));
  chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(1, SpriteDataMem::Header, true), header);
  chain.transfer(vif_code(VifCode::Kind::NOP), vif_unpack_v4_32(3, SpriteDataMem::Vector, true),
                 bytes_of(vector));
  chain.transfer(vif_code(VifCode::Kind::NOP), vif_unpack_v4_32(5, SpriteDataMem::Adgif, true),
                 bytes_of(adgif));
  chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::MSCAL, program));
}

// The Jak 1 sprite bucket: distorter setup with no distorters, frame setup,
// one group-0 2D sprite, then one group-1 HUD sprite.
SyntheticChain make_jak1_stereo_sprite_chain(const Jak1StereoSpriteFixture& fixture) {
  static_assert(sizeof(SpriteFrameDataJak1) % 16 == 0);
  static_assert(sizeof(SpriteHudMatrixData) == 80 * 16);
  SyntheticChain chain;
  chain.empty_next();
  chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::DIRECT, 7),
                 make_distorter_setup(0x1c0, 8));
  chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::PC_PORT),
                 std::vector<u8>(16, 0));
  chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(0x8b, 0x160, false), make_sine_tables());
  chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::DIRECT, 3),
                 make_sprite_direct_setup());
  chain.transfer(vif_stcycl(4, 4),
                 vif_unpack_v4_32(sizeof(SpriteFrameDataJak1) / 16, SpriteDataMem::FrameData,
                                  false),
                 bytes_of(fixture.frame));
  chain.transfer(vif_code(VifCode::Kind::MSCALF, SpriteProgMem::Init),
                 vif_code(VifCode::Kind::FLUSHE));
  chain.transfer(vif_code(VifCode::Kind::BASE, SpriteDataMem::Buffer0),
                 vif_code(VifCode::Kind::OFFSET, SpriteDataMem::Buffer1));
  chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(5, SpriteDataMem::Matrix, false),
                 bytes_of(fixture.matrix));
  append_one_sprite_chunk(chain, fixture.world, fixture.adgif, SpriteProgMem::Sprites2dGrp0);
  chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::FLUSHE));
  chain.transfer(vif_stcycl(4, 4), vif_unpack_v4_32(80, SpriteDataMem::Matrix, false),
                 bytes_of(fixture.hud_matrix));
  append_one_sprite_chunk(chain, fixture.hud, fixture.adgif, SpriteProgMem::Sprites2dHud_Jak1);
  chain.transfer(vif_code(VifCode::Kind::NOP), vif_code(VifCode::Kind::FLUSHE));
  return chain;
}

struct PixelExtent {
  int count = 0;
  float min_x = 1e9f;
  float max_x = -1e9f;
  float min_y = 1e9f;
  float max_y = -1e9f;

  void include(float x, float y) {
    count++;
    min_x = std::min(min_x, x);
    max_x = std::max(max_x, x);
    min_y = std::min(min_y, y);
    max_y = std::max(max_y, y);
  }
};

void include_ndc(PixelExtent* extent, float ndc_x, float ndc_y) {
  extent->include((ndc_x + 1.f) * 0.5f * kDistortTargetWidth,
                  (1.f - ndc_y) * 0.5f * kDistortTargetHeight);
}

// Covered pixel centers must reach the projected corners to within one pixel.
bool extent_matches(const PixelExtent& covered, const PixelExtent& expected) {
  return covered.count > 0 && std::abs(covered.min_x - expected.min_x) <= 1.f &&
         std::abs(covered.max_x - expected.max_x) <= 1.f &&
         std::abs(covered.min_y - expected.min_y) <= 1.f &&
         std::abs(covered.max_y - expected.max_y) <= 1.f;
}

// Projects the billboard's camera-space corners through the true game clip
// (w = depth), then through the eye transform.
PixelExtent expected_world_extent(const metal_renderer::ViewTransform& transform) {
  PixelExtent extent;
  for (float corner_x : {-1.f, 1.f}) {
    for (float corner_y : {-1.f, 1.f}) {
      const float x =
          kStereoWorldX + kStereoBasisX * kStereoWorldScale * corner_x / kStereoCameraX;
      const float y =
          kStereoWorldY + kStereoBasisY * kStereoWorldScale * corner_y / kStereoCameraY;
      const std::array<float, 4> game_clip = {
          kStereoPfog0 * kStereoCameraX * x / 256.f,
          -kStereoPfog0 * kStereoCameraY * y / 128.f * kJak1ScissorAdjust,
          kStereoPfog0 * kStereoDepthTerm / 16777216.f,
          kStereoWorldDepth,
      };
      const auto eye_clip = metal_renderer::apply(transform, game_clip);
      ASSERT(eye_clip[3] > 0.f);
      include_ndc(&extent, eye_clip[0] / eye_clip[3], eye_clip[1] / eye_clip[3]);
    }
  }
  return extent;
}

// HUD corners in authored screen space; no eye transform applies.
PixelExtent expected_hud_extent() {
  PixelExtent extent;
  for (float corner_x : {-1.f, 1.f}) {
    for (float corner_y : {-1.f, 1.f}) {
      include_ndc(&extent, (kStereoHudX + kStereoBasisX * kStereoHudScale * corner_x) / 256.f,
                  -(kStereoHudY + kStereoBasisY * kStereoHudScale * corner_y) / 128.f *
                      kJak1ScissorAdjust);
    }
  }
  return extent;
}

void test_jak1_group0_2d_sprites_follow_eye_transform_and_hud_does_not() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  ASSERT(device);
  id<MTLCommandQueue> queue = [device newCommandQueue];
  ASSERT(queue);

  dispatch_data_t library_data = dispatch_data_create(
      g_goalpad_metallib, g_goalpad_metallib_size, nullptr,
      DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError* library_error = nil;
  id<MTLLibrary> library = [device newLibraryWithData:library_data error:&library_error];
  if (!library && library_error) {
    std::printf("Metal library error: %s\n", library_error.localizedDescription.UTF8String);
  }
  ASSERT(library);

  MetalPsoCache pso_cache;
  MetalSamplerCache sampler_cache;
  MetalStreamBuffer stream;
  ASSERT(pso_cache.init(device, library));
  sampler_cache.init(device);
  stream.init(device);

  const std::size_t initial_live_textures = metal_texture_live_count();
  TexturePool texture_pool(GameVersion::Jak1);
  ASSERT(metal_setup_placeholder(device, queue, texture_pool));
  const u64 placeholder = texture_pool.get_placeholder_texture();

  std::vector<u8> source(kJak1StereoTextureSize * kJak1StereoTextureSize * 4, 255);
  const u64 source_handle = metal_upload_texture_rgba8(
      device, queue, source.data(), kJak1StereoTextureSize, kJak1StereoTextureSize);
  ASSERT(source_handle);
  PcTextureId source_id;
  {
    std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
    TextureInput input;
    input.debug_page_name = "SYNTHETIC";
    input.debug_name = "jak1-stereo-sprite";
    input.id = PcTextureId::from_combo_id(
        (static_cast<u32>(kJak1StereoTexturePage) << 16) | kJak1StereoTextureIndex);
    source_id = input.id;
    input.gpu_texture = source_handle;
    input.src_data = source.data();
    input.w = kJak1StereoTextureSize;
    input.h = kJak1StereoTextureSize;
    texture_pool.give_texture(input);
  }
  publish_synthetic_texture(texture_pool, kJak1StereoTexturePage, kJak1StereoTextureIndex,
                            kJak1StereoTextureTbp);
  ASSERT(texture_pool.lookup(kJak1StereoTextureTbp).value_or(0) == source_handle);

  // Column-major clip_from_game_clip. Left: x' = x + 0.5w, y' = y + 0.25w.
  // Right: x' = 0.75x - 0.25w, w' = w + 0.5x (not affine, so the result
  // depends on using the full homogeneous VU output).
  metal_renderer::ViewTransform left;
  left.clip_from_game_clip[3 * 4 + 0] = 0.5f;
  left.clip_from_game_clip[3 * 4 + 1] = 0.25f;
  metal_renderer::ViewTransform right;
  right.clip_from_game_clip[0 * 4 + 0] = 0.75f;
  right.clip_from_game_clip[3 * 4 + 0] = -0.25f;
  right.clip_from_game_clip[0 * 4 + 3] = 0.5f;
  const metal_renderer::ViewTransform identity;

  constexpr Pixel clear = {16, 32, 48, 255};
  const PixelExtent hud_expected = expected_hud_extent();
  const Jak1StereoSpriteFixture fixture;
  const metal_renderer::ViewTransform* views[] = {&identity, &left, &right};
  PixelExtent world_by_view[3];
  int view_index = 0;
  for (const auto* transform : views) {
    stream.reset();
    auto chain = make_jak1_stereo_sprite_chain(fixture);
    const u32 next_bucket = chain.finish();

    auto* color_desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:kDistortTargetWidth
                                    height:kDistortTargetHeight
                                 mipmapped:NO];
    color_desc.usage = MTLTextureUsageRenderTarget;
#if TARGET_OS_OSX
    color_desc.storageMode = MTLStorageModeManaged;
#else
    color_desc.storageMode = MTLStorageModeShared;
#endif
    id<MTLTexture> color = [device newTextureWithDescriptor:color_desc];
    ASSERT(color);

    auto* depth_desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                     width:kDistortTargetWidth
                                    height:kDistortTargetHeight
                                 mipmapped:NO];
    depth_desc.usage = MTLTextureUsageRenderTarget;
    depth_desc.storageMode = MTLStorageModePrivate;
    id<MTLTexture> depth = [device newTextureWithDescriptor:depth_desc];
    ASSERT(depth);

    id<MTLCommandBuffer> commands = [queue commandBuffer];
    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor =
        MTLClearColorMake(clear.r / 255.f, clear.g / 255.f, clear.b / 255.f, 1.f);
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 0;
    pass.stencilAttachment.texture = depth;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.clearStencil = 0;

    MetalFrameContext ctx;
    ctx.cmds = commands;
    ctx.enc = [commands renderCommandEncoderWithDescriptor:pass];
    ctx.pso_cache = &pso_cache;
    ctx.sampler_cache = &sampler_cache;
    ctx.stream = &stream;
    ctx.color_format = MTLPixelFormatBGRA8Unorm;
    ctx.depth_format = MTLPixelFormatDepth32Float_Stencil8;
    ctx.game_color = color;
    ctx.game_depth = depth;
    ctx.game_viewport = {0.0, 0.0, kDistortTargetWidth, kDistortTargetHeight, 0.0, 1.0};
    [ctx.enc setCullMode:MTLCullModeNone];
    [ctx.enc setViewport:ctx.game_viewport];

    MetalSharedRenderState state;
    state.version = GameVersion::Jak1;
    state.next_bucket = next_bucket;
    state.texture_pool = &texture_pool;
    state.game_res_w = kDistortTargetWidth;
    state.game_res_h = kDistortTargetHeight;
    state.view_transform = *transform;
    DmaFollower dma(chain.bytes.data(), 0, chain.bytes.size());
    MetalSpriteRenderer renderer("synthetic-jak1-stereo-sprite", 63);
    renderer.render(dma, &state, ctx);

    ASSERT(dma.current_tag_offset() == next_bucket);
    ASSERT(renderer.stats().count_2d_grp0 == 1);
    ASSERT(renderer.stats().count_2d_grp1 == 1);
    ASSERT(renderer.stats().normal_sprites_submitted == 2);
    ASSERT(renderer.stats().missing_textures == 0);
    [ctx.enc endEncoding];
#if TARGET_OS_OSX
    id<MTLBlitCommandEncoder> sync = [commands blitCommandEncoder];
    [sync synchronizeResource:color];
    [sync endEncoding];
#endif
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted && commands.error) {
      std::printf("Metal command-buffer error: %s\n",
                  commands.error.localizedDescription.UTF8String);
    }
    ASSERT(commands.status == MTLCommandBufferStatusCompleted);

    std::vector<u8> pixels(kDistortTargetWidth * kDistortTargetHeight * 4);
    [color getBytes:pixels.data()
        bytesPerRow:kDistortTargetWidth * 4
         fromRegion:MTLRegionMake2D(0, 0, kDistortTargetWidth, kDistortTargetHeight)
        mipmapLevel:0];

    PixelExtent world_covered;
    PixelExtent hud_covered;
    for (int y = 0; y < kDistortTargetHeight; y++) {
      for (int x = 0; x < kDistortTargetWidth; x++) {
        const Pixel pixel = read_bgra_pixel(pixels, x, y);
        if (pixel.r >= 250 && pixel.g <= 5 && pixel.b <= 5) {
          world_covered.include(x + 0.5f, y + 0.5f);
        } else if (pixel.g >= 250 && pixel.r <= 5 && pixel.b <= 5) {
          hud_covered.include(x + 0.5f, y + 0.5f);
        } else {
          ASSERT(pixel_near(pixel, clear, 0));
        }
      }
    }
    const PixelExtent world_expected = expected_world_extent(*transform);
    if (!extent_matches(world_covered, world_expected) ||
        !extent_matches(hud_covered, hud_expected)) {
      std::printf(
          "jak1 stereo sprite view %d: world covered x[%.1f, %.1f] y[%.1f, %.1f] expected "
          "x[%.1f, %.1f] y[%.1f, %.1f]; hud covered x[%.1f, %.1f] y[%.1f, %.1f] expected "
          "x[%.1f, %.1f] y[%.1f, %.1f]\n",
          view_index, world_covered.min_x, world_covered.max_x, world_covered.min_y,
          world_covered.max_y, world_expected.min_x, world_expected.max_x, world_expected.min_y,
          world_expected.max_y, hud_covered.min_x, hud_covered.max_x, hud_covered.min_y,
          hud_covered.max_y, hud_expected.min_x, hud_expected.max_x, hud_expected.min_y,
          hud_expected.max_y);
    }
    ASSERT(extent_matches(world_covered, world_expected));
    ASSERT(extent_matches(hud_covered, hud_expected));
    world_by_view[view_index++] = world_covered;
  }

  // The fixture must actually separate the views; otherwise an untransformed
  // sprite could satisfy every case.
  ASSERT(std::abs(world_by_view[1].min_x - world_by_view[0].min_x) > 8.f);
  ASSERT(std::abs(world_by_view[2].min_x - world_by_view[0].min_x) > 8.f);
  ASSERT(std::abs(world_by_view[2].min_x - world_by_view[1].min_x) > 8.f);

  {
    std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
    texture_pool.unload_texture(source_id, source_handle);
  }
  metal_texture_release(source_handle);
  metal_texture_release(placeholder);
  texture_pool.set_placeholder(0);
  ASSERT(metal_texture_live_count() == initial_live_textures);
  std::puts("jak2-metal-sprite-renderer-test: jak1 group-0 2D stereo/HUD screen PASS");
}

}  // namespace

int main() {
  @autoreleasepool {
    test_empty_jak2_bucket();
    test_normal_jak2_parser_and_residual_accounting();
    test_constants_led_glow_retains_finite_output_and_exact_adgif();
    test_constants_led_glow_rejects_clipped_output();
    test_malformed_constants_led_glow_fails_closed();
    test_constants_led_glow_accepts_source_record_limit();
    test_constants_led_glow_record_overflow_fails_closed();
    test_jak2_hud_program();
    test_jak2_glow_marker_is_not_submitted_as_an_ordinary_sprite();
    test_control_led_glow_without_constants_remains_explicitly_unsupported();
    test_mech_flame_identity_publication_and_alpha();
    test_multi_distorter_spatial_sampling_and_alpha();
    test_jak1_group0_2d_sprites_follow_eye_transform_and_hud_does_not();
  }
  std::puts("jak2-metal-sprite-renderer-test: PASS");
  return 0;
}
