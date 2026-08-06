#pragma once

#include <cstring>
#include <vector>

#include "common/dma/dma.h"
#include "common/dma/gs.h"

namespace metal_renderer {

struct Jak2Bucket4TextureUploadFixture {
  std::vector<u8> ee_memory;
  u32 chain_offset = 0;
  u32 outer_direct_tag_offset = 0;
  u32 ordinary_descriptor_data_offset = 0;
  u32 first_finish_tag_offset = 0;
  u32 erase_setup_tag_offset = 0;
  u32 erase_clear_tag_offset = 0;
  u32 generic_upload_data_offset = 0;
  u32 clut_upload_data_offset = 0;
  u32 second_finish_tag_offset = 0;
};

namespace jak2_bucket4_fixture_detail {

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

inline u64 packed_gif_tag(u32 register_count, bool pre = false, GsPrim::Kind prim = {}) {
  return 1 | (1ull << 15) | (static_cast<u64>(pre) << 46) |
         (static_cast<u64>(prim) << 47) | (static_cast<u64>(register_count) << 60);
}

inline u64 gif_registers(std::initializer_list<GifTag::RegisterDescriptor> registers) {
  u64 result = 0;
  u32 index = 0;
  for (const auto reg : registers) {
    result |= static_cast<u64>(reg) << (index++ * 4);
  }
  return result;
}

}  // namespace jak2_bucket4_fixture_detail

/*!
 * Public original-data-free fixture matching the observed mixed Jak II bucket-4 grammar.
 * Scalar values are synthetic and the embedded EE source ranges point into this fixture only.
 */
inline Jak2Bucket4TextureUploadFixture make_jak2_bucket4_texture_upload_fixture(
    u32 ee_base = 0) {
  using namespace jak2_bucket4_fixture_detail;
  Jak2Bucket4TextureUploadFixture out;
  out.ee_memory.resize(static_cast<std::size_t>(ee_base) + 0x20000);
  out.chain_offset = ee_base + 0x100;
  const u32 kBucketOffset = out.chain_offset + 4 * 16;
  const u32 kBucketEnd = kBucketOffset + 16;
  const u32 kOrdinary = ee_base + 0x4000;
  const u32 kFirstArray = ee_base + 0x4200;
  const u32 kSecondArray = ee_base + 0x4400;
  const u32 kOrdinaryPage = ee_base + 0x6000;
  const u32 kGenericSource = ee_base + 0x8000;
  const u32 kClutSource = ee_base + 0xa000;

  constexpr u32 kBucketCount = 327;
  for (u32 bucket = 0; bucket < kBucketCount; ++bucket) {
    put_tag(out.ee_memory, out.chain_offset + bucket * 16, DmaTag::Kind::CNT, 0, 0, 0, 0);
  }
  put_tag(out.ee_memory, out.chain_offset + kBucketCount * 16, DmaTag::Kind::END, 0, 0, 0, 0);

  put_tag(out.ee_memory, kBucketOffset, DmaTag::Kind::NEXT, 0, kOrdinary, 0, 0);

  out.outer_direct_tag_offset = kOrdinary;
  put_tag(out.ee_memory, kOrdinary, DmaTag::Kind::CNT, 2, 0, 0,
          vif(VifCode::Kind::DIRECT, 2));
  put_u64(out.ee_memory, kOrdinary + 16, packed_gif_tag(1));
  put_u64(out.ee_memory, kOrdinary + 24,
          gif_registers({GifTag::RegisterDescriptor::AD}));
  put_u64(out.ee_memory, kOrdinary + 32, 1);
  put_u64(out.ee_memory, kOrdinary + 40, static_cast<u64>(GsRegisterAddress::TEXFLUSH));

  const u32 kDescriptor = kOrdinary + 48;
  put_tag(out.ee_memory, kDescriptor, DmaTag::Kind::CNT, 1, 0,
          vif(VifCode::Kind::PC_PORT), 3);
  out.ordinary_descriptor_data_offset = kDescriptor + 16;
  put_u64(out.ee_memory, out.ordinary_descriptor_data_offset, kOrdinaryPage);
  put_u64(out.ee_memory, out.ordinary_descriptor_data_offset + 8, static_cast<u64>(-1ll));
  put_tag(out.ee_memory, kDescriptor + 32, DmaTag::Kind::NEXT, 0, kFirstArray, 0, 0);

  put_tag(out.ee_memory, kFirstArray, DmaTag::Kind::CNT, 0, 0,
          vif(VifCode::Kind::PC_PORT, 12), 0);
  const u32 kCloud = kFirstArray + 16;
  put_tag(out.ee_memory, kCloud, DmaTag::Kind::CNT, 7, 0,
          vif(VifCode::Kind::PC_PORT, 41), 0);
  for (u32 offset = 0; offset < 104; offset += 4) {
    const float value = 1.f + static_cast<float>(offset / 4);
    std::memcpy(out.ee_memory.data() + kCloud + 16 + offset, &value, sizeof(value));
  }
  put_u32(out.ee_memory, kCloud + 16 + 104, 0x1234);
  out.first_finish_tag_offset = kCloud + 16 + 112;
  put_tag(out.ee_memory, out.first_finish_tag_offset, DmaTag::Kind::CNT, 0, 0,
          vif(VifCode::Kind::PC_PORT, 13), 0);
  put_tag(out.ee_memory, out.first_finish_tag_offset + 16, DmaTag::Kind::NEXT, 0, kSecondArray,
          0, 0);

  put_tag(out.ee_memory, kSecondArray, DmaTag::Kind::CNT, 0, 0,
          vif(VifCode::Kind::PC_PORT, 12), 0);
  put_tag(out.ee_memory, kSecondArray + 16, DmaTag::Kind::CNT, 0, 0,
          vif(VifCode::Kind::PC_PORT, 14), 0);

  out.erase_setup_tag_offset = kSecondArray + 32;
  put_tag(out.ee_memory, out.erase_setup_tag_offset, DmaTag::Kind::CNT, 10, 0,
          vif(VifCode::Kind::FLUSHA), vif(VifCode::Kind::DIRECT, 10));
  const u32 kSetupData = kSecondArray + 48;
  put_u64(out.ee_memory, kSetupData, packed_gif_tag(9));
  put_u64(out.ee_memory, kSetupData + 8,
          gif_registers({GifTag::RegisterDescriptor::AD, GifTag::RegisterDescriptor::AD,
                         GifTag::RegisterDescriptor::AD, GifTag::RegisterDescriptor::AD,
                         GifTag::RegisterDescriptor::AD, GifTag::RegisterDescriptor::AD,
                         GifTag::RegisterDescriptor::AD, GifTag::RegisterDescriptor::AD,
                         GifTag::RegisterDescriptor::AD}));
  constexpr u32 kWidth = 16;
  constexpr u32 kHeight = 16;
  constexpr u32 kEraseDestination = 0x1200;
  constexpr GsRegisterAddress kSetupRegisters[] = {
      GsRegisterAddress::SCISSOR_1, GsRegisterAddress::XYOFFSET_1,
      GsRegisterAddress::FRAME_1,   GsRegisterAddress::TEST_1,
      GsRegisterAddress::ALPHA_1,   GsRegisterAddress::CLAMP_1,
      GsRegisterAddress::TEXA,      GsRegisterAddress::ZBUF_1,
      GsRegisterAddress::TEXFLUSH,
  };
  constexpr u64 scissor = static_cast<u64>(kWidth - 1) << 16 |
                          static_cast<u64>(kHeight - 1) << 48;
  constexpr u64 frame =
      kEraseDestination / 32 | (static_cast<u64>((kWidth + 63) / 64) << 16);
  constexpr u64 kXyOffset = 0x8000ull | (0x8000ull << 32);
  constexpr u64 kTest = 0x11;
  constexpr u64 kAlpha = 0x22;
  constexpr u64 kClamp = 0x1;
  constexpr u64 kTexa = 0x80ull | (0x80ull << 32);
  constexpr u64 kZbuf = 0x130ull | (1ull << 24) | (1ull << 32);
  constexpr u64 kSetupValues[] = {scissor, kXyOffset, frame, kTest, kAlpha,
                                  kClamp,   kTexa,     kZbuf, 0};
  for (u32 i = 0; i < 9; ++i) {
    put_u64(out.ee_memory, kSetupData + 16 + i * 16, kSetupValues[i]);
    put_u64(out.ee_memory, kSetupData + 24 + i * 16,
            static_cast<u64>(kSetupRegisters[i]));
  }

  out.erase_clear_tag_offset = out.erase_setup_tag_offset + 16 + 160;
  put_tag(out.ee_memory, out.erase_clear_tag_offset, DmaTag::Kind::CNT, 4, 0, 0,
          vif(VifCode::Kind::DIRECT, 4));
  const u32 kClearData = kSecondArray + 224;
  put_u64(out.ee_memory, kClearData,
          packed_gif_tag(3, true, GsPrim::Kind::SPRITE));
  put_u64(out.ee_memory, kClearData + 8,
          gif_registers({GifTag::RegisterDescriptor::RGBAQ,
                         GifTag::RegisterDescriptor::XYZ2,
                         GifTag::RegisterDescriptor::XYZ2}));
  const u32 clear[4] = {17, 34, 51, 68};
  std::memcpy(out.ee_memory.data() + kClearData + 16, clear, sizeof(clear));
  const u32 first_vertex[4] = {2048 * 16, 2048 * 16, 0x00ffffff, 0};
  const u32 second_vertex[4] = {(2048 + kWidth) * 16, (2048 + kHeight) * 16, 0x00ffffff, 0};
  std::memcpy(out.ee_memory.data() + kClearData + 32, first_vertex, sizeof(first_vertex));
  std::memcpy(out.ee_memory.data() + kClearData + 48, second_vertex, sizeof(second_vertex));

  const u32 kGenericTag = kSecondArray + 288;
  put_tag(out.ee_memory, kGenericTag, DmaTag::Kind::CNT, 1, 0,
          vif(VifCode::Kind::PC_PORT, 16), 0);
  out.generic_upload_data_offset = kGenericTag + 16;
  put_u32(out.ee_memory, out.generic_upload_data_offset, kGenericSource);
  const u16 generic_size[2] = {256, 1};
  std::memcpy(out.ee_memory.data() + out.generic_upload_data_offset + 4, generic_size,
              sizeof(generic_size));
  put_u32(out.ee_memory, out.generic_upload_data_offset + 8, 0x1300);
  out.ee_memory[out.generic_upload_data_offset + 12] = 19;
  out.ee_memory[out.generic_upload_data_offset + 13] = 1;

  const u32 kClutTag = kSecondArray + 320;
  put_tag(out.ee_memory, kClutTag, DmaTag::Kind::CNT, 1, 0,
          vif(VifCode::Kind::PC_PORT, 15), 0);
  out.clut_upload_data_offset = kClutTag + 16;
  put_u32(out.ee_memory, out.clut_upload_data_offset, kClutSource);
  const u16 clut_size[2] = {16, 16};
  std::memcpy(out.ee_memory.data() + out.clut_upload_data_offset + 4, clut_size,
              sizeof(clut_size));
  put_u32(out.ee_memory, out.clut_upload_data_offset + 8, kEraseDestination);
  out.ee_memory[out.clut_upload_data_offset + 12] = 0;

  out.second_finish_tag_offset = kSecondArray + 352;
  put_tag(out.ee_memory, out.second_finish_tag_offset, DmaTag::Kind::CNT, 0, 0,
          vif(VifCode::Kind::PC_PORT, 13), vif(VifCode::Kind::PC_PORT));
  put_tag(out.ee_memory, out.second_finish_tag_offset + 16, DmaTag::Kind::NEXT, 0, kBucketEnd, 0,
          0);
  return out;
}

/*! Public exact fixture for the title chain's ordinary TEXFLUSH/page-descriptor variant. */
inline Jak2Bucket4TextureUploadFixture
make_jak2_bucket4_ordinary_only_texture_upload_fixture(u32 ee_base = 0) {
  using namespace jak2_bucket4_fixture_detail;
  auto out = make_jak2_bucket4_texture_upload_fixture(ee_base);
  const u32 closing_boundary = out.ordinary_descriptor_data_offset + 16;
  const u32 bucket_end = out.chain_offset + 5 * 16;
  put_tag(out.ee_memory, closing_boundary, DmaTag::Kind::NEXT, 0, bucket_end, 0, 0);
  return out;
}

}  // namespace metal_renderer
