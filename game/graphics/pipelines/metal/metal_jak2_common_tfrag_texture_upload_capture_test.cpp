#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/goal_constants.h"

namespace {

constexpr u32 kChainOffset = 0x100;
constexpr u32 kOrdinaryOffset = 0x4000;
constexpr u32 kAnimatorOffset = 0x5000;
constexpr u32 kDirectSetupOffset = 0x6000;
constexpr u32 kExtraTransferOffset = 0x6800;
constexpr u32 kCommonPrisAnimatorOffset = 0x6a00;
constexpr u32 kTexturePageOffset = 0x7000;
constexpr u32 kAnimatorBodyTagOffset = kAnimatorOffset + 16;
constexpr u32 kAnimatorBodyOffset = kAnimatorBodyTagOffset + 16;
constexpr u32 kAnimatorFinishOffset = kAnimatorBodyOffset + 496;
constexpr u32 kAnimatorNextOffset = kAnimatorFinishOffset + 16;
constexpr u32 kSecurityEnvironmentAnimatorFinishOffset = kAnimatorBodyOffset + 336;
constexpr u32 kSecurityEnvironmentAnimatorNextOffset =
    kSecurityEnvironmentAnimatorFinishOffset + 16;
constexpr u32 kSecurityAnimatorFinishOffset = kAnimatorBodyOffset + 832;
constexpr u32 kSecurityAnimatorNextOffset = kSecurityAnimatorFinishOffset + 16;
constexpr u32 kEyeOrdinaryOffset = 0x8000;
constexpr u32 kEyeFirstOffset = 0x9000;
constexpr u32 kEyeSecondOffset = 0xa000;
constexpr u32 kEyeDirectOffset = 0xb000;
constexpr u32 kPris2MercSetupOffset = 0xc000;
constexpr u32 kPris2MercModelOffset = 0xd000;
constexpr u32 kPris2MercPaddingOffset = 0xd800;
constexpr u32 kPris2MercDefaultEndOffset = 0xe000;
constexpr u32 kPrisAnimatorOffset = 0x8800;
constexpr u32 kPrisAnimatorBodyTagOffset = kPrisAnimatorOffset + 16;
constexpr u32 kPrisAnimatorBodyOffset = kPrisAnimatorBodyTagOffset + 16;
constexpr u32 kPrisAnimatorFinishOffset =
    kPrisAnimatorBodyOffset + metal_renderer::kJak2PrisPrisonJakAnimatorBodyBytes;
constexpr u32 kPrisAnimatorLinkerOffset = kPrisAnimatorFinishOffset + 16;
constexpr u32 kEyePageOffset = 0x18000;
constexpr std::size_t kMemorySize = 0x20000;
constexpr u32 kPcPortVif = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
constexpr u32 kDirectVif = static_cast<u32>(VifCode::Kind::DIRECT) << 24;

using Capture = metal_renderer::Jak2CommonTfragTextureUploadCapture;
using Classification = metal_renderer::Jak2CommonTfragTextureUploadClass;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

u32 bucket_offset(u32 bucket_id = metal_renderer::kJak2CommonTfragTextureUploadBucket) {
  return kChainOffset + bucket_id * 16;
}

void put_u32(std::vector<u8>* memory, u32 offset, u32 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_u64(std::vector<u8>* memory, u32 offset, u64 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

u64 get_u64(const std::vector<u8>& memory, u32 offset) {
  u64 value = 0;
  std::memcpy(&value, memory.data() + offset, sizeof(value));
  return value;
}

u32 get_u32(const std::vector<u8>& memory, u32 offset) {
  u32 value = 0;
  std::memcpy(&value, memory.data() + offset, sizeof(value));
  return value;
}

void put_float(std::vector<u8>* memory, u32 offset, float value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_tag(std::vector<u8>* memory,
             u32 offset,
             DmaTag::Kind kind,
             u16 qwc,
             u32 address,
             u32 vif0,
             u32 vif1,
             bool spr = false) {
  u64 tag = static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
            (static_cast<u64>(address) << 32);
  if (spr) {
    tag |= 1ull << 63;
  }
  put_u64(memory, offset, tag);
  put_u32(memory, offset + 8, vif0);
  put_u32(memory, offset + 12, vif1);
}

constexpr u64 make_gif_tag_word(u32 nloop, bool pre, u32 prim, u32 nreg) {
  return static_cast<u64>(nloop) | (1ull << 15) | (static_cast<u64>(pre) << 46) |
         (static_cast<u64>(prim) << 47) | (static_cast<u64>(nreg) << 60);
}

constexpr u64 make_scissor(u32 x0, u32 x1, u32 y0, u32 y1) {
  return static_cast<u64>(x0) | (static_cast<u64>(x1) << 16) |
         (static_cast<u64>(y0) << 32) | (static_cast<u64>(y1) << 48);
}

void put_ad_gif_header(std::vector<u8>* packet,
                       u32 payload_offset,
                       u32 nloop,
                       u32 nreg,
                       u64 registers) {
  put_u64(packet, payload_offset, make_gif_tag_word(nloop, false, 0, nreg));
  put_u64(packet, payload_offset + 8, registers);
}

u32 put_gs_set(std::vector<u8>* packet,
               u32 tag_offset,
               GsRegisterAddress address,
               u64 value) {
  put_tag(packet, tag_offset, DmaTag::Kind::CNT, 2, 0, 0, kDirectVif | 2);
  put_ad_gif_header(packet, tag_offset + 16, 1, 1, 0xeeeeeeeeeeeeeeeeull);
  put_u64(packet, tag_offset + 32, value);
  put_u64(packet, tag_offset + 40, static_cast<u64>(address));
  return tag_offset + 48;
}

u32 put_display_setup(std::vector<u8>* packet, u32 tag_offset, bool eye64) {
  constexpr std::array<GsRegisterAddress, 7> kAddresses = {
      GsRegisterAddress::SCISSOR_1, GsRegisterAddress::XYOFFSET_1,
      GsRegisterAddress::FRAME_1,   GsRegisterAddress::TEST_1,
      GsRegisterAddress::TEXA,      GsRegisterAddress::ZBUF_1,
      GsRegisterAddress::TEXFLUSH};
  const u32 width = eye64 ? 128 : 64;
  const u32 height = eye64 ? 256 : 512;
  const u32 xy_offset = eye64 ? 1024 : 512;
  const u32 fbw = eye64 ? 2 : 1;
  const std::array<u64, 7> values = {
      make_scissor(0, width - 1, 0, height - 1),
      static_cast<u64>(xy_offset) | (static_cast<u64>(xy_offset) << 32),
      124ull | (static_cast<u64>(fbw) << 16),
      0x30000,
      0x8000000080ull,
      0x130ull | (1ull << 24) | (1ull << 32),
      0};
  put_tag(packet, tag_offset, DmaTag::Kind::CNT, 8, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 8);
  const u32 payload_offset = tag_offset + 16;
  put_ad_gif_header(packet, payload_offset, 1, kAddresses.size(), 0xeeeeeeeeeeeeeeeeull);
  for (std::size_t i = 0; i < kAddresses.size(); ++i) {
    put_u64(packet, payload_offset + 16 + static_cast<u32>(i) * 16, values[i]);
    put_u64(packet, payload_offset + 24 + static_cast<u32>(i) * 16,
            static_cast<u64>(kAddresses[i]));
  }
  return tag_offset + 144;
}

u32 put_display_reset(std::vector<u8>* packet, u32 tag_offset) {
  constexpr std::array<GsRegisterAddress, 7> kAddresses = {
      GsRegisterAddress::SCISSOR_1, GsRegisterAddress::XYOFFSET_1,
      GsRegisterAddress::FRAME_1,   GsRegisterAddress::TEST_1,
      GsRegisterAddress::TEXA,      GsRegisterAddress::ZBUF_1,
      GsRegisterAddress::TEXFLUSH};
  constexpr std::array<u64, 7> kValues = {
      make_scissor(0, 511, 0, 415), 0x730000007000ull,
      0x198ull | (8ull << 16),       0x50000ull,
      0x8000000000ull,               0x130ull | (1ull << 24),
      0};
  put_tag(packet, tag_offset, DmaTag::Kind::CNT, 8, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 8);
  const u32 payload_offset = tag_offset + 16;
  put_ad_gif_header(packet, payload_offset, 1, kAddresses.size(), 0xeeeeeeeeeeeeeeeeull);
  for (std::size_t i = 0; i < kAddresses.size(); ++i) {
    put_u64(packet, payload_offset + 16 + static_cast<u32>(i) * 16, kValues[i]);
    put_u64(packet, payload_offset + 24 + static_cast<u32>(i) * 16,
            static_cast<u64>(kAddresses[i]));
  }
  return tag_offset + 144;
}

u32 put_eye_adgif(std::vector<u8>* packet,
                  u32 tag_offset,
                  bool eye64,
                  u32 texture_seed,
                  u64 alpha) {
  put_tag(packet, tag_offset, DmaTag::Kind::CNT, 6, 0, 0, kDirectVif | 6);
  const u32 payload_offset = tag_offset + 16;
  put_ad_gif_header(packet, payload_offset, 5, 1,
                    static_cast<u64>(GifTag::RegisterDescriptor::AD));
  const u64 max_uv = eye64 ? 63 : 31;
  const u64 clamp = 1ull | (1ull << 2) | (max_uv << 14) | (max_uv << 34);
  const u64 tex0 = (texture_seed & 0x3fff) | (1ull << 14) |
                   (static_cast<u64>(GsTex0::PSM::PSMT8) << 20) | (5ull << 26) |
                   (5ull << 30) | (1ull << 34) | (1ull << 61);
  const u64 tex0_addr = static_cast<u64>(GsRegisterAddress::TEX0_1);
  const u64 tex1_addr = static_cast<u64>(GsRegisterAddress::TEX1_1) | 0x70c00700ull;
  const u64 mip_addr = static_cast<u64>(GsRegisterAddress::MIPTBP1_1) | 0x123400ull;
  const std::array<u64, 10> adgif = {
      tex0,   tex0_addr,
      0x101,  tex1_addr,
      0x202,  mip_addr,
      clamp,  static_cast<u64>(GsRegisterAddress::CLAMP_1),
      alpha,  static_cast<u64>(GsRegisterAddress::ALPHA_1)};
  for (std::size_t i = 0; i < adgif.size(); ++i) {
    put_u64(packet, payload_offset + 16 + static_cast<u32>(i) * 8, adgif[i]);
  }
  return tag_offset + 112;
}

u32 put_eye_sprite(std::vector<u8>* packet,
                   u32 tag_offset,
                   bool alpha_blend,
                   u32 alpha,
                   u32 x0,
                   u32 y0,
                   u32 x1,
                   u32 y1,
                   bool background) {
  constexpr u64 kRegisters =
      static_cast<u64>(GifTag::RegisterDescriptor::RGBAQ) |
      (static_cast<u64>(GifTag::RegisterDescriptor::UV) << 4) |
      (static_cast<u64>(GifTag::RegisterDescriptor::XYZ2) << 8) |
      (static_cast<u64>(GifTag::RegisterDescriptor::UV) << 12) |
      (static_cast<u64>(GifTag::RegisterDescriptor::XYZ2) << 16);
  const u32 prim = static_cast<u32>(GsPrim::Kind::SPRITE) | (1u << 4) |
                   (static_cast<u32>(alpha_blend) << 6) | (1u << 8);
  put_tag(packet, tag_offset, DmaTag::Kind::CNT, 6, 0, 0, kDirectVif | 6);
  const u32 payload_offset = tag_offset + 16;
  put_u64(packet, payload_offset, make_gif_tag_word(1, true, prim, 5));
  put_u64(packet, payload_offset + 8, kRegisters);
  put_u32(packet, payload_offset + 16, 128);
  put_u32(packet, payload_offset + 20, 128);
  put_u32(packet, payload_offset + 24, 128);
  put_u32(packet, payload_offset + 28, alpha);
  put_u64(packet, payload_offset + 32, 0);
  put_u64(packet, payload_offset + 40, 0);
  put_u32(packet, payload_offset + 48, x0);
  put_u32(packet, payload_offset + 52, y0);
  put_u32(packet, payload_offset + 56, 0xffffff);
  put_u32(packet, payload_offset + 60, 0);
  put_u32(packet, payload_offset + 64, background ? 0 : 512);
  put_u32(packet, payload_offset + 68, background ? 0 : 512);
  put_u64(packet, payload_offset + 72, 0);
  put_u32(packet, payload_offset + 80, x1);
  put_u32(packet, payload_offset + 84, y1);
  put_u32(packet, payload_offset + 88, 0xffffff);
  put_u32(packet, payload_offset + 92, 0);
  return tag_offset + 112;
}

Capture capture(const std::vector<u8>& packet,
                u32 bucket_id = metal_renderer::kJak2CommonTfragTextureUploadBucket) {
  return metal_renderer::capture_jak2_tfrag_texture_upload(packet.data(), packet.size(),
                                                           kChainOffset, bucket_id);
}

std::vector<u8> make_empty_fixture(
    u32 bucket_id = metal_renderer::kJak2CommonTfragTextureUploadBucket) {
  std::vector<u8> packet(kMemorySize);
  put_tag(&packet, bucket_offset(bucket_id), DmaTag::Kind::CNT, 0, 0, 0, 0);
  return packet;
}

std::vector<u8> make_ordinary_fixture(
    u32 bucket_id = metal_renderer::kJak2CommonTfragTextureUploadBucket) {
  std::vector<u8> packet(kMemorySize);
  const u32 end_offset = bucket_offset(bucket_id) + 16;
  put_tag(&packet, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0);
  put_tag(&packet, kOrdinaryOffset, DmaTag::Kind::CNT, 1, 0, kPcPortVif, 3);
  std::fill_n(packet.begin() + kOrdinaryOffset + 16, 16, 0x31);
  put_tag(&packet, kOrdinaryOffset + 32, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  return packet;
}

std::vector<u8> make_normal_ordinary_fixture(u32 bucket_id,
                                             s64 mode = -1,
                                             u32 dma_relocation = 0) {
  std::vector<u8> packet(kMemorySize);
  const u32 ordinary_offset = kOrdinaryOffset + dma_relocation;
  const u32 direct_setup_offset = kDirectSetupOffset + dma_relocation;
  const u32 end_offset = bucket_offset(bucket_id) + 16;
  put_tag(&packet, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0, ordinary_offset, 0, 0);
  put_tag(&packet, ordinary_offset, DmaTag::Kind::CNT, 1, 0, kPcPortVif, 3);
  put_u64(&packet, ordinary_offset + 16, kTexturePageOffset);
  put_u64(&packet, ordinary_offset + 24, static_cast<u64>(mode));
  put_tag(&packet, ordinary_offset + 32, DmaTag::Kind::NEXT, 0, direct_setup_offset, 0, 0);
  put_tag(&packet, direct_setup_offset, DmaTag::Kind::CNT, 10, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 10);
  std::fill_n(packet.begin() + direct_setup_offset + 16, 160, 0x52);
  put_tag(&packet, direct_setup_offset + 176, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  return packet;
}

std::vector<u8> make_pris2_merc_capture_fixture() {
  constexpr u32 bucket_id = 229;
  std::vector<u8> packet(kMemorySize);
  const u32 end_offset = bucket_offset(bucket_id) + 16;
  const u32 gs_setup_offset = kPris2MercSetupOffset + 176;
  const u32 setup_link_offset = gs_setup_offset + 64;
  const u32 model_link_offset = kPris2MercModelOffset + 16 + 1664;
  const u32 default_end_link_offset = kPris2MercDefaultEndOffset + 16 + 160;

  put_tag(&packet, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0,
          kPris2MercSetupOffset, 0, 0);
  put_tag(&packet, kPris2MercSetupOffset, DmaTag::Kind::CNT, 10, 0,
          (static_cast<u32>(VifCode::Kind::STCYCL) << 24) | 0x404,
          static_cast<u32>(VifCode::Kind::STMOD) << 24);
  std::fill_n(packet.begin() + kPris2MercSetupOffset + 16, 160, 0x61);
  put_tag(&packet, gs_setup_offset, DmaTag::Kind::CNT, 3, 0, 0, kDirectVif | 3);
  std::fill_n(packet.begin() + gs_setup_offset + 16, 48, 0x62);
  put_tag(&packet, setup_link_offset, DmaTag::Kind::NEXT, 0, kPris2MercModelOffset, 0,
          0);
  put_tag(&packet, kPris2MercModelOffset, DmaTag::Kind::CNT, 104, 0, 0, kPcPortVif);
  std::fill_n(packet.begin() + kPris2MercModelOffset + 16, 1664, 0x63);
  put_tag(&packet, model_link_offset, DmaTag::Kind::NEXT, 0,
          kPris2MercPaddingOffset, 0, 0);
  put_tag(&packet, kPris2MercPaddingOffset, DmaTag::Kind::NEXT, 0,
          kPris2MercDefaultEndOffset, 0, 0);
  put_tag(&packet, kPris2MercDefaultEndOffset, DmaTag::Kind::CNT, 10, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 10);
  std::fill_n(packet.begin() + kPris2MercDefaultEndOffset + 16, 160, 0x64);
  put_tag(&packet, default_end_link_offset, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  return packet;
}

std::vector<u8> make_water_ordinary_fixture(u32 bucket_id, s64 mode = -1) {
  auto packet = make_ordinary_fixture(bucket_id);
  put_u64(&packet, kOrdinaryOffset + 16, kTexturePageOffset);
  put_u64(&packet, kOrdinaryOffset + 24, static_cast<u64>(mode));
  packet[kTexturePageOffset + 8] = 0x44;
  return packet;
}

std::vector<u8> make_unobserved_direct_first_fixture(u32 bucket_id) {
  std::vector<u8> packet(kMemorySize);
  const u32 end_offset = bucket_offset(bucket_id) + 16;
  put_tag(&packet, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0, kDirectSetupOffset, 0, 0);
  put_tag(&packet, kDirectSetupOffset, DmaTag::Kind::CNT, 10, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 10);
  std::fill_n(packet.begin() + kDirectSetupOffset + 16, 160, 0x52);
  put_tag(&packet, kDirectSetupOffset + 176, DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0);
  put_tag(&packet, kOrdinaryOffset, DmaTag::Kind::CNT, 1, 0, kPcPortVif, 3);
  put_u64(&packet, kOrdinaryOffset + 16, kTexturePageOffset);
  put_u64(&packet, kOrdinaryOffset + 24, static_cast<u64>(-1));
  put_tag(&packet, kOrdinaryOffset + 32, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  return packet;
}

std::vector<u8> make_normal_shrub_fixture(u32 bucket_id) {
  std::vector<u8> packet(kMemorySize);
  const u32 end_offset = bucket_offset(bucket_id) + 16;
  put_tag(&packet, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0);
  put_tag(&packet, kOrdinaryOffset, DmaTag::Kind::CNT, 2, 0, 0, kDirectVif | 2);
  std::fill_n(packet.begin() + kOrdinaryOffset + 16, 32, 0x41);
  put_tag(&packet, kOrdinaryOffset + 48, DmaTag::Kind::NEXT, 0, kDirectSetupOffset, 0, 0);
  put_tag(&packet, kDirectSetupOffset, DmaTag::Kind::CNT, 10, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 10);
  std::fill_n(packet.begin() + kDirectSetupOffset + 16, 160, 0x52);
  put_tag(&packet, kDirectSetupOffset + 176, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  return packet;
}

std::vector<u8> make_common_pris_fixture(s64 mode = -1) {
  constexpr u32 bucket_id = metal_renderer::kJak2CommonPrisTextureUploadBucket;
  std::vector<u8> packet(kMemorySize);
  const u32 end_offset = bucket_offset(bucket_id) + 16;
  put_tag(&packet, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0);
  put_tag(&packet, kOrdinaryOffset, DmaTag::Kind::CNT, 1, 0, kPcPortVif, 3);
  put_u64(&packet, kOrdinaryOffset + 16, kTexturePageOffset);
  put_u64(&packet, kOrdinaryOffset + 24, static_cast<u64>(mode));
  put_tag(&packet, kOrdinaryOffset + 32, DmaTag::Kind::NEXT, 0,
          kCommonPrisAnimatorOffset, 0, 0);
  put_tag(&packet, kCommonPrisAnimatorOffset, DmaTag::Kind::CNT, 0, 0,
          kPcPortVif | 12, 0);
  put_tag(&packet, kCommonPrisAnimatorOffset + 16, DmaTag::Kind::CNT, 2, 0,
          kPcPortVif | metal_renderer::kJak2CommonPrisDarkJakAnimatorOpcode, 0);
  put_float(&packet, kCommonPrisAnimatorOffset + 32, 0.5f);
  for (u32 i = 0; i < 12; ++i) {
    packet[kCommonPrisAnimatorOffset + 36 + i] = static_cast<u8>(0xa0 + i);
  }
  constexpr std::array<u32, metal_renderer::kJak2CommonPrisDarkJakAnimatorTbpCount> kTbps = {
      0x1200, 0x1210, 0x1220, 0x1230};
  for (std::size_t i = 0; i < kTbps.size(); ++i) {
    put_u32(&packet, kCommonPrisAnimatorOffset + 48 + static_cast<u32>(i) * 4, kTbps[i]);
  }
  put_tag(&packet, kCommonPrisAnimatorOffset + 64, DmaTag::Kind::CNT, 0, 0,
          kPcPortVif | 13, 0);
  put_tag(&packet, kCommonPrisAnimatorOffset + 80, DmaTag::Kind::NEXT, 0,
          kDirectSetupOffset, 0, 0);
  put_tag(&packet, kDirectSetupOffset, DmaTag::Kind::CNT, 10, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 10);
  std::fill_n(packet.begin() + kDirectSetupOffset + 16, 160, 0x52);
  put_tag(&packet, kDirectSetupOffset + 176, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  packet[kTexturePageOffset + 8] = 0x44;
  return packet;
}

std::vector<u8> make_common_pris_opcode22_capture_fixture() {
  return make_common_pris_fixture();
}

struct EyeChunkSpec {
  bool eye64 = false;
  u32 pair_index = 0;
};

struct PrisEyeFixture {
  std::vector<u8> packet;
  u32 first_eye_offset = 0;
};

u32 put_different_eyes_chunk(std::vector<u8>* packet,
                             u32 tag_offset,
                             const EyeChunkSpec& spec) {
  const u32 eye_width = spec.eye64 ? 64 : 32;
  const u32 full_width = eye_width * 2;
  const u32 group = spec.eye64 ? spec.pair_index / 4 : spec.pair_index;
  const u32 y0 = group * eye_width;
  u32 cursor = put_display_setup(packet, tag_offset, spec.eye64);
  cursor = put_gs_set(packet, cursor, GsRegisterAddress::TEST_1, 0x30003);

  u32 texture_seed = 0x100 + spec.pair_index * 16;
  cursor = put_eye_adgif(packet, cursor, spec.eye64, texture_seed++, 0x44);
  cursor = put_gs_set(packet, cursor, GsRegisterAddress::SCISSOR_1,
                      make_scissor(0, full_width - 1, y0, y0 + eye_width - 1));
  const u32 background_x0 = eye_width * 16;
  const u32 background_y0 = (group * eye_width + eye_width) * 16;
  cursor = put_eye_sprite(packet, cursor, false, 128, background_x0, background_y0,
                          (eye_width + full_width) * 16,
                          background_y0 + eye_width * 16, true);
  cursor = put_gs_set(packet, cursor, GsRegisterAddress::SCISSOR_1,
                      make_scissor(0, eye_width - 1, y0, y0 + eye_width - 1));
  cursor = put_eye_sprite(packet, cursor, false, 128, eye_width * 16,
                          (y0 + eye_width) * 16, eye_width * 2 * 16,
                          (y0 + eye_width * 2) * 16, false);
  cursor = put_eye_adgif(packet, cursor, spec.eye64, texture_seed++, 0x44);
  cursor = put_gs_set(packet, cursor, GsRegisterAddress::SCISSOR_1,
                      make_scissor(eye_width, full_width - 1, y0,
                                    y0 + eye_width - 1));
  cursor = put_eye_sprite(packet, cursor, false, 128, eye_width * 2 * 16,
                          (y0 + eye_width) * 16, eye_width * 3 * 16,
                          (y0 + eye_width * 2) * 16, false);

  cursor = put_gs_set(packet, cursor, GsRegisterAddress::TEST_1, 0x33001);
  cursor = put_eye_adgif(packet, cursor, spec.eye64, texture_seed++, 0x44);
  cursor = put_gs_set(packet, cursor, GsRegisterAddress::SCISSOR_1,
                      make_scissor(0, eye_width - 1, y0, y0 + eye_width - 1));
  cursor = put_eye_sprite(packet, cursor, true, 128, eye_width * 16,
                          (y0 + eye_width) * 16, eye_width * 2 * 16,
                          (y0 + eye_width * 2) * 16, false);
  cursor = put_eye_adgif(packet, cursor, spec.eye64, texture_seed++, 0x44);
  cursor = put_gs_set(packet, cursor, GsRegisterAddress::SCISSOR_1,
                      make_scissor(eye_width, full_width - 1, y0,
                                    y0 + eye_width - 1));
  cursor = put_eye_sprite(packet, cursor, true, 128, eye_width * 2 * 16,
                          (y0 + eye_width) * 16, eye_width * 3 * 16,
                          (y0 + eye_width * 2) * 16, false);

  cursor = put_gs_set(packet, cursor, GsRegisterAddress::TEST_1, 0x30003);
  cursor = put_eye_adgif(packet, cursor, spec.eye64, texture_seed++, 1);
  cursor = put_gs_set(packet, cursor, GsRegisterAddress::SCISSOR_1,
                      make_scissor(0, eye_width - 1, y0, y0 + eye_width - 1));
  cursor = put_eye_sprite(packet, cursor, true, 0, eye_width * 16, y0 * 16,
                          eye_width * 2 * 16, (y0 + eye_width) * 16, false);
  cursor = put_eye_adgif(packet, cursor, spec.eye64, texture_seed, 1);
  cursor = put_gs_set(packet, cursor, GsRegisterAddress::SCISSOR_1,
                      make_scissor(eye_width, full_width - 1, y0,
                                    y0 + eye_width - 1));
  cursor = put_eye_sprite(packet, cursor, true, 0, eye_width * 3 * 16, y0 * 16,
                          eye_width * 2 * 16, (y0 + eye_width) * 16, false);

  cursor = put_display_reset(packet, cursor);
  return put_gs_set(packet, cursor, GsRegisterAddress::ALPHA_1, 0x44);
}

void put_pris_prison_jak_animator(std::vector<u8>* packet, u32 dma_relocation,
                                  u32 next_offset) {
  constexpr std::array<u32, metal_renderer::kJak2PrisPrisonJakAnimatorTbpCount> kTbps = {
      0x1000, metal_renderer::kJak2PrisPrisonJakAnimatorMissingTbp, 0x1020, 0x1030,
      0x1040, 0x1050, 0x1060};
  const u32 animator_offset = kPrisAnimatorOffset + dma_relocation;
  const u32 body_tag_offset = kPrisAnimatorBodyTagOffset + dma_relocation;
  const u32 body_offset = kPrisAnimatorBodyOffset + dma_relocation;
  const u32 finish_offset = kPrisAnimatorFinishOffset + dma_relocation;
  const u32 linker_offset = kPrisAnimatorLinkerOffset + dma_relocation;
  put_tag(packet, animator_offset, DmaTag::Kind::CNT, 0, 0, kPcPortVif | 12, 0);
  put_tag(packet, body_tag_offset, DmaTag::Kind::CNT,
          metal_renderer::kJak2PrisPrisonJakAnimatorBodyBytes / 16, 0,
          kPcPortVif | metal_renderer::kJak2PrisPrisonJakAnimatorOpcode, 0);
  put_float(packet, body_offset, 0.5f);
  for (std::size_t i = 0; i < kTbps.size(); ++i) {
    put_u32(packet, body_offset + 16 + static_cast<u32>(i) * sizeof(u32), kTbps[i]);
  }
  // pc-clut-blender leaves morph.yzw and the final four bytes unspecified.
  for (u32 i = 0; i < 12; ++i) {
    (*packet)[body_offset + 4 + i] = static_cast<u8>(0xc0 + i);
  }
  for (u32 i = 0; i < 4; ++i) {
    (*packet)[body_offset + 44 + i] = static_cast<u8>(0xd0 + i);
  }
  put_tag(packet, finish_offset, DmaTag::Kind::CNT, 0, 0, kPcPortVif | 13, 0);
  put_tag(packet, linker_offset, DmaTag::Kind::NEXT, 0, next_offset, 0, 0);
}

PrisEyeFixture make_pris_eye_fixture(u32 bucket_id,
                                     const std::vector<EyeChunkSpec>& chunks,
                                     u32 dma_relocation = 0,
                                     bool prison_jak_animator = false) {
  PrisEyeFixture fixture{std::vector<u8>(kMemorySize), kEyeFirstOffset + dma_relocation};
  const u32 ordinary_offset = kEyeOrdinaryOffset + dma_relocation;
  const u32 first_offset = kEyeFirstOffset + dma_relocation;
  const u32 second_offset = kEyeSecondOffset + dma_relocation;
  const u32 direct_offset = kEyeDirectOffset + dma_relocation;
  const u32 end_offset = bucket_offset(bucket_id) + 16;

  put_tag(&fixture.packet, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0,
          ordinary_offset, 0, 0);
  put_tag(&fixture.packet, ordinary_offset, DmaTag::Kind::CNT, 1, 0, kPcPortVif, 3);
  put_u64(&fixture.packet, ordinary_offset + 16, kEyePageOffset);
  put_u64(&fixture.packet, ordinary_offset + 24, static_cast<u64>(-1));
  const u32 first_chain_offset = prison_jak_animator
                                     ? kPrisAnimatorOffset + dma_relocation
                                     : chunks.empty() ? direct_offset : first_offset;
  put_tag(&fixture.packet, ordinary_offset + 32, DmaTag::Kind::NEXT, 0,
          first_chain_offset, 0, 0);
  if (prison_jak_animator) {
    put_pris_prison_jak_animator(&fixture.packet, dma_relocation,
                                 chunks.empty() ? direct_offset : first_offset);
  }

  u32 linker_offset = 0;
  if (!chunks.empty()) {
    linker_offset = put_different_eyes_chunk(&fixture.packet, first_offset, chunks.at(0));
    if (chunks.size() == 2) {
      put_tag(&fixture.packet, linker_offset, DmaTag::Kind::NEXT, 0, second_offset, 0, 0);
      linker_offset = put_different_eyes_chunk(&fixture.packet, second_offset, chunks.at(1));
    }
    put_tag(&fixture.packet, linker_offset, DmaTag::Kind::NEXT, 0, direct_offset, 0, 0);
  }
  put_tag(&fixture.packet, direct_offset, DmaTag::Kind::CNT, 10, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 10);
  std::fill_n(fixture.packet.begin() + direct_offset + 16, 160, 0x52);
  put_tag(&fixture.packet, direct_offset + 176, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  fixture.packet[kEyePageOffset + 8] = 0x44;
  return fixture;
}

std::vector<u8> make_common_pris_combined_fixture() {
  auto packet = make_common_pris_fixture();
  constexpr u32 bucket_id = metal_renderer::kJak2CommonPrisTextureUploadBucket;
  const u32 end_offset = bucket_offset(bucket_id) + 16;
  put_tag(&packet, kCommonPrisAnimatorOffset + 80, DmaTag::Kind::NEXT, 0,
          kEyeFirstOffset, 0, 0);
  u32 linker = put_different_eyes_chunk(&packet, kEyeFirstOffset, {false, 0});
  put_tag(&packet, linker, DmaTag::Kind::NEXT, 0, kEyeSecondOffset, 0, 0);
  linker = put_different_eyes_chunk(&packet, kEyeSecondOffset, {false, 1});
  put_tag(&packet, linker, DmaTag::Kind::NEXT, 0, kDirectSetupOffset, 0, 0);
  put_tag(&packet, kDirectSetupOffset + 176, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  return packet;
}

std::vector<u8> make_common_pris_one_eye_fixture() {
  auto packet = make_common_pris_fixture();
  constexpr u32 bucket_id = metal_renderer::kJak2CommonPrisTextureUploadBucket;
  const u32 end_offset = bucket_offset(bucket_id) + 16;
  put_tag(&packet, kCommonPrisAnimatorOffset + 80, DmaTag::Kind::NEXT, 0,
          kEyeFirstOffset, 0, 0);
  const u32 linker = put_different_eyes_chunk(&packet, kEyeFirstOffset, {false, 0});
  put_tag(&packet, linker, DmaTag::Kind::NEXT, 0, kDirectSetupOffset, 0, 0);
  put_tag(&packet, kDirectSetupOffset + 176, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  return packet;
}

void put_animator_array(std::vector<u8>* packet,
                        u32 offset,
                        u16 opcode,
                        u16 payload_qwc,
                        u32 next_offset,
                        u32 finish_vif1 = 0) {
  put_tag(packet, offset, DmaTag::Kind::CNT, 0, 0, kPcPortVif | 12, 0);
  offset += 16;
  put_tag(packet, offset, DmaTag::Kind::CNT, payload_qwc, 0, kPcPortVif | opcode, 0);
  std::fill_n(packet->begin() + offset + 16, static_cast<std::size_t>(payload_qwc) * 16, 0x72);
  offset += 16 + static_cast<u32>(payload_qwc) * 16;
  put_tag(packet, offset, DmaTag::Kind::CNT, 0, 0, kPcPortVif | 13, finish_vif1);
  offset += 16;
  put_tag(packet, offset, DmaTag::Kind::NEXT, 0, next_offset, 0, 0);
}

std::vector<u8> make_animator_fixture(u32 finish_vif1 = 0) {
  std::vector<u8> packet(kMemorySize);
  const u32 end_offset = bucket_offset() + 16;
  put_tag(&packet, bucket_offset(), DmaTag::Kind::NEXT, 0, kAnimatorOffset, 0, 0);
  put_animator_array(&packet, kAnimatorOffset, 27, 31, end_offset, finish_vif1);
  return packet;
}

std::vector<u8> make_ordinary_and_animator_fixture() {
  auto packet = make_ordinary_fixture();
  const u32 ordinary_boundary = kOrdinaryOffset + 32;
  const u32 end_offset = bucket_offset() + 16;
  put_tag(&packet, ordinary_boundary, DmaTag::Kind::NEXT, 0, kAnimatorOffset, 0, 0);
  put_animator_array(&packet, kAnimatorOffset, 27, 31, end_offset);
  return packet;
}

void put_layer_values(std::vector<u8>* packet, u32 offset, float base, u8 padding);

std::vector<u8> make_common_water_fixed_animation_fixture(u16 opcode,
                                                          float time,
                                                          u32 destination_tbp,
                                                          u64 page_offset,
                                                          u32 dma_relocation) {
  constexpr u32 bucket_id = metal_renderer::kJak2CommonWaterTextureUploadBucket;
  std::vector<u8> packet(kMemorySize);
  const u32 ordinary_offset = kOrdinaryOffset + dma_relocation;
  const u32 animator_offset = kAnimatorOffset + dma_relocation;
  const u32 animator_body_tag_offset = animator_offset + 16;
  const u32 animator_body_offset = animator_body_tag_offset + 16;
  const u32 animator_finish_offset = animator_body_offset + 336;
  const u32 animator_next_offset = animator_finish_offset + 16;
  const u32 direct_setup_offset = kDirectSetupOffset + dma_relocation;
  const u32 end_offset = bucket_offset(bucket_id) + 16;

  put_tag(&packet, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0, ordinary_offset, 0, 0);
  put_tag(&packet, ordinary_offset, DmaTag::Kind::CNT, 1, 0, kPcPortVif, 3);
  put_u64(&packet, ordinary_offset + 16, page_offset);
  put_u64(&packet, ordinary_offset + 24, static_cast<u64>(-1));
  put_tag(&packet, ordinary_offset + 32, DmaTag::Kind::NEXT, 0, animator_offset, 0, 0);

  put_tag(&packet, animator_offset, DmaTag::Kind::CNT, 0, 0, kPcPortVif | 12, 0);
  put_tag(&packet, animator_body_tag_offset, DmaTag::Kind::CNT, 21, 0,
          kPcPortVif | opcode, 0);
  put_float(&packet, animator_body_offset, time);
  put_u32(&packet, animator_body_offset + 4, destination_tbp);
  for (u32 i = 0; i < 4; ++i) {
    put_layer_values(&packet, animator_body_offset + 16 + i * 80,
                     20.f + static_cast<float>(i) * 20.f,
                     static_cast<u8>(0xc0 + i));
  }
  put_tag(&packet, animator_finish_offset, DmaTag::Kind::CNT, 0, 0,
          kPcPortVif | 13, 0);
  put_tag(&packet, animator_next_offset, DmaTag::Kind::NEXT, 0,
          direct_setup_offset, 0, 0);

  put_tag(&packet, direct_setup_offset, DmaTag::Kind::CNT, 10, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 10);
  std::fill_n(packet.begin() + direct_setup_offset + 16, 160, 0x52);
  put_tag(&packet, direct_setup_offset + 176, DmaTag::Kind::NEXT, 0,
          end_offset, 0, 0);
  if (page_offset + metal_renderer::kJak2Bucket4OrdinaryPageHeaderBytes <= packet.size()) {
    packet[page_offset + 8] = 0x44;
  }
  return packet;
}

std::vector<u8> make_common_water_execution_fixture(u32 dma_relocation = 0) {
  return make_common_water_fixed_animation_fixture(30, 1200.f, 0x2345,
                                                   kTexturePageOffset, dma_relocation);
}

std::vector<u8> make_common_water_bomb_fixture(u32 dma_relocation = 0,
                                               u64 page_offset = kTexturePageOffset) {
  return make_common_water_fixed_animation_fixture(28, 0.f, 0x80, page_offset,
                                                   dma_relocation);
}

std::vector<u8> make_malformed_common_water_capture_fixture() {
  constexpr u32 bucket_id = metal_renderer::kJak2CommonWaterTextureUploadBucket;
  std::vector<u8> packet(kMemorySize);
  const u32 end_offset = bucket_offset(bucket_id) + 16;
  put_tag(&packet, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0, kAnimatorOffset, 0, 0);
  put_tag(&packet, kAnimatorOffset, DmaTag::Kind::CNT, 1, 0, kPcPortVif | 12, 0);
  std::fill_n(packet.begin() + kAnimatorOffset + 16, 16, 0x73);
  put_tag(&packet, kAnimatorOffset + 32, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  return packet;
}

void put_layer_values(std::vector<u8>* packet, u32 offset, float base, u8 padding) {
  for (u32 i = 0; i < 18; ++i) {
    put_float(packet, offset + i * sizeof(float), base + static_cast<float>(i) * 0.25f);
  }
  std::fill_n(packet->begin() + offset + 72, 8, padding);
}

std::vector<u8> make_common_water_dot_only_fixture() {
  constexpr u32 bucket_id = metal_renderer::kJak2CommonWaterTextureUploadBucket;
  auto packet = make_common_water_execution_fixture();
  const u32 dot_finish_offset = kAnimatorBodyOffset + 496;
  const u32 dot_next_offset = dot_finish_offset + 16;
  put_tag(&packet, kAnimatorBodyTagOffset, DmaTag::Kind::CNT, 31, 0,
          kPcPortVif | 30, 0);
  put_float(&packet, kAnimatorBodyOffset, 300.f);
  put_u32(&packet, kAnimatorBodyOffset + 4, 0x3456);
  for (u32 i = 0; i < 6; ++i) {
    put_layer_values(&packet, kAnimatorBodyOffset + 16 + i * 80,
                     100.f + static_cast<float>(i) * 20.f,
                     static_cast<u8>(0xd0 + i));
  }
  put_tag(&packet, dot_finish_offset, DmaTag::Kind::CNT, 0, 0, kPcPortVif | 13, 0);
  put_tag(&packet, dot_next_offset, DmaTag::Kind::NEXT, 0, kDirectSetupOffset, 0, 0);
  put_tag(&packet, kDirectSetupOffset + 176, DmaTag::Kind::NEXT, 0,
          bucket_offset(bucket_id) + 16, 0, 0);
  return packet;
}

std::vector<u8> make_common_execution_fixture() {
  std::vector<u8> packet(kMemorySize);
  const u32 end_offset = bucket_offset() + 16;
  const u32 ordinary_next_offset = kOrdinaryOffset + 32;

  put_tag(&packet, bucket_offset(), DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0);
  put_tag(&packet, kOrdinaryOffset, DmaTag::Kind::CNT, 1, 0, kPcPortVif, 3);
  put_u64(&packet, kOrdinaryOffset + 16, kTexturePageOffset);
  put_u64(&packet, kOrdinaryOffset + 24, static_cast<u64>(-1));
  put_tag(&packet, ordinary_next_offset, DmaTag::Kind::NEXT, 0, kAnimatorOffset, 0, 0);

  put_tag(&packet, kAnimatorOffset, DmaTag::Kind::CNT, 0, 0, kPcPortVif | 12, 0);
  put_tag(&packet, kAnimatorBodyTagOffset, DmaTag::Kind::CNT, 31, 0, kPcPortVif | 27, 0);
  put_float(&packet, kAnimatorBodyOffset, 42.5f);
  put_u32(&packet, kAnimatorBodyOffset + 4, 0x1234);
  for (u32 i = 0; i < 8; ++i) {
    packet[kAnimatorBodyOffset + 8 + i] = static_cast<u8>(0xa0 + i);
  }
  for (u32 i = 0; i < 6; ++i) {
    put_layer_values(&packet, kAnimatorBodyOffset + 16 + i * 80,
                     10.f + static_cast<float>(i) * 20.f, static_cast<u8>(0xb0 + i));
  }
  put_tag(&packet, kAnimatorFinishOffset, DmaTag::Kind::CNT, 0, 0, kPcPortVif | 13, 0);
  put_tag(&packet, kAnimatorNextOffset, DmaTag::Kind::NEXT, 0, kDirectSetupOffset, 0, 0);

  put_tag(&packet, kDirectSetupOffset, DmaTag::Kind::CNT, 10, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 10);
  std::fill_n(packet.begin() + kDirectSetupOffset + 16, 160, 0x52);
  put_tag(&packet, kDirectSetupOffset + 176, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);

  packet[kTexturePageOffset + 8] = 0x44;
  return packet;
}

std::vector<u8> make_water_security_fixture(u32 bucket_id) {
  std::vector<u8> packet(kMemorySize);
  const u32 end_offset = bucket_offset(bucket_id) + 16;

  put_tag(&packet, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0);
  put_tag(&packet, kOrdinaryOffset, DmaTag::Kind::CNT, 1, 0, kPcPortVif, 3);
  put_u64(&packet, kOrdinaryOffset + 16, kTexturePageOffset);
  put_u64(&packet, kOrdinaryOffset + 24, static_cast<u64>(-1));
  put_tag(&packet, kOrdinaryOffset + 32, DmaTag::Kind::NEXT, 0, kAnimatorOffset, 0, 0);

  put_tag(&packet, kAnimatorOffset, DmaTag::Kind::CNT, 0, 0, kPcPortVif | 12, 0);
  put_tag(&packet, kAnimatorBodyTagOffset, DmaTag::Kind::CNT, 52, 0, kPcPortVif | 30, 0);
  put_float(&packet, kAnimatorBodyOffset, 1200.f);
  put_u32(&packet, kAnimatorBodyOffset + 4, 0x2345);
  for (u32 i = 0; i < 4; ++i) {
    put_layer_values(&packet, kAnimatorBodyOffset + 16 + i * 80,
                     20.f + static_cast<float>(i) * 20.f,
                     static_cast<u8>(0xc0 + i));
  }

  constexpr u32 kDotOffset = kAnimatorBodyOffset + 336;
  put_float(&packet, kDotOffset, 300.f);
  put_u32(&packet, kDotOffset + 4, 0x3456);
  for (u32 i = 0; i < 6; ++i) {
    put_layer_values(&packet, kDotOffset + 16 + i * 80,
                     100.f + static_cast<float>(i) * 20.f,
                     static_cast<u8>(0xd0 + i));
  }
  put_tag(&packet, kSecurityAnimatorFinishOffset, DmaTag::Kind::CNT, 0, 0,
          kPcPortVif | 13, 0);
  put_tag(&packet, kSecurityAnimatorNextOffset, DmaTag::Kind::NEXT, 0,
          kDirectSetupOffset, 0, 0);

  put_tag(&packet, kDirectSetupOffset, DmaTag::Kind::CNT, 10, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 10);
  std::fill_n(packet.begin() + kDirectSetupOffset + 16, 160, 0x52);
  put_tag(&packet, kDirectSetupOffset + 176, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);

  packet[kTexturePageOffset + 8] = 0x44;
  return packet;
}

bool metadata_matches(const Capture& lhs, const Capture& rhs) {
  if (lhs.valid != rhs.valid || lhs.present != rhs.present ||
      lhs.classification != rhs.classification || lhs.transfer_count != rhs.transfer_count ||
      lhs.total_payload_bytes != rhs.total_payload_bytes ||
      lhs.inert_transfers != rhs.inert_transfers ||
      lhs.ordinary_descriptors != rhs.ordinary_descriptors ||
      lhs.direct_setup_transfers != rhs.direct_setup_transfers ||
      lhs.gs_setup_transfers != rhs.gs_setup_transfers ||
      lhs.animator_arrays != rhs.animator_arrays ||
      lhs.animator_body_transfers != rhs.animator_body_transfers ||
      lhs.animator_payload_bytes != rhs.animator_payload_bytes ||
      lhs.opcode_counts != rhs.opcode_counts || lhs.eye_markers != rhs.eye_markers ||
      lhs.other_transfers != rhs.other_transfers ||
      lhs.malformed_transfers != rhs.malformed_transfers) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.transfer_count; ++i) {
    const auto& a = lhs.transfers[i];
    const auto& b = rhs.transfers[i];
    if (a.relative_tag_offset != b.relative_tag_offset ||
        a.payload_bytes != b.payload_bytes || a.qwc != b.qwc ||
        a.vif0_immediate != b.vif0_immediate || a.vif1_immediate != b.vif1_immediate ||
        a.tag_kind != b.tag_kind || a.vif0_kind != b.vif0_kind ||
        a.vif1_kind != b.vif1_kind) {
      return false;
    }
  }
  return true;
}

void test_exact_empty_and_ordinary_metadata() {
  auto result = capture(make_empty_fixture());
  check(result.valid && !result.present && result.classification == Classification::Absent &&
            result.transfer_count == 1 && result.inert_transfers == 1 &&
            result.total_payload_bytes == 0,
        "an exact empty bucket is classified as absent");

  result = capture(make_ordinary_fixture());
  check(result.valid && result.present &&
            result.classification == Classification::OrdinaryOnly &&
            result.transfer_count == 3 && result.inert_transfers == 2 &&
            result.ordinary_descriptors == 1 && result.total_payload_bytes == 16 &&
            result.opcode_counts[0] == 0 &&
            result.transfers[0].relative_tag_offset == 0 &&
            result.transfers[1].relative_tag_offset == kOrdinaryOffset - bucket_offset() &&
            result.transfers[1].payload_bytes == 16 && result.transfers[1].qwc == 1 &&
            result.transfers[1].vif0_kind == static_cast<u8>(VifCode::Kind::PC_PORT) &&
            result.transfers[1].vif1_immediate == 3,
        "a descriptor-only bucket owns its exact scalar transfer metadata");

  auto alternate_opcode = make_ordinary_fixture();
  put_u32(&alternate_opcode, kOrdinaryOffset + 8, kPcPortVif | 27);
  result = capture(alternate_opcode);
  check(result.valid && result.classification == Classification::EyeOrOther &&
            result.ordinary_descriptors == 0 && result.other_transfers == 1,
        "a nonzero PC_PORT immediate cannot be classified as the exact ordinary descriptor");
}

void test_texture_bucket_allowlist() {
  for (const u32 bucket_id : metal_renderer::kJak2NormalTfragTextureUploadBuckets) {
    auto result = capture(make_empty_fixture(bucket_id), bucket_id);
    check(result.valid && !result.present &&
              result.classification == Classification::Absent,
          "each audited normal TFRAG texture bucket accepts an exact empty chain");
    result = capture(make_ordinary_fixture(bucket_id), bucket_id);
    check(result.valid && result.present && result.ordinary_descriptors == 1 &&
              result.classification == Classification::OrdinaryOnly,
          "each audited normal TFRAG texture bucket recognizes an ordinary descriptor");
  }

  for (const u32 bucket_id : metal_renderer::kJak2NormalShrubTextureUploadBuckets) {
    const auto result = capture(make_empty_fixture(bucket_id), bucket_id);
    check(result.valid && !result.present && result.classification == Classification::Absent,
          "each audited normal/common SHRUB texture bucket accepts an exact empty chain");
  }

  for (const u32 bucket_id : metal_renderer::kJak2AlphaTextureUploadBuckets) {
    const auto result = capture(make_empty_fixture(bucket_id), bucket_id);
    check(result.valid && !result.present && result.classification == Classification::Absent,
          "each source-identical alpha texture bucket accepts an exact empty chain");
  }

  for (const u32 bucket_id : metal_renderer::kJak2PrisTextureUploadBuckets) {
    auto result = capture(make_empty_fixture(bucket_id), bucket_id);
    check(result.valid && !result.present && result.classification == Classification::Absent,
          "each per-level PRIS texture bucket accepts an exact empty chain");

    std::vector<u8> eye(kMemorySize);
    const u32 end_offset = bucket_offset(bucket_id) + 16;
    put_tag(&eye, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0);
    put_tag(&eye, kOrdinaryOffset, DmaTag::Kind::CNT, 8, 0, 0, kDirectVif | 8);
    put_tag(&eye, kOrdinaryOffset + 144, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
    result = capture(eye, bucket_id);
    check(result.valid && result.present && result.classification == Classification::EyeOrOther &&
              result.eye_markers == 1 && result.other_transfers == 0 &&
              result.total_payload_bytes == 128,
          "each per-level PRIS texture bucket reports a qwc-8 eye marker without promotion");
  }

  for (const u32 bucket_id : metal_renderer::kJak2WaterTextureUploadBuckets) {
    const auto result = capture(make_empty_fixture(bucket_id), bucket_id);
    check(result.valid && !result.present && result.classification == Classification::Absent,
          "each source-identical water texture bucket accepts an exact empty chain");
  }

  auto common_pris = capture(make_empty_fixture(metal_renderer::kJak2CommonPrisTextureUploadBucket),
                             metal_renderer::kJak2CommonPrisTextureUploadBucket);
  check(common_pris.valid && !common_pris.present &&
            common_pris.classification == Classification::Absent,
        "the audited common PRIS texture bucket accepts an exact empty chain");

  const auto packet = make_empty_fixture();
  check(!metal_renderer::capture_jak2_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, 8)
             .valid,
        "an unaudited bucket cannot enter the TFRAG texture classifier");
}

void test_pris2_diagnostic_capture() {
  constexpr u32 kPris2TextureBucket = 228;
  constexpr u32 kPris2MercBucket = 229;
  static_assert(metal_renderer::kJak2Pris2CaptureBuckets[0] == kPris2TextureBucket);
  static_assert(metal_renderer::kJak2Pris2CaptureBuckets[1] == kPris2MercBucket);

  for (const u32 bucket_id : metal_renderer::kJak2Pris2CaptureBuckets) {
    const auto result = capture(make_empty_fixture(bucket_id), bucket_id);
    check(result.valid && !result.present && result.classification == Classification::Absent &&
              result.transfer_count == 1 && result.inert_transfers == 1 &&
              result.total_payload_bytes == 0,
          "each selected PRIS2 diagnostic bucket accepts an exact empty chain");
  }

  auto texture_packet = make_normal_ordinary_fixture(kPris2TextureBucket);
  auto result = capture(texture_packet, kPris2TextureBucket);
  check(result.valid && result.present &&
            result.classification == Classification::OrdinaryOnly &&
            result.transfer_count == 5 && result.total_payload_bytes == 176 &&
            result.inert_transfers == 3 && result.ordinary_descriptors == 1 &&
            result.direct_setup_transfers == 1 && result.animator_arrays == 0 &&
            result.eye_markers == 0 && result.other_transfers == 0 &&
            result.malformed_transfers == 0,
        "PRIS2 bucket 228 reports the audited descriptor-and-standard-reset envelope");
  check(!metal_renderer::plan_jak2_normal_tfrag_texture_upload(
             texture_packet.data(), texture_packet.size(), kChainOffset, kPris2TextureBucket,
             texture_packet.data(), texture_packet.size()) &&
            !metal_renderer::plan_jak2_pris_eye_texture_upload(
                texture_packet.data(), texture_packet.size(), kChainOffset, kPris2TextureBucket,
                texture_packet.data(), texture_packet.size()),
        "bucket 228 stays outside the common texture and six-bucket PRIS plan allowlists");

  const auto merc_packet = make_pris2_merc_capture_fixture();
  result = capture(merc_packet, kPris2MercBucket);
  check(result.valid && result.present &&
            result.classification == Classification::EyeOrOther &&
            result.transfer_count == 9 && result.total_payload_bytes == 2032 &&
            result.inert_transfers == 5 && result.ordinary_descriptors == 0 &&
            result.direct_setup_transfers == 1 && result.animator_arrays == 0 &&
            result.eye_markers == 0 && result.other_transfers == 3 &&
            result.malformed_transfers == 0 && result.transfers[1].payload_bytes == 160 &&
            result.transfers[1].vif0_kind == static_cast<u8>(VifCode::Kind::STCYCL) &&
            result.transfers[2].payload_bytes == 48 &&
            result.transfers[2].vif1_kind == static_cast<u8>(VifCode::Kind::DIRECT) &&
            result.transfers[4].payload_bytes == 1664 &&
            result.transfers[4].vif1_kind == static_cast<u8>(VifCode::Kind::PC_PORT) &&
            result.transfers[7].payload_bytes == 160,
        "PRIS2 bucket 229 reproduces the retained nine-transfer, 2032-byte Merc envelope");

  for (u32 bucket_id = 224; bucket_id <= 251; ++bucket_id) {
    if (bucket_id == kPris2TextureBucket || bucket_id == kPris2MercBucket) {
      continue;
    }
    const auto other = make_empty_fixture(bucket_id);
    check(!capture(other, bucket_id).valid,
          "every other PRIS2 bucket remains outside the diagnostic allowlist");
  }
}

void test_common_water_execution_plan() {
  constexpr u32 bucket_id = metal_renderer::kJak2CommonWaterTextureUploadBucket;
  static_assert(bucket_id == 306);

  auto packet = make_empty_fixture(bucket_id);
  auto result = capture(packet, bucket_id);
  const auto absent = metal_renderer::plan_jak2_common_water_texture_upload(
      packet.data(), packet.size(), kChainOffset, packet.data(), packet.size());
  check(result.valid && !result.present && result.classification == Classification::Absent &&
            result.transfer_count == 1 && result.inert_transfers == 1 && absent.has_value() &&
            !absent->present &&
            absent->variant == metal_renderer::Jak2CommonWaterTextureUploadVariant::Absent,
        "common-water bucket 306 accepts the exact empty plan");

  packet = make_common_water_execution_fixture();
  const auto plan = metal_renderer::plan_jak2_common_water_texture_upload(
      packet.data(), packet.size(), kChainOffset, packet.data(), packet.size(), &result);
  check(plan.has_value() && plan->present &&
            plan->variant == metal_renderer::Jak2CommonWaterTextureUploadVariant::
                                 DescriptorSecurityEnvironmentAndStandardReset &&
            plan->ordinary.page_offset == kTexturePageOffset && plan->ordinary.mode == -1 &&
            plan->ordinary.page_header[8] == 0x44 &&
            plan->security_environment.time == 1200.f &&
            plan->security_environment.destination_tbp == 0x2345 &&
            plan->security_environment.layers[0].start.color[0] == 20.f &&
            plan->security_environment.layers[1].end.st_rot == 84.25f && result.valid &&
            result.present &&
            result.classification == Classification::OrdinaryAndAnimator &&
            result.transfer_count == 9 && result.total_payload_bytes == 512 &&
            result.inert_transfers == 4 && result.ordinary_descriptors == 1 &&
            result.direct_setup_transfers == 1 &&
            result.animator_arrays == 1 && result.animator_body_transfers == 1 &&
            result.animator_payload_bytes == 336 && result.opcode_counts[12] == 1 &&
            result.opcode_counts[13] == 1 && result.opcode_counts[30] == 1 &&
            result.eye_markers == 0 && result.other_transfers == 0 &&
            result.malformed_transfers == 0,
        "common-water bucket 306 owns the exact descriptor/environment/reset plan");

  auto unsupported_finish_packet = make_common_water_execution_fixture();
  put_u32(&unsupported_finish_packet, kSecurityEnvironmentAnimatorFinishOffset + 12,
          kPcPortVif);
  const auto unsupported_finish_plan = metal_renderer::plan_jak2_common_water_texture_upload(
      unsupported_finish_packet.data(), unsupported_finish_packet.size(), kChainOffset,
      unsupported_finish_packet.data(), unsupported_finish_packet.size(), &result);
  check(!unsupported_finish_plan.has_value() && !result.valid && result.malformed_transfers == 1,
        "common-water capture requires the source-defined NOP finish VIF1");

  constexpr u64 kLiveBombPageOffset = 0x1dc3384;
  auto bomb_packet = make_common_water_bomb_fixture(0, kLiveBombPageOffset);
  std::vector<u8> bomb_live_memory(EE_MAIN_MEM_SIZE);
  bomb_live_memory[kLiveBombPageOffset + 8] = 0xb3;
  const auto bomb_plan = metal_renderer::plan_jak2_common_water_texture_upload(
      bomb_packet.data(), bomb_packet.size(), kChainOffset, bomb_live_memory.data(),
      bomb_live_memory.size(), &result);
  check(bomb_plan.has_value() && bomb_plan->present &&
            bomb_plan->variant == metal_renderer::Jak2CommonWaterTextureUploadVariant::
                                      DescriptorBombAndStandardReset &&
            bomb_plan->ordinary.page_offset == kLiveBombPageOffset &&
            bomb_plan->ordinary.mode == -1 && bomb_plan->ordinary.page_header[8] == 0xb3 &&
            bomb_plan->bomb.time == 0.f && bomb_plan->bomb.destination_tbp == 0x80 &&
            bomb_plan->bomb.layers[0].start.color[0] == 20.f &&
            bomb_plan->bomb.layers[1].end.st_rot == 84.25f && result.valid &&
            result.present && result.transfer_count == 9 &&
            result.total_payload_bytes == 512 && result.animator_payload_bytes == 336 &&
            result.opcode_counts[12] == 1 && result.opcode_counts[13] == 1 &&
            result.opcode_counts[28] == 1 &&
            get_u32(bomb_packet, kAnimatorBodyTagOffset + 8) == 0x0800001c &&
            result.transfers[5].vif1_kind == static_cast<u8>(VifCode::Kind::NOP),
        "common-water bucket 306 owns the exact live opcode-28 bomb envelope");

  auto relocated_bomb_packet = make_common_water_bomb_fixture(0x200, kLiveBombPageOffset);
  auto relocated_bomb = metal_renderer::plan_jak2_common_water_texture_upload(
      relocated_bomb_packet.data(), relocated_bomb_packet.size(), kChainOffset,
      bomb_live_memory.data(), bomb_live_memory.size());
  check(relocated_bomb.has_value() &&
            metal_renderer::jak2_common_water_texture_upload_plans_match(
                *bomb_plan, *relocated_bomb),
        "live and relocated copied opcode-28 bomb plans match by owned semantics");
  put_float(&relocated_bomb_packet, kAnimatorBodyOffset + 0x200, 1.f);
  relocated_bomb = metal_renderer::plan_jak2_common_water_texture_upload(
      relocated_bomb_packet.data(), relocated_bomb_packet.size(), kChainOffset,
      bomb_live_memory.data(), bomb_live_memory.size());
  check(relocated_bomb.has_value() &&
            !metal_renderer::jak2_common_water_texture_upload_plans_match(
                *bomb_plan, *relocated_bomb),
        "live/copy matching rejects a changed bomb scalar");

  auto relocated_packet = make_common_water_execution_fixture(0x200);
  auto relocated = metal_renderer::plan_jak2_common_water_texture_upload(
      relocated_packet.data(), relocated_packet.size(), kChainOffset,
      relocated_packet.data(), relocated_packet.size());
  check(relocated.has_value() &&
            metal_renderer::jak2_common_water_texture_upload_plans_match(*plan, *relocated),
        "live and relocated copied common-water plans match by owned semantics");

  put_float(&relocated_packet, kAnimatorBodyOffset + 0x200, 1201.f);
  relocated = metal_renderer::plan_jak2_common_water_texture_upload(
      relocated_packet.data(), relocated_packet.size(), kChainOffset,
      relocated_packet.data(), relocated_packet.size());
  check(relocated.has_value() &&
            !metal_renderer::jak2_common_water_texture_upload_plans_match(*plan, *relocated),
        "live/copy matching rejects a changed security-environment scalar");

  auto changed_page_packet = make_common_water_execution_fixture();
  changed_page_packet[kTexturePageOffset + 8] = 0x45;
  const auto changed_page = metal_renderer::plan_jak2_common_water_texture_upload(
      changed_page_packet.data(), changed_page_packet.size(), kChainOffset,
      changed_page_packet.data(), changed_page_packet.size());
  check(changed_page.has_value() &&
            !metal_renderer::jak2_common_water_texture_upload_plans_match(*plan, *changed_page),
        "live/copy matching rejects a changed owned texture-page header");

  std::fill(packet.begin(), packet.end(), 0xa5);
  check(plan->ordinary.page_header[8] == 0x44 &&
            plan->security_environment.time == 1200.f &&
            plan->security_environment.layers[1].end.source_padding.back() == 0xc3,
        "snapshot reuse cannot change owned common-water page or animator data");

  packet = make_water_security_fixture(bucket_id);
  check(!metal_renderer::plan_jak2_common_water_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size()),
        "bucket 306 rejects the qwc-52 environment-plus-dot water variant");

  packet = make_common_water_dot_only_fixture();
  check(!metal_renderer::plan_jak2_common_water_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size()),
        "bucket 306 rejects a qwc-31 dot-only opcode-30 body");

  packet = make_water_ordinary_fixture(bucket_id);
  check(!metal_renderer::plan_jak2_common_water_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size()),
        "bucket 306 rejects an unobserved descriptor-only form");

  packet = make_common_water_execution_fixture();
  put_tag(&packet, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0, kAnimatorOffset, 0, 0);
  put_tag(&packet, kSecurityEnvironmentAnimatorNextOffset, DmaTag::Kind::NEXT, 0,
          kOrdinaryOffset, 0, 0);
  put_tag(&packet, kOrdinaryOffset + 32, DmaTag::Kind::NEXT, 0, kDirectSetupOffset, 0, 0);
  check(!metal_renderer::plan_jak2_common_water_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size()),
        "bucket 306 rejects an animator before its ordinary page descriptor");

  packet = make_common_water_execution_fixture();
  put_float(&packet, kAnimatorBodyOffset, std::numeric_limits<float>::quiet_NaN());
  check(!metal_renderer::plan_jak2_common_water_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size()),
        "bucket 306 rejects a nonfinite security-environment time");

  packet = make_common_water_execution_fixture();
  put_u32(&packet, kAnimatorBodyOffset + 4, 0x40000);
  check(!metal_renderer::plan_jak2_common_water_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size()),
        "bucket 306 rejects an out-of-VRAM security-environment destination");

  result = capture(make_malformed_common_water_capture_fixture(), bucket_id);
  check(!result.valid && !result.present && result.classification == Classification::Malformed &&
            result.transfer_count == 2 && result.total_payload_bytes == 16 &&
            result.malformed_transfers == 1,
        "common-water bucket 306 bounds malformed animator metadata before planning");
}

void test_pris_eye_execution_plan() {
  for (const u32 bucket_id : metal_renderer::kJak2PrisTextureUploadBuckets) {
    auto empty = make_empty_fixture(bucket_id);
    const auto absent = metal_renderer::plan_jak2_pris_eye_texture_upload(
        empty.data(), empty.size(), kChainOffset, bucket_id, empty.data(), empty.size());
    check(absent.has_value() && !absent->present && absent->bucket_id == bucket_id &&
              absent->chunk_count == 0 && absent->eye_slot_mask == 0,
          "each per-level PRIS bucket accepts only the exact empty absent plan");

    auto ordinary = make_normal_ordinary_fixture(bucket_id);
    metal_renderer::Jak2CommonTfragTextureUploadCapture capture;
    const auto ordinary_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
        ordinary.data(), ordinary.size(), kChainOffset, bucket_id, ordinary.data(),
        ordinary.size(), &capture);
    check(ordinary_plan.has_value() && ordinary_plan->present &&
              ordinary_plan->bucket_id == bucket_id &&
              ordinary_plan->ordinary.page_offset == kTexturePageOffset &&
              ordinary_plan->ordinary.mode == -1 && ordinary_plan->chunk_count == 0 &&
              ordinary_plan->eye_slot_mask == 0 &&
              ordinary_plan->direct_reset_transfer_index == 3 &&
              ordinary_plan->terminal_transfer_index == 4 &&
              ordinary_plan->semantic_fingerprint != 0 && capture.valid &&
              capture.classification == Classification::OrdinaryOnly &&
              capture.transfer_count == 5 && capture.total_payload_bytes == 176 &&
              capture.inert_transfers == 3 && capture.ordinary_descriptors == 1 &&
              capture.direct_setup_transfers == 1 && capture.gs_setup_transfers == 0 &&
              capture.eye_markers == 0 && capture.other_transfers == 0 &&
              capture.malformed_transfers == 0,
          "each per-level PRIS bucket preflights its ordinary-only source envelope");

    auto fixture = make_pris_eye_fixture(bucket_id, {{false, 2}});
    const auto plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
        fixture.packet.data(), fixture.packet.size(), kChainOffset, bucket_id,
        fixture.packet.data(), fixture.packet.size(), &capture);
    check(plan.has_value() && plan->present && plan->bucket_id == bucket_id &&
              plan->ordinary.page_offset == kEyePageOffset && plan->ordinary.mode == -1 &&
              plan->chunk_count == 1 && plan->eye_slot_mask == 0x30 &&
              plan->chunks[0].resolution ==
                  metal_renderer::Jak2PrisEyeResolution::Eye32 &&
              plan->chunks[0].pair_index == 2 &&
              plan->chunks[0].start_transfer_index == 3 &&
              plan->chunks[0].linker_transfer_index == 29 &&
              plan->chunks[0].transfer_count ==
                  metal_renderer::kJak2PrisEyeChunkTransferCount &&
              plan->chunks[0].payload_bytes ==
                  metal_renderer::kJak2PrisEyeChunkPayloadBytes &&
              plan->chunks[0].semantic_fingerprint != 0 &&
              plan->direct_reset_transfer_index == 30 && plan->terminal_transfer_index == 31 &&
              plan->semantic_fingerprint != 0 && capture.valid &&
              capture.transfer_count == 32 && capture.total_payload_bytes == 2032 &&
              capture.inert_transfers == 4 && capture.ordinary_descriptors == 1 &&
              capture.direct_setup_transfers == 1 && capture.gs_setup_transfers == 11 &&
              capture.eye_markers == 2 && capture.other_transfers == 13 &&
              capture.malformed_transfers == 0,
          "each per-level PRIS bucket preflights the exact 32-transfer eye envelope");
  }

  auto two = make_pris_eye_fixture(196, {{false, 0}, {false, 1}});
  metal_renderer::Jak2CommonTfragTextureUploadCapture capture;
  const auto two_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
      two.packet.data(), two.packet.size(), kChainOffset, 196, two.packet.data(),
      two.packet.size(), &capture);
  check(two_plan.has_value() && two_plan->present && two_plan->chunk_count == 2 &&
            two_plan->chunks[0].pair_index == 0 && two_plan->chunks[1].pair_index == 1 &&
            two_plan->chunks[0].start_transfer_index == 3 &&
            two_plan->chunks[0].linker_transfer_index == 29 &&
            two_plan->chunks[1].start_transfer_index == 30 &&
            two_plan->chunks[1].linker_transfer_index == 56 &&
            two_plan->direct_reset_transfer_index == 57 &&
            two_plan->terminal_transfer_index == 58 && two_plan->eye_slot_mask == 0xf &&
            capture.transfer_count == 59 && capture.total_payload_bytes == 3888 &&
            capture.inert_transfers == 5 && capture.ordinary_descriptors == 1 &&
            capture.direct_setup_transfers == 1 && capture.gs_setup_transfers == 22 &&
            capture.eye_markers == 4 && capture.other_transfers == 26 &&
            capture.malformed_transfers == 0,
        "the opening envelope preflights two complete source-ordered eye chunks");

  auto eye64 = make_pris_eye_fixture(200, {{true, 8}});
  const auto eye64_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
      eye64.packet.data(), eye64.packet.size(), kChainOffset, 200, eye64.packet.data(),
      eye64.packet.size());
  check(eye64_plan.has_value() && eye64_plan->present &&
            eye64_plan->chunks[0].resolution ==
                metal_renderer::Jak2PrisEyeResolution::Eye64 &&
            eye64_plan->chunks[0].pair_index == 8 &&
            eye64_plan->chunks[0].eye_slot_mask == (3ull << 16),
        "the same typed grammar distinguishes the source 64-wide eye variant");
}

void test_pris_eye_live_copy_semantics() {
  auto live = make_pris_eye_fixture(196, {{false, 0}, {true, 8}});
  auto copied = make_pris_eye_fixture(196, {{false, 0}, {true, 8}}, 0x4000);
  const auto live_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
      live.packet.data(), live.packet.size(), kChainOffset, 196, live.packet.data(),
      live.packet.size());
  auto copied_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
      copied.packet.data(), copied.packet.size(), kChainOffset, 196, copied.packet.data(),
      copied.packet.size());
  check(live_plan.has_value() && copied_plan.has_value() &&
            live_plan->chunks[0].start_relative_tag_offset !=
                copied_plan->chunks[0].start_relative_tag_offset &&
            live_plan->chunks[1].linker_relative_tag_offset !=
                copied_plan->chunks[1].linker_relative_tag_offset &&
            metal_renderer::jak2_pris_eye_texture_upload_plans_match(*live_plan,
                                                                      *copied_plan),
        "live and relocated copied plans match by owned semantics rather than DMA placement");

  const u64 copied_tex0 = get_u64(copied.packet, copied.first_eye_offset + 224);
  put_u64(&copied.packet, copied.first_eye_offset + 224,
          (copied_tex0 & ~0x3fffull) | 0x321);
  copied_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
      copied.packet.data(), copied.packet.size(), kChainOffset, 196, copied.packet.data(),
      copied.packet.size());
  check(copied_plan.has_value() &&
            !metal_renderer::jak2_pris_eye_texture_upload_plans_match(*live_plan,
                                                                       *copied_plan),
        "a semantic payload mutation between live capture and copied execution is rejected");
}

void test_pris_prison_jak_animator_variants() {
  for (const u32 bucket_id : metal_renderer::kJak2PrisTextureUploadBuckets) {
    auto animator_only = make_pris_eye_fixture(bucket_id, {}, 0, true);
    Capture capture;
    const auto plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
        animator_only.packet.data(), animator_only.packet.size(), kChainOffset, bucket_id,
        animator_only.packet.data(), animator_only.packet.size(), &capture);
    check(plan.has_value() && plan->present && plan->has_prison_jak_animator &&
              plan->prison_jak_animator.start_transfer_index == 3 &&
              plan->prison_jak_animator.body_transfer_index == 4 &&
              plan->prison_jak_animator.finish_transfer_index == 5 &&
              plan->prison_jak_animator.linker_transfer_index == 6 &&
              plan->prison_jak_animator.morph == 0.5f &&
              plan->prison_jak_animator.destination_tbps[1] ==
                  metal_renderer::kJak2PrisPrisonJakAnimatorMissingTbp &&
              plan->prison_jak_animator.source_padding[0] == 0xc0 &&
              plan->prison_jak_animator.source_padding[15] == 0xd3 &&
              plan->chunk_count == 0 && plan->direct_reset_transfer_index == 7 &&
              plan->terminal_transfer_index == 8 && plan->semantic_fingerprint != 0 &&
              capture.valid &&
              capture.classification == Classification::OrdinaryAndAnimator &&
              capture.transfer_count == 9 && capture.total_payload_bytes == 224 &&
              capture.inert_transfers == 4 && capture.animator_arrays == 1 &&
              capture.animator_body_transfers == 1 && capture.animator_payload_bytes == 48 &&
              capture.opcode_counts[metal_renderer::kJak2PrisPrisonJakAnimatorOpcode] == 1 &&
              capture.eye_markers == 0 && capture.other_transfers == 0,
          "all six per-level PRIS buckets accept the exact animator-only prison-Jak composite");
  }

  auto one = make_pris_eye_fixture(204, {{false, 2}}, 0, true);
  Capture one_capture;
  const auto one_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
      one.packet.data(), one.packet.size(), kChainOffset, 204, one.packet.data(),
      one.packet.size(), &one_capture);
  check(one_plan.has_value() && one_plan->has_prison_jak_animator && one_plan->chunk_count == 1 &&
            one_plan->prison_jak_animator.linker_transfer_index == 6 &&
            one_plan->chunks[0].start_transfer_index == 7 &&
            one_plan->chunks[0].linker_transfer_index == 33 &&
            one_plan->direct_reset_transfer_index == 34 &&
            one_plan->terminal_transfer_index == 35 && one_capture.transfer_count == 36 &&
            one_capture.total_payload_bytes == 2080 && one_capture.inert_transfers == 5 &&
            one_capture.eye_markers == 2 && one_capture.other_transfers == 13,
        "the prison-Jak animator can precede one existing eye chunk without changing eye parsing");

  auto two = make_pris_eye_fixture(196, {{false, 0}, {false, 1}}, 0, true);
  Capture two_capture;
  const auto two_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
      two.packet.data(), two.packet.size(), kChainOffset, 196, two.packet.data(),
      two.packet.size(), &two_capture);
  check(two_plan.has_value() && two_plan->has_prison_jak_animator && two_plan->chunk_count == 2 &&
            two_plan->chunks[0].start_transfer_index == 7 &&
            two_plan->chunks[0].linker_transfer_index == 33 &&
            two_plan->chunks[1].start_transfer_index == 34 &&
            two_plan->chunks[1].linker_transfer_index == 60 &&
            two_plan->direct_reset_transfer_index == 61 &&
            two_plan->terminal_transfer_index == 62 && two_capture.transfer_count == 63 &&
            two_capture.total_payload_bytes == 3936 && two_capture.inert_transfers == 6 &&
            two_capture.eye_markers == 4 && two_capture.other_transfers == 26,
        "the prison-Jak animator can precede two existing eye chunks at the transfer bound");

  auto live = make_pris_eye_fixture(200, {{false, 0}}, 0, true);
  auto copied = make_pris_eye_fixture(200, {{false, 0}}, 0x4000, true);
  const auto live_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
      live.packet.data(), live.packet.size(), kChainOffset, 200, live.packet.data(),
      live.packet.size());
  auto copied_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
      copied.packet.data(), copied.packet.size(), kChainOffset, 200, copied.packet.data(),
      copied.packet.size());
  check(live_plan.has_value() && copied_plan.has_value() &&
            live_plan->prison_jak_animator.start_relative_tag_offset !=
                copied_plan->prison_jak_animator.start_relative_tag_offset &&
            metal_renderer::jak2_pris_eye_texture_upload_plans_match(*live_plan, *copied_plan),
        "relocated prison-Jak live and copied plans match by owned animator semantics");

  put_u32(&copied.packet, kPrisAnimatorBodyOffset + 0x4000 + 4, 0xee);
  copied_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
      copied.packet.data(), copied.packet.size(), kChainOffset, 200, copied.packet.data(),
      copied.packet.size());
  check(copied_plan.has_value() &&
            !metal_renderer::jak2_pris_eye_texture_upload_plans_match(*live_plan, *copied_plan),
        "opaque prison-Jak source bytes remain covered by the copied-plan fingerprint");

  auto bad_qwc = make_pris_eye_fixture(200, {}, 0, true);
  put_u64(&bad_qwc.packet, kPrisAnimatorBodyTagOffset,
          2ull | (static_cast<u64>(DmaTag::Kind::CNT) << 28));
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             bad_qwc.packet.data(), bad_qwc.packet.size(), kChainOffset, 200,
             bad_qwc.packet.data(), bad_qwc.packet.size())
             .has_value(),
        "a prison-Jak body with the wrong qwc is rejected");

  auto bad_opcode = make_pris_eye_fixture(200, {}, 0, true);
  put_u32(&bad_opcode.packet, kPrisAnimatorBodyTagOffset + 8, kPcPortVif | 22);
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             bad_opcode.packet.data(), bad_opcode.packet.size(), kChainOffset, 200,
             bad_opcode.packet.data(), bad_opcode.packet.size())
             .has_value(),
        "a dark-Jak opcode in the prison-Jak body position is rejected");

  auto bad_order = make_pris_eye_fixture(200, {}, 0, true);
  put_u32(&bad_order.packet, kPrisAnimatorOffset + 8,
          kPcPortVif | metal_renderer::kJak2PrisPrisonJakAnimatorOpcode);
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             bad_order.packet.data(), bad_order.packet.size(), kChainOffset, 200,
             bad_order.packet.data(), bad_order.packet.size())
             .has_value(),
        "a prison-Jak body before its start marker is rejected");

  auto bad_tbp = make_pris_eye_fixture(200, {}, 0, true);
  put_u32(&bad_tbp.packet, kPrisAnimatorBodyOffset + 16,
          metal_renderer::kJak2PrisPrisonJakAnimatorTbpUpperBound);
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             bad_tbp.packet.data(), bad_tbp.packet.size(), kChainOffset, 200,
             bad_tbp.packet.data(), bad_tbp.packet.size())
             .has_value(),
        "a prison-Jak TBP outside PS2 VRAM is rejected while UINT32_MAX remains valid");

  auto bad_morph = make_pris_eye_fixture(200, {}, 0, true);
  put_float(&bad_morph.packet, kPrisAnimatorBodyOffset, 2.f);
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             bad_morph.packet.data(), bad_morph.packet.size(), kChainOffset, 200,
             bad_morph.packet.data(), bad_morph.packet.size())
             .has_value(),
        "a prison-Jak morph outside the source [0,1] range is rejected");

  auto nonfinite_morph = make_pris_eye_fixture(200, {}, 0, true);
  put_u32(&nonfinite_morph.packet, kPrisAnimatorBodyOffset, 0x7fc00000);
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             nonfinite_morph.packet.data(), nonfinite_morph.packet.size(), kChainOffset, 200,
             nonfinite_morph.packet.data(), nonfinite_morph.packet.size())
             .has_value(),
        "a non-finite prison-Jak morph is rejected");
}

void test_pris_ordinary_live_copy_semantics() {
  auto live = make_normal_ordinary_fixture(200);
  auto copied = make_normal_ordinary_fixture(200, -1, 0x2000);
  const auto live_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
      live.data(), live.size(), kChainOffset, 200, live.data(), live.size());
  auto copied_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
      copied.data(), copied.size(), kChainOffset, 200, copied.data(), copied.size());
  check(live_plan.has_value() && copied_plan.has_value() && live_plan->present &&
            copied_plan->present && live_plan->chunk_count == 0 &&
            copied_plan->chunk_count == 0 &&
            live_plan->direct_reset_relative_tag_offset !=
                copied_plan->direct_reset_relative_tag_offset &&
            metal_renderer::jak2_pris_eye_texture_upload_plans_match(*live_plan,
                                                                      *copied_plan),
        "relocated ordinary-only live and copied plans match by owned page semantics");

  constexpr u32 kAlternatePageOffset = kTexturePageOffset + 0x100;
  put_u64(&copied, kOrdinaryOffset + 0x2000 + 16, kAlternatePageOffset);
  copied_plan = metal_renderer::plan_jak2_pris_eye_texture_upload(
      copied.data(), copied.size(), kChainOffset, 200, copied.data(), copied.size());
  check(copied_plan.has_value() && copied_plan->present && copied_plan->chunk_count == 0 &&
            !metal_renderer::jak2_pris_eye_texture_upload_plans_match(*live_plan,
                                                                       *copied_plan),
        "an ordinary descriptor mutation between live capture and copied execution is rejected");
}

void test_pris_eye_shape_fails_closed() {
  auto transposed_gif_shape = make_pris_eye_fixture(200, {{false, 2}});
  put_u64(&transposed_gif_shape.packet, transposed_gif_shape.first_eye_offset + 16,
          make_gif_tag_word(7, false, 0, 1));
  metal_renderer::Jak2PrisEyeTextureUploadRejection rejection;
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             transposed_gif_shape.packet.data(), transposed_gif_shape.packet.size(),
             kChainOffset, 200, transposed_gif_shape.packet.data(),
             transposed_gif_shape.packet.size(), nullptr, &rejection)
             .has_value() &&
            rejection.reason ==
                metal_renderer::Jak2PrisEyeTextureUploadRejectReason::SetupTag &&
            rejection.chunk_index == 0 &&
            rejection.body_index == metal_renderer::kJak2PrisEyeRejectIndexNotApplicable &&
            std::strcmp(metal_renderer::jak2_pris_eye_texture_upload_reject_reason_name(
                            rejection.reason),
                        "setup-tag") == 0,
        "a GS-set tag with its source NLOOP and NREG transposed is rejected");

  auto truncated_gif_registers = make_pris_eye_fixture(200, {{false, 2}});
  put_u64(&truncated_gif_registers.packet,
          truncated_gif_registers.first_eye_offset + 24,
          static_cast<u64>(GifTag::RegisterDescriptor::AD));
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             truncated_gif_registers.packet.data(), truncated_gif_registers.packet.size(),
             kChainOffset, 200, truncated_gif_registers.packet.data(),
             truncated_gif_registers.packet.size())
             .has_value(),
        "a GS-set tag without the source's full A+D register list is rejected");

  auto malformed = make_pris_eye_fixture(200, {{false, 2}});
  put_u64(&malformed.packet, malformed.first_eye_offset + 336,
          make_scissor(0, 62, 64, 95));
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             malformed.packet.data(), malformed.packet.size(), kChainOffset, 200,
             malformed.packet.data(), malformed.packet.size())
             .has_value(),
        "a body scissor outside the exact 32-wide coordinate grammar is rejected");

  auto bad_uv = make_pris_eye_fixture(200, {{false, 2}});
  put_u32(&bad_uv.packet, bad_uv.first_eye_offset + 592, 511);
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             bad_uv.packet.data(), bad_uv.packet.size(), kChainOffset, 200,
             bad_uv.packet.data(), bad_uv.packet.size())
             .has_value(),
        "a sprite UV extent that disagrees with its TEX0 width is rejected");

  auto clipped_dynamic = make_pris_eye_fixture(200, {{false, 2}});
  put_u32(&clipped_dynamic.packet, clipped_dynamic.first_eye_offset + 576, 0);
  put_u32(&clipped_dynamic.packet, clipped_dynamic.first_eye_offset + 608, 16);
  put_u32(&clipped_dynamic.packet, clipped_dynamic.first_eye_offset + 1440, 0);
  put_u32(&clipped_dynamic.packet, clipped_dynamic.first_eye_offset + 1472, 16);
  put_u32(&clipped_dynamic.packet, clipped_dynamic.first_eye_offset + 1764, 0);
  put_u32(&clipped_dynamic.packet, clipped_dynamic.first_eye_offset + 1796, 16);
  check(metal_renderer::plan_jak2_pris_eye_texture_upload(
            clipped_dynamic.packet.data(), clipped_dynamic.packet.size(), kChainOffset, 200,
            clipped_dynamic.packet.data(), clipped_dynamic.packet.size())
            .has_value(),
        "source-valid clipped iris, pupil, and lid rectangles remain executable");

  auto bad_lid_x = make_pris_eye_fixture(200, {{false, 2}});
  put_u32(&bad_lid_x.packet, bad_lid_x.first_eye_offset + 1760, 0);
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             bad_lid_x.packet.data(), bad_lid_x.packet.size(), kChainOffset, 200,
             bad_lid_x.packet.data(), bad_lid_x.packet.size())
             .has_value(),
        "a lid rectangle with the wrong source-fixed X coordinate is rejected");

  auto reversed_dynamic = make_pris_eye_fixture(200, {{false, 2}});
  put_u32(&reversed_dynamic.packet, reversed_dynamic.first_eye_offset + 576, 20);
  put_u32(&reversed_dynamic.packet, reversed_dynamic.first_eye_offset + 608, 10);
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             reversed_dynamic.packet.data(), reversed_dynamic.packet.size(), kChainOffset,
             200, reversed_dynamic.packet.data(), reversed_dynamic.packet.size())
             .has_value(),
        "a non-lid dynamic rectangle with reversed X order is rejected");

  auto invalid_raw_xyz = make_pris_eye_fixture(200, {{false, 2}});
  put_u32(&invalid_raw_xyz.packet, invalid_raw_xyz.first_eye_offset + 576, 0x10000);
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             invalid_raw_xyz.packet.data(), invalid_raw_xyz.packet.size(), kChainOffset, 200,
             invalid_raw_xyz.packet.data(), invalid_raw_xyz.packet.size())
             .has_value(),
        "a dynamic rectangle coordinate outside the source u16 range is rejected");

  auto bad_background = make_pris_eye_fixture(200, {{false, 2}});
  put_u32(&bad_background.packet, bad_background.first_eye_offset + 416, 511);
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             bad_background.packet.data(), bad_background.packet.size(), kChainOffset, 200,
             bad_background.packet.data(), bad_background.packet.size())
             .has_value(),
        "the non-dynamic background rectangle remains exact");

  auto bad_tex0 = make_pris_eye_fixture(200, {{false, 2}});
  const u64 tex0 = get_u64(bad_tex0.packet, bad_tex0.first_eye_offset + 224);
  put_u64(&bad_tex0.packet, bad_tex0.first_eye_offset + 224, tex0 & ~(1ull << 34));
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             bad_tex0.packet.data(), bad_tex0.packet.size(), kChainOffset, 200,
             bad_tex0.packet.data(), bad_tex0.packet.size())
             .has_value(),
        "an eye source TEX0 without texture alpha is rejected");

  auto bad_adgif_address = make_pris_eye_fixture(200, {{false, 2}});
  const u32 tex1_address_offset = bad_adgif_address.first_eye_offset + 248;
  const u64 tex1_address = get_u64(bad_adgif_address.packet, tex1_address_offset);
  put_u64(&bad_adgif_address.packet, tex1_address_offset,
          (tex1_address & ~0xffull) | static_cast<u8>(GsRegisterAddress::TEX1_2));
  rejection = {};
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             bad_adgif_address.packet.data(), bad_adgif_address.packet.size(), kChainOffset,
             200, bad_adgif_address.packet.data(), bad_adgif_address.packet.size(), nullptr,
             &rejection)
             .has_value() &&
            rejection.reason ==
                metal_renderer::Jak2PrisEyeTextureUploadRejectReason::BodyAdgif &&
            rejection.chunk_index == 0 && rejection.body_index == 0,
        "an eye adgif with source metadata but the wrong low-byte GS address is rejected");

  auto duplicate = make_pris_eye_fixture(196, {{false, 1}, {false, 1}});
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             duplicate.packet.data(), duplicate.packet.size(), kChainOffset, 196,
             duplicate.packet.data(), duplicate.packet.size())
             .has_value(),
        "two chunks that target the same eye slots are rejected before execution");

  auto missing_reset = make_ordinary_fixture(200);
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             missing_reset.data(), missing_reset.size(), kChainOffset, 200,
             missing_reset.data(), missing_reset.size())
             .has_value(),
        "a PRIS descriptor without its exact source Direct reset is rejected");

  auto common = make_empty_fixture(metal_renderer::kJak2CommonPrisTextureUploadBucket);
  check(!metal_renderer::plan_jak2_pris_eye_texture_upload(
             common.data(), common.size(), kChainOffset,
             metal_renderer::kJak2CommonPrisTextureUploadBucket, common.data(), common.size())
             .has_value(),
        "common PRIS bucket 220 remains outside the per-level eye grammar");
}

void test_common_pris_execution_plan() {
  auto packet = make_common_pris_fixture();
  metal_renderer::Jak2CommonTfragTextureUploadCapture capture;
  const auto plan = metal_renderer::plan_jak2_common_pris_texture_upload(
      packet.data(), packet.size(), kChainOffset, packet.data(), packet.size(), &capture);
  check(plan.has_value() && plan->present &&
            plan->ordinary.page_offset == kTexturePageOffset && plan->ordinary.mode == -1 &&
            plan->dark_jak_animator.morph == 0.5f &&
            plan->dark_jak_animator.destination_tbps ==
                std::array<u32, 4>{0x1200, 0x1210, 0x1220, 0x1230} &&
            plan->chunk_count == 0 && capture.valid && capture.transfer_count == 9 &&
            capture.total_payload_bytes == 208 && capture.inert_transfers == 4 &&
            capture.ordinary_descriptors == 1 && capture.gs_setup_transfers == 0 &&
            capture.direct_setup_transfers == 1 && capture.animator_arrays == 1 &&
            capture.animator_body_transfers == 1 && capture.animator_payload_bytes == 32 &&
            capture.opcode_counts[12] == 1 && capture.opcode_counts[22] == 1 &&
            capture.opcode_counts[13] == 1 && capture.eye_markers == 0 &&
            capture.other_transfers == 0,
        "common PRIS owns the exact nine-transfer Dark Jak form");

  auto copied_packet = make_common_pris_fixture();
  std::copy_n(packet.begin() + kTexturePageOffset,
              metal_renderer::kJak2Bucket4OrdinaryPageHeaderBytes,
              copied_packet.begin() + kTexturePageOffset);
  const auto copied_plan = metal_renderer::plan_jak2_common_pris_texture_upload(
      copied_packet.data(), copied_packet.size(), kChainOffset, copied_packet.data(),
      copied_packet.size());
  check(copied_plan.has_value() &&
            metal_renderer::jak2_common_pris_texture_upload_plans_match(*plan, *copied_plan),
        "independently reparsed common PRIS plans match semantically");

  copied_packet[kCommonPrisAnimatorOffset + 48] ^= 1;
  const auto changed_plan = metal_renderer::plan_jak2_common_pris_texture_upload(
      copied_packet.data(), copied_packet.size(), kChainOffset, copied_packet.data(),
      copied_packet.size());
  check(changed_plan.has_value() &&
            !metal_renderer::jak2_common_pris_texture_upload_plans_match(*plan, *changed_plan),
        "a copied opcode-22 payload mutation fails the semantic equality barrier");

  copied_packet = make_common_pris_fixture();
  copied_packet[kDirectSetupOffset + 16] ^= 1;
  const auto changed_reset_plan = metal_renderer::plan_jak2_common_pris_texture_upload(
      copied_packet.data(), copied_packet.size(), kChainOffset, copied_packet.data(),
      copied_packet.size());
  check(changed_reset_plan.has_value() &&
            !metal_renderer::jak2_common_pris_texture_upload_plans_match(
                *plan, *changed_reset_plan),
        "a copied dynamic 160-byte reset mutation fails the semantic equality barrier");

  packet = make_common_pris_one_eye_fixture();
  const auto one_eye = metal_renderer::plan_jak2_common_pris_texture_upload(
      packet.data(), packet.size(), kChainOffset, packet.data(), packet.size(), &capture);
  auto copied_one_eye_packet = make_common_pris_one_eye_fixture();
  const auto copied_one_eye = metal_renderer::plan_jak2_common_pris_texture_upload(
      copied_one_eye_packet.data(), copied_one_eye_packet.size(), kChainOffset,
      copied_one_eye_packet.data(), copied_one_eye_packet.size());
  check(one_eye.has_value() && one_eye->present && one_eye->chunk_count == 1 &&
            one_eye->eye_slot_mask != 0 && capture.classification == Classification::EyeOrOther &&
            capture.transfer_count == 36 &&
            capture.total_payload_bytes == 2064 && capture.inert_transfers == 5 &&
            capture.ordinary_descriptors == 1 && capture.direct_setup_transfers == 1 &&
            capture.gs_setup_transfers == 11 && capture.animator_arrays == 1 &&
            capture.animator_body_transfers == 1 && capture.animator_payload_bytes == 32 &&
            capture.opcode_counts[12] == 1 && capture.opcode_counts[22] == 1 &&
            capture.opcode_counts[13] == 1 && capture.eye_markers == 2 &&
            capture.other_transfers == 13 && capture.malformed_transfers == 0 &&
            copied_one_eye.has_value() &&
            metal_renderer::jak2_common_pris_texture_upload_plans_match(*one_eye,
                                                                         *copied_one_eye),
        "common PRIS owns and semantically matches the exact 36-transfer one-eye form");

  packet = make_common_pris_combined_fixture();
  const auto combined = metal_renderer::plan_jak2_common_pris_texture_upload(
      packet.data(), packet.size(), kChainOffset, packet.data(), packet.size(), &capture);
  check(combined.has_value() && combined->present && combined->chunk_count == 2 &&
            combined->eye_slot_mask != 0 && capture.transfer_count == 63 &&
            capture.total_payload_bytes == 3920 && capture.inert_transfers == 6 &&
            capture.ordinary_descriptors == 1 && capture.direct_setup_transfers == 1 &&
            capture.gs_setup_transfers == 22 && capture.animator_arrays == 1 &&
            capture.animator_body_transfers == 1 && capture.animator_payload_bytes == 32 &&
            capture.opcode_counts[12] == 1 && capture.opcode_counts[22] == 1 &&
            capture.opcode_counts[13] == 1 && capture.eye_markers == 4 &&
            capture.other_transfers == 26,
        "common PRIS owns the exact 63-transfer Dark Jak plus two-eye form");

  packet[kEyeSecondOffset + 24] ^= 1;
  check(!metal_renderer::plan_jak2_common_pris_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
             .has_value(),
        "a malformed combined eye body fails before any execution plan is returned");

  packet = make_empty_fixture(metal_renderer::kJak2CommonPrisTextureUploadBucket);
  const auto absent = metal_renderer::plan_jak2_common_pris_texture_upload(
      packet.data(), packet.size(), kChainOffset, packet.data(), packet.size());
  check(absent.has_value() && !absent->present,
        "common PRIS preserves the exact absent plan");

  packet = make_common_pris_fixture(0);
  check(!metal_renderer::plan_jak2_common_pris_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
             .has_value(),
        "common PRIS rejects an unobserved upload mode");

  packet = make_common_pris_fixture();
  put_u32(&packet, kCommonPrisAnimatorOffset + 12, kDirectVif | 3);
  check(!metal_renderer::plan_jak2_common_pris_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
             .has_value(),
        "common PRIS rejects a non-source animator start");
}

void test_common_pris_opcode22_capture() {
  const auto result =
      capture(make_common_pris_opcode22_capture_fixture(),
              metal_renderer::kJak2CommonPrisTextureUploadBucket);
  check(result.valid && result.present &&
            result.classification == Classification::OrdinaryAndAnimator &&
            result.transfer_count == 9 && result.total_payload_bytes == 208 &&
            result.inert_transfers == 4 && result.ordinary_descriptors == 1 &&
            result.gs_setup_transfers == 0 && result.direct_setup_transfers == 1 &&
            result.animator_arrays == 1 && result.animator_body_transfers == 1 &&
            result.animator_payload_bytes == 32 && result.opcode_counts[12] == 1 &&
            result.opcode_counts[13] == 1 && result.opcode_counts[22] == 1 &&
            result.other_transfers == 0 && result.malformed_transfers == 0,
        "common PRIS capture exposes the live nine-transfer/208-byte opcode-22 grammar");
}

void test_normal_tfrag_execution_plan() {
  for (const u32 bucket_id : metal_renderer::kJak2NormalTfragTextureUploadBuckets) {
    auto packet = make_normal_ordinary_fixture(bucket_id);
    metal_renderer::Jak2CommonTfragTextureUploadCapture result;
    const auto plan = metal_renderer::plan_jak2_normal_tfrag_texture_upload(
        packet.data(), packet.size(), kChainOffset, bucket_id, packet.data(), packet.size(),
        &result);
    check(plan.has_value() && plan->present && plan->bucket_id == bucket_id &&
              plan->ordinary.page_offset == kTexturePageOffset && plan->ordinary.mode == -1 &&
              result.valid && result.classification == Classification::OrdinaryOnly &&
              result.transfer_count == 5 && result.inert_transfers == 3 &&
              result.ordinary_descriptors == 1 && result.direct_setup_transfers == 1 &&
              result.other_transfers == 0 && result.total_payload_bytes == 176,
          "each normal TFRAG texture bucket produces one owned ordinary upload plan");
  }

  auto packet = make_normal_ordinary_fixture(7);
  const auto owned = metal_renderer::plan_jak2_normal_tfrag_texture_upload(
      packet.data(), packet.size(), kChainOffset, 7, packet.data(), packet.size());
  std::fill_n(packet.begin() + kTexturePageOffset,
              metal_renderer::kJak2Bucket4OrdinaryPageHeaderBytes, 0xa5);
  check(owned.has_value() && owned->ordinary.page_header[0] == 0,
        "the normal TFRAG plan owns its validated page header");

  packet = make_ordinary_fixture(7);
  check(!metal_renderer::plan_jak2_normal_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, 7, packet.data(), packet.size())
             .has_value(),
        "a normal TFRAG descriptor without the exact Direct tail is rejected");

  packet = make_normal_ordinary_fixture(7, 0);
  check(!metal_renderer::plan_jak2_normal_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, 7, packet.data(), packet.size())
             .has_value(),
        "a normal TFRAG descriptor with an unobserved upload mode is rejected");

  packet = make_normal_ordinary_fixture(7);
  put_u32(&packet, kDirectSetupOffset + 8, 0);
  check(!metal_renderer::plan_jak2_normal_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, 7, packet.data(), packet.size())
             .has_value(),
        "a normal TFRAG packet with a non-FLUSHA tail is rejected");

  packet = make_unobserved_direct_first_fixture(7);
  check(!metal_renderer::plan_jak2_normal_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, 7, packet.data(), packet.size())
             .has_value(),
        "the unobserved Direct-before-descriptor order is rejected");
}

void test_normal_shrub_execution_plan() {
  for (const u32 bucket_id : metal_renderer::kJak2NormalShrubTextureUploadBuckets) {
    auto packet = make_normal_shrub_fixture(bucket_id);
    metal_renderer::Jak2CommonTfragTextureUploadCapture result;
    const auto plan = metal_renderer::plan_jak2_normal_shrub_texture_upload(
        packet.data(), packet.size(), kChainOffset, bucket_id, &result);
    check(plan.has_value() && plan->present && plan->bucket_id == bucket_id && result.valid &&
              result.classification == Classification::GsSetupOnly &&
              result.transfer_count == 5 && result.total_payload_bytes == 192 &&
              result.inert_transfers == 3 && result.gs_setup_transfers == 1 &&
              result.direct_setup_transfers == 1 && result.ordinary_descriptors == 0 &&
              result.other_transfers == 0,
          "each normal/common SHRUB texture bucket produces one exact Direct-only no-op plan");
  }

  auto packet = make_normal_shrub_fixture(73);
  put_u32(&packet, kOrdinaryOffset + 12, kDirectVif | 3);
  check(!metal_renderer::plan_jak2_normal_shrub_texture_upload(
             packet.data(), packet.size(), kChainOffset, 73)
             .has_value(),
        "a normal SHRUB setup with a non-source Direct length is rejected");

  packet = make_normal_ordinary_fixture(73);
  check(!metal_renderer::plan_jak2_normal_shrub_texture_upload(
             packet.data(), packet.size(), kChainOffset, 73)
             .has_value(),
        "an unobserved ordinary page descriptor is rejected for normal SHRUB setup");
}

void test_alpha_execution_plan() {
  for (const u32 bucket_id : metal_renderer::kJak2AlphaTextureUploadBuckets) {
    auto packet = make_normal_shrub_fixture(bucket_id);
    metal_renderer::Jak2CommonTfragTextureUploadCapture result;
    const auto plan = metal_renderer::plan_jak2_alpha_texture_upload(
        packet.data(), packet.size(), kChainOffset, bucket_id, &result);
    check(plan.has_value() && plan->present && plan->bucket_id == bucket_id && result.valid &&
              result.classification == Classification::GsSetupOnly &&
              result.transfer_count == 5 && result.total_payload_bytes == 192 &&
              result.inert_transfers == 3 && result.gs_setup_transfers == 1 &&
              result.direct_setup_transfers == 1 && result.ordinary_descriptors == 0 &&
              result.other_transfers == 0,
          "each alpha texture bucket produces the exact Direct-only inert plan");
  }

  auto packet = make_normal_ordinary_fixture(metal_renderer::kJak2AlphaTextureUploadBuckets[0]);
  check(!metal_renderer::plan_jak2_alpha_texture_upload(
             packet.data(), packet.size(), kChainOffset,
             metal_renderer::kJak2AlphaTextureUploadBuckets[0])
             .has_value(),
        "an unobserved ordinary page descriptor is rejected for alpha setup");
}

void test_water_execution_plan() {
  for (const u32 bucket_id : metal_renderer::kJak2WaterTextureUploadBuckets) {
    auto packet = make_empty_fixture(bucket_id);
    const auto absent = metal_renderer::plan_jak2_water_texture_upload(
        packet.data(), packet.size(), kChainOffset, bucket_id, packet.data(), packet.size());
    check(absent.has_value() && !absent->present &&
              absent->variant == metal_renderer::Jak2WaterTextureUploadVariant::Absent,
          "each water texture bucket preserves the exact absent plan");

    packet = make_water_ordinary_fixture(bucket_id);
    metal_renderer::Jak2CommonTfragTextureUploadCapture result;
    const auto plan = metal_renderer::plan_jak2_water_texture_upload(
        packet.data(), packet.size(), kChainOffset, bucket_id, packet.data(), packet.size(),
        &result);
    check(plan.has_value() && plan->present && plan->bucket_id == bucket_id &&
              plan->variant ==
                  metal_renderer::Jak2WaterTextureUploadVariant::DescriptorOnly &&
              plan->ordinary.page_offset == kTexturePageOffset && plan->ordinary.mode == -1 &&
              result.valid && result.classification == Classification::OrdinaryOnly &&
              result.transfer_count == 3 && result.inert_transfers == 2 &&
              result.ordinary_descriptors == 1 && result.direct_setup_transfers == 0 &&
              result.other_transfers == 0 && result.total_payload_bytes == 16,
          "each water texture bucket accepts the source-exact descriptor-only upload");
  }

  auto packet = make_normal_shrub_fixture(metal_renderer::kJak2WaterTextureUploadBuckets[0]);
  check(!metal_renderer::plan_jak2_water_texture_upload(
             packet.data(), packet.size(), kChainOffset,
             metal_renderer::kJak2WaterTextureUploadBuckets[0], packet.data(), packet.size())
             .has_value(),
        "the alpha-style Direct-only setup is rejected for water texture upload");

  for (const u32 bucket_id : metal_renderer::kJak2WaterTextureUploadBuckets) {
    packet = make_normal_ordinary_fixture(bucket_id);
    metal_renderer::Jak2CommonTfragTextureUploadCapture result;
    const auto plan = metal_renderer::plan_jak2_water_texture_upload(
        packet.data(), packet.size(), kChainOffset, bucket_id, packet.data(), packet.size(),
        &result);
    check(plan.has_value() && plan->present && !plan->has_security_animator &&
              plan->variant ==
                  metal_renderer::Jak2WaterTextureUploadVariant::DescriptorAndStandardReset &&
              plan->bucket_id == bucket_id &&
              plan->ordinary.page_offset == kTexturePageOffset &&
              result.valid && result.classification == Classification::OrdinaryOnly &&
              result.transfer_count == 5 && result.total_payload_bytes == 176 &&
              result.inert_transfers == 3 && result.ordinary_descriptors == 1 &&
              result.direct_setup_transfers == 1 && result.animator_arrays == 0 &&
              result.other_transfers == 0,
          "each water slot accepts the exact descriptor/standard-reset envelope");
  }

  constexpr u32 negative_bucket_id = metal_renderer::kJak2WaterTextureUploadBuckets[0];
  packet = make_unobserved_direct_first_fixture(negative_bucket_id);
  check(!metal_renderer::plan_jak2_water_texture_upload(
             packet.data(), packet.size(), kChainOffset, negative_bucket_id, packet.data(),
             packet.size())
             .has_value(),
        "a standard reset before the water descriptor is rejected");

  packet = make_normal_ordinary_fixture(negative_bucket_id);
  put_tag(&packet, kDirectSetupOffset, DmaTag::Kind::CNT, 9, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 9);
  check(!metal_renderer::plan_jak2_water_texture_upload(
             packet.data(), packet.size(), kChainOffset, negative_bucket_id, packet.data(),
             packet.size())
             .has_value(),
        "a water reset other than the exact qwc-10 Direct transfer is rejected");

  packet = make_normal_ordinary_fixture(negative_bucket_id);
  put_tag(&packet, kDirectSetupOffset + 176, DmaTag::Kind::NEXT, 0,
          kExtraTransferOffset, 0, 0);
  put_tag(&packet, kExtraTransferOffset, DmaTag::Kind::NEXT, 0,
          bucket_offset(negative_bucket_id) + 16, 0, 0);
  check(!metal_renderer::plan_jak2_water_texture_upload(
             packet.data(), packet.size(), kChainOffset, negative_bucket_id, packet.data(),
             packet.size())
             .has_value(),
        "an extra transfer after the water standard reset is rejected");

  for (const u32 bucket_id : metal_renderer::kJak2WaterTextureUploadBuckets) {
    packet = make_water_security_fixture(bucket_id);
    metal_renderer::Jak2CommonTfragTextureUploadCapture result;
    const auto plan = metal_renderer::plan_jak2_water_texture_upload(
        packet.data(), packet.size(), kChainOffset, bucket_id, packet.data(), packet.size(),
        &result);
    check(plan.has_value() && plan->present && plan->has_security_animator &&
              plan->variant == metal_renderer::Jak2WaterTextureUploadVariant::
                                   DescriptorSecurityAndStandardReset &&
              plan->bucket_id == bucket_id &&
              plan->ordinary.page_offset == kTexturePageOffset &&
              result.valid && result.classification == Classification::OrdinaryAndAnimator &&
              result.transfer_count == 9 && result.total_payload_bytes == 1008 &&
              result.inert_transfers == 4 && result.ordinary_descriptors == 1 &&
              result.direct_setup_transfers == 1 && result.animator_arrays == 1 &&
              result.animator_body_transfers == 1 && result.animator_payload_bytes == 832 &&
              result.opcode_counts[12] == 1 && result.opcode_counts[13] == 1 &&
              result.opcode_counts[30] == 1 && result.other_transfers == 0,
          "each water slot accepts the exact descriptor/security/reset startup envelope");
    check(plan->security.environment.time == 1200.f &&
              plan->security.environment.destination_tbp == 0x2345 &&
              plan->security.environment.layers[0].start.color[0] == 20.f &&
              plan->security.environment.layers[1].end.st_rot == 84.25f &&
              plan->security.dot.time == 300.f &&
              plan->security.dot.destination_tbp == 0x3456 &&
              plan->security.dot.layers[0].start.color[0] == 100.f &&
              plan->security.dot.layers[2].end.source_padding.back() == 0xd5,
          "the opcode-30 security body is copied into two typed owned animation plans");
  }
}

void test_water_security_shape_and_payload_fail_closed() {
  constexpr u32 bucket_id = metal_renderer::kJak2WaterTextureUploadBuckets[0];
  const auto rejected = [](const std::vector<u8>& packet) {
    return !metal_renderer::plan_jak2_water_texture_upload(
                packet.data(), packet.size(), kChainOffset,
                metal_renderer::kJak2WaterTextureUploadBuckets[0], packet.data(), packet.size())
                .has_value();
  };

  auto packet = make_water_security_fixture(bucket_id);
  put_u32(&packet, kAnimatorBodyTagOffset + 8, kPcPortVif | 29);
  check(rejected(packet), "a water animator opcode other than security 30 is rejected");

  packet = make_water_security_fixture(bucket_id);
  put_tag(&packet, kAnimatorBodyTagOffset, DmaTag::Kind::CNT, 51, 0, kPcPortVif | 30, 0);
  check(rejected(packet), "a security body other than qwc 52 is rejected");

  packet = make_water_security_fixture(bucket_id);
  put_tag(&packet, kSecurityAnimatorNextOffset, DmaTag::Kind::NEXT, 0,
          bucket_offset(bucket_id) + 16, 0, 0);
  check(rejected(packet), "a security water bucket without its terminal GS reset is rejected");

  packet = make_water_security_fixture(bucket_id);
  put_tag(&packet, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0, kAnimatorOffset, 0, 0);
  put_tag(&packet, kSecurityAnimatorNextOffset, DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0);
  put_tag(&packet, kOrdinaryOffset + 32, DmaTag::Kind::NEXT, 0, kDirectSetupOffset, 0, 0);
  check(rejected(packet), "an animator-before-descriptor water order is rejected");

  packet = make_water_security_fixture(bucket_id);
  put_float(&packet, kAnimatorBodyOffset + 336,
            std::numeric_limits<float>::quiet_NaN());
  check(rejected(packet), "a nonfinite security-dot time is rejected");

  packet = make_water_security_fixture(bucket_id);
  put_u32(&packet, kAnimatorBodyOffset + 4, 0x40000);
  check(rejected(packet), "an out-of-VRAM security destination is rejected");

  packet = make_water_security_fixture(bucket_id);
  put_float(&packet, kAnimatorBodyOffset + 336 + 16 + 48,
            std::numeric_limits<float>::infinity());
  check(rejected(packet), "a nonfinite security LayerVals scalar is rejected");

  packet = make_water_security_fixture(bucket_id);
  const auto owned = metal_renderer::plan_jak2_water_texture_upload(
      packet.data(), packet.size(), kChainOffset, bucket_id, packet.data(), packet.size());
  check(owned.has_value(), "the exact security fixture produces an owned plan");
  std::fill(packet.begin(), packet.end(), 0xa5);
  check(owned->ordinary.page_header[8] == 0x44 &&
            owned->security.environment.time == 1200.f &&
            owned->security.dot.destination_tbp == 0x3456 &&
            owned->security.dot.layers[2].end.source_padding.back() == 0xd5,
        "snapshot reuse cannot change any owned opcode-30 plan data");
}

void test_common_opcode27_execution_plan() {
  auto packet = make_common_execution_fixture();
  metal_renderer::Jak2CommonTfragTextureUploadCapture result;
  const auto plan = metal_renderer::plan_jak2_common_tfrag_texture_upload(
      packet.data(), packet.size(), kChainOffset, packet.data(), packet.size(), &result);
  check(plan.has_value() && plan->present && result.valid && result.present &&
            result.classification == Classification::OrdinaryAndAnimator &&
            result.transfer_count == 9 && result.total_payload_bytes == 672 &&
            result.inert_transfers == 4 && result.ordinary_descriptors == 1 &&
            result.animator_arrays == 1 && result.animator_body_transfers == 1 &&
            result.animator_payload_bytes == 496 && result.direct_setup_transfers == 1 &&
            result.eye_markers == 0 && result.other_transfers == 0 &&
            result.opcode_counts[12] == 1 && result.opcode_counts[13] == 1 &&
            result.opcode_counts[27] == 1,
        "the exact nine-transfer live bucket-187 envelope produces an owned plan");
  check(plan->ordinary.page_offset == kTexturePageOffset && plan->ordinary.mode == -1 &&
            plan->ordinary.page_header[8] == 0x44,
        "the common plan owns the validated live ordinary page header");

  const auto& skull_gem = plan->skull_gem;
  check(skull_gem.time == 42.5f && skull_gem.destination_tbp == 0x1234 &&
            skull_gem.source_header_tail.front() == 0xa0 &&
            skull_gem.source_header_tail.back() == 0xa7,
        "the opcode-27 header owns its finite time, destination, and unwritten source tail");
  check(skull_gem.layers[0].start.color[0] == 10.f &&
            skull_gem.layers[0].start.scale[0] == 11.f &&
            skull_gem.layers[0].start.offset[1] == 11.75f &&
            skull_gem.layers[0].start.st_scale[0] == 12.f &&
            skull_gem.layers[0].start.st_offset[1] == 12.75f &&
            skull_gem.layers[0].start.qs[3] == 13.75f &&
            skull_gem.layers[0].start.rot == 14.f &&
            skull_gem.layers[0].start.st_rot == 14.25f &&
            skull_gem.layers[0].start.source_padding.front() == 0xb0 &&
            skull_gem.layers[2].end.color[0] == 110.f &&
            skull_gem.layers[2].end.source_padding.back() == 0xb5,
        "all three start/end LayerVals pairs own typed floats and source-copied padding");
}

void test_empty_common_execution_plan() {
  auto packet = make_empty_fixture();
  metal_renderer::Jak2CommonTfragTextureUploadCapture result;
  const auto plan = metal_renderer::plan_jak2_common_tfrag_texture_upload(
      packet.data(), packet.size(), kChainOffset, packet.data(), packet.size(), &result);
  check(plan.has_value() && !plan->present && result.valid && !result.present &&
            result.classification == Classification::Absent,
        "an exact empty common bucket produces an explicit absent plan");
}

void test_common_opcode27_shape_variants_fail_closed() {
  auto packet = make_common_execution_fixture();
  put_tag(&packet, bucket_offset(), DmaTag::Kind::NEXT, 0, kAnimatorOffset, 0, 0);
  put_tag(&packet, kAnimatorNextOffset, DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0);
  put_tag(&packet, kOrdinaryOffset + 32, DmaTag::Kind::NEXT, 0, kDirectSetupOffset, 0, 0);
  check(!metal_renderer::plan_jak2_common_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
             .has_value(),
        "a reordered animator-before-page envelope is rejected");

  packet = make_common_execution_fixture();
  put_tag(&packet, kDirectSetupOffset + 176, DmaTag::Kind::NEXT, 0, kExtraTransferOffset, 0, 0);
  put_tag(&packet, kExtraTransferOffset, DmaTag::Kind::NEXT, 0, bucket_offset() + 16, 0, 0);
  check(!metal_renderer::plan_jak2_common_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
             .has_value(),
        "an extra inert transfer cannot enter the exact nine-transfer plan");

  packet = make_common_execution_fixture();
  put_u32(&packet, kAnimatorBodyTagOffset + 8, kPcPortVif | 28);
  check(!metal_renderer::plan_jak2_common_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
             .has_value(),
        "an animator opcode other than skull-gem 27 is rejected");

  packet = make_common_execution_fixture();
  put_tag(&packet, kAnimatorBodyTagOffset, DmaTag::Kind::CNT, 30, 0, kPcPortVif | 27, 0);
  check(!metal_renderer::plan_jak2_common_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
             .has_value(),
        "an opcode-27 body other than qwc 31 is rejected");

  packet = make_common_execution_fixture();
  put_u32(&packet, kAnimatorFinishOffset + 12, kPcPortVif);
  check(!capture(packet).valid &&
            !metal_renderer::plan_jak2_common_tfrag_texture_upload(
                 packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
                 .has_value(),
        "the alternate PC_PORT finish is rejected before opcode-27 planning");

  packet = make_common_execution_fixture();
  put_tag(&packet, kDirectSetupOffset, DmaTag::Kind::CNT, 9, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 9);
  check(!metal_renderer::plan_jak2_common_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
             .has_value(),
        "a terminal Direct reset other than qwc 10 is rejected");
}

void test_common_opcode27_raw_body_header_fails_closed() {
  const auto rejected = [](const std::vector<u8>& packet) {
    return !metal_renderer::plan_jak2_common_tfrag_texture_upload(
                packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
                .has_value();
  };

  auto packet = make_common_execution_fixture();
  put_u32(&packet, kAnimatorBodyTagOffset + 8, kPcPortVif | (1u << 16) | 27);
  check(rejected(packet), "opcode-27 VIF0 NUM bits are rejected");

  packet = make_common_execution_fixture();
  put_u32(&packet, kAnimatorBodyTagOffset + 8, kPcPortVif | (1u << 31) | 27);
  check(rejected(packet), "opcode-27 VIF0 IRQ is rejected");

  packet = make_common_execution_fixture();
  put_u32(&packet, kAnimatorBodyTagOffset + 12, 1u << 16);
  check(rejected(packet), "opcode-27 VIF1 NUM bits are rejected");

  packet = make_common_execution_fixture();
  put_u32(&packet, kAnimatorBodyTagOffset + 12, 1u << 31);
  check(rejected(packet), "opcode-27 VIF1 IRQ is rejected");

  packet = make_common_execution_fixture();
  put_u64(&packet, kAnimatorBodyTagOffset,
          31ull | (static_cast<u64>(DmaTag::Kind::CNT) << 28) | (1ull << 26));
  check(rejected(packet), "opcode-27 CNT control bits are rejected");
}

void test_common_opcode27_payload_validation() {
  auto packet = make_common_execution_fixture();
  put_float(&packet, kAnimatorBodyOffset, std::numeric_limits<float>::quiet_NaN());
  check(!metal_renderer::plan_jak2_common_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
             .has_value(),
        "a nonfinite skull-gem time is rejected");

  packet = make_common_execution_fixture();
  put_float(&packet, kAnimatorBodyOffset + 16 + 48,
            std::numeric_limits<float>::infinity());
  check(!metal_renderer::plan_jak2_common_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
             .has_value(),
        "a nonfinite LayerVals scalar is rejected");

  packet = make_common_execution_fixture();
  put_u32(&packet, kAnimatorBodyOffset + 4, 0x40000);
  check(!metal_renderer::plan_jak2_common_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
             .has_value(),
        "an opcode-27 destination outside PS2 VRAM is rejected");

  packet = make_common_execution_fixture();
  put_u64(&packet, kOrdinaryOffset + 16, packet.size());
  check(!metal_renderer::plan_jak2_common_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
             .has_value(),
        "an out-of-range ordinary page is rejected");

  packet = make_common_execution_fixture();
  put_u32(&packet, kTexturePageOffset + 12, std::numeric_limits<u32>::max());
  check(!metal_renderer::plan_jak2_common_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
             .has_value(),
        "an invalid live page header is rejected");

  packet = make_common_execution_fixture();
  put_u64(&packet, kOrdinaryOffset + 24, 0);
  check(!metal_renderer::plan_jak2_common_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
             .has_value(),
        "an ordinary page mode other than minus one is rejected");
}

void test_common_opcode27_plan_owns_reused_sources() {
  auto packet = make_common_execution_fixture();
  const auto plan = metal_renderer::plan_jak2_common_tfrag_texture_upload(
      packet.data(), packet.size(), kChainOffset, packet.data(), packet.size());
  check(plan.has_value(), "the exact common fixture produces a plan before source reuse");
  std::fill(packet.begin(), packet.end(), 0xa5);
  check(plan->ordinary.page_header[8] == 0x44 && plan->skull_gem.time == 42.5f &&
            plan->skull_gem.destination_tbp == 0x1234 &&
            plan->skull_gem.source_header_tail.front() == 0xa0 &&
            plan->skull_gem.layers[0].start.color[0] == 10.f &&
            plan->skull_gem.layers[2].end.st_rot == 114.25f &&
            plan->skull_gem.layers[2].end.source_padding.back() == 0xb5,
        "packet and live-page reuse cannot change any owned opcode-27 plan data");
}

void test_animator_and_combined_metadata() {
  auto result = capture(make_animator_fixture());
  check(result.valid && result.present &&
            result.classification == Classification::AnimatorOnly &&
            result.transfer_count == 5 && result.inert_transfers == 2 &&
            result.animator_arrays == 1 && result.animator_body_transfers == 1 &&
            result.animator_payload_bytes == 496 && result.total_payload_bytes == 496 &&
            result.opcode_counts[12] == 1 && result.opcode_counts[13] == 1 &&
            result.opcode_counts[27] == 1,
        "a bounded fixed animator array is classified from metadata only");

  result = capture(make_ordinary_and_animator_fixture());
  check(result.valid && result.present &&
            result.classification == Classification::OrdinaryAndAnimator &&
            result.transfer_count == 7 && result.inert_transfers == 3 &&
            result.ordinary_descriptors == 1 && result.animator_arrays == 1 &&
            result.animator_body_transfers == 1 && result.animator_payload_bytes == 496 &&
            result.total_payload_bytes == 512,
        "ordered descriptor and animator metadata remain a composite classification");

  result = capture(make_animator_fixture(kPcPortVif));
  check(!result.valid && result.malformed_transfers == 1,
        "an animator finish with a second PC_PORT VIF is rejected");

  std::vector<u8> qwc8_animator(kMemorySize);
  const u32 end_offset = bucket_offset() + 16;
  put_tag(&qwc8_animator, bucket_offset(), DmaTag::Kind::NEXT, 0, kAnimatorOffset, 0, 0);
  put_animator_array(&qwc8_animator, kAnimatorOffset, 27, 8, end_offset);
  result = capture(qwc8_animator);
  check(result.valid && result.classification == Classification::AnimatorOnly &&
            result.eye_markers == 0 && result.animator_payload_bytes == 128,
        "a qwc-8 transfer delegated inside an animator array is not mislabeled as eye DMA");
}

void test_payload_contents_are_never_part_of_classification() {
  auto first = make_ordinary_and_animator_fixture();
  auto second = first;
  std::fill_n(first.begin() + kOrdinaryOffset + 16, 16, 0x00);
  std::fill_n(second.begin() + kOrdinaryOffset + 16, 16, 0xff);
  std::fill_n(first.begin() + kAnimatorOffset + 32, 496, 0x11);
  std::fill_n(second.begin() + kAnimatorOffset + 32, 496, 0xee);
  check(metadata_matches(capture(first), capture(second)),
        "payload changes cannot influence the metadata-only classifier");
}

void test_capture_owns_metadata_after_snapshot_reuse() {
  auto packet = make_ordinary_and_animator_fixture();
  const auto result = capture(packet);
  const auto expected_first = result.transfers[0];
  const auto expected_animator = result.transfers[4];
  const auto expected_opcodes = result.opcode_counts;
  std::fill(packet.begin(), packet.end(), 0xa5);
  check(result.transfers[0].relative_tag_offset == expected_first.relative_tag_offset &&
            result.transfers[0].tag_kind == expected_first.tag_kind &&
            result.transfers[4].qwc == expected_animator.qwc &&
            result.transfers[4].vif0_immediate == expected_animator.vif0_immediate &&
            result.opcode_counts == expected_opcodes,
        "snapshot reuse cannot change owned transfer metadata");
}

void test_eye_and_other_work_are_not_promoted() {
  std::vector<u8> eye(kMemorySize);
  const u32 end_offset = bucket_offset() + 16;
  put_tag(&eye, bucket_offset(), DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0);
  put_tag(&eye, kOrdinaryOffset, DmaTag::Kind::CNT, 8, 0, 0, kDirectVif | 8);
  put_tag(&eye, kOrdinaryOffset + 144, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  auto result = capture(eye);
  check(result.valid && result.classification == Classification::EyeOrOther &&
            result.eye_markers == 1 && result.other_transfers == 0 &&
            result.total_payload_bytes == 128,
        "an outer qwc-8 eye marker is classified without inspecting its payload");

  std::vector<u8> other(kMemorySize);
  put_tag(&other, bucket_offset(), DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0);
  put_tag(&other, kOrdinaryOffset, DmaTag::Kind::CNT, 2, 0, 0, kDirectVif | 2);
  put_tag(&other, kOrdinaryOffset + 48, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  result = capture(other);
  check(result.valid && result.classification == Classification::EyeOrOther &&
            result.eye_markers == 0 && result.gs_setup_transfers == 1 &&
            result.other_transfers == 0,
        "an isolated GS setup remains visible and cannot look like the exact SHRUB envelope");

  std::vector<u8> alternate_inert(kMemorySize);
  put_tag(&alternate_inert, bucket_offset(), DmaTag::Kind::REF, 0, 0, 0, 0);
  result = capture(alternate_inert);
  check(result.valid && result.classification == Classification::EyeOrOther &&
            result.inert_transfers == 0 && result.other_transfers == 1,
        "a bounded but noncanonical inert tag is not classified as an exact empty bucket");
}

void test_payload_total_does_not_define_the_family() {
  auto packet = make_ordinary_and_animator_fixture();
  constexpr u32 kOtherOffset = 0x6000;
  const u32 animator_boundary = kAnimatorOffset + 16 + 16 + 31 * 16 + 16;
  const u32 end_offset = bucket_offset() + 16;
  put_tag(&packet, animator_boundary, DmaTag::Kind::NEXT, 0, kOtherOffset, 0, 0);
  put_tag(&packet, kOtherOffset, DmaTag::Kind::CNT, 10, 0, 0, kDirectVif | 10);
  put_tag(&packet, kOtherOffset + 176, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);

  const auto result = capture(packet);
  check(result.valid && result.total_payload_bytes == 672 &&
            result.classification == Classification::EyeOrOther &&
            result.ordinary_descriptors == 1 && result.animator_arrays == 1 &&
            result.other_transfers == 1,
        "a 672-byte total remains classified by transfer metadata rather than arithmetic");
}

void test_animator_structure_fails_closed() {
  auto packet = make_animator_fixture();
  put_tag(&packet, kAnimatorOffset, DmaTag::Kind::CNT, 1, 0, kPcPortVif | 12, 0);
  auto result = capture(packet);
  check(!result.valid && result.classification == Classification::Malformed &&
            result.malformed_transfers == 1,
        "a nonzero animator-start payload is malformed");

  packet = make_animator_fixture();
  const u32 opcode_offset = kAnimatorOffset + 16;
  put_u32(&packet, opcode_offset + 8, kPcPortVif | 12);
  result = capture(packet);
  check(!result.valid && result.malformed_transfers == 1,
        "a nested animator start is malformed");

  packet = make_animator_fixture();
  const u32 finish_offset = opcode_offset + 16 + 31 * 16;
  put_u32(&packet, finish_offset + 12, 1);
  result = capture(packet);
  check(!result.valid && result.malformed_transfers == 1,
        "a finish with unsupported VIF1 metadata is malformed");

  packet = make_animator_fixture();
  put_u32(&packet, finish_offset + 8, 0);
  result = capture(packet);
  check(!result.valid && result.malformed_transfers == 1,
        "an unterminated animator array is malformed");
}

void test_follower_range_alignment_cycle_and_kind_bounds() {
  const auto valid = make_empty_fixture();
  check(!metal_renderer::capture_jak2_common_tfrag_texture_upload(nullptr, valid.size(),
                                                                  kChainOffset)
             .valid,
        "a null snapshot is rejected");
  check(!metal_renderer::capture_jak2_common_tfrag_texture_upload(
             valid.data(), bucket_offset() + 15, kChainOffset)
             .valid,
        "a truncated bucket-table entry is rejected");
  check(!metal_renderer::capture_jak2_common_tfrag_texture_upload(
             valid.data(), valid.size(), kChainOffset + 1)
             .valid,
        "a misaligned chain base is rejected");

  auto packet = make_empty_fixture();
  put_tag(&packet, bucket_offset(), DmaTag::Kind::NEXT, 0,
          static_cast<u32>(packet.size() + 16), 0, 0);
  check(!capture(packet).valid, "an out-of-range NEXT target is rejected");

  packet = make_empty_fixture();
  put_tag(&packet, bucket_offset(), DmaTag::Kind::NEXT, 0, kOrdinaryOffset + 1, 0, 0);
  check(!capture(packet).valid, "a misaligned NEXT target is rejected");

  packet = make_empty_fixture();
  put_tag(&packet, bucket_offset(), DmaTag::Kind::REF, 1,
          static_cast<u32>(packet.size() - 8), 0, 0);
  check(!capture(packet).valid, "an out-of-range REF payload is rejected");

  packet = make_empty_fixture();
  put_tag(&packet, bucket_offset(), DmaTag::Kind::REFS, 1, kOrdinaryOffset + 1, 0, 0);
  check(!capture(packet).valid, "a misaligned REFS payload is rejected");

  packet = make_empty_fixture();
  put_tag(&packet, bucket_offset(), DmaTag::Kind::NEXT, 0, bucket_offset(), 0, 0);
  const auto cycle = capture(packet);
  check(!cycle.valid && cycle.transfer_count == 1 && cycle.malformed_transfers == 1,
        "a self-cycle is rejected before rereading its tag");

  packet = make_empty_fixture();
  constexpr u32 kBeforeBucketOffset = 0x800;
  put_tag(&packet, bucket_offset(), DmaTag::Kind::NEXT, 0, kBeforeBucketOffset, 0, 0);
  put_tag(&packet, kBeforeBucketOffset, DmaTag::Kind::CNT, 0, 0, 0, 0);
  const auto before_bucket = capture(packet);
  check(!before_bucket.valid && before_bucket.transfer_count == 1 &&
            before_bucket.malformed_transfers == 1,
        "a tag before the bucket-table entry fails checked relative-offset arithmetic");

  packet = make_empty_fixture();
  put_tag(&packet, bucket_offset(), DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0, true);
  check(!capture(packet).valid, "an SPR tag is rejected");

  packet = make_empty_fixture();
  put_tag(&packet, bucket_offset(), DmaTag::Kind::CALL, 0, kOrdinaryOffset, 0, 0);
  check(!capture(packet).valid, "an unsupported CALL is rejected rather than guessed");
}

void test_transfer_limit_is_enforced() {
  std::vector<u8> packet(kMemorySize);
  put_tag(&packet, bucket_offset(), DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0);
  for (std::size_t i = 0;
       i < metal_renderer::kJak2CommonTfragTextureUploadMaximumTransfers;
       ++i) {
    put_tag(&packet, kOrdinaryOffset + static_cast<u32>(i) * 16, DmaTag::Kind::CNT, 0, 0, 0, 0);
  }
  const auto result = capture(packet);
  check(!result.valid &&
            result.transfer_count ==
                metal_renderer::kJak2CommonTfragTextureUploadMaximumTransfers &&
            result.malformed_transfers == 1,
        "the fixed transfer bound fails closed without overrunning owned metadata");
}

void test_subtitle_capture_is_metadata_only() {
  constexpr u32 kSubtitleBucket = metal_renderer::kJak2SubtitleBucket;
  auto empty = make_empty_fixture(kSubtitleBucket);
  const auto empty_before = empty;
  auto result = capture(empty, kSubtitleBucket);
  check(empty == empty_before && result.valid && !result.present &&
            result.classification == Classification::Absent && result.transfer_count == 1 &&
            result.total_payload_bytes == 0,
        "bucket 322 accepts the exact empty form as immutable metadata only");

  auto mixed = make_ordinary_fixture(kSubtitleBucket);
  const u32 end_offset = bucket_offset(kSubtitleBucket) + 16;
  put_tag(&mixed, kOrdinaryOffset + 32, DmaTag::Kind::NEXT, 0, kAnimatorOffset, 0, 0);
  put_animator_array(&mixed, kAnimatorOffset, 22, 1, kDirectSetupOffset);
  put_tag(&mixed, kDirectSetupOffset, DmaTag::Kind::CNT, 10, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 10);
  std::fill_n(mixed.begin() + kDirectSetupOffset + 16, 160, 0x52);
  put_tag(&mixed, kDirectSetupOffset + 176, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  const auto mixed_before = mixed;
  result = capture(mixed, kSubtitleBucket);
  check(mixed == mixed_before && result.valid && result.present &&
            result.classification == Classification::OrdinaryAndAnimator &&
            result.ordinary_descriptors == 1 && result.animator_arrays == 1 &&
            result.animator_body_transfers == 1 && result.direct_setup_transfers == 1 &&
            result.opcode_counts[22] == 1 &&
            !metal_renderer::plan_jak2_normal_tfrag_texture_upload(
                 mixed.data(), mixed.size(), kChainOffset, kSubtitleBucket, mixed.data(),
                 mixed.size())
                 .has_value(),
        "bucket 322 records mixed animator/upload/Direct metadata without an executable plan");

  std::vector<u8> direct_only(kMemorySize);
  put_tag(&direct_only, bucket_offset(kSubtitleBucket), DmaTag::Kind::NEXT, 0,
          kDirectSetupOffset, 0, 0);
  put_tag(&direct_only, kDirectSetupOffset, DmaTag::Kind::CNT, 10, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 10);
  put_tag(&direct_only, kDirectSetupOffset + 176, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  const auto direct_before = direct_only;
  result = capture(direct_only, kSubtitleBucket);
  check(direct_only == direct_before && result.valid && result.present &&
            result.classification == Classification::EyeOrOther &&
            result.direct_setup_transfers == 1,
        "bucket-322 Direct-like metadata stays unclassified and immutable");

  auto malformed = make_empty_fixture(kSubtitleBucket);
  put_tag(&malformed, bucket_offset(kSubtitleBucket), DmaTag::Kind::CALL, 0,
          kAnimatorOffset, 0, 0);
  const auto malformed_before = malformed;
  result = capture(malformed, kSubtitleBucket);
  check(malformed == malformed_before && !result.valid && result.malformed_transfers == 1,
        "bucket-322 malformed metadata is retained without mutation or grammar inference");
}

}  // namespace

int main() {
  test_exact_empty_and_ordinary_metadata();
  test_texture_bucket_allowlist();
  test_pris2_diagnostic_capture();
  test_common_water_execution_plan();
  test_pris_eye_execution_plan();
  test_pris_eye_live_copy_semantics();
  test_pris_prison_jak_animator_variants();
  test_pris_ordinary_live_copy_semantics();
  test_pris_eye_shape_fails_closed();
  test_normal_tfrag_execution_plan();
  test_common_pris_execution_plan();
  test_common_pris_opcode22_capture();
  test_normal_shrub_execution_plan();
  test_alpha_execution_plan();
  test_water_execution_plan();
  test_water_security_shape_and_payload_fail_closed();
  test_common_opcode27_execution_plan();
  test_empty_common_execution_plan();
  test_common_opcode27_shape_variants_fail_closed();
  test_common_opcode27_raw_body_header_fails_closed();
  test_common_opcode27_payload_validation();
  test_common_opcode27_plan_owns_reused_sources();
  test_animator_and_combined_metadata();
  test_payload_contents_are_never_part_of_classification();
  test_capture_owns_metadata_after_snapshot_reuse();
  test_eye_and_other_work_are_not_promoted();
  test_payload_total_does_not_define_the_family();
  test_animator_structure_fails_closed();
  test_follower_range_alignment_cycle_and_kind_bounds();
  test_transfer_limit_is_enforced();
  test_subtitle_capture_is_metadata_only();
  std::puts("PASS: Jak II foreground texture-upload metadata capture");
  return 0;
}
