#pragma once

#include <cstring>
#include <initializer_list>
#include <vector>

#include "common/dma/dma.h"
#include "common/dma/gs.h"

#include "game/graphics/pipelines/metal/metal_jak2_raw_image_upload_plan.h"

namespace metal_renderer {

struct Jak2RawImageUploadFixture {
  std::vector<u8> ee_memory;
  u32 chain_offset = 0;
  u32 bucket_offset = 0;
  u32 black_sprite_tag_offset = 0;
  u32 start_tag_offset = 0;
  u32 upload_tag_offset = 0;
  u32 upload_data_offset = 0;
  u32 finish_tag_offset = 0;
  u32 state_tag_offset = 0;
  u32 sprite_tag_offset = 0;
  u32 final_boundary_offset = 0;
  u32 source_offset = 0;
};

enum class Jak2RawImageFixtureLayout {
  DirectOnly,
  DirectBeforeUpload,
  UploadBeforeDirect,
  MixedOverlay,
};

namespace jak2_raw_image_fixture_detail {

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
  const u64 tag = static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                  (static_cast<u64>(address) << 32);
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

inline u64 gif_tag(u32 nloop,
                   bool eop,
                   bool pre,
                   GsPrim prim,
                   GifTag::Format format,
                   u32 nreg) {
  return nloop | (static_cast<u64>(eop) << 15) | (static_cast<u64>(pre) << 46) |
         (prim.data << 47) | (static_cast<u64>(format) << 58) |
         (static_cast<u64>(nreg) << 60);
}

inline u64 xyzf(u16 x, u16 y, u32 z) {
  return static_cast<u64>(x) | (static_cast<u64>(y) << 16) |
         (static_cast<u64>(z) << 32);
}

inline void put_packed_rgba(std::vector<u8>& memory,
                            u32 offset,
                            u8 r,
                            u8 g,
                            u8 b,
                            u8 a) {
  put_u32(memory, offset, r);
  put_u32(memory, offset + 4, g);
  put_u32(memory, offset + 8, b);
  put_u32(memory, offset + 12, a);
}

inline void put_packed_xy(std::vector<u8>& memory,
                          u32 offset,
                          u32 x,
                          u32 y,
                          u32 z = 0) {
  put_u32(memory, offset, x);
  put_u32(memory, offset + 4, y);
  put_u32(memory, offset + 8, z);
  put_u32(memory, offset + 12, 0);
}

}  // namespace jak2_raw_image_fixture_detail

/*!
 * Public original-data-free fixtures assembled from tracked DEBUG_NO_ZBUF1
 * producers and the draw-raw-image grammar. Image-bearing layouts use a
 * synthetic opaque-red field.
 */
inline Jak2RawImageUploadFixture make_jak2_raw_image_fixture(
    Jak2RawImageFixtureLayout layout,
    u32 memory_base = 0) {
  using namespace jak2_raw_image_fixture_detail;
  Jak2RawImageUploadFixture out;
  constexpr u32 kChainOffset = 0x100;
  constexpr u32 kPayloadOffset = 0x3000;
  constexpr u32 kSourceOffset = 0x10000;
  constexpr std::size_t kMemorySize = 0xf0000;
  constexpr u32 kBucketCount = 327;
  constexpr u32 kOpaqueRed = 0xff0000ffu;
  out.ee_memory.resize(static_cast<std::size_t>(memory_base) + kMemorySize);
  out.chain_offset = memory_base + kChainOffset;
  out.bucket_offset = out.chain_offset + kJak2RawImageUploadBucket * 16;
  out.source_offset = memory_base + kSourceOffset;

  for (u32 bucket = 0; bucket < kBucketCount; ++bucket) {
    put_tag(out.ee_memory, out.chain_offset + bucket * 16, DmaTag::Kind::CNT);
  }
  put_tag(out.ee_memory, out.chain_offset + kBucketCount * 16, DmaTag::Kind::END);
  put_tag(out.ee_memory, out.bucket_offset, DmaTag::Kind::NEXT, 0,
          memory_base + kPayloadOffset);

  u32 cursor = memory_base + kPayloadOffset;
  const auto emit_black_sprite = [&]() {
    out.black_sprite_tag_offset = cursor;
    put_tag(out.ee_memory, cursor, DmaTag::Kind::CNT, 3, 0, 0,
            vif(VifCode::Kind::DIRECT, 3));
    const u32 black = cursor + 16;
    put_u64(out.ee_memory, black,
            gif_tag(1, true, false, GsPrim{}, GifTag::Format::REGLIST, 4));
    put_u64(out.ee_memory, black + 8,
            registers({GifTag::RegisterDescriptor::PRIM,
                       GifTag::RegisterDescriptor::RGBAQ,
                       GifTag::RegisterDescriptor::XYZF2,
                       GifTag::RegisterDescriptor::XYZF2}));
    put_u64(out.ee_memory, black + 16,
            static_cast<u64>(GsPrim::Kind::SPRITE) | (1ull << 6));
    put_u64(out.ee_memory, black + 24, 0x80000000ull);
    put_u64(out.ee_memory, black + 32, xyzf(0x7000, 0x7300, 0x3fffff));
    put_u64(out.ee_memory, black + 40, xyzf(0x9000, 0x8d00, 0x3fffff));
    cursor += 64;
  };

  const auto emit_raw_upload = [&]() {
    out.start_tag_offset = cursor;
    put_tag(out.ee_memory, cursor, DmaTag::Kind::CNT, 0, 0,
            vif(VifCode::Kind::PC_PORT, 12), 0);
    out.upload_tag_offset = cursor + 16;
    put_tag(out.ee_memory, out.upload_tag_offset, DmaTag::Kind::CNT, 1, 0,
            vif(VifCode::Kind::PC_PORT, 16), 0);
    out.upload_data_offset = out.upload_tag_offset + 16;
    put_u32(out.ee_memory, out.upload_data_offset, out.source_offset);
    const u16 dimensions[2] = {kJak2RawImageWidth, kJak2RawImageHeight};
    std::memcpy(out.ee_memory.data() + out.upload_data_offset + 4, dimensions,
                sizeof(dimensions));
    put_u32(out.ee_memory, out.upload_data_offset + 8, kJak2RawImageDestination);
    out.ee_memory[out.upload_data_offset + 12] = kJak2RawImagePsmct32;
    out.ee_memory[out.upload_data_offset + 13] = 1;
    out.finish_tag_offset = cursor + 48;
    put_tag(out.ee_memory, out.finish_tag_offset, DmaTag::Kind::CNT, 0, 0,
            vif(VifCode::Kind::PC_PORT, 13), 0);
    cursor += 64;
  };

  const auto emit_textured_sprite = [&]() {
    out.state_tag_offset = cursor;
    put_tag(out.ee_memory, cursor, DmaTag::Kind::CNT, 7, 0, 0,
            vif(VifCode::Kind::DIRECT, 7));
    const u32 state = cursor + 16;
    put_u64(out.ee_memory, state,
            gif_tag(1, true, false, GsPrim{}, GifTag::Format::PACKED, 6));
    put_u64(out.ee_memory, state + 8,
            registers({GifTag::RegisterDescriptor::AD, GifTag::RegisterDescriptor::AD,
                       GifTag::RegisterDescriptor::AD, GifTag::RegisterDescriptor::AD,
                       GifTag::RegisterDescriptor::AD, GifTag::RegisterDescriptor::AD}));
    constexpr u64 kTest =
        1ull | (1ull << 1) | (3ull << 12) | (1ull << 16) | (1ull << 17);
    constexpr u64 kTex0 =
        (8ull << 14) | (9ull << 26) | (9ull << 30) | (1ull << 34);
    constexpr u64 kTex1 = (1ull << 5) | (1ull << 6);
    constexpr u64 kValues[6] = {kTest, 0, kTex0, kTex1, 0b101, 0};
    constexpr GsRegisterAddress kAddresses[6] = {
        GsRegisterAddress::TEST_1, GsRegisterAddress::ALPHA_1,
        GsRegisterAddress::TEX0_1, GsRegisterAddress::TEX1_1,
        GsRegisterAddress::CLAMP_1, GsRegisterAddress::TEXFLUSH,
    };
    for (u32 i = 0; i < 6; ++i) {
      put_u64(out.ee_memory, state + 16 + i * 16, kValues[i]);
      put_u64(out.ee_memory, state + 24 + i * 16,
              static_cast<u64>(kAddresses[i]));
    }

    out.sprite_tag_offset = cursor + 128;
    put_tag(out.ee_memory, out.sprite_tag_offset, DmaTag::Kind::CNT, 6, 0, 0,
            vif(VifCode::Kind::DIRECT, 6));
    const u32 sprite = out.sprite_tag_offset + 16;
    const GsPrim sprite_prim(static_cast<u64>(GsPrim::Kind::SPRITE) | (1ull << 4) |
                             (1ull << 8));
    put_u64(out.ee_memory, sprite,
            gif_tag(1, true, true, sprite_prim, GifTag::Format::PACKED, 5));
    put_u64(out.ee_memory, sprite + 8,
            registers({GifTag::RegisterDescriptor::RGBAQ,
                       GifTag::RegisterDescriptor::UV,
                       GifTag::RegisterDescriptor::XYZ2,
                       GifTag::RegisterDescriptor::UV,
                       GifTag::RegisterDescriptor::XYZ2}));
    put_packed_rgba(out.ee_memory, sprite + 16, 0x80, 0x80, 0x80, 0x80);
    put_packed_xy(out.ee_memory, sprite + 32, 0, 0);
    put_packed_xy(out.ee_memory, sprite + 48, 0x7000, 0x7300);
    put_packed_xy(out.ee_memory, sprite + 64, kJak2RawImageWidth * 16,
                  kJak2RawImageHeight * 16);
    put_packed_xy(out.ee_memory, sprite + 80, 0x9000, 0x8d00);
    cursor += 240;
  };

  if (layout == Jak2RawImageFixtureLayout::DirectOnly ||
      layout == Jak2RawImageFixtureLayout::DirectBeforeUpload ||
      layout == Jak2RawImageFixtureLayout::MixedOverlay) {
    emit_black_sprite();
  }
  if (layout != Jak2RawImageFixtureLayout::DirectOnly) {
    emit_raw_upload();
  }
  if (layout == Jak2RawImageFixtureLayout::UploadBeforeDirect ||
      layout == Jak2RawImageFixtureLayout::MixedOverlay) {
    emit_textured_sprite();
  }

  out.final_boundary_offset = cursor;
  put_tag(out.ee_memory, out.final_boundary_offset, DmaTag::Kind::NEXT, 0,
          out.bucket_offset + 16);

  for (std::size_t i = 0;
       i < static_cast<std::size_t>(kJak2RawImageWidth) * kJak2RawImageHeight; ++i) {
    put_u32(out.ee_memory, out.source_offset + static_cast<u32>(i * sizeof(u32)),
            kOpaqueRed);
  }
  return out;
}

inline Jak2RawImageUploadFixture make_jak2_raw_image_upload_fixture(u32 memory_base = 0) {
  return make_jak2_raw_image_fixture(Jak2RawImageFixtureLayout::MixedOverlay, memory_base);
}

inline Jak2RawImageUploadFixture make_jak2_raw_image_direct_only_fixture(u32 memory_base = 0) {
  return make_jak2_raw_image_fixture(Jak2RawImageFixtureLayout::DirectOnly, memory_base);
}

inline Jak2RawImageUploadFixture make_jak2_raw_image_direct_before_upload_fixture(
    u32 memory_base = 0) {
  return make_jak2_raw_image_fixture(Jak2RawImageFixtureLayout::DirectBeforeUpload,
                                     memory_base);
}

inline Jak2RawImageUploadFixture make_jak2_raw_image_upload_before_direct_fixture(
    u32 memory_base = 0) {
  return make_jak2_raw_image_fixture(Jak2RawImageFixtureLayout::UploadBeforeDirect,
                                     memory_base);
}

inline Jak2RawImageUploadFixture make_jak2_raw_image_mixed_overlay_fixture(
    u32 memory_base = 0) {
  return make_jak2_raw_image_fixture(Jak2RawImageFixtureLayout::MixedOverlay, memory_base);
}

}  // namespace metal_renderer
