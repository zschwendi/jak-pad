#pragma once

#include <cstring>
#include <initializer_list>
#include <vector>

#include "common/dma/dma.h"
#include "common/dma/gs.h"

#include "game/graphics/pipelines/metal/metal_jak2_subtitle_bucket322_plan.h"

namespace metal_renderer {

enum class Jak2SubtitleBucket322FixtureLayout {
  OpaqueDirect,
  IntroHudSprite,
  IntroTwoHudSprites,
  SubtitleImage,
  Mixed,
};

struct Jak2SubtitleBucket322Fixture {
  std::vector<u8> ee_memory;
  u32 chain_offset = 0;
  u32 bucket_offset = 0;
  u32 payload_offset = 0;
  u32 final_boundary_offset = 0;
  u32 clut_source_offset = 0;
  u32 image_source_offset = 0;
  u32 first_upload_data_offset = 0;
  u32 second_upload_data_offset = 0;
  u32 image_setup_tag_offset = 0;
};

namespace jak2_subtitle_bucket322_fixture_detail {

inline u32 vif(VifCode::Kind kind, u16 immediate = 0) {
  return (static_cast<u32>(kind) << 24) | immediate;
}

inline void put_u32(std::vector<u8>& memory, u32 offset, u32 value) {
  std::memcpy(memory.data() + offset, &value, sizeof(value));
}

inline void put_u64(std::vector<u8>& memory, u32 offset, u64 value) {
  std::memcpy(memory.data() + offset, &value, sizeof(value));
}

inline void put_tag(std::vector<u8>& memory,
                    u32 offset,
                    DmaTag::Kind kind,
                    u16 qwc = 0,
                    u32 address = 0,
                    u32 vif0 = 0,
                    u32 vif1 = 0) {
  const u64 tag =
      static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) | (static_cast<u64>(address) << 32);
  put_u64(memory, offset, tag);
  put_u32(memory, offset + 8, vif0);
  put_u32(memory, offset + 12, vif1);
}

inline u64 registers(std::initializer_list<GifTag::RegisterDescriptor> values) {
  u64 result = 0;
  u32 index = 0;
  for (const auto value : values) {
    result |= static_cast<u64>(value) << (index++ * 4);
  }
  return result;
}

inline u64 gif_tag(u32 nloop, bool eop, bool pre, GsPrim prim, GifTag::Format format, u32 nreg) {
  return nloop | (static_cast<u64>(eop) << 15) | (static_cast<u64>(pre) << 46) | (prim.data << 47) |
         (static_cast<u64>(format) << 58) | (static_cast<u64>(nreg) << 60);
}

inline void put_packed_rgba(std::vector<u8>& memory,
                            u32 offset,
                            u8 red,
                            u8 green,
                            u8 blue,
                            u8 alpha) {
  put_u32(memory, offset, red);
  put_u32(memory, offset + 4, green);
  put_u32(memory, offset + 8, blue);
  put_u32(memory, offset + 12, alpha);
}

inline void put_packed_xy(std::vector<u8>& memory, u32 offset, u32 x, u32 y, u32 z = 0xffffff) {
  put_u32(memory, offset, x);
  put_u32(memory, offset + 4, y);
  put_u32(memory, offset + 8, z);
  put_u32(memory, offset + 12, 0);
}

inline u32 ceil_log2(u16 value) {
  u32 result = 0;
  u32 remaining = static_cast<u32>(value) - 1;
  while (remaining != 0) {
    result++;
    remaining >>= 1;
  }
  return result;
}

inline u64 make_tex0(u16 width, u16 height) {
  const u32 log2 = ceil_log2(height) & 0xf;
  return 1ull | ((static_cast<u64>(width) >> 6) & 0x3f) << 14 |
         (static_cast<u64>(GsTex0::PSM::PSMT4) << 20) | (static_cast<u64>(log2) << 26) |
         (static_cast<u64>(log2) << 30) | (1ull << 34) | (1ull << 61);
}

inline void emit_opaque_direct(std::vector<u8>& memory, u32* cursor) {
  put_tag(memory, *cursor, DmaTag::Kind::CNT, 3, 0, 0, vif(VifCode::Kind::DIRECT, 3));
  const u32 payload = *cursor + 16;
  put_u64(memory, payload, gif_tag(1, true, false, GsPrim{}, GifTag::Format::REGLIST, 4));
  put_u64(memory, payload + 8,
          registers({GifTag::RegisterDescriptor::PRIM, GifTag::RegisterDescriptor::RGBAQ,
                     GifTag::RegisterDescriptor::XYZF2, GifTag::RegisterDescriptor::XYZF2}));
  *cursor += 64;
}

inline void emit_hud_sprite(std::vector<u8>& memory, u32* cursor) {
  put_tag(memory, *cursor, DmaTag::Kind::CNT, 6, 0, 0, vif(VifCode::Kind::DIRECT, 6));
  const u32 shader = *cursor + 16;
  put_u64(memory, shader, gif_tag(5, true, false, GsPrim{}, GifTag::Format::PACKED, 1));
  put_u64(memory, shader + 8, registers({GifTag::RegisterDescriptor::AD}));
  constexpr GsRegisterAddress kAddresses[5] = {
      GsRegisterAddress::TEX0_1, GsRegisterAddress::TEX1_1, GsRegisterAddress::MIPTBP1_1,
      GsRegisterAddress::CLAMP_1, GsRegisterAddress::ALPHA_1};
  for (u32 i = 0; i < 5; ++i) {
    put_u64(memory, shader + 16 + i * 16, 0x100 + i);
    put_u64(memory, shader + 24 + i * 16, static_cast<u64>(kAddresses[i]));
  }
  *cursor += 112;

  put_tag(memory, *cursor, DmaTag::Kind::CNT, 13, 0, 0, vif(VifCode::Kind::DIRECT, 13));
  const u32 draw = *cursor + 16;
  const GsPrim prim(static_cast<u64>(GsPrim::Kind::TRI_STRIP) | (1ull << 4) | (1ull << 6));
  put_u64(memory, draw, gif_tag(1, true, true, prim, GifTag::Format::PACKED, 12));
  put_u64(memory, draw + 8,
          registers({GifTag::RegisterDescriptor::RGBAQ, GifTag::RegisterDescriptor::ST,
                     GifTag::RegisterDescriptor::XYZ2, GifTag::RegisterDescriptor::RGBAQ,
                     GifTag::RegisterDescriptor::ST, GifTag::RegisterDescriptor::XYZ2,
                     GifTag::RegisterDescriptor::RGBAQ, GifTag::RegisterDescriptor::ST,
                     GifTag::RegisterDescriptor::XYZ2, GifTag::RegisterDescriptor::RGBAQ,
                     GifTag::RegisterDescriptor::ST, GifTag::RegisterDescriptor::XYZ2}));
  for (u32 i = 0; i < 12; ++i) {
    put_u64(memory, draw + 16 + i * 16, 0x200 + i);
  }
  *cursor += 224;
}

inline void emit_image_sprite(std::vector<u8>& memory,
                              u32* cursor,
                              u16 width,
                              u16 height,
                              bool foreground) {
  put_tag(memory, *cursor, DmaTag::Kind::CNT, 6, 0, 0, vif(VifCode::Kind::DIRECT, 6));
  const u32 sprite = *cursor + 16;
  const GsPrim prim(static_cast<u64>(GsPrim::Kind::SPRITE) | (1ull << 4) | (1ull << 6) |
                    (1ull << 8));
  put_u64(memory, sprite, gif_tag(1, true, true, prim, GifTag::Format::PACKED, 5));
  put_u64(memory, sprite + 8,
          registers({GifTag::RegisterDescriptor::RGBAQ, GifTag::RegisterDescriptor::UV,
                     GifTag::RegisterDescriptor::XYZ2, GifTag::RegisterDescriptor::UV,
                     GifTag::RegisterDescriptor::XYZ2}));
  const u8 color = foreground ? 128 : 0;
  put_packed_rgba(memory, sprite + 16, color, color, color, 128);
  put_packed_xy(memory, sprite + 32, 0, 0, 0);
  put_packed_xy(memory, sprite + 48, foreground ? 0x7100 : 0x7000, foreground ? 0x7400 : 0x7300, 0);
  put_packed_xy(memory, sprite + 64, static_cast<u32>(width) * 16, static_cast<u32>(height) * 16,
                0);
  put_packed_xy(memory, sprite + 80, foreground ? 0x9100 : 0x9000, foreground ? 0x8e00 : 0x8d00, 0);
  *cursor += 112;
}

inline void emit_subtitle_image(Jak2SubtitleBucket322Fixture* fixture, u32* cursor) {
  constexpr u16 kWidth = 128;
  constexpr u16 kHeight = 64;
  auto& memory = fixture->ee_memory;
  put_tag(memory, *cursor, DmaTag::Kind::CNT, 0, 0, vif(VifCode::Kind::PC_PORT, 12), 0);
  *cursor += 16;

  put_tag(memory, *cursor, DmaTag::Kind::CNT, 1, 0, vif(VifCode::Kind::PC_PORT, 16), 0);
  fixture->first_upload_data_offset = *cursor + 16;
  put_u32(memory, fixture->first_upload_data_offset, fixture->clut_source_offset);
  const u16 clut_dimensions[2] = {2, 8};
  std::memcpy(memory.data() + fixture->first_upload_data_offset + 4, clut_dimensions,
              sizeof(clut_dimensions));
  put_u32(memory, fixture->first_upload_data_offset + 8, 0);
  memory[fixture->first_upload_data_offset + 12] = static_cast<u8>(GsTex0::PSM::PSMCT32);
  memory[fixture->first_upload_data_offset + 13] = 0;
  *cursor += 32;

  put_tag(memory, *cursor, DmaTag::Kind::CNT, 1, 0, vif(VifCode::Kind::PC_PORT, 16), 0);
  fixture->second_upload_data_offset = *cursor + 16;
  put_u32(memory, fixture->second_upload_data_offset, fixture->image_source_offset);
  const u16 image_dimensions[2] = {kWidth, kHeight};
  std::memcpy(memory.data() + fixture->second_upload_data_offset + 4, image_dimensions,
              sizeof(image_dimensions));
  put_u32(memory, fixture->second_upload_data_offset + 8, 1);
  memory[fixture->second_upload_data_offset + 12] = static_cast<u8>(GsTex0::PSM::PSMT4);
  memory[fixture->second_upload_data_offset + 13] = 1;
  *cursor += 32;

  put_tag(memory, *cursor, DmaTag::Kind::CNT, 0, 0, vif(VifCode::Kind::PC_PORT, 13), 0);
  *cursor += 16;

  fixture->image_setup_tag_offset = *cursor;
  put_tag(memory, *cursor, DmaTag::Kind::CNT, 7, 0, 0, vif(VifCode::Kind::DIRECT, 7));
  const u32 setup = *cursor + 16;
  put_u64(memory, setup, gif_tag(1, true, false, GsPrim{}, GifTag::Format::PACKED, 6));
  put_u64(memory, setup + 8,
          registers({GifTag::RegisterDescriptor::AD, GifTag::RegisterDescriptor::AD,
                     GifTag::RegisterDescriptor::AD, GifTag::RegisterDescriptor::AD,
                     GifTag::RegisterDescriptor::AD, GifTag::RegisterDescriptor::AD}));
  constexpr u64 kTest = 1ull | (1ull << 1) | (3ull << 12) | (1ull << 16) | (1ull << 17);
  const u64 values[6] = {kTest, 0x44, make_tex0(kWidth, kHeight), 0x60, 5, 0};
  constexpr GsRegisterAddress kAddresses[6] = {
      GsRegisterAddress::TEST_1, GsRegisterAddress::ALPHA_1, GsRegisterAddress::TEX0_1,
      GsRegisterAddress::TEX1_1, GsRegisterAddress::CLAMP_1, GsRegisterAddress::TEXFLUSH};
  for (u32 i = 0; i < 6; ++i) {
    put_u64(memory, setup + 16 + i * 16, values[i]);
    put_u64(memory, setup + 24 + i * 16, static_cast<u64>(kAddresses[i]));
  }
  *cursor += 128;
  emit_image_sprite(memory, cursor, kWidth, kHeight, false);
  emit_image_sprite(memory, cursor, kWidth, kHeight, true);
}

}  // namespace jak2_subtitle_bucket322_fixture_detail

inline Jak2SubtitleBucket322Fixture make_jak2_subtitle_bucket322_fixture(
    Jak2SubtitleBucket322FixtureLayout layout) {
  using namespace jak2_subtitle_bucket322_fixture_detail;
  Jak2SubtitleBucket322Fixture fixture;
  constexpr u32 kChainOffset = 0x100;
  constexpr u32 kPayloadOffset = 0x3000;
  constexpr u32 kSecondPayloadOffset = 0x5000;
  constexpr u32 kClutSourceOffset = 0x20000;
  constexpr u32 kImageSourceOffset = 0x21000;
  constexpr std::size_t kMemorySize = 0x30000;
  constexpr u32 kBucketCount = 327;
  fixture.ee_memory.resize(kMemorySize);
  fixture.chain_offset = kChainOffset;
  fixture.bucket_offset = kChainOffset + kJak2SubtitleBucket322 * 16;
  fixture.payload_offset = kPayloadOffset;
  fixture.clut_source_offset = kClutSourceOffset;
  fixture.image_source_offset = kImageSourceOffset;

  for (u32 bucket = 0; bucket < kBucketCount; ++bucket) {
    put_tag(fixture.ee_memory, kChainOffset + bucket * 16, DmaTag::Kind::CNT);
  }
  put_tag(fixture.ee_memory, kChainOffset + kBucketCount * 16, DmaTag::Kind::END);
  put_tag(fixture.ee_memory, fixture.bucket_offset, DmaTag::Kind::NEXT, 0, kPayloadOffset);

  u32 cursor = kPayloadOffset;
  switch (layout) {
    case Jak2SubtitleBucket322FixtureLayout::OpaqueDirect:
      emit_opaque_direct(fixture.ee_memory, &cursor);
      break;
    case Jak2SubtitleBucket322FixtureLayout::IntroHudSprite:
      emit_hud_sprite(fixture.ee_memory, &cursor);
      break;
    case Jak2SubtitleBucket322FixtureLayout::IntroTwoHudSprites:
      emit_hud_sprite(fixture.ee_memory, &cursor);
      emit_hud_sprite(fixture.ee_memory, &cursor);
      break;
    case Jak2SubtitleBucket322FixtureLayout::SubtitleImage:
      emit_subtitle_image(&fixture, &cursor);
      break;
    case Jak2SubtitleBucket322FixtureLayout::Mixed:
      emit_opaque_direct(fixture.ee_memory, &cursor);
      put_tag(fixture.ee_memory, cursor, DmaTag::Kind::NEXT, 0, kSecondPayloadOffset);
      cursor = kSecondPayloadOffset;
      emit_subtitle_image(&fixture, &cursor);
      emit_hud_sprite(fixture.ee_memory, &cursor);
      break;
  }
  fixture.final_boundary_offset = cursor;
  put_tag(fixture.ee_memory, cursor, DmaTag::Kind::NEXT, 0, fixture.bucket_offset + 16);
  return fixture;
}

inline Jak2SubtitleBucket322Fixture make_jak2_subtitle_bucket322_strict_empty_fixture() {
  using namespace jak2_subtitle_bucket322_fixture_detail;
  Jak2SubtitleBucket322Fixture fixture;
  constexpr u32 kChainOffset = 0x100;
  constexpr std::size_t kMemorySize = 0x30000;
  constexpr u32 kBucketCount = 327;
  fixture.ee_memory.resize(kMemorySize);
  fixture.chain_offset = kChainOffset;
  fixture.bucket_offset = kChainOffset + kJak2SubtitleBucket322 * 16;
  for (u32 bucket = 0; bucket < kBucketCount; ++bucket) {
    put_tag(fixture.ee_memory, kChainOffset + bucket * 16, DmaTag::Kind::CNT);
  }
  put_tag(fixture.ee_memory, kChainOffset + kBucketCount * 16, DmaTag::Kind::END);
  return fixture;
}

}  // namespace metal_renderer
