#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

#include "common/dma/dma.h"
#include "common/dma/gs.h"

#include "game/graphics/opengl_renderer/buckets.h"

namespace metal_renderer::jak2_test {

inline constexpr std::size_t kBucketCount = static_cast<std::size_t>(jak2::BucketId::MAX_BUCKETS);
inline constexpr std::size_t kScreenFilterSetupQwords = 2;
inline constexpr std::size_t kScreenFilterSpriteQwords = 3;

using ScreenFilterSetup = std::array<u8, kScreenFilterSetupQwords * 16>;
using ScreenFilterSprite = std::array<u8, kScreenFilterSpriteQwords * 16>;

template <std::size_t Size>
void put_u64(std::array<u8, Size>& bytes, std::size_t offset, u64 value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

inline u64 make_gif_tag(u32 loops, GifTag::Format format, u32 register_count) {
  const u64 low = static_cast<u64>(loops) | (1ull << 15) | (static_cast<u64>(format) << 58) |
                  (static_cast<u64>(register_count) << 60);
  return low;
}

inline ScreenFilterSetup make_screen_filter_setup() {
  ScreenFilterSetup packet = {};
  put_u64(packet, 0, make_gif_tag(1, GifTag::Format::PACKED, 1));
  put_u64(packet, 8, static_cast<u64>(GifTag::RegisterDescriptor::AD));

  constexpr u64 kTest = 1ull | (static_cast<u64>(GsTest::AlphaFail::RGB_ONLY) << 12) |
                        (1ull << 16) | (static_cast<u64>(GsTest::ZTest::ALWAYS) << 17);
  put_u64(packet, 16, kTest);
  put_u64(packet, 24, static_cast<u64>(GsRegisterAddress::TEST_1));
  return packet;
}

inline ScreenFilterSprite make_screen_filter_sprite() {
  ScreenFilterSprite packet = {};
  constexpr u64 kRegisters = static_cast<u64>(GifTag::RegisterDescriptor::PRIM) |
                             (static_cast<u64>(GifTag::RegisterDescriptor::RGBAQ) << 4) |
                             (static_cast<u64>(GifTag::RegisterDescriptor::XYZF2) << 8) |
                             (static_cast<u64>(GifTag::RegisterDescriptor::XYZF2) << 12);
  put_u64(packet, 0, make_gif_tag(1, GifTag::Format::REGLIST, 4));
  put_u64(packet, 8, kRegisters);

  constexpr u64 kPrim = static_cast<u64>(GsPrim::Kind::SPRITE) | (1ull << 6);
  constexpr u64 kRgbaq = 0xffull | (0x60ull << 8) | (0x40ull << 24);
  constexpr auto xyzf2 = [](u16 x, u16 y) {
    return static_cast<u64>(x) | (static_cast<u64>(y) << 16) | (0x3fffffull << 32);
  };
  put_u64(packet, 16, kPrim);
  put_u64(packet, 24, kRgbaq);
  put_u64(packet, 32, xyzf2(0x7000, 0x7300));
  put_u64(packet, 40, xyzf2(0x9000, 0x8d00));
  return packet;
}

inline u32 vif_direct(u32 qwords) {
  return (static_cast<u32>(VifCode::Kind::DIRECT) << 24) | (qwords & 0xffff);
}

inline void put_dma_tag(std::vector<u8>& chain,
                        std::size_t offset,
                        DmaTag::Kind kind,
                        u16 qwc = 0,
                        u32 address = 0,
                        u32 vif0 = 0,
                        u32 vif1 = 0) {
  const u64 value =
      static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) | (static_cast<u64>(address) << 32);
  std::memcpy(chain.data() + offset, &value, sizeof(value));
  std::memcpy(chain.data() + offset + 8, &vif0, sizeof(vif0));
  std::memcpy(chain.data() + offset + 12, &vif1, sizeof(vif1));
}

inline std::vector<u8> make_screen_filter_chain(u32 base_address, bool include_deferred_probe) {
  static_assert(kBucketCount == 327);
  constexpr std::size_t kSlotsBytes = (kBucketCount + 1) * 16;
  constexpr std::size_t kDeferredProbeBytes = 48;
  constexpr std::size_t kScreenFilterBytes = 128;
  const std::size_t deferred_offset = kSlotsBytes;
  const std::size_t screen_filter_offset =
      deferred_offset + (include_deferred_probe ? kDeferredProbeBytes : 0);
  std::vector<u8> chain(screen_filter_offset + kScreenFilterBytes, 0);

  for (std::size_t bucket = 0; bucket < kBucketCount; bucket++) {
    put_dma_tag(chain, bucket * 16, DmaTag::Kind::CNT);
  }
  put_dma_tag(chain, kBucketCount * 16, DmaTag::Kind::END);

  if (include_deferred_probe) {
    constexpr std::size_t kDeferredBucket = static_cast<std::size_t>(jak2::BucketId::BUCKET_2);
    put_dma_tag(chain, kDeferredBucket * 16, DmaTag::Kind::NEXT, 0,
                base_address + static_cast<u32>(deferred_offset));
    put_dma_tag(chain, deferred_offset, DmaTag::Kind::CNT, 1);
    std::memset(chain.data() + deferred_offset + 16, 0xa5, 16);
    put_dma_tag(chain, deferred_offset + 32, DmaTag::Kind::NEXT, 0,
                base_address + static_cast<u32>((kDeferredBucket + 1) * 16));
  }

  constexpr std::size_t kScreenFilter = static_cast<std::size_t>(jak2::BucketId::SCREEN_FILTER);
  put_dma_tag(chain, kScreenFilter * 16, DmaTag::Kind::NEXT, 0,
              base_address + static_cast<u32>(screen_filter_offset));
  std::size_t cursor = screen_filter_offset;
  const auto setup = make_screen_filter_setup();
  put_dma_tag(chain, cursor, DmaTag::Kind::CNT, kScreenFilterSetupQwords, 0, 0,
              vif_direct(kScreenFilterSetupQwords));
  std::memcpy(chain.data() + cursor + 16, setup.data(), setup.size());
  cursor += 16 + setup.size();

  const auto sprite = make_screen_filter_sprite();
  put_dma_tag(chain, cursor, DmaTag::Kind::CNT, kScreenFilterSpriteQwords, 0, 0,
              vif_direct(kScreenFilterSpriteQwords));
  std::memcpy(chain.data() + cursor + 16, sprite.data(), sprite.size());
  cursor += 16 + sprite.size();

  put_dma_tag(chain, cursor, DmaTag::Kind::NEXT, 0,
              base_address + static_cast<u32>((kScreenFilter + 1) * 16));
  return chain;
}

}  // namespace metal_renderer::jak2_test
