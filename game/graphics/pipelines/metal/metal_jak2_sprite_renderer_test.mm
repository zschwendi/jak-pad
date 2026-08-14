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

std::vector<u8> make_distorter_setup() {
  std::vector<u8> data(7 * 16, 0);
  const u64 gif_tag_lo = 1 | (1ull << 15) | (6ull << 60);
  const u64 gif_tag_hi = static_cast<u64>(GifTag::RegisterDescriptor::AD);
  write_u64(data, 0, gif_tag_lo);
  write_u64(data, 8, gif_tag_hi);
  write_u64(data, 16, gs_zbuf(0x130, true));
  write_u64(data, 32, gs_tex0(0, 8, 0, 9, 9));
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

void publish_synthetic_mech_flame(TexturePool& texture_pool) {
  constexpr u32 kGoalFalse = 0xffffffff;
  constexpr std::size_t kTexturePointerCount = kMechFlameTextureIndex + 1;
  constexpr std::size_t kTextureDescriptionOffset =
      sizeof(GoalTexturePage) + kTexturePointerCount * sizeof(u32);
  std::vector<u8> memory(kTextureDescriptionOffset + sizeof(GoalTexture), 0);

  GoalTexturePage page = {};
  page.id = kEffectsTexturePage;
  page.length = kTexturePointerCount;
  std::memcpy(memory.data(), &page, sizeof(page));
  for (std::size_t index = 0; index < kTexturePointerCount; index++) {
    write_u32(memory, sizeof(GoalTexturePage) + index * sizeof(u32), kGoalFalse);
  }
  write_u32(memory,
            sizeof(GoalTexturePage) + kMechFlameTextureIndex * sizeof(u32),
            kTextureDescriptionOffset);

  GoalTexture texture = {};
  texture.num_mips = 1;
  texture.dest[0] = kMechFlameTextureTbp;
  std::memcpy(memory.data() + kTextureDescriptionOffset, &texture, sizeof(texture));
  texture_pool.handle_upload_now(memory.data(), -1, memory.data(), kGoalFalse, false);
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
  }
  std::puts("jak2-metal-sprite-renderer-test: PASS");
  return 0;
}
