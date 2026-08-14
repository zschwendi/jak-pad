#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"
#include "common/util/compress.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_jak2_blit_display_plan.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_fixture.h"
#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"
#include "game/graphics/pipelines/metal/metal_jak2_effects_bucket315_capture.h"
#include "game/graphics/pipelines/metal/metal_eye_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_gmerc_warp_bucket317_plan.h"
#include "game/graphics/pipelines/metal/metal_jak2_pris2_bucket228_plan.h"
#include "game/graphics/pipelines/metal/metal_jak2_raw_image_upload_fixture.h"
#include "game/graphics/pipelines/metal/metal_jak2_shadow_bucket195_capture.h"
#include "game/graphics/pipelines/metal/metal_jak2_sky_post_texture_upload_plan.h"
#include "game/graphics/pipelines/metal/metal_jak2_warp_texture_upload_plan.h"
#include "game/graphics/pipelines/metal/metal_jak2_warp_renderer.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/pipelines/metal/metal_merc_model_pool.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/kernel/core/kernel_core.h"
#include "game/runtime.h"

#import <QuartzCore/CAMetalLayer.h>

namespace {

static_assert(offsetof(goal_jak2_metal_host_metrics, shadow_bucket195_execution) +
                  sizeof(goal_jak2_shadow_bucket195_execution_metrics) ==
              sizeof(goal_jak2_metal_host_metrics));

constexpr u32 kChainOffset = 0x100000;
constexpr u32 kBucketCount = static_cast<u32>(jak2::BucketId::MAX_BUCKETS);
constexpr u32 kSkyDrawBucket = static_cast<u32>(jak2::BucketId::SKY_DRAW);
constexpr u32 kScreenFilterBucket = static_cast<u32>(jak2::BucketId::SCREEN_FILTER);
constexpr u32 kDebugNoZbuf2Bucket = static_cast<u32>(jak2::BucketId::DEBUG_NO_ZBUF2);
constexpr u32 kMapTextureUploadBucket = static_cast<u32>(jak2::BucketId::TEX_ALL_MAP);
constexpr u32 kProgressBucket = static_cast<u32>(jak2::BucketId::PROGRESS);
constexpr u32 kEffectsBucket = metal_renderer::kJak2EffectsBucket;
constexpr u32 kSkyDrawPayloadOffset = kChainOffset + 0x4000;
constexpr u32 kScreenFilterPayloadOffset = kChainOffset + 0x5000;
constexpr u32 kDebugNoZbuf2PayloadOffset = kChainOffset + 0x6000;
constexpr u32 kSpriteTextureUploadBucket =
    static_cast<u32>(jak2::BucketId::TEX_ALL_SPRITE);
constexpr u32 kSpriteTextureUploadGroupOffset = kChainOffset + 0x10000;
constexpr u32 kSpriteTextureUploadGroupStride = 0x100;
constexpr u32 kSpriteTextureUploadTailOffset = kChainOffset + 0x11000;
constexpr u32 kMapTextureUploadGroupOffset = kChainOffset + 0x12000;
constexpr u32 kMapTextureUploadTailOffset = kChainOffset + 0x13000;
constexpr u32 kProgressPayloadOffset = kChainOffset + 0x14000;
constexpr u32 kWaterSecurityBucket = 252;
constexpr u32 kWaterSecurityDescriptorOffset = kChainOffset + 0x15000;
constexpr u32 kWaterSecurityAnimatorOffset = kChainOffset + 0x15100;
constexpr u32 kWaterSecurityDirectOffset = kChainOffset + 0x15800;
constexpr u32 kPrisOrdinaryBucket = 200;
constexpr u32 kPrisOrdinaryDescriptorOffset = kChainOffset + 0x16000;
constexpr u32 kPrisOrdinaryDirectOffset = kChainOffset + 0x16100;
constexpr u32 kPrisonClutBucket = 204;
constexpr u32 kOtherPrisAnimatorBucket = 208;
constexpr u32 kPrisonClutDescriptorOffset = kChainOffset + 0x16200;
constexpr u32 kPrisonClutAnimatorOffset = kChainOffset + 0x16300;
constexpr u32 kPrisonClutDirectOffset = kChainOffset + 0x16400;
constexpr u32 kDuplicatePrisonClutDescriptorOffset = kChainOffset + 0x16500;
constexpr u32 kDuplicatePrisonClutAnimatorOffset = kChainOffset + 0x16600;
constexpr u32 kDuplicatePrisonClutDirectOffset = kChainOffset + 0x16700;
constexpr u32 kCommonPrisBucket = metal_renderer::kJak2CommonPrisTextureUploadBucket;
constexpr u32 kCommonWaterBucket = metal_renderer::kJak2CommonWaterTextureUploadBucket;
static_assert(kCommonWaterBucket == 306 &&
              kCommonWaterBucket == static_cast<u32>(jak2::BucketId::TEX_LCOM_WATER));
constexpr u32 kCommonPrisDescriptorOffset = kChainOffset + 0x16800;
constexpr u32 kCommonPrisAnimatorOffset = kChainOffset + 0x16a00;
constexpr u32 kCommonPrisDirectOffset = kChainOffset + 0x16b00;
constexpr u32 kPris2Bucket228 = metal_renderer::kJak2Pris2TextureUploadBucket;
constexpr u32 kPris2DescriptorOffset = kChainOffset + 0x16c00;
constexpr u32 kPris2DirectOffset = kChainOffset + 0x16d00;
constexpr u32 kEffectsLightningPayloadOffset = kChainOffset + 0x17000;
constexpr u32 kCommonWaterDescriptorOffset = kChainOffset + 0x17100;
constexpr u32 kCommonWaterAnimatorOffset = kChainOffset + 0x17200;
constexpr u32 kCommonWaterDirectOffset = kChainOffset + 0x1a000;
constexpr u32 kLiveBombTexturePageOffset = 0x1dc3384;
constexpr u32 kSkyPostBucket = metal_renderer::kJak2SkyPostTextureUploadBucket;
constexpr u32 kSkyPostGroupOffset = kChainOffset + 0x17400;
constexpr u32 kSkyPostDirectOffset = kChainOffset + 0x17500;
constexpr u32 kWarpTextureUploadBucket = metal_renderer::kJak2WarpTextureUploadBucket;
constexpr u32 kWarpTextureUploadGroupOffset = kChainOffset + 0x17800;
constexpr u32 kWarpTextureUploadGroupStride = 0x100;
constexpr u32 kWarpTextureUploadTailOffset = kChainOffset + 0x18000;
constexpr u32 kWarpTextureUploadAnimatorOffset = kChainOffset + 0x18100;
constexpr u32 kSubtitleBucket = metal_renderer::kJak2SubtitleBucket;
static_assert(kSubtitleBucket == static_cast<u32>(jak2::BucketId::SUBTITLE));
constexpr u32 kSubtitleCaptureOffset = kChainOffset + 0x18200;
constexpr u32 kSubtitleMalformedOffset = kChainOffset + 0x18400;
constexpr u32 kShadowBucket = metal_renderer::kJak2ShadowBucket195;
static_assert(kShadowBucket == static_cast<u32>(jak2::BucketId::SHADOW));
constexpr u32 kShadowCaptureOffset = kChainOffset + 0x18500;
constexpr u32 kGmercWarpBucket = metal_renderer::kJak2GmercWarpBucket;
static_assert(kGmercWarpBucket == static_cast<u32>(jak2::BucketId::GMERC_WARP));
constexpr u32 kGmercWarpPayloadOffset = kChainOffset + 0x18c00;
constexpr u32 kBlitDisplayBucket = metal_renderer::kJak2BlitDisplayBucket;
static_assert(kBlitDisplayBucket == static_cast<u32>(jak2::BucketId::BUCKET_3));
constexpr u32 kBlitDisplayPayloadOffset = kChainOffset + 0x19000;
constexpr std::size_t kGifQwords = 7;
constexpr std::size_t kGifBytes = kGifQwords * 16;
constexpr u16 kTexturePageId = 11;
constexpr u32 kTexturePageOffset = 0x200000;
constexpr u32 kTexturePageStride = 0x100;
constexpr u32 kTextureObjectOffset = 0x201000;
constexpr u32 kTextureNameOffset = 0x202000;
constexpr u32 kTextureVram = 0x700;
constexpr u32 kRelocatedTextureVram = 0x720;
constexpr u32 kSyntheticS7 = 0x7f00000;

int failures = 0;

void check(bool condition, const char* message) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
  if (!condition) {
    failures++;
  }
}

std::array<u8, 4> first_pixel(u64 handle) {
  std::array<u8, 4> pixel = {};
  id<MTLTexture> texture = metal_texture_lookup(handle);
  if (texture) {
    [texture getBytes:pixel.data()
          bytesPerRow:4
           fromRegion:MTLRegionMake2D(0, 0, 1, 1)
          mipmapLevel:0];
  }
  return pixel;
}

void put_tag(u32 offset,
             DmaTag::Kind kind,
             u16 qwc = 0,
             u32 address = 0,
             u32 vif0 = 0,
             u32 vif1 = 0) {
  const u64 value =
      static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) | (static_cast<u64>(address) << 32);
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + offset, &value, sizeof(value));
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + offset + 8, &vif0, sizeof(vif0));
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + offset + 12, &vif1, sizeof(vif1));
}

u32 gmerc_warp_vif(VifCode::Kind kind, u16 immediate = 0, u8 num = 0) {
  return (static_cast<u32>(kind) << 24) | (static_cast<u32>(num) << 16) | immediate;
}

u32 gmerc_warp_stcycl(u16 cl, u16 wl) {
  return gmerc_warp_vif(VifCode::Kind::STCYCL, cl | (wl << 8));
}

void append_gmerc_warp_u32(std::vector<u8>* bytes, u32 value) {
  const auto offset = bytes->size();
  bytes->resize(offset + 4);
  std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

void write_gmerc_warp_u64(std::vector<u8>* bytes, std::size_t offset, u64 value) {
  ASSERT(offset + sizeof(value) <= bytes->size());
  std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

void write_gmerc_warp_float(std::vector<u8>* bytes, std::size_t offset, float value) {
  ASSERT(offset + sizeof(value) <= bytes->size());
  std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

void append_gmerc_warp_float(std::vector<u8>* bytes, float value) {
  const auto offset = bytes->size();
  bytes->resize(offset + sizeof(value));
  write_gmerc_warp_float(bytes, offset, value);
}

std::vector<u8> make_gmerc_warp_zbuf_direct() {
  std::vector<u8> bytes(32, 0);
  write_gmerc_warp_u64(&bytes, 0, 1ull | (1ull << 15) | (1ull << 60));
  write_gmerc_warp_u64(&bytes, 8, static_cast<u64>(GifTag::RegisterDescriptor::AD));
  write_gmerc_warp_u64(&bytes, 16, 0x130ull | (1ull << 24));
  write_gmerc_warp_u64(&bytes, 24, static_cast<u64>(GsRegisterAddress::ZBUF_1));
  return bytes;
}

std::vector<u8> make_gmerc_warp_constants() {
  std::vector<u8> bytes(128, 0);
  write_gmerc_warp_float(&bytes, 0, 1.f);
  write_gmerc_warp_float(&bytes, 8, 255.f);
  constexpr float hvdf[4] = {2048.f, 2048.f, 8388607.f, 0.f};
  std::memcpy(bytes.data() + 32, hvdf, sizeof(hvdf));
  return bytes;
}

u64 gmerc_warp_tex0() {
  return metal_renderer::kJak2WarpTextureTbp | (1ull << 14) | (6ull << 26) | (6ull << 30) |
         (1ull << 34);
}

std::vector<u8> make_gmerc_warp_fragment() {
  std::vector<u8> bytes(112, 0);
  float matrix[16] = {};
  matrix[0] = 1.f;
  matrix[5] = 1.f;
  matrix[10] = 1.f;
  matrix[11] = 1.f;
  std::memcpy(bytes.data(), matrix, sizeof(matrix));
  write_gmerc_warp_u64(&bytes, 64, 1ull << 46);
  write_gmerc_warp_u64(&bytes, 80, 0x44);
  write_gmerc_warp_u64(&bytes, 88, static_cast<u64>(GsRegisterAddress::ALPHA_1));
  write_gmerc_warp_u64(
      &bytes, 96, (1ull << 16) | (static_cast<u64>(GsTest::ZTest::GEQUAL) << 17));
  write_gmerc_warp_u64(&bytes, 104, static_cast<u64>(GsRegisterAddress::TEST_1));
  AdGifData adgif = {};
  adgif.tex0_data = gmerc_warp_tex0();
  adgif.tex0_addr = static_cast<u64>(GsRegisterAddress::TEX0_1);
  adgif.tex1_addr = static_cast<u64>(GsRegisterAddress::TEX1_1) | (4ull << 32);
  adgif.mip_addr = static_cast<u64>(GsRegisterAddress::MIPTBP1_1);
  adgif.clamp_addr = static_cast<u64>(GsRegisterAddress::CLAMP_1);
  adgif.alpha_addr = static_cast<u64>(GsRegisterAddress::MIPTBP2_1);
  const auto adgif_offset = bytes.size();
  bytes.resize(adgif_offset + sizeof(adgif));
  std::memcpy(bytes.data() + adgif_offset, &adgif, sizeof(adgif));
  append_gmerc_warp_u32(&bytes, gmerc_warp_stcycl(3, 1));
  append_gmerc_warp_u32(&bytes, gmerc_warp_vif(VifCode::Kind::UNPACK_V3_32, 0, 4));
  constexpr float x[4] = {128.f, -128.f, 128.f, -128.f};
  constexpr float y[4] = {128.f, 128.f, -128.f, -128.f};
  for (int i = 0; i < 4; ++i) {
    append_gmerc_warp_float(&bytes, x[i]);
    append_gmerc_warp_float(&bytes, y[i]);
    append_gmerc_warp_float(&bytes, -1.f);
  }
  append_gmerc_warp_u32(&bytes, gmerc_warp_vif(VifCode::Kind::UNPACK_V4_8, 0, 4));
  for (int i = 0; i < 4; ++i) {
    append_gmerc_warp_u32(&bytes, 0x80808080);
  }
  append_gmerc_warp_u32(&bytes, gmerc_warp_vif(VifCode::Kind::UNPACK_V2_16, 0, 4));
  constexpr s16 s = 2048;
  constexpr s16 t = 2912;
  for (int i = 0; i < 4; ++i) {
    const auto offset = bytes.size();
    bytes.resize(offset + 4);
    std::memcpy(bytes.data() + offset, &s, sizeof(s));
    std::memcpy(bytes.data() + offset + 2, &t, sizeof(t));
  }
  append_gmerc_warp_u32(&bytes, gmerc_warp_stcycl(4, 4));
  append_gmerc_warp_u32(&bytes, gmerc_warp_vif(VifCode::Kind::MSCAL, 0x24));
  while (bytes.size() % 16) {
    append_gmerc_warp_u32(&bytes, 0);
  }
  return bytes;
}

std::vector<u8> make_gmerc_warp_continued_fragment_transfer() {
  auto bytes = make_gmerc_warp_fragment();
  append_gmerc_warp_u32(&bytes, gmerc_warp_stcycl(4, 4));
  append_gmerc_warp_u32(&bytes, gmerc_warp_vif(VifCode::Kind::UNPACK_V4_32, 0, 12));
  while (bytes.size() % 16) {
    append_gmerc_warp_u32(&bytes, 0);
  }
  const auto second = make_gmerc_warp_fragment();
  bytes.insert(bytes.end(), second.begin(), second.begin() + 192);
  append_gmerc_warp_u32(&bytes, gmerc_warp_stcycl(3, 1));
  append_gmerc_warp_u32(&bytes, gmerc_warp_vif(VifCode::Kind::UNPACK_V4_8, 0, 4));
  for (int i = 0; i < 4; ++i) {
    append_gmerc_warp_u32(&bytes, 0x80808080);
  }
  append_gmerc_warp_u32(&bytes, gmerc_warp_vif(VifCode::Kind::UNPACK_V2_16, 0, 4));
  constexpr s16 s = 2048;
  constexpr s16 t = 2912;
  for (int i = 0; i < 4; ++i) {
    const auto offset = bytes.size();
    bytes.resize(offset + 4);
    std::memcpy(bytes.data() + offset, &s, sizeof(s));
    std::memcpy(bytes.data() + offset + 2, &t, sizeof(t));
  }
  append_gmerc_warp_u32(&bytes, gmerc_warp_stcycl(4, 4));
  append_gmerc_warp_u32(&bytes, gmerc_warp_vif(VifCode::Kind::MSCAL, 0x24));
  while (bytes.size() % 16) {
    append_gmerc_warp_u32(&bytes, 0);
  }
  return bytes;
}

void make_empty_chain() {
  static_assert(kBucketCount == 327);
  std::memset(static_cast<u8*>(g_ee_main_mem) + kChainOffset, 0, (kBucketCount + 1) * 16);
  for (u32 bucket = 0; bucket < kBucketCount; bucket++) {
    put_tag(kChainOffset + bucket * 16, DmaTag::Kind::CNT);
  }
  put_tag(kChainOffset + kBucketCount * 16, DmaTag::Kind::END);
}

u32 effects_vif(VifCode::Kind kind,
                u16 immediate = 0,
                u8 count = 0,
                bool interrupt = false) {
  return (static_cast<u32>(interrupt) << 31) | (static_cast<u32>(kind) << 24) |
         (static_cast<u32>(count) << 16) | immediate;
}

void make_effects_lightning_chain(u32 fragments = 0,
                                  u32 texture_tbp = kTextureVram,
                                  bool malformed_direct = false,
                                  u32 tex1_mmin = 1,
                                  u32 tex1_mxl = 0) {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  const u32 bucket_offset = kChainOffset + kEffectsBucket * 16;
  std::memset(ee + kEffectsLightningPayloadOffset, 0, 0x400);
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kEffectsLightningPayloadOffset,
          effects_vif(VifCode::Kind::MARK));
  u32 cursor = kEffectsLightningPayloadOffset;
  put_tag(cursor, DmaTag::Kind::CNT, 2, 0, 0,
          malformed_direct ? effects_vif(VifCode::Kind::NOP)
                           : effects_vif(VifCode::Kind::DIRECT, 2, 0, true));
  const u64 gif_tag = 1ull | (1ull << 15) | (1ull << 60);
  const u64 zbuf = 0x130ull | (1ull << 24) | (1ull << 32);
  const u64 zbuf_address = static_cast<u64>(GsRegisterAddress::ZBUF_1);
  const u64 ad_register = static_cast<u64>(GifTag::RegisterDescriptor::AD);
  std::memcpy(ee + cursor + 16, &gif_tag, sizeof(gif_tag));
  std::memcpy(ee + cursor + 24, &ad_register, sizeof(ad_register));
  std::memcpy(ee + cursor + 32, &zbuf, sizeof(zbuf));
  std::memcpy(ee + cursor + 40, &zbuf_address, sizeof(zbuf_address));
  cursor += 48;
  put_tag(cursor, DmaTag::Kind::CNT, 8, 0, effects_vif(VifCode::Kind::STCYCL, 0x404),
          effects_vif(VifCode::Kind::UNPACK_V4_32, 897, 8));
  cursor += 144;
  put_tag(cursor, DmaTag::Kind::CNT, 2, 0,
          effects_vif(VifCode::Kind::MSCALF, 0, 0, true),
          effects_vif(VifCode::Kind::STMOD));
  cursor += 48;
  put_tag(cursor, DmaTag::Kind::CNT);
  cursor += 16;
  u16 header_address = 837;
  u16 vertex_address = 9;
  for (u32 fragment = 0; fragment < fragments; ++fragment) {
    constexpr u32 kVertexCount = 4;
    put_tag(cursor, DmaTag::Kind::CNT, 12, 0, 0,
            effects_vif(VifCode::Kind::UNPACK_V4_32, header_address, 12));
    auto* header = ee + cursor + 16;
    const auto prim_control = [](GsPrim::Kind kind) {
      const u32 prim = static_cast<u32>(kind) | (1u << 3) | (1u << 4) | (1u << 6);
      return (1u << 14) | (prim << 15) | (3u << 28);
    };
    const u32 fan = prim_control(GsPrim::Kind::TRI_FAN);
    const u32 strip = prim_control(GsPrim::Kind::TRI_STRIP);
    const u32 regs = static_cast<u32>(GifTag::RegisterDescriptor::ST) |
                     (static_cast<u32>(GifTag::RegisterDescriptor::RGBAQ) << 4) |
                     (static_cast<u32>(GifTag::RegisterDescriptor::XYZF2) << 8);
    const u32 one = 1;
    const u32 limit = 0x7f;
    std::memcpy(header + 64, &fan, sizeof(fan));
    std::memcpy(header + 68, &strip, sizeof(strip));
    std::memcpy(header + 72, &regs, sizeof(regs));
    std::memcpy(header + 76, &one, sizeof(one));
    std::memcpy(header + 88, &limit, sizeof(limit));
    std::memcpy(header + 92, &kVertexCount, sizeof(kVertexCount));
    std::memcpy(header + 104, &limit, sizeof(limit));
    AdGifData adgif = {};
    adgif.tex0_data = texture_tbp | (1ull << 14) | (2ull << 26) | (2ull << 30) |
                      (1ull << 34) | (1ull << 61);
    adgif.tex0_addr = static_cast<u64>(GsRegisterAddress::TEX0_1);
    adgif.tex1_data = (static_cast<u64>(tex1_mxl) << 2) | (1ull << 5) |
                      (static_cast<u64>(tex1_mmin) << 6);
    adgif.tex1_addr = static_cast<u64>(GsRegisterAddress::TEX1_1) |
                      (static_cast<u64>(0x8000u | kVertexCount) << 32);
    adgif.mip_addr = static_cast<u64>(GsRegisterAddress::MIPTBP1_1);
    adgif.clamp_data = 0b0101;
    adgif.clamp_addr = static_cast<u64>(GsRegisterAddress::CLAMP_1);
    adgif.alpha_data = (2ull << 2) | (1ull << 6) | (0x80ull << 32);
    adgif.alpha_addr = static_cast<u64>(GsRegisterAddress::ALPHA_1);
    std::memcpy(header + 112, &adgif, sizeof(adgif));
    cursor += 208;
    put_tag(cursor, DmaTag::Kind::CNT, 12, 0, 0,
            effects_vif(VifCode::Kind::UNPACK_V4_32, vertex_address, 12));
    cursor += 208;
    put_tag(cursor, DmaTag::Kind::CNT, 0, 0, 0,
            effects_vif(VifCode::Kind::MSCAL, 6, 0, true));
    cursor += 16;
    header_address = 1704 - header_address;
    vertex_address += 279;
    if (vertex_address > 567) {
      vertex_address = 9;
    }
  }
  put_tag(cursor, DmaTag::Kind::CNT);
  cursor += 16;
  put_tag(cursor, DmaTag::Kind::CNT, 10, 0,
          effects_vif(VifCode::Kind::FLUSHA, 0, 0, true),
          effects_vif(VifCode::Kind::DIRECT, 10, 0, true));
  cursor += 176;
  put_tag(cursor, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
}

void make_shadow_bucket195_chain(bool ready = false) {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  constexpr u32 kStcycl = static_cast<u32>(VifCode::Kind::STCYCL) << 24;
  constexpr u32 kUnpackV432 = static_cast<u32>(VifCode::Kind::UNPACK_V4_32) << 24;
  constexpr u32 kUnpackV48 = static_cast<u32>(VifCode::Kind::UNPACK_V4_8) << 24;
  constexpr u32 kMscalf = static_cast<u32>(VifCode::Kind::MSCALF) << 24;
  constexpr u32 kFlushe = static_cast<u32>(VifCode::Kind::FLUSHE) << 24;
  constexpr u32 kFlush = static_cast<u32>(VifCode::Kind::FLUSH) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  const u32 bucket_offset = kChainOffset + kShadowBucket * 16;
  std::memset(ee + kShadowCaptureOffset, 0, 0x700);
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kShadowCaptureOffset);
  u32 cursor = kShadowCaptureOffset;
  put_tag(cursor, DmaTag::Kind::CNT, 13, 0, kStcycl | 0x404,
          kUnpackV432 | (13 << 16) | 0x370);
  if (ready) {
    constexpr float kHvdf[3] = {2048.f, 2048.f, 12582912.f};
    constexpr float kFog = 1.f;
    std::memcpy(ee + cursor + 16 + 64, kHvdf, sizeof(kHvdf));
    std::memcpy(ee + cursor + 16 + 80, &kFog, sizeof(kFog));
  }
  cursor += 224;
  put_tag(cursor, DmaTag::Kind::CNT, 4, 0, kStcycl | 0x404,
          kUnpackV432 | (4 << 16) | 0x3ac);
  cursor += 80;
  put_tag(cursor, DmaTag::Kind::CNT, 4, 0, kStcycl | 0x404,
          kUnpackV432 | (4 << 16));
  if (ready) {
    std::array<float, 16> perspective = {};
    perspective[0] = -4096.f;
    perspective[5] = -6656.f;
    perspective[12] = 2048.f;
    perspective[13] = 3328.f;
    perspective[15] = -1.f;
    std::memcpy(ee + cursor + 16, perspective.data(), sizeof(perspective));
  }
  cursor += 80;
  put_tag(cursor, DmaTag::Kind::CNT, 0, 0, kMscalf | 10, kFlushe);
  cursor += 16;
  put_tag(cursor, DmaTag::Kind::CNT);
  cursor += 16;
  struct ShadowVertex {
    float x;
    float y;
    float z;
    u32 pad;
  };
  constexpr std::array<ShadowVertex, 4> kTop = {{{0.4375f, 0.46875f, 0.75f, 0},
                                                  {0.4375f, 0.53125f, 0.75f, 0},
                                                  {0.5625f, 0.46875f, 0.75f, 0},
                                                  {0.5625f, 0.53125f, 0.75f, 0}}};
  constexpr std::array<ShadowVertex, 4> kBottom = {{{0.5f, 0.46875f, 0.75f, 0},
                                                     {0.5f, 0.53125f, 0.75f, 0},
                                                     {0.5625f, 0.46875f, 0.75f, 0},
                                                     {0.5625f, 0.53125f, 0.75f, 0}}};
  const u8 vertex_count = ready ? 4 : 3;
  put_tag(cursor, DmaTag::Kind::CNT, vertex_count, 0, kFlush,
          kUnpackV432 | (vertex_count << 16) | 4);
  if (ready) {
    std::memcpy(ee + cursor + 16, kTop.data(), sizeof(kTop));
  }
  cursor += 16 + vertex_count * 16;
  if (ready) {
    put_tag(cursor, DmaTag::Kind::CNT, 4, 0, 0,
            kUnpackV432 | (4 << 16) | 174);
    std::memcpy(ee + cursor + 16, kBottom.data(), sizeof(kBottom));
    cursor += 80;
  }
  put_tag(cursor, DmaTag::Kind::CNT, 2, 0, 0,
          kUnpackV48 | (4 << 16) | (1 << 14) | 344);
  const u32 header = ready ? 2 : 0x101;
  constexpr std::array<u8, 4> kRecord0 = {0, 1, 2, 1};
  constexpr std::array<u8, 4> kRecord1 = {2, 1, 3, 1};
  std::memcpy(ee + cursor + 16, &header, sizeof(header));
  std::memcpy(ee + cursor + 20, kRecord0.data(), kRecord0.size());
  if (ready) {
    std::memcpy(ee + cursor + 24, kRecord1.data(), kRecord1.size());
  }
  const u32 trailing_mscalf = kMscalf | (ready ? 2 : 6);
  std::memcpy(ee + cursor + 16 + 4 * 4 + 12, &trailing_mscalf,
              sizeof(trailing_mscalf));
  cursor += 48;
  put_tag(cursor, DmaTag::Kind::CNT, 6, 0, kFlusha, kDirect | 6);
  cursor += 112;
  put_tag(cursor, DmaTag::Kind::CNT, 35, 0, kFlusha, kDirect | 35);
  constexpr std::array<u8, 4> kColor = {64, 192, 128, 128};
  std::memcpy(ee + cursor + 16 + 24, kColor.data(), kColor.size());
  cursor += 576;
  put_tag(cursor, DmaTag::Kind::CNT, 8, 0, kFlusha, kDirect | 8);
  cursor += 144;
  put_tag(cursor, DmaTag::Kind::NEXT, 0, cursor + 16);
  cursor += 16;
  put_tag(cursor, DmaTag::Kind::CNT, 10, 0, kFlusha, kDirect | 10);
  cursor += 176;
  put_tag(cursor, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
}

metal_renderer::Jak2Opcode27LayerValues identity_layer_values();

void make_common_water_environment_chain(bool dot_only) {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr s64 kMode = -1;
  constexpr u64 kPageOffset = kTexturePageOffset;
  const u32 bucket_offset = kChainOffset + kCommonWaterBucket * 16;

  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kCommonWaterDescriptorOffset);
  put_tag(kCommonWaterDescriptorOffset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
  std::memcpy(ee + kCommonWaterDescriptorOffset + 16, &kPageOffset, sizeof(kPageOffset));
  std::memcpy(ee + kCommonWaterDescriptorOffset + 24, &kMode, sizeof(kMode));
  put_tag(kCommonWaterDescriptorOffset + 32, DmaTag::Kind::NEXT, 0,
          kCommonWaterAnimatorOffset);
  put_tag(kCommonWaterAnimatorOffset, DmaTag::Kind::CNT, 0, 0, kPcPort | 12, 0);
  const u32 body_tag = kCommonWaterAnimatorOffset + 16;
  const u32 body_offset = body_tag + 16;
  u32 body_bytes = 0;
  if (dot_only) {
    metal_renderer::Jak2Opcode30SecurityDotPlan dot;
    dot.time = 0.f;
    dot.destination_tbp = 0x761;
    for (auto& layer : dot.layers) {
      layer.start = identity_layer_values();
      layer.end = identity_layer_values();
    }
    body_bytes = sizeof(dot);
    put_tag(body_tag, DmaTag::Kind::CNT, body_bytes / 16, 0, kPcPort | 30, 0);
    std::memcpy(ee + body_offset, &dot, sizeof(dot));
  } else {
    metal_renderer::Jak2Opcode30SecurityEnvironmentPlan environment;
    environment.time = 0.f;
    environment.destination_tbp = 0x760;
    for (auto& layer : environment.layers) {
      layer.start = identity_layer_values();
      layer.end = identity_layer_values();
    }
    body_bytes = sizeof(environment);
    put_tag(body_tag, DmaTag::Kind::CNT, body_bytes / 16, 0, kPcPort | 30, 0);
    std::memcpy(ee + body_offset, &environment, sizeof(environment));
  }
  const u32 finish_offset = body_offset + body_bytes;
  put_tag(finish_offset, DmaTag::Kind::CNT, 0, 0, kPcPort | 13, 0);
  put_tag(finish_offset + 16, DmaTag::Kind::NEXT, 0, kCommonWaterDirectOffset);
  put_tag(kCommonWaterDirectOffset, DmaTag::Kind::CNT, 10, 0, kFlusha, kDirect | 10);
  put_tag(kCommonWaterDirectOffset + 176, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
}

void make_common_water_bomb_chain() {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr s64 kMode = -1;
  constexpr u64 kPageOffset = kLiveBombTexturePageOffset;
  const u32 bucket_offset = kChainOffset + kCommonWaterBucket * 16;

  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kCommonWaterDescriptorOffset);
  put_tag(kCommonWaterDescriptorOffset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
  std::memcpy(ee + kCommonWaterDescriptorOffset + 16, &kPageOffset, sizeof(kPageOffset));
  std::memcpy(ee + kCommonWaterDescriptorOffset + 24, &kMode, sizeof(kMode));
  put_tag(kCommonWaterDescriptorOffset + 32, DmaTag::Kind::NEXT, 0,
          kCommonWaterAnimatorOffset);
  put_tag(kCommonWaterAnimatorOffset, DmaTag::Kind::CNT, 0, 0, kPcPort | 12, 0);
  const u32 body_tag = kCommonWaterAnimatorOffset + 16;
  metal_renderer::Jak2Opcode28BombPlan bomb;
  bomb.time = 0.f;
  bomb.destination_tbp = 0x80;
  for (auto& layer : bomb.layers) {
    layer.start = identity_layer_values();
    layer.end = identity_layer_values();
  }
  put_tag(body_tag, DmaTag::Kind::CNT, sizeof(bomb) / 16, 0, 0x0800001c, 0);
  std::memcpy(ee + body_tag + 16, &bomb, sizeof(bomb));
  const u32 finish_offset = body_tag + 16 + sizeof(bomb);
  put_tag(finish_offset, DmaTag::Kind::CNT, 0, 0, kPcPort | 13, 0);
  put_tag(finish_offset + 16, DmaTag::Kind::NEXT, 0, kCommonWaterDirectOffset);
  put_tag(kCommonWaterDirectOffset, DmaTag::Kind::CNT, 10, 0, kFlusha, kDirect | 10);
  put_tag(kCommonWaterDirectOffset + 176, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
}

metal_renderer::Jak2Opcode27LayerValues identity_layer_values() {
  metal_renderer::Jak2Opcode27LayerValues values;
  values.color = {1.f, 1.f, 1.f, 1.f};
  values.scale = {1.f, 1.f};
  values.offset = {0.5f, 0.5f};
  values.st_scale = {1.f, 1.f};
  values.st_offset = {0.5f, 0.5f};
  values.qs = {1.f, 1.f, 1.f, 1.f};
  return values;
}

void make_water_security_chain() {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr s64 kMode = -1;
  const u32 bucket_offset = kChainOffset + kWaterSecurityBucket * 16;

  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kWaterSecurityDescriptorOffset);
  put_tag(kWaterSecurityDescriptorOffset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
  constexpr u64 kPageOffset = kTexturePageOffset;
  std::memcpy(ee + kWaterSecurityDescriptorOffset + 16, &kPageOffset, sizeof(kPageOffset));
  std::memcpy(ee + kWaterSecurityDescriptorOffset + 24, &kMode, sizeof(kMode));
  put_tag(kWaterSecurityDescriptorOffset + 32, DmaTag::Kind::NEXT, 0,
          kWaterSecurityAnimatorOffset);

  put_tag(kWaterSecurityAnimatorOffset, DmaTag::Kind::CNT, 0, 0, kPcPort | 12, 0);
  const u32 animator_body_tag = kWaterSecurityAnimatorOffset + 16;
  put_tag(animator_body_tag, DmaTag::Kind::CNT, 52, 0, kPcPort | 30, 0);
  metal_renderer::Jak2Opcode30SecurityPlan plan;
  plan.environment.time = 0.f;
  plan.environment.destination_tbp = 0x760;
  for (auto& layer : plan.environment.layers) {
    layer.start = identity_layer_values();
    layer.end = identity_layer_values();
  }
  plan.dot.time = 0.f;
  plan.dot.destination_tbp = 0x761;
  for (auto& layer : plan.dot.layers) {
    layer.start = identity_layer_values();
    layer.end = identity_layer_values();
  }
  std::memcpy(ee + animator_body_tag + 16, &plan, sizeof(plan));
  const u32 animator_finish = animator_body_tag + 16 + sizeof(plan);
  put_tag(animator_finish, DmaTag::Kind::CNT, 0, 0, kPcPort | 13, 0);
  put_tag(animator_finish + 16, DmaTag::Kind::NEXT, 0, kWaterSecurityDirectOffset);

  put_tag(kWaterSecurityDirectOffset, DmaTag::Kind::CNT, 10, 0, kFlusha, kDirect | 10);
  put_tag(kWaterSecurityDirectOffset + 176, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
}

void make_pris_ordinary_only_chain() {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr s64 kMode = -1;
  constexpr u64 kPageOffset = kTexturePageOffset;
  const u32 bucket_offset = kChainOffset + kPrisOrdinaryBucket * 16;

  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kPrisOrdinaryDescriptorOffset);
  put_tag(kPrisOrdinaryDescriptorOffset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
  std::memcpy(ee + kPrisOrdinaryDescriptorOffset + 16, &kPageOffset, sizeof(kPageOffset));
  std::memcpy(ee + kPrisOrdinaryDescriptorOffset + 24, &kMode, sizeof(kMode));
  put_tag(kPrisOrdinaryDescriptorOffset + 32, DmaTag::Kind::NEXT, 0,
          kPrisOrdinaryDirectOffset);
  put_tag(kPrisOrdinaryDirectOffset, DmaTag::Kind::CNT, 10, 0, kFlusha, kDirect | 10);
  std::memset(ee + kPrisOrdinaryDirectOffset + 16, 0x52, 160);
  put_tag(kPrisOrdinaryDirectOffset + 176, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
}

void make_pris2_ordinary_only_chain(u32 bucket_id) {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr s64 kMode = -1;
  constexpr u64 kPageOffset = kTexturePageOffset;
  const u32 bucket_offset = kChainOffset + bucket_id * 16;

  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kPris2DescriptorOffset);
  put_tag(kPris2DescriptorOffset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
  std::memcpy(ee + kPris2DescriptorOffset + 16, &kPageOffset, sizeof(kPageOffset));
  std::memcpy(ee + kPris2DescriptorOffset + 24, &kMode, sizeof(kMode));
  put_tag(kPris2DescriptorOffset + 32, DmaTag::Kind::NEXT, 0, kPris2DirectOffset);
  put_tag(kPris2DirectOffset, DmaTag::Kind::CNT, 10, 0, kFlusha, kDirect | 10);
  std::memset(ee + kPris2DirectOffset + 16, 0x52, 160);
  put_tag(kPris2DirectOffset + 176, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
}

void make_sky_post_texture_upload_chain(s64 mode = -1) {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr u64 kPageOffset = kTexturePageOffset;
  const u32 bucket_offset = kChainOffset + kSkyPostBucket * 16;

  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kSkyPostGroupOffset);
  put_tag(kSkyPostGroupOffset, DmaTag::Kind::CNT, 2, 0, 0, kDirect | 2);
  std::memset(ee + kSkyPostGroupOffset + 16, 0, 32);
  const u32 descriptor_offset = kSkyPostGroupOffset + 48;
  put_tag(descriptor_offset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
  std::memcpy(ee + descriptor_offset + 16, &kPageOffset, sizeof(kPageOffset));
  std::memcpy(ee + descriptor_offset + 24, &mode, sizeof(mode));
  put_tag(descriptor_offset + 32, DmaTag::Kind::NEXT, 0, kSkyPostDirectOffset);
  put_tag(kSkyPostDirectOffset, DmaTag::Kind::CNT, 10, 0, kFlusha, kDirect | 10);
  std::memset(ee + kSkyPostDirectOffset + 16, 0, 160);
  put_tag(kSkyPostDirectOffset + 176, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
}

void write_empty_texture_page(u32 offset, u32 id);

void make_warp_texture_upload_chain(u32 upload_count, s64 mode = -1) {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  const u32 bucket_offset = kChainOffset + kWarpTextureUploadBucket * 16;
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kWarpTextureUploadGroupOffset);
  for (u32 i = 0; i < upload_count; ++i) {
    const u32 group = kWarpTextureUploadGroupOffset + i * kWarpTextureUploadGroupStride;
    const u32 next = i + 1 == upload_count ? kWarpTextureUploadTailOffset
                                           : group + kWarpTextureUploadGroupStride;
    put_tag(group, DmaTag::Kind::CNT, 2, 0, 0, kDirect | 2);
    std::memset(ee + group + 16, static_cast<int>(0x20 + i), 32);
    const u32 descriptor = group + 48;
    const u64 page_offset = kTexturePageOffset + i * kTexturePageStride;
    put_tag(descriptor, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
    std::memcpy(ee + descriptor + 16, &page_offset, sizeof(page_offset));
    std::memcpy(ee + descriptor + 24, &mode, sizeof(mode));
    put_tag(group + 80, DmaTag::Kind::NEXT, 0, next);
    write_empty_texture_page(static_cast<u32>(page_offset), kTexturePageId + i);
  }
  put_tag(kWarpTextureUploadTailOffset, DmaTag::Kind::CNT, 10, 0, kFlusha, kDirect | 10);
  std::memset(ee + kWarpTextureUploadTailOffset + 16, 0x9a, 160);
  put_tag(kWarpTextureUploadTailOffset + 176, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
}

void make_warp_texture_mixed_chain() {
  make_warp_texture_upload_chain(1);
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  put_tag(kWarpTextureUploadGroupOffset + 80, DmaTag::Kind::NEXT, 0,
          kWarpTextureUploadAnimatorOffset);
  put_tag(kWarpTextureUploadAnimatorOffset, DmaTag::Kind::CNT, 0, 0, kPcPort | 12, 0);
  put_tag(kWarpTextureUploadAnimatorOffset + 16, DmaTag::Kind::NEXT, 0,
          kWarpTextureUploadTailOffset);
}

void make_gmerc_warp_chain(u32 fragments, bool short_setup = false) {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  const u32 bucket_offset = kChainOffset + kGmercWarpBucket * 16;
  const u32 next_bucket = bucket_offset + 16;
  std::memset(ee + kGmercWarpPayloadOffset, 0, 0x1800);
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kGmercWarpPayloadOffset,
          short_setup ? 0 : gmerc_warp_vif(VifCode::Kind::MARK), 0);
  u32 cursor = kGmercWarpPayloadOffset;
  auto append = [&](const std::vector<u8>& payload, u32 vif0, u32 vif1,
                    DmaTag::Kind kind = DmaTag::Kind::CNT, u32 address = 0) {
    put_tag(cursor, kind, static_cast<u16>(payload.size() / 16), address, vif0, vif1);
    if (!payload.empty()) {
      std::memcpy(ee + cursor + 16, payload.data(), payload.size());
    }
    cursor += 16 + static_cast<u32>(payload.size());
  };

  append(make_gmerc_warp_zbuf_direct(), 0, gmerc_warp_vif(VifCode::Kind::DIRECT, 2));
  append(make_gmerc_warp_constants(), gmerc_warp_stcycl(4, 4),
         gmerc_warp_vif(VifCode::Kind::UNPACK_V4_32, 0x381, 8));
  if (short_setup) {
    append(std::vector<u8>(32, 0x56), gmerc_warp_vif(VifCode::Kind::MSCALF),
           gmerc_warp_vif(VifCode::Kind::STMOD), DmaTag::Kind::NEXT, next_bucket);
    return;
  }
  append(std::vector<u8>(32, 0x56), gmerc_warp_vif(VifCode::Kind::MSCALF),
         gmerc_warp_vif(VifCode::Kind::STMOD));
  append({}, 0, 0);
  if (fragments == 1) {
    append(make_gmerc_warp_fragment(), gmerc_warp_stcycl(4, 4),
           gmerc_warp_vif(VifCode::Kind::UNPACK_V4_32, 0x8000, 12));
  } else if (fragments == 2) {
    append(make_gmerc_warp_continued_fragment_transfer(), gmerc_warp_stcycl(4, 4),
           gmerc_warp_vif(VifCode::Kind::UNPACK_V4_32, 0x8000, 12));
    std::vector<u8> positions;
    constexpr float x[4] = {128.f, -128.f, 128.f, -128.f};
    constexpr float y[4] = {128.f, 128.f, -128.f, -128.f};
    for (int i = 0; i < 4; ++i) {
      append_gmerc_warp_float(&positions, x[i]);
      append_gmerc_warp_float(&positions, y[i]);
      append_gmerc_warp_float(&positions, -1.f);
    }
    append(positions, 0, gmerc_warp_vif(VifCode::Kind::UNPACK_V3_32, 0, 4));
    append({}, 0, gmerc_warp_vif(VifCode::Kind::MSCAL, 0x24));
  }
  append(std::vector<u8>(160, 0xde), gmerc_warp_vif(VifCode::Kind::FLUSHA),
         gmerc_warp_vif(VifCode::Kind::DIRECT, 10));
  append({}, 0, 0, DmaTag::Kind::NEXT, next_bucket);
}

void make_malformed_gmerc_warp_chain() {
  make_gmerc_warp_chain(1);
  const u32 nop = 0;
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + kGmercWarpPayloadOffset + 12, &nop,
              sizeof(nop));
}

void make_subtitle_mixed_capture_chain() {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  const u32 bucket_offset = kChainOffset + kSubtitleBucket * 16;
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kSubtitleCaptureOffset);
  put_tag(kSubtitleCaptureOffset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
  put_tag(kSubtitleCaptureOffset + 32, DmaTag::Kind::CNT, 0, 0, kPcPort | 12, 0);
  put_tag(kSubtitleCaptureOffset + 48, DmaTag::Kind::CNT, 1, 0, kPcPort | 22, 0);
  put_tag(kSubtitleCaptureOffset + 80, DmaTag::Kind::CNT, 0, 0, kPcPort | 13, 0);
  put_tag(kSubtitleCaptureOffset + 96, DmaTag::Kind::CNT, 10, 0, kFlusha, kDirect | 10);
  put_tag(kSubtitleCaptureOffset + 272, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
  std::memset(ee + kSubtitleCaptureOffset + 16, 0, 16);
  std::memset(ee + kSubtitleCaptureOffset + 64, 0, 16);
  std::memset(ee + kSubtitleCaptureOffset + 112, 0, 160);
}

void make_subtitle_direct_only_chain() {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  const u32 bucket_offset = kChainOffset + kSubtitleBucket * 16;
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kSubtitleCaptureOffset);
  put_tag(kSubtitleCaptureOffset, DmaTag::Kind::CNT, 10, 0, kFlusha, kDirect | 10);
  put_tag(kSubtitleCaptureOffset + 176, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
  std::memset(ee + kSubtitleCaptureOffset + 16, 0, 160);
}

void make_subtitle_capture_malformed_chain() {
  make_empty_chain();
  const u32 bucket_offset = kChainOffset + kSubtitleBucket * 16;
  put_tag(bucket_offset, DmaTag::Kind::CALL, 0, kSubtitleMalformedOffset);
  put_tag(kSubtitleMalformedOffset, DmaTag::Kind::RET);
}

void make_common_pris_opcode22_capture_chain() {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr s64 kMode = -1;
  constexpr u64 kPageOffset = kTexturePageOffset;
  const u32 bucket_offset = kChainOffset + kCommonPrisBucket * 16;

  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kCommonPrisDescriptorOffset);
  put_tag(kCommonPrisDescriptorOffset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
  std::memcpy(ee + kCommonPrisDescriptorOffset + 16, &kPageOffset, sizeof(kPageOffset));
  std::memcpy(ee + kCommonPrisDescriptorOffset + 24, &kMode, sizeof(kMode));
  put_tag(kCommonPrisDescriptorOffset + 32, DmaTag::Kind::NEXT, 0,
          kCommonPrisAnimatorOffset);
  put_tag(kCommonPrisAnimatorOffset, DmaTag::Kind::CNT, 0, 0, kPcPort | 12, 0);
  put_tag(kCommonPrisAnimatorOffset + 16, DmaTag::Kind::CNT, 2, 0,
          kPcPort | 22, 0);
  constexpr float kMorph = 0.5f;
  constexpr std::array<u32, 4> kTbps = {0x1200, 0x1210, 0x1220, 0x1230};
  std::memcpy(ee + kCommonPrisAnimatorOffset + 32, &kMorph, sizeof(kMorph));
  for (u32 i = 0; i < 12; ++i) {
    ee[kCommonPrisAnimatorOffset + 36 + i] = static_cast<u8>(0xa0 + i);
  }
  std::memcpy(ee + kCommonPrisAnimatorOffset + 48, kTbps.data(), sizeof(kTbps));
  put_tag(kCommonPrisAnimatorOffset + 64, DmaTag::Kind::CNT, 0, 0,
          kPcPort | 13, 0);
  put_tag(kCommonPrisAnimatorOffset + 80, DmaTag::Kind::NEXT, 0,
          kCommonPrisDirectOffset);
  put_tag(kCommonPrisDirectOffset, DmaTag::Kind::CNT, 10, 0, kFlusha, kDirect | 10);
  std::memset(ee + kCommonPrisDirectOffset + 16, 0x52, 160);
  put_tag(kCommonPrisDirectOffset + 176, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
}

void write_prison_clut_bucket(float morph,
                              u32 bucket_id,
                              u32 descriptor_offset,
                              u32 animator_offset,
                              u32 direct_offset,
                              u32 destination_tbp_bias) {
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr s64 kMode = -1;
  constexpr u64 kPageOffset = kTexturePageOffset;
  const std::array<u32, 7> destination_tbps = {
      0x1000 + destination_tbp_bias, 0x1010 + destination_tbp_bias,
      0x1020 + destination_tbp_bias, 0x1030 + destination_tbp_bias,
      0x1040 + destination_tbp_bias, 0x1050 + destination_tbp_bias,
      0x1060 + destination_tbp_bias};
  const u32 bucket_offset = kChainOffset + bucket_id * 16;

  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, descriptor_offset);
  put_tag(descriptor_offset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
  std::memcpy(ee + descriptor_offset + 16, &kPageOffset, sizeof(kPageOffset));
  std::memcpy(ee + descriptor_offset + 24, &kMode, sizeof(kMode));
  put_tag(descriptor_offset + 32, DmaTag::Kind::NEXT, 0, animator_offset);

  put_tag(animator_offset, DmaTag::Kind::CNT, 0, 0, kPcPort | 12, 0);
  const u32 body_tag = animator_offset + 16;
  put_tag(body_tag, DmaTag::Kind::CNT, 3, 0, kPcPort | 23, 0);
  std::memset(ee + body_tag + 16, 0, 48);
  std::memcpy(ee + body_tag + 16, &morph, sizeof(morph));
  std::memcpy(ee + body_tag + 32, destination_tbps.data(), sizeof(destination_tbps));
  const u32 finish_tag = body_tag + 16 + 48;
  put_tag(finish_tag, DmaTag::Kind::CNT, 0, 0, kPcPort | 13, 0);
  put_tag(finish_tag + 16, DmaTag::Kind::NEXT, 0, direct_offset);

  put_tag(direct_offset, DmaTag::Kind::CNT, 10, 0, kFlusha, kDirect | 10);
  std::memset(ee + direct_offset + 16, 0x53, 160);
  put_tag(direct_offset + 176, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
}

void make_prison_clut_chain(float morph,
                            u32 bucket_id = kPrisonClutBucket,
                            u32 destination_tbp_bias = 0) {
  make_empty_chain();
  write_prison_clut_bucket(morph, bucket_id, kPrisonClutDescriptorOffset,
                           kPrisonClutAnimatorOffset, kPrisonClutDirectOffset,
                           destination_tbp_bias);
}

void make_duplicate_prison_clut_chain() {
  make_empty_chain();
  write_prison_clut_bucket(0.5f, kPrisonClutBucket, kPrisonClutDescriptorOffset,
                           kPrisonClutAnimatorOffset, kPrisonClutDirectOffset, 0);
  write_prison_clut_bucket(0.75f, kOtherPrisAnimatorBucket,
                           kDuplicatePrisonClutDescriptorOffset,
                           kDuplicatePrisonClutAnimatorOffset,
                           kDuplicatePrisonClutDirectOffset, 0x100);
}

void put_u64(std::array<u8, kGifBytes>& payload, std::size_t offset, u64 value) {
  std::memcpy(payload.data() + offset, &value, sizeof(value));
}

void put_rgbaq(std::array<u8, kGifBytes>& payload, std::size_t offset) {
  constexpr std::array<u32, 4> kGreen = {0, 255, 0, 128};
  std::memcpy(payload.data() + offset, kGreen.data(), 16);
}

void put_xyzf2(std::array<u8, kGifBytes>& payload, std::size_t offset, u32 x, u32 y) {
  constexpr u64 kZ = 0xffffff;
  std::memcpy(payload.data() + offset, &x, sizeof(x));
  std::memcpy(payload.data() + offset + 4, &y, sizeof(y));
  put_u64(payload, offset + 8, kZ << 4);
}

std::array<u8, kGifBytes> make_direct_triangle() {
  std::array<u8, kGifBytes> payload = {};
  constexpr u64 kNloop = 1;
  constexpr u64 kEop = 1ull << 15;
  constexpr u64 kPre = 1ull << 46;
  constexpr u64 kPrim = static_cast<u64>(GsPrim::Kind::TRI) | (1ull << 3) | (1ull << 6);
  constexpr u64 kNreg = 6ull << 60;
  put_u64(payload, 0, kNloop | kEop | kPre | (kPrim << 47) | kNreg);

  constexpr u64 kRgbaq = static_cast<u64>(GifTag::RegisterDescriptor::RGBAQ);
  constexpr u64 kXyzf2 = static_cast<u64>(GifTag::RegisterDescriptor::XYZF2);
  constexpr u64 kRegisters =
      kRgbaq | (kXyzf2 << 4) | (kRgbaq << 8) | (kXyzf2 << 12) | (kRgbaq << 16) | (kXyzf2 << 20);
  put_u64(payload, 8, kRegisters);

  put_rgbaq(payload, 16);
  put_xyzf2(payload, 32, 0x8000, 0x7800);
  put_rgbaq(payload, 48);
  put_xyzf2(payload, 64, 0x7800, 0x8800);
  put_rgbaq(payload, 80);
  put_xyzf2(payload, 96, 0x8800, 0x8800);
  return payload;
}

void make_direct_chain(u32 bucket, u32 payload_offset) {
  make_empty_chain();
  const auto payload = make_direct_triangle();
  const u32 bucket_offset = kChainOffset + bucket * 16;
  const u32 next_bucket_offset = bucket_offset + 16;
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, payload_offset);
  const u32 direct = (static_cast<u32>(VifCode::Kind::DIRECT) << 24) | static_cast<u32>(kGifQwords);
  put_tag(payload_offset, DmaTag::Kind::CNT, static_cast<u16>(kGifQwords), 0, 0, direct);
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + payload_offset + 16, payload.data(), payload.size());
  put_tag(payload_offset + 16 + kGifBytes, DmaTag::Kind::NEXT, 0, next_bucket_offset);
}

void make_screen_filter_chain() {
  make_direct_chain(kScreenFilterBucket, kScreenFilterPayloadOffset);
}

void make_sky_draw_chain() {
  static_assert(kSkyDrawBucket == 5);
  make_direct_chain(kSkyDrawBucket, kSkyDrawPayloadOffset);
}

void make_debug_no_zbuf2_chain() {
  static_assert(kDebugNoZbuf2Bucket == 325);
  make_direct_chain(kDebugNoZbuf2Bucket, kDebugNoZbuf2PayloadOffset);
}

void put_direct_texflush_payload(u32 offset, u16 qwc) {
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  const u64 gif_tag = static_cast<u64>(qwc - 1) | (1ull << 15) | (1ull << 60);
  const u64 ad = static_cast<u64>(GifTag::RegisterDescriptor::AD);
  std::memcpy(ee + offset, &gif_tag, sizeof(gif_tag));
  std::memcpy(ee + offset + 8, &ad, sizeof(ad));
  for (u16 i = 1; i < qwc; ++i) {
    const u64 texflush = static_cast<u64>(GsRegisterAddress::TEXFLUSH);
    std::memcpy(ee + offset + i * 16 + 8, &texflush, sizeof(texflush));
  }
}

void make_sprite_texture_upload_chain(u32 upload_count = 1, s64 mode = -1) {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  std::memset(ee + kSpriteTextureUploadGroupOffset, 0,
              kSpriteTextureUploadGroupStride * upload_count);
  std::memset(ee + kSpriteTextureUploadTailOffset, 0, 192);

  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  const u32 bucket_offset = kChainOffset + kSpriteTextureUploadBucket * 16;

  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kSpriteTextureUploadGroupOffset);
  for (u32 i = 0; i < upload_count; ++i) {
    const u32 group_offset = kSpriteTextureUploadGroupOffset + i * kSpriteTextureUploadGroupStride;
    const u32 next_offset = i + 1 == upload_count
                                ? kSpriteTextureUploadTailOffset
                                : group_offset + kSpriteTextureUploadGroupStride;
    put_tag(group_offset, DmaTag::Kind::CNT, 2, 0, 0, kDirect | 2);
    const u32 descriptor_offset = group_offset + 48;
    put_tag(descriptor_offset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
    const u64 page_offset = kTexturePageOffset + i * kTexturePageStride;
    std::memcpy(ee + descriptor_offset + 16, &page_offset, sizeof(page_offset));
    std::memcpy(ee + descriptor_offset + 24, &mode, sizeof(mode));
    put_tag(descriptor_offset + 32, DmaTag::Kind::NEXT, 0, next_offset);
  }

  put_tag(kSpriteTextureUploadTailOffset, DmaTag::Kind::CNT, 10, 0, kFlusha,
          kDirect | 10);
  put_tag(kSpriteTextureUploadTailOffset + 176, DmaTag::Kind::NEXT, 0,
          bucket_offset + 16);
}

void put_map_texture_upload(u32 upload_count = 1, s64 mode = -1) {
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  std::memset(ee + kMapTextureUploadGroupOffset, 0, 0x100 * upload_count);
  std::memset(ee + kMapTextureUploadTailOffset, 0, 192);

  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  const u32 bucket_offset = kChainOffset + kMapTextureUploadBucket * 16;
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kMapTextureUploadGroupOffset);
  for (u32 i = 0; i < upload_count; ++i) {
    const u32 group_offset = kMapTextureUploadGroupOffset + i * 0x100;
    const u32 next_offset = i + 1 == upload_count
                                ? kMapTextureUploadTailOffset
                                : group_offset + 0x100;
    put_tag(group_offset, DmaTag::Kind::CNT, 2, 0, 0, kDirect | 2);
    put_direct_texflush_payload(group_offset + 16, 2);
    const u32 descriptor_offset = group_offset + 48;
    put_tag(descriptor_offset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
    const u64 page_offset = kTexturePageOffset + i * kTexturePageStride;
    std::memcpy(ee + descriptor_offset + 16, &page_offset, sizeof(page_offset));
    std::memcpy(ee + descriptor_offset + 24, &mode, sizeof(mode));
    put_tag(descriptor_offset + 32, DmaTag::Kind::NEXT, 0, next_offset);
  }

  put_tag(kMapTextureUploadTailOffset, DmaTag::Kind::CNT, 10, 0, kFlusha,
          kDirect | 10);
  put_direct_texflush_payload(kMapTextureUploadTailOffset + 16, 10);
  put_tag(kMapTextureUploadTailOffset + 176, DmaTag::Kind::NEXT, 0,
          bucket_offset + 16);
}

void put_map_descriptor_first_upload(u32 upload_count = 1, s64 mode = -1) {
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  std::memset(ee + kMapTextureUploadGroupOffset, 0, 0x100 * upload_count);

  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  const u32 bucket_offset = kChainOffset + kMapTextureUploadBucket * 16;
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kMapTextureUploadGroupOffset);
  for (u32 i = 0; i < upload_count; ++i) {
    const u32 descriptor_offset = kMapTextureUploadGroupOffset + i * 0x100;
    const u32 next_offset =
        i + 1 == upload_count ? bucket_offset + 16 : descriptor_offset + 0x100;
    put_tag(descriptor_offset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
    const u64 page_offset = kTexturePageOffset + i * kTexturePageStride;
    std::memcpy(ee + descriptor_offset + 16, &page_offset, sizeof(page_offset));
    std::memcpy(ee + descriptor_offset + 24, &mode, sizeof(mode));
    put_tag(descriptor_offset + 32, DmaTag::Kind::NEXT, 0, next_offset);
  }
}

void put_map_descriptor_prefix_legacy_upload() {
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  std::memset(ee + kMapTextureUploadGroupOffset, 0, 0x200);
  std::memset(ee + kMapTextureUploadTailOffset, 0, 192);

  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr s64 kMode = -1;
  const u32 bucket_offset = kChainOffset + kMapTextureUploadBucket * 16;
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kMapTextureUploadGroupOffset);

  put_tag(kMapTextureUploadGroupOffset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
  const u64 prefix_page_offset = kTexturePageOffset;
  std::memcpy(ee + kMapTextureUploadGroupOffset + 16, &prefix_page_offset,
              sizeof(prefix_page_offset));
  std::memcpy(ee + kMapTextureUploadGroupOffset + 24, &kMode, sizeof(kMode));
  put_tag(kMapTextureUploadGroupOffset + 32, DmaTag::Kind::NEXT, 0,
          kMapTextureUploadGroupOffset + 0x100);

  const u32 legacy_group_offset = kMapTextureUploadGroupOffset + 0x100;
  put_tag(legacy_group_offset, DmaTag::Kind::CNT, 2, 0, 0, kDirect | 2);
  put_direct_texflush_payload(legacy_group_offset + 16, 2);
  const u32 descriptor_offset = legacy_group_offset + 48;
  put_tag(descriptor_offset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
  const u64 legacy_page_offset = kTexturePageOffset + kTexturePageStride;
  std::memcpy(ee + descriptor_offset + 16, &legacy_page_offset, sizeof(legacy_page_offset));
  std::memcpy(ee + descriptor_offset + 24, &kMode, sizeof(kMode));
  put_tag(descriptor_offset + 32, DmaTag::Kind::NEXT, 0, kMapTextureUploadTailOffset);

  put_tag(kMapTextureUploadTailOffset, DmaTag::Kind::CNT, 10, 0, kFlusha,
          kDirect | 10);
  put_direct_texflush_payload(kMapTextureUploadTailOffset + 16, 10);
  put_tag(kMapTextureUploadTailOffset + 176, DmaTag::Kind::NEXT, 0,
          bucket_offset + 16);
}

bool is_zero(const goal_jak2_metal_frame_summary& summary) {
  return summary.width == 0 && summary.height == 0 && summary.byte_count == 0 &&
         summary.hash == 0 && summary.non_black_pixels == 0 &&
         summary.nonzero_alpha_pixels == 0 && summary.max_alpha == 0;
}

bool sky_batch_is_zero(const goal_jak2_metal_host_metrics& metrics) {
  return metrics.last_sky_draw_batch_valid == 0 &&
         metrics.last_sky_draw_batch_textured == 0 &&
         metrics.last_sky_draw_batch_vertices == 0 &&
         metrics.last_sky_draw_batch_nonzero_rgb_vertices == 0 &&
         metrics.last_sky_draw_batch_tex0_tbp == 0 &&
         metrics.last_sky_draw_batch_tex0_tcc == 0 &&
         metrics.last_sky_draw_batch_tex0_decal == 0 &&
         metrics.last_sky_draw_batch_texture_lookup_hit == 0 &&
         metrics.last_sky_draw_batch_used_placeholder == 0 &&
         metrics.last_sky_draw_batch_write_rgb == 0 &&
         metrics.last_sky_draw_batch_blend_enabled == 0 &&
         metrics.last_sky_draw_batch_blend_a == 0 &&
         metrics.last_sky_draw_batch_blend_b == 0 &&
         metrics.last_sky_draw_batch_blend_c == 0 &&
         metrics.last_sky_draw_batch_blend_d == 0 &&
         metrics.last_sky_draw_batch_alpha_test_enabled == 0 &&
         metrics.last_sky_draw_batch_alpha_test_mode == 0 &&
         metrics.last_sky_draw_batch_alpha_aref == 0 &&
         metrics.last_sky_draw_batch_alpha_afail == 0;
}

template <std::size_t BucketCount>
bool texture_captures_are_empty(
    const goal_jak2_tfrag_texture_upload_metrics* uploads,
    const std::array<u32, BucketCount>& kBuckets) {
  for (std::size_t i = 0; i < kBuckets.size(); ++i) {
    const auto& upload = uploads[i];
    if (upload.bucket_id != kBuckets[i] || upload.captures != 1 ||
        upload.present_captures != 0 || upload.classifications[1] != 1 ||
        upload.transfers != 1 || upload.inert_transfers != 1 || upload.payload_bytes != 0 ||
        upload.ordinary_descriptors != 0 || upload.animator_arrays != 0 ||
        upload.eye_markers != 0 || upload.other_transfers != 0 ||
        upload.malformed_transfers != 0) {
      return false;
    }
  }
  return true;
}

bool texture_capture_is_empty(const goal_jak2_tfrag_texture_upload_metrics& upload,
                              u32 bucket_id) {
  return upload.bucket_id == bucket_id && upload.captures == 1 &&
         upload.present_captures == 0 && upload.classifications[1] == 1 &&
         upload.transfers == 1 && upload.inert_transfers == 1 && upload.payload_bytes == 0 &&
         upload.ordinary_descriptors == 0 && upload.animator_arrays == 0 &&
         upload.eye_markers == 0 && upload.other_transfers == 0 &&
         upload.malformed_transfers == 0;
}

tfrag3::IndexTexture synthetic_dark_jak_index_texture(std::string_view name, u8 bias) {
  tfrag3::IndexTexture texture;
  texture.w = 2;
  texture.h = 2;
  texture.index_data = {0, 1, 2, 3};
  texture.level_names = {"GAME.DGO"};
  texture.name = name;
  texture.tpage_name = "synthetic-dark-jak-clut";
  for (std::size_t entry = 0; entry < texture.color_table.size(); ++entry) {
    texture.color_table[entry][0] = static_cast<u8>(entry + bias);
    texture.color_table[entry][1] = static_cast<u8>(entry + bias + 1);
    texture.color_table[entry][2] = static_cast<u8>(entry + bias + 2);
    texture.color_table[entry][3] = static_cast<u8>(255 - entry);
  }
  return texture;
}

void add_dark_jak_sources(tfrag3::Level* level) {
  constexpr std::array<std::array<std::string_view, 3>, 4> kNames = {{
      {"jakbsmall-eyebrow", "jakbsmall-eyebrow-norm", "jakbsmall-eyebrow-dark"},
      {"jakbsmall-face", "jakbsmall-face-norm", "jakbsmall-face-dark"},
      {"jakbsmall-finger", "jakbsmall-finger-norm", "jakbsmall-finger-dark"},
      {"jakbsmall-hair", "jakbsmall-hair-norm", "jakbsmall-hair-dark"},
  }};
  for (std::size_t slot = 0; slot < kNames.size(); ++slot) {
    level->index_textures.push_back(synthetic_dark_jak_index_texture(kNames[slot][0], 0));
    level->index_textures.push_back(
        synthetic_dark_jak_index_texture(kNames[slot][1], static_cast<u8>(slot * 8 + 4)));
    level->index_textures.push_back(
        synthetic_dark_jak_index_texture(kNames[slot][2], static_cast<u8>(slot * 8 + 20)));
  }
}

tfrag3::Texture synthetic_source_texture(const char* name, u32 color);

bool write_synthetic_fr3(const std::filesystem::path& path,
                         const std::string& level_name,
                         bool with_texture) {
  tfrag3::Level level;
  level.level_name = level_name;
  if (with_texture) {
    tfrag3::Texture texture;
    texture.w = 16;
    texture.h = 16;
    texture.combo_id = (static_cast<u32>(kTexturePageId) << 16);
    texture.data.resize(16 * 16, 0xff40c020);
    texture.debug_name = "host-residency-texture";
    texture.debug_tpage_name = "host-residency-page";
    texture.load_to_pool = true;
    level.textures.push_back(std::move(texture));
    level.textures.push_back(synthetic_source_texture("bomb-gradient", 0xff000000));
    level.textures.push_back(synthetic_source_texture("bomb-gradient-rim", 0xff204060));
    level.textures.push_back(synthetic_source_texture("bomb-gradient-flames", 0xff604020));
    level.textures.push_back(synthetic_source_texture("security-env-dest", 0xff000000));
    level.textures.push_back(synthetic_source_texture("security-env-uscroll", 0xff102030));
    add_dark_jak_sources(&level);
  }

  Serializer serializer;
  level.serialize(serializer);
  const auto serialized = serializer.get_save_result();
  const auto compressed = compression::compress_zstd(serialized.first, serialized.second);
  if (compressed.empty()) {
    return false;
  }
  file_util::write_binary_file(path, compressed.data(), compressed.size());
  return std::filesystem::exists(path);
}

tfrag3::Texture synthetic_source_texture(const char* name, u32 color) {
  tfrag3::Texture texture;
  texture.w = 2;
  texture.h = 2;
  texture.data.assign(4, color);
  texture.debug_name = name;
  texture.debug_tpage_name = "synthetic-security-water";
  texture.load_to_pool = false;
  return texture;
}

tfrag3::IndexTexture synthetic_prison_index_texture(std::string name, u8 bias) {
  tfrag3::IndexTexture texture;
  texture.w = 2;
  texture.h = 2;
  texture.index_data = {0, 1, 2, 3};
  texture.level_names = {"LDJAKBRN.DGO"};
  texture.name = std::move(name);
  texture.tpage_name = "synthetic-prison-clut";
  for (std::size_t entry = 0; entry < texture.color_table.size(); ++entry) {
    texture.color_table[entry][0] = static_cast<u8>(entry + bias);
    texture.color_table[entry][1] = static_cast<u8>(entry + bias + 1);
    texture.color_table[entry][2] = static_cast<u8>(entry + bias + 2);
    texture.color_table[entry][3] = static_cast<u8>(255 - entry);
  }
  return texture;
}

bool write_prison_clut_fr3(const std::filesystem::path& path) {
  constexpr std::array<std::array<const char*, 3>, 6> kNames = {{
      {"jak-orig-arm-formorph", "jak-orig-arm-formorph-start",
       "jak-orig-arm-formorph-end"},
      {"jak-orig-eyebrow-formorph", "jak-orig-eyebrow-formorph-start",
       "jak-orig-eyebrow-formorph-end"},
      {"jak-orig-finger-formorph", "jak-orig-finger-formorph-start",
       "jak-orig-finger-formorph-end"},
      {"jakb-facelft", "jakb-facelft-norm", "jakb-facelft-dark"},
      {"jakb-facert", "jakb-facert-norm", "jakb-facert-dark"},
      {"jakb-hairtrans", "jakb-hairtrans-norm", "jakb-hairtrans-dark"},
  }};
  tfrag3::Level level;
  level.level_name = "prison-clut-common";
  for (std::size_t slot = 0; slot < kNames.size(); ++slot) {
    level.index_textures.push_back(synthetic_prison_index_texture(kNames[slot][0], 0));
    level.index_textures.push_back(
        synthetic_prison_index_texture(kNames[slot][1], static_cast<u8>(slot * 8 + 4)));
    level.index_textures.push_back(
        synthetic_prison_index_texture(kNames[slot][2], static_cast<u8>(slot * 8 + 20)));
  }
  constexpr std::array<std::array<const char*, 3>, 4> kDarkJakNames = {{
      {"jakbsmall-eyebrow", "jakbsmall-eyebrow-norm", "jakbsmall-eyebrow-dark"},
      {"jakbsmall-face", "jakbsmall-face-norm", "jakbsmall-face-dark"},
      {"jakbsmall-finger", "jakbsmall-finger-norm", "jakbsmall-finger-dark"},
      {"jakbsmall-hair", "jakbsmall-hair-norm", "jakbsmall-hair-dark"},
  }};
  for (std::size_t slot = 0; slot < kDarkJakNames.size(); ++slot) {
    level.index_textures.push_back(synthetic_prison_index_texture(kDarkJakNames[slot][0], 0));
    level.index_textures.push_back(synthetic_prison_index_texture(
        kDarkJakNames[slot][1], static_cast<u8>(slot * 8 + 4)));
    level.index_textures.push_back(synthetic_prison_index_texture(
        kDarkJakNames[slot][2], static_cast<u8>(slot * 8 + 20)));
  }

  Serializer serializer;
  level.serialize(serializer);
  const auto serialized = serializer.get_save_result();
  const auto compressed = compression::compress_zstd(serialized.first, serialized.second);
  if (compressed.empty()) {
    return false;
  }
  file_util::write_binary_file(path, compressed.data(), compressed.size());
  return std::filesystem::exists(path);
}

bool write_security_fr3(const std::filesystem::path& path,
                        const std::string& level_name,
                        bool common) {
  tfrag3::Level level;
  level.level_name = level_name;
  if (common) {
    level.textures.push_back(synthetic_source_texture("common-white", 0xffffffff));
    level.textures.push_back(synthetic_source_texture("security-env-dest", 0xff000000));
    level.textures.push_back(synthetic_source_texture("security-env-uscroll", 0xff102030));
    add_dark_jak_sources(&level);
  } else {
    level.textures.push_back(synthetic_source_texture("security-env-dest", 0xff000000));
    level.textures.push_back(synthetic_source_texture("security-env-uscroll", 0xff102030));
    level.textures.push_back(synthetic_source_texture("security-dot-dest", 0xff000000));
    level.textures.push_back(synthetic_source_texture("security-dot-src", 0xff403020));
  }

  Serializer serializer;
  level.serialize(serializer);
  const auto serialized = serializer.get_save_result();
  const auto compressed = compression::compress_zstd(serialized.first, serialized.second);
  if (compressed.empty()) {
    return false;
  }
  file_util::write_binary_file(path, compressed.data(), compressed.size());
  return std::filesystem::exists(path);
}

void write_texture_page() {
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  GoalTexturePage page = {};
  page.id = kTexturePageId;
  page.length = 1;
  page.name_ptr = kTextureNameOffset;
  std::memcpy(ee + kTexturePageOffset, &page, sizeof(page));
  std::memcpy(ee + kTexturePageOffset + sizeof(page), &kTextureObjectOffset,
              sizeof(kTextureObjectOffset));

  GoalTexture texture = {};
  texture.w = 16;
  texture.h = 16;
  texture.num_mips = 1;
  texture.name_ptr = kTextureNameOffset;
  texture.dest[0] = kTextureVram;
  std::memcpy(ee + kTextureObjectOffset, &texture, sizeof(texture));
  const char name[] = "host-residency-texture";
  std::memcpy(ee + kTextureNameOffset + 4, name, sizeof(name));
}

void write_empty_texture_page(u32 offset, u32 id) {
  GoalTexturePage page = {};
  page.id = id;
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + offset, &page, sizeof(page));
}

void append_qword(std::vector<u8>* data, u64 low, u64 high) {
  const std::size_t offset = data->size();
  data->resize(offset + 16);
  std::memcpy(data->data() + offset, &low, sizeof(low));
  std::memcpy(data->data() + offset + 8, &high, sizeof(high));
}

void append_gif_tag(std::vector<u8>* data,
                    u32 loops,
                    u64 registers,
                    u32 register_count,
                    bool eop,
                    bool pre,
                    u64 prim) {
  const u64 low = loops | (static_cast<u64>(eop) << 15) | (static_cast<u64>(pre) << 46) |
                  (prim << 47) | (static_cast<u64>(register_count) << 60);
  append_qword(data, low, registers);
}

void append_st(std::vector<u8>* data, float s, float t) {
  const std::size_t offset = data->size();
  data->resize(offset + 16);
  const float q = 1.f;
  std::memcpy(data->data() + offset, &s, sizeof(s));
  std::memcpy(data->data() + offset + 4, &t, sizeof(t));
  std::memcpy(data->data() + offset + 8, &q, sizeof(q));
}

void append_rgbaq(std::vector<u8>* data) {
  constexpr std::array<u32, 4> kWhite = {128, 128, 128, 128};
  const std::size_t offset = data->size();
  data->resize(offset + 16);
  std::memcpy(data->data() + offset, kWhite.data(), 16);
}

void append_xyzf2(std::vector<u8>* data, u32 x, u32 y) {
  constexpr u64 kZ = 0xffffff;
  append_qword(data, static_cast<u64>(x) | (static_cast<u64>(y) << 32), kZ << 4);
}

void put_textured_direct_draw(u32 bucket, u32 payload_offset, u32 texture_vram) {
  std::vector<u8> payload;

  constexpr u64 kAd = static_cast<u64>(GifTag::RegisterDescriptor::AD);
  append_gif_tag(&payload, 3, kAd, 1, false, false, 0);
  const u64 tex0 = texture_vram | (1ull << 14) | (4ull << 26) | (4ull << 30) | (1ull << 34);
  append_qword(&payload, tex0, static_cast<u64>(GsRegisterAddress::TEX0_1));
  append_qword(&payload, (1ull << 5) | (1ull << 6),
               static_cast<u64>(GsRegisterAddress::TEX1_1));
  append_qword(&payload, 0b101, static_cast<u64>(GsRegisterAddress::CLAMP_1));

  constexpr u64 kSt = static_cast<u64>(GifTag::RegisterDescriptor::ST);
  constexpr u64 kRgbaq = static_cast<u64>(GifTag::RegisterDescriptor::RGBAQ);
  constexpr u64 kXyzf2 = static_cast<u64>(GifTag::RegisterDescriptor::XYZF2);
  constexpr u64 kRegisters = kSt | (kRgbaq << 4) | (kXyzf2 << 8);
  constexpr u64 kPrim = static_cast<u64>(GsPrim::Kind::TRI) | (1ull << 3) | (1ull << 4) |
                        (1ull << 6);
  append_gif_tag(&payload, 3, kRegisters, 3, true, true, kPrim);
  append_st(&payload, 0.f, 0.f);
  append_rgbaq(&payload);
  append_xyzf2(&payload, 0x8000, 0x7800);
  append_st(&payload, 0.f, 1.f);
  append_rgbaq(&payload);
  append_xyzf2(&payload, 0x7800, 0x8800);
  append_st(&payload, 1.f, 1.f);
  append_rgbaq(&payload);
  append_xyzf2(&payload, 0x8800, 0x8800);

  const u32 bucket_offset = kChainOffset + bucket * 16;
  const u32 next_bucket_offset = bucket_offset + 16;
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, payload_offset);
  const u32 direct = (static_cast<u32>(VifCode::Kind::DIRECT) << 24) |
                     static_cast<u32>(payload.size() / 16);
  put_tag(payload_offset, DmaTag::Kind::CNT, static_cast<u16>(payload.size() / 16), 0, 0,
          direct);
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + payload_offset + 16, payload.data(),
              payload.size());
  put_tag(payload_offset + 16 + payload.size(), DmaTag::Kind::NEXT, 0,
          next_bucket_offset);
}

void make_textured_sky_draw_chain(u32 texture_vram) {
  make_empty_chain();
  put_textured_direct_draw(kSkyDrawBucket, kSkyDrawPayloadOffset, texture_vram);
}

void put_blit_display_snapshot(bool copy_back) {
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  const u32 bucket_offset = kChainOffset + kBlitDisplayBucket * 16;
  std::memset(static_cast<u8*>(g_ee_main_mem) + kBlitDisplayPayloadOffset, 0, 0x80);
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kBlitDisplayPayloadOffset);
  u32 cursor = kBlitDisplayPayloadOffset;
  put_tag(cursor, DmaTag::Kind::CNT, 1, 0, kPcPort | 0x10,
          kPcPort | metal_renderer::kJak2BlitDisplayTbp);
  cursor += 32;
  if (copy_back) {
    put_tag(cursor, DmaTag::Kind::NEXT, 0, cursor + 16);
    cursor += 16;
    put_tag(cursor, DmaTag::Kind::CNT, 0, 0, kPcPort | 0x11, kPcPort);
    cursor += 16;
  }
  put_tag(cursor, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
}

void make_blit_snapshot_sky_draw_chain(bool copy_back) {
  make_empty_chain();
  put_blit_display_snapshot(copy_back);
  put_textured_direct_draw(kSkyDrawBucket, kSkyDrawPayloadOffset,
                           metal_renderer::kJak2BlitDisplayTbp);
}

void make_map_texture_upload_and_progress_chain(s64 mode = -1) {
  make_empty_chain();
  put_map_texture_upload(1, mode);
  put_textured_direct_draw(kProgressBucket, kProgressPayloadOffset, kTextureVram);
}

void make_map_descriptor_first_upload_and_progress_chain(s64 mode = -1) {
  make_empty_chain();
  put_map_descriptor_first_upload(1, mode);
  put_textured_direct_draw(kProgressBucket, kProgressPayloadOffset, kTextureVram);
}

void make_map_descriptor_prefix_legacy_upload_and_progress_chain() {
  make_empty_chain();
  put_map_descriptor_prefix_legacy_upload();
  put_textured_direct_draw(kProgressBucket, kProgressPayloadOffset, kTextureVram);
}

}  // namespace

int main() {
  const auto& host_policy_table = metal_renderer::jak2_metal_bucket_table();
  check(host_policy_table[static_cast<std::size_t>(jak2::BucketId::GMERC_L0_ALPHA)].behavior ==
                metal_renderer::Jak2MetalBucketBehavior::Generic2 &&
            host_policy_table[static_cast<std::size_t>(jak2::BucketId::GMERC_L5_WATER)].behavior ==
                metal_renderer::Jak2MetalBucketBehavior::Generic2 &&
            host_policy_table[kEffectsBucket].behavior ==
                metal_renderer::Jak2MetalBucketBehavior::EffectsLightning &&
            host_policy_table[kWarpTextureUploadBucket].behavior ==
                metal_renderer::Jak2MetalBucketBehavior::HostTextureUpload &&
            host_policy_table[kGmercWarpBucket].behavior ==
                metal_renderer::Jak2MetalBucketBehavior::Warp &&
            host_policy_table[kCommonWaterBucket].behavior ==
                metal_renderer::Jak2MetalBucketBehavior::HostTextureUpload &&
            metal_renderer::jak2_metal_host_policy_table_is_audited(),
        "b306 upload, b315 Lightning, b316 upload, and b317 framebuffer warp keep distinct policies");

  goal_jak2_metal_host_metrics frame_gate = {};
  frame_gate.chains = 1;
  frame_gate.completed_chains = 1;
  frame_gate.last_buckets_dispatched = kBucketCount;
  frame_gate.command_buffers_committed = 1;
  frame_gate.command_buffers_completed = 1;
  frame_gate.drawables_acquired = 1;
  frame_gate.submissions = 1;
  frame_gate.presentation_drops = 1;
  frame_gate.presentation_order_mismatches = 1;
  check(goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 0) &&
            !goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 1),
        "presentation drops and ordering are diagnostic unless explicitly required");
  frame_gate.presentation_drops = 0;
  frame_gate.presentation_order_mismatches = 0;
  check(goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 0) &&
            !goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 1),
        "a missing drawable callback passes only the GPU-completion gate");
  frame_gate.presentations = 1;
  check(goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 1),
        "an exact drawable callback passes the presentation-required gate");
  frame_gate.command_buffer_errors = 1;
  check(!goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 0) &&
            !goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 1),
        "a GPU failure is rejected by both frame gates");
  frame_gate.command_buffer_errors = 0;
  frame_gate.ocean_command_buffers_committed = 1;
  frame_gate.ocean_command_buffer_errors = 1;
  check(!goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 0) &&
            !goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 1),
        "a private ocean GPU failure is rejected by both frame gates");
  frame_gate.ocean_command_buffers_committed = 0;
  frame_gate.ocean_command_buffer_errors = 0;
  frame_gate.drawable_misses = 1;
  check(!goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 0) &&
            !goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 1),
        "a drawable miss is rejected by both frame gates");
  frame_gate.drawable_misses = 0;
  frame_gate.late_present_submissions = 1;
  check(!goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 0) &&
            !goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 1),
        "a late submission is rejected by both frame gates");

  check(goal_kernel_core_initialize() == GOAL_KERNEL_CORE_OK,
        "initialized the Jak 2 kernel arena for the copied chain");
  if (failures) {
    return 1;
  }
  make_empty_chain();

  const auto fixture_root =
      std::filesystem::temp_directory_path() / "goalpad-jak2-metal-host-residency-test";
  std::error_code fixture_error;
  std::filesystem::remove_all(fixture_root, fixture_error);
  fixture_error.clear();
  std::filesystem::create_directories(fixture_root / "fr3", fixture_error);
  std::filesystem::create_directories(fixture_root / "wrong", fixture_error);
  std::filesystem::create_directories(fixture_root / "security", fixture_error);
  std::filesystem::create_directories(fixture_root / "prison", fixture_error);
  check(!fixture_error &&
            write_synthetic_fr3(fixture_root / "fr3/GAME.fr3", "synthetic-common-key", true) &&
            write_synthetic_fr3(fixture_root / "fr3/arena.fr3", "synthetic-arena-key", false) &&
            write_synthetic_fr3(fixture_root / "wrong/arena.fr3", "wrong-directory-key", false) &&
            write_security_fr3(fixture_root / "security/GAME.fr3", "security-common", true) &&
            write_security_fr3(fixture_root / "security/ctywide.fr3", "ctywide", false) &&
            write_prison_clut_fr3(fixture_root / "prison/GAME.fr3"),
        "created public synthetic FR3 level-art fixtures");
  if (failures) {
    goal_kernel_core_shutdown();
    return 1;
  }

  const std::size_t initial_level_count = metal_level_data::level_count();
  const std::size_t initial_merc_level_count = metal_merc_models().level_count();
  const std::size_t initial_merc_model_count = metal_merc_models().model_count();
  const std::size_t initial_texture_count = metal_texture_live_count();

  check(goal_jak2_metal_host_create_presenting(nullptr) == nullptr,
        "rejected presenting mode without an app-owned CAMetalLayer");

  goal_jak2_metal_host* missing_host = goal_jak2_metal_host_create();
  check(missing_host != nullptr,
        "created a host after auditing the table's explicit Generic2 behavior");
  check(missing_host &&
            !goal_jak2_metal_host_configure_level_art(
                missing_host, (fixture_root / "missing").string().c_str()),
        "rejected a missing FR3 directory");
  goal_gfx_host rejected_callbacks = {};
  check(missing_host && !goal_jak2_metal_host_copy_gfx_host(missing_host, &rejected_callbacks),
        "a failed common-art load cannot publish runtime callbacks");
  goal_jak2_metal_host_destroy(missing_host);
  check(metal_level_data::level_count() == initial_level_count &&
            metal_merc_models().level_count() == initial_merc_level_count &&
            metal_merc_models().model_count() == initial_merc_model_count &&
            metal_texture_live_count() == initial_texture_count,
        "missing-directory rejection released its placeholder without leaking level art");

  goal_jak2_metal_host* wrong_host = goal_jak2_metal_host_create();
  check(wrong_host != nullptr, "created a host for wrong-directory rejection");
  check(wrong_host &&
            !goal_jak2_metal_host_configure_level_art(
                wrong_host, (fixture_root / "wrong").string().c_str()),
        "rejected an FR3 directory without GAME.fr3");
  goal_jak2_metal_host_destroy(wrong_host);
  check(metal_level_data::level_count() == initial_level_count &&
            metal_merc_models().level_count() == initial_merc_level_count &&
            metal_merc_models().model_count() == initial_merc_model_count &&
            metal_texture_live_count() == initial_texture_count,
        "wrong-directory rejection left no global Metal resources");

  goal_jak2_metal_host* host = goal_jak2_metal_host_create();
  check(host != nullptr, "created the process-singleton Jak 2 Metal host");
  if (!host) {
    goal_kernel_core_shutdown();
    return 1;
  }
  check(goal_jak2_metal_host_create() == nullptr,
        "rejected a second live Jak 2 Metal host");
  const std::string fr3_directory = (fixture_root / "fr3").string();
  check(goal_jak2_metal_host_configure_level_art(host, fr3_directory.c_str()),
        "synchronously loaded synthetic GAME.fr3 before publishing callbacks");
  const std::size_t configured_texture_count = metal_texture_live_count();
  check(metal_level_data::level_count() == initial_level_count + 1 &&
            metal_merc_models().level_count() == initial_merc_level_count + 1 &&
            metal_merc_models().model_count() == initial_merc_model_count &&
            configured_texture_count ==
                initial_texture_count + 20 + METAL_NUM_EYE_PAIRS * 2,
        "common art, Bomb and Dark Jak sources/defaults, placeholder, OCEAN targets, and detached "
        "eye targets are resident");
  check(goal_jak2_metal_host_configure_level_art(host, fr3_directory.c_str()) &&
            metal_level_data::level_count() == initial_level_count + 1 &&
            metal_merc_models().level_count() == initial_merc_level_count + 1 &&
            metal_merc_models().model_count() == initial_merc_model_count &&
            metal_texture_live_count() == configured_texture_count,
        "same-directory configuration is idempotent");
  check(!goal_jak2_metal_host_configure_level_art(
            host, (fixture_root / "wrong").string().c_str()),
        "rejected reconfiguration to another FR3 directory");

  goal_gfx_host callbacks = {};
  check(goal_jak2_metal_host_copy_gfx_host(host, &callbacks),
        "copied the app-owned graphics callback table");
  check(callbacks.send_chain && callbacks.sync_path && callbacks.vsync,
        "the copied host contains every required synchronous callback");
  check(callbacks.texture_upload_now && callbacks.texture_relocate && callbacks.set_levels,
        "the copied host contains real texture-residency callbacks");

  const char* common_again[] = {"GAME"};
  callbacks.set_levels(common_again, 1);
  check(metal_level_data::level_count() == initial_level_count + 1 &&
            metal_merc_models().level_count() == initial_merc_level_count + 1 &&
            metal_texture_live_count() == configured_texture_count,
        "set-levels does not duplicate the configured GAME pair");

  const char* arena[] = {"arena"};
  callbacks.set_levels(arena, 1);
  check(metal_level_data::level_count() == initial_level_count + 2 &&
            metal_merc_models().level_count() == initial_merc_level_count + 2 &&
            metal_merc_models().model_count() == initial_merc_model_count,
        "set-levels paired one requested FR3 under its serialized key");
  callbacks.set_levels(arena, 1);
  check(metal_level_data::level_count() == initial_level_count + 2 &&
            metal_merc_models().level_count() == initial_merc_level_count + 2 &&
            metal_merc_models().model_count() == initial_merc_model_count &&
            metal_texture_live_count() == configured_texture_count,
        "set-levels loads each requested basename only once and retains it");

  write_texture_page();
  callbacks.texture_upload_now(static_cast<u8*>(g_ee_main_mem) + kTexturePageOffset, -1,
                               kSyntheticS7);
  callbacks.texture_relocate(kRelocatedTextureVram, kTextureVram, 0);

  goal_jak2_metal_frame_summary frame_summary = {1, 1, 1, 1, 1, 1, 1};
  check(!goal_jak2_metal_host_read_last_frame(host, &frame_summary) && is_zero(frame_summary),
        "nil-layer mode safely rejects readback before a frame exists");

  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  callbacks.sync_path();
  callbacks.vsync();
  goal_jak2_metal_host_metrics metrics = {};
  check(goal_jak2_metal_host_get_metrics(host, &metrics), "copied the host metrics after dispatch");
  check(metrics.chains == 1 && metrics.completed_chains == 1 && metrics.failed_chains == 0 &&
            metrics.last_buckets_dispatched == kBucketCount,
        "one copied 327-bucket chain completed policy dispatch");
  constexpr std::array<u32, GOAL_JAK2_TFRAG_TEXTURE_UPLOAD_BUCKET_COUNT> kTfragBuckets = {
      7, 18, 29, 40, 51, 62};
  constexpr std::array<u32, GOAL_JAK2_SHRUB_TEXTURE_UPLOAD_BUCKET_COUNT> kShrubBuckets = {
      73, 82, 91, 100, 109, 118, 191};
  constexpr std::array<u32, GOAL_JAK2_ALPHA_TEXTURE_UPLOAD_BUCKET_COUNT> kAlphaBuckets = {
      127, 137, 147, 157, 167, 177};
  constexpr std::array<u32, GOAL_JAK2_PRIS_TEXTURE_UPLOAD_BUCKET_COUNT> kPrisBuckets = {
      196, 200, 204, 208, 212, 216};
  constexpr std::array<u32, GOAL_JAK2_PRIS2_CAPTURE_BUCKET_COUNT> kPris2CaptureBuckets = {228,
                                                                                         229};
  constexpr std::array<u32, GOAL_JAK2_WATER_TEXTURE_UPLOAD_BUCKET_COUNT> kWaterBuckets = {
      252, 261, 270, 279, 288, 297};
  check(texture_captures_are_empty(metrics.tfrag_texture_uploads, kTfragBuckets),
        "the host records all six empty normal TFRAG texture setup buckets");
  check(texture_captures_are_empty(metrics.shrub_texture_uploads, kShrubBuckets),
        "the host records all seven empty normal/common SHRUB texture setup buckets");
  check(texture_captures_are_empty(metrics.alpha_texture_uploads, kAlphaBuckets),
        "the host records all six empty source-identical alpha texture setup buckets");
  check(texture_captures_are_empty(metrics.pris_texture_uploads, kPrisBuckets),
        "the host records all six empty per-level PRIS texture buckets without executing them");
  check(texture_captures_are_empty(metrics.pris2_bucket_captures, kPris2CaptureBuckets) &&
            metrics.pris2_bucket_captures[0].executions == 0 &&
            metrics.pris2_bucket_captures[1].executions == 0,
        "empty bucket 228 is a structural no-op while bucket 229 stays diagnostic-only");
  check(metrics.last_pris_eye_dispatches ==
            kPrisBuckets.size() + metal_renderer::kJak2Pris2TextureUploadBuckets.size() &&
            metrics.last_pris_eye_present_dispatches == 0 &&
            metrics.last_pris_eye_chunks == 0 && metrics.last_eye_composed == 0 &&
            metrics.last_eye_command_buffers_committed == 0 &&
            metrics.last_eye_command_buffers_completed == 0 &&
            metrics.last_eye_command_buffer_errors == 0,
        "all six PRIS and six PRIS2 callbacks run once without eye execution");
  check(texture_captures_are_empty(metrics.water_texture_uploads, kWaterBuckets),
        "the host records all six empty source-identical water texture upload buckets");
  check(texture_capture_is_empty(metrics.common_water_texture_upload, kCommonWaterBucket) &&
            metrics.common_water_texture_upload.executions == 0,
        "the host dispatches empty common-water bucket 306 without upload execution");
  check(texture_capture_is_empty(metrics.common_tfrag_texture_upload, 187),
        "the host accepts an exact empty host-owned common TFRAG texture bucket");
  check(texture_capture_is_empty(metrics.common_pris_texture_upload, kCommonPrisBucket) &&
            metrics.common_pris_texture_upload.executions == 0,
        "the absent common PRIS callback runs once without upload, CLUT, or eye execution");
  check(metrics.common_tfrag_ordinary_uploads == 0 &&
            metrics.common_tfrag_skull_gem_preparations == 0 &&
            metrics.common_tfrag_skull_gem_publications == 0 &&
            metrics.common_tfrag_skull_gem_texture == 0,
        "an empty common TFRAG bucket does not prepare or publish the skull-gem texture");
  check(metrics.command_buffers_committed == 0 && metrics.command_buffers_completed == 0 &&
            metrics.command_buffer_errors == 0 &&
            metrics.ocean_draws == 0 && metrics.ocean_triangles == 0 &&
            metrics.ocean_missing_textures == 0 &&
            metrics.ocean_command_buffers_committed == 0 &&
            metrics.ocean_command_buffers_completed == 0 &&
            metrics.ocean_command_buffer_errors == 0 && metrics.drawables_acquired == 0 &&
            metrics.drawable_misses == 0 && metrics.late_present_submissions == 0 &&
            metrics.draws == 0 && metrics.triangles == 0 && metrics.submissions == 0 &&
            metrics.last_sky_draw_draws == 0 && metrics.last_sky_draw_triangles == 0 &&
            metrics.last_screen_filter_draws == 0 && metrics.last_screen_filter_triangles == 0 &&
            metrics.last_debug_no_zbuf2_draws == 0 &&
            metrics.last_debug_no_zbuf2_triangles == 0 &&
            metrics.last_tie_draws == 0 && metrics.last_tie_triangles == 0 &&
            metrics.last_background_missing_levels == 0 &&
            metrics.last_background_missing_textures == 0 &&
            metrics.last_background_anim_slot_draws == 0 &&
            metrics.last_merc_models == 0 && metrics.last_merc_draws == 0 &&
            metrics.last_merc_triangles == 0 && metrics.last_merc_malformed_dma == 0 &&
            metrics.last_merc_missing_models == 0 &&
            metrics.last_merc_bad_bone_pointers == 0 &&
            metrics.last_merc_missing_bone_slots == 0 &&
            metrics.last_merc_nonfinite_bone_matrices == 0 &&
            metrics.last_merc_degenerate_bone_matrices == 0 &&
            metrics.last_merc_incoherent_bone_sources == 0 &&
            metrics.last_generic_draw_buckets == 0 && metrics.last_generic_draws == 0 &&
            metrics.last_generic_triangles == 0 && metrics.last_generic_missing_textures == 0 &&
            metrics.last_generic_unexpected_dma == 0 &&
            metrics.presentations == 0 && metrics.presentation_drops == 0 &&
            metrics.presentation_order_mismatches == 0 && metrics.unsupported_blends == 0,
        "nil-layer lifecycle dispatches without committing, drawing, or presenting");
  check(sky_batch_is_zero(metrics), "an empty chain exposes no SKY_DRAW batch facts");
  check(!goal_jak2_metal_host_wait_for_last_frame(host, 0.01, 0),
        "nil-layer mode rejects a completion wait without changing its dispatch result");

  make_screen_filter_chain();
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(host, &metrics),
        "copied the host metrics after synthetic SCREEN_FILTER dispatch");
  check(metrics.chains == 2 && metrics.completed_chains == 2 && metrics.failed_chains == 0 &&
            metrics.last_buckets_dispatched == kBucketCount,
        "the synthetic SCREEN_FILTER chain completed all 327 policy buckets");
  check(metrics.draws == 1 && metrics.triangles == 1 && metrics.last_screen_filter_draws == 1 &&
            metrics.last_screen_filter_triangles == 1 &&
            metrics.last_sky_draw_draws == 0 && metrics.last_sky_draw_triangles == 0 &&
            metrics.last_debug_no_zbuf2_draws == 0 &&
            metrics.last_debug_no_zbuf2_triangles == 0,
        "SCREEN_FILTER records its deterministic Direct draw and triangle");
  check(sky_batch_is_zero(metrics), "SCREEN_FILTER facts do not leak into SKY_DRAW metrics");
  check(metrics.command_buffers_committed == 0 && metrics.command_buffers_completed == 0 &&
            metrics.command_buffer_errors == 0 && metrics.drawables_acquired == 0 &&
            metrics.drawable_misses == 0 && metrics.late_present_submissions == 0 &&
            metrics.submissions == 0 && metrics.presentations == 0 &&
            metrics.presentation_drops == 0 && metrics.presentation_order_mismatches == 0,
        "nil-layer SCREEN_FILTER drawing remains submission- and presentation-free");
  frame_summary = {1, 1, 1, 1, 1, 1, 1};
  check(!goal_jak2_metal_host_read_last_frame(host, &frame_summary) && is_zero(frame_summary),
        "nil-layer SCREEN_FILTER encoding still exposes no completed frame readback");

  make_debug_no_zbuf2_chain();
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(host, &metrics),
        "copied the host metrics after synthetic DEBUG_NO_ZBUF2 dispatch");
  check(metrics.chains == 3 && metrics.completed_chains == 3 && metrics.failed_chains == 0 &&
            metrics.last_buckets_dispatched == kBucketCount,
        "the synthetic DEBUG_NO_ZBUF2 chain completed all 327 policy buckets");
  check(metrics.draws == 1 && metrics.triangles == 1 &&
            metrics.last_debug_no_zbuf2_draws == 1 &&
            metrics.last_debug_no_zbuf2_triangles == 1 &&
            metrics.last_sky_draw_draws == 0 && metrics.last_sky_draw_triangles == 0 &&
            metrics.last_screen_filter_draws == 0 && metrics.last_screen_filter_triangles == 0,
        "DEBUG_NO_ZBUF2 owns the deterministic Direct draw and triangle exactly");
  check(sky_batch_is_zero(metrics), "DEBUG_NO_ZBUF2 facts do not leak into SKY_DRAW metrics");
  check(metrics.command_buffers_committed == 0 && metrics.command_buffers_completed == 0 &&
            metrics.command_buffer_errors == 0 && metrics.drawables_acquired == 0 &&
            metrics.drawable_misses == 0 && metrics.late_present_submissions == 0 &&
            metrics.submissions == 0 && metrics.presentations == 0 &&
            metrics.presentation_drops == 0 && metrics.presentation_order_mismatches == 0 &&
            metrics.unsupported_blends == 0,
        "nil-layer DEBUG_NO_ZBUF2 drawing remains submission- and presentation-free");

  make_sky_draw_chain();
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(host, &metrics),
        "copied the host metrics after synthetic SKY_DRAW dispatch");
  check(metrics.chains == 4 && metrics.completed_chains == 4 && metrics.failed_chains == 0 &&
            metrics.last_buckets_dispatched == kBucketCount,
        "the synthetic SKY_DRAW chain completed all 327 policy buckets");
  check(metrics.draws == 1 && metrics.triangles == 1 && metrics.last_sky_draw_draws == 1 &&
            metrics.last_sky_draw_triangles == 1 && metrics.last_screen_filter_draws == 0 &&
            metrics.last_screen_filter_triangles == 0 &&
            metrics.last_debug_no_zbuf2_draws == 0 &&
            metrics.last_debug_no_zbuf2_triangles == 0,
        "SKY_DRAW owns the deterministic Direct draw and triangle exactly");
  check(metrics.last_sky_draw_batch_valid == 1 &&
            metrics.last_sky_draw_batch_textured == 0 &&
            metrics.last_sky_draw_batch_vertices == 3 &&
            metrics.last_sky_draw_batch_nonzero_rgb_vertices == 3 &&
            metrics.last_sky_draw_batch_tex0_tbp == 0 &&
            metrics.last_sky_draw_batch_tex0_tcc == 0 &&
            metrics.last_sky_draw_batch_tex0_decal == 0 &&
            metrics.last_sky_draw_batch_texture_lookup_hit == 0 &&
            metrics.last_sky_draw_batch_used_placeholder == 0 &&
            metrics.last_sky_draw_batch_write_rgb == 1,
        "SKY_DRAW exposes its basic vertex, TEX0, texture, and RGB-write facts");
  check(metrics.last_sky_draw_batch_blend_enabled == 1 &&
            metrics.last_sky_draw_batch_blend_a ==
                static_cast<uint32_t>(GsAlpha::BlendMode::SOURCE) &&
            metrics.last_sky_draw_batch_blend_b ==
                static_cast<uint32_t>(GsAlpha::BlendMode::DEST) &&
            metrics.last_sky_draw_batch_blend_c ==
                static_cast<uint32_t>(GsAlpha::BlendMode::SOURCE) &&
            metrics.last_sky_draw_batch_blend_d ==
                static_cast<uint32_t>(GsAlpha::BlendMode::DEST) &&
            metrics.last_sky_draw_batch_alpha_test_enabled == 0 &&
            metrics.last_sky_draw_batch_alpha_test_mode ==
                static_cast<uint32_t>(GsTest::AlphaTest::NOTEQUAL) &&
            metrics.last_sky_draw_batch_alpha_aref == 0 &&
            metrics.last_sky_draw_batch_alpha_afail ==
                static_cast<uint32_t>(GsTest::AlphaFail::KEEP),
        "SKY_DRAW exposes its GS blend and alpha-test facts");
  check(metrics.command_buffers_committed == 0 && metrics.command_buffers_completed == 0 &&
            metrics.command_buffer_errors == 0 && metrics.drawables_acquired == 0 &&
            metrics.drawable_misses == 0 && metrics.late_present_submissions == 0 &&
            metrics.submissions == 0 && metrics.presentations == 0 &&
            metrics.presentation_drops == 0 && metrics.presentation_order_mismatches == 0 &&
            metrics.unsupported_blends == 0,
        "nil-layer SKY_DRAW drawing remains submission- and presentation-free");

  make_textured_sky_draw_chain(kRelocatedTextureVram);
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(host, &metrics),
        "copied metrics after real upload and relocate callbacks");
  check(metrics.chains == 5 && metrics.completed_chains == 5 && metrics.failed_chains == 0 &&
            metrics.texture_uploads == 1 && metrics.texture_relocations == 1,
        "real texture upload and relocation preserved their host counters");
  check(metrics.last_sky_draw_batch_valid == 1 &&
            metrics.last_sky_draw_batch_textured == 1 &&
            metrics.last_sky_draw_batch_tex0_tbp == kRelocatedTextureVram &&
            metrics.last_sky_draw_batch_texture_lookup_hit == 1 &&
            metrics.last_sky_draw_batch_used_placeholder == 0,
        "SKY_DRAW resolved the configured GAME texture through upload and relocation mapping");

  make_blit_snapshot_sky_draw_chain(false);
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(host, &metrics),
        "copied metrics after a BlitDisplays snapshot feeds SKY_DRAW");
  check(metrics.chains == 6 && metrics.completed_chains == 6 && metrics.failed_chains == 0 &&
            metrics.last_blit_display_snapshot_requested == 1 &&
            metrics.last_blit_display_copy_back_requested == 0 &&
            metrics.last_blit_display_copy_back_performed == 0 &&
            metrics.last_blit_display_texture_tbp == metal_renderer::kJak2BlitDisplayTbp &&
            metrics.last_blit_display_texture_lookup_hit == 1 &&
            metrics.last_blit_display_used_placeholder == 0,
        "host ABI exposes the successful snapshot, TBP, lookup, and non-placeholder result");
  check(metrics.last_sky_draw_batch_valid == 1 &&
            metrics.last_sky_draw_batch_textured == 1 &&
            metrics.last_sky_draw_batch_tex0_tbp == metal_renderer::kJak2BlitDisplayTbp &&
            metrics.last_sky_draw_batch_texture_lookup_hit == 1 &&
            metrics.last_sky_draw_batch_used_placeholder == 0 &&
            metrics.last_sky_draw_batch_blend_enabled == 1 &&
            metrics.last_sky_draw_batch_blend_a ==
                static_cast<uint32_t>(GsAlpha::BlendMode::SOURCE) &&
            metrics.last_sky_draw_batch_blend_b ==
                static_cast<uint32_t>(GsAlpha::BlendMode::DEST) &&
            metrics.last_sky_draw_batch_blend_c ==
                static_cast<uint32_t>(GsAlpha::BlendMode::SOURCE) &&
            metrics.last_sky_draw_batch_blend_d ==
                static_cast<uint32_t>(GsAlpha::BlendMode::DEST),
        "SKY_DRAW ABI correlates the snapshot TBP lookup with its exact blend equation");

  make_blit_snapshot_sky_draw_chain(true);
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(host, &metrics),
        "copied metrics after the first-menu snapshot and copy-back shape");
  check(metrics.chains == 7 && metrics.completed_chains == 7 && metrics.failed_chains == 0 &&
            metrics.last_blit_display_snapshot_requested == 1 &&
            metrics.last_blit_display_copy_back_requested == 1 &&
            metrics.last_blit_display_copy_back_performed == 1 &&
            metrics.last_blit_display_texture_tbp == metal_renderer::kJak2BlitDisplayTbp &&
            metrics.last_blit_display_texture_lookup_hit == 1 &&
            metrics.last_blit_display_used_placeholder == 0 &&
            metrics.last_sky_draw_batch_tex0_tbp == metal_renderer::kJak2BlitDisplayTbp &&
            metrics.last_sky_draw_batch_texture_lookup_hit == 1 &&
            metrics.last_sky_draw_batch_used_placeholder == 0,
        "host ABI distinguishes the first-menu copy-back while SKY_DRAW samples the snapshot");

  const char* missing_level[] = {"missing-level"};
  callbacks.set_levels(missing_level, 1);
  check(metal_level_data::level_count() == initial_level_count + 2 &&
            metal_merc_models().level_count() == initial_merc_level_count + 2 &&
            metal_merc_models().model_count() == initial_merc_model_count,
        "a missing requested FR3 leaves neither loader partially resident");
  make_empty_chain();
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(host, &metrics) && metrics.chains == 8 &&
            metrics.completed_chains == 7 && metrics.failed_chains == 1,
        "a requested FR3 load failure fails the next renderer chain closed");

  goal_jak2_metal_host_destroy(host);
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(!goal_jak2_metal_host_get_metrics(host, &metrics),
        "a destroyed host no longer exposes state while stale callbacks remain inert");
  check(metal_level_data::level_count() == initial_level_count &&
            metal_merc_models().level_count() == initial_merc_level_count &&
            metal_merc_models().model_count() == initial_merc_model_count &&
            metal_texture_live_count() == initial_texture_count,
        "destroy unloaded recorded serialized keys in reverse and released every texture handle");

  goal_jak2_metal_host* effects_host = goal_jak2_metal_host_create();
  goal_gfx_host effects_callbacks = {};
  check(effects_host && goal_jak2_metal_host_configure_level_art(effects_host, fr3_directory.c_str()) &&
            goal_jak2_metal_host_copy_gfx_host(effects_host, &effects_callbacks),
        "created a host for exact bucket-315 Lightning execution");
  goal_jak2_metal_host_metrics effects_metrics = {};
  make_empty_chain();
  effects_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(effects_host, &effects_metrics) &&
            effects_metrics.chains == 1 && effects_metrics.completed_chains == 1 &&
            effects_metrics.failed_chains == 0 &&
            effects_metrics.effects_bucket315.absent_captures == 1 &&
            effects_metrics.effects_bucket315_execution.callback_dispatches == 1 &&
            effects_metrics.effects_bucket315_execution.completed_executions == 1 &&
            effects_metrics.effects_bucket315_execution.last_expected_fragments == 0 &&
            effects_metrics.effects_bucket315_execution.last_expected_vertices == 0 &&
            effects_metrics.effects_bucket315_execution.last_expected_adgifs == 0 &&
            effects_metrics.effects_bucket315_execution.last_expected_draws == 0 &&
            effects_metrics.effects_bucket315_execution.last_actual_draws == 0,
        "absent bucket 315 dispatches its callback once with an exact zero-draw gate");

  make_effects_lightning_chain();
  effects_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(effects_host, &effects_metrics) &&
            effects_metrics.chains == 2 && effects_metrics.completed_chains == 2 &&
            effects_metrics.failed_chains == 0 && effects_metrics.effects_bucket315.captures == 2 &&
            effects_metrics.effects_bucket315.valid_captures == 2 &&
            effects_metrics.effects_bucket315.lightning_captures == 1 &&
            effects_metrics.effects_bucket315.malformed_captures == 0 &&
            effects_metrics.effects_bucket315.other_captures == 0 &&
            effects_metrics.effects_bucket315.last_transfer_count == 8 &&
            effects_metrics.effects_bucket315.last_payload_bytes == 352 &&
            effects_metrics.effects_bucket315.last_classification ==
                static_cast<uint8_t>(metal_renderer::Jak2EffectsBucket315CaptureClass::Lightning) &&
            effects_metrics.effects_bucket315_execution.callback_dispatches == 2 &&
            effects_metrics.effects_bucket315_execution.completed_executions == 2 &&
            effects_metrics.effects_bucket315_execution.last_actual_fragments == 0 &&
            effects_metrics.effects_bucket315_execution.last_actual_draw_buckets == 0 &&
            effects_metrics.effects_bucket315_execution.last_actual_draws == 0,
        "the exact 8-transfer, 352-byte setup-only packet executes without a draw");

  write_texture_page();
  effects_callbacks.texture_upload_now(static_cast<u8*>(g_ee_main_mem) + kTexturePageOffset, -1,
                                       kSyntheticS7);
  make_effects_lightning_chain(1, kTextureVram, false, 4, 0);
  effects_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(effects_host, &effects_metrics) &&
            effects_metrics.chains == 3 && effects_metrics.completed_chains == 3 &&
            effects_metrics.failed_chains == 0 &&
            effects_metrics.effects_bucket315_execution.callback_dispatches == 3 &&
            effects_metrics.effects_bucket315_execution.completed_executions == 3 &&
            effects_metrics.effects_bucket315_execution.last_expected_fragments == 1 &&
            effects_metrics.effects_bucket315_execution.last_expected_vertices == 4 &&
            effects_metrics.effects_bucket315_execution.last_expected_adgifs == 1 &&
            effects_metrics.effects_bucket315_execution.last_expected_draws == 1 &&
            effects_metrics.effects_bucket315_execution.last_actual_fragments == 1 &&
            effects_metrics.effects_bucket315_execution.last_actual_vertices == 4 &&
            effects_metrics.effects_bucket315_execution.last_actual_adgifs == 1 &&
            effects_metrics.effects_bucket315_execution.last_actual_draw_buckets == 1 &&
            effects_metrics.effects_bucket315_execution.last_actual_draws == 1 &&
            effects_metrics.effects_bucket315_execution.last_actual_missing_textures == 0 &&
            effects_metrics.effects_bucket315_execution.last_actual_placeholder_draws == 0 &&
            effects_metrics.effects_bucket315_execution.last_actual_unsupported_blends == 0 &&
            effects_metrics.effects_bucket315_execution.last_actual_unexpected_dma == 0 &&
            effects_metrics.effects_bucket315_execution.last_actual_overflow == 0,
        "one active title MMIN=4 MXL=0 fragment passes callback, parser, draw, and error-count gates");

  const u32 copied_before_malformed = effects_metrics.last_copied_bytes;
  const u64 callbacks_before_malformed =
      effects_metrics.effects_bucket315_execution.callback_dispatches;
  const std::size_t textures_before_malformed = metal_texture_live_count();
  make_effects_lightning_chain(0, kTextureVram, true);
  put_tag(kChainOffset + kEffectsBucket * 16, DmaTag::Kind::NEXT, 0, 0xfffffff0,
          effects_vif(VifCode::Kind::MARK));
  effects_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(effects_host, &effects_metrics) &&
            effects_metrics.chains == 4 && effects_metrics.completed_chains == 3 &&
            effects_metrics.failed_chains == 1 &&
            effects_metrics.effects_bucket315.malformed_captures == 1 &&
            effects_metrics.last_copied_bytes == copied_before_malformed &&
            effects_metrics.effects_bucket315_execution.callback_dispatches ==
                callbacks_before_malformed &&
            metal_texture_live_count() == textures_before_malformed,
        "malformed live bucket 315 is rejected before copy, callback, or texture mutation");

  constexpr u32 kMissingEffectsTextureTbp = 0x7e1;
  make_effects_lightning_chain(1, kMissingEffectsTextureTbp);
  effects_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(effects_host, &effects_metrics) &&
            effects_metrics.chains == 5 && effects_metrics.completed_chains == 3 &&
            effects_metrics.failed_chains == 2 &&
            effects_metrics.effects_bucket315_execution.callback_dispatches == 4 &&
            effects_metrics.effects_bucket315_execution.completed_executions == 3 &&
            effects_metrics.effects_bucket315_execution.last_actual_fragments == 1 &&
            effects_metrics.effects_bucket315_execution.last_actual_draw_buckets == 1 &&
            effects_metrics.effects_bucket315_execution.last_actual_draws == 0 &&
            effects_metrics.effects_bucket315_execution.last_actual_missing_textures == 1 &&
            effects_metrics.effects_bucket315_execution.last_actual_placeholder_draws == 0 &&
            metal_texture_live_count() == textures_before_malformed,
        "a missing Lightning texture records no visible draw and fails the host execution gate");
  goal_jak2_metal_host_destroy(effects_host);

  goal_jak2_metal_host* shadow_host = goal_jak2_metal_host_create();
  goal_gfx_host shadow_callbacks = {};
  const std::size_t shadow_initial_live_count = metal_texture_live_count();
  check(shadow_host && goal_jak2_metal_host_copy_gfx_host(shadow_host, &shadow_callbacks) &&
            metal_renderer::jak2_metal_bucket_table()[kShadowBucket].behavior ==
                metal_renderer::Jak2MetalBucketBehavior::Shadow2,
        "created a host with the exact Shadow2 bucket-195 route");
  goal_jak2_metal_host_metrics shadow_metrics = {};
  make_empty_chain();
  shadow_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(shadow_host, &shadow_metrics) &&
            shadow_metrics.chains == 1 && shadow_metrics.completed_chains == 1 &&
            shadow_metrics.failed_chains == 0 &&
            shadow_metrics.shadow_bucket195.observations == 1 &&
            shadow_metrics.shadow_bucket195.absent == 1 &&
            shadow_metrics.shadow_bucket195.observed == 0 &&
            shadow_metrics.shadow_bucket195.last_transfer_count == 1 &&
            shadow_metrics.shadow_bucket195.last_total_payload_bytes == 0 &&
            shadow_metrics.shadow_bucket195.last_reached_boundary == 1 &&
            shadow_metrics.shadow_bucket195_execution.completed_executions == 1 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_executions == 1 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_absent == 1 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_draws == 0 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_reached_boundary == 1,
        "bucket 195 parses live/copy and executes its exact Absent form without drawing");

  make_shadow_bucket195_chain();
  shadow_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(shadow_host, &shadow_metrics) &&
            shadow_metrics.chains == 2 && shadow_metrics.completed_chains == 2 &&
            shadow_metrics.failed_chains == 0 &&
            shadow_metrics.shadow_bucket195.observations == 2 &&
            shadow_metrics.shadow_bucket195.absent == 1 &&
            shadow_metrics.shadow_bucket195.observed == 1 &&
            shadow_metrics.shadow_bucket195.malformed == 0 &&
            shadow_metrics.shadow_bucket195.limit_exceeded == 0 &&
            shadow_metrics.shadow_bucket195.last_transfer_count == 14 &&
            shadow_metrics.shadow_bucket195.last_v4_32_transfer_count == 4 &&
            shadow_metrics.shadow_bucket195.last_v4_8_transfer_count == 1 &&
            shadow_metrics.shadow_bucket195.last_v4_32_unpack_count == 24 &&
            shadow_metrics.shadow_bucket195.last_v4_8_unpack_count == 4 &&
            shadow_metrics.shadow_bucket195.last_direct_transfer_count == 4 &&
            shadow_metrics.shadow_bucket195.last_total_payload_bytes == 1360 &&
            shadow_metrics.shadow_bucket195.last_direct_payload_bytes == 944 &&
            shadow_metrics.shadow_bucket195.last_flusha_direct_payload_bytes == 944 &&
            shadow_metrics.shadow_bucket195.last_semantic_fingerprint != 0 &&
            shadow_metrics.shadow_bucket195.last_terminal_qwc == 0 &&
            shadow_metrics.shadow_bucket195.last_terminal_tag_kind ==
                static_cast<uint8_t>(DmaTag::Kind::NEXT) &&
            shadow_metrics.shadow_bucket195.last_reached_boundary == 1 &&
            shadow_metrics.shadow_bucket195_execution.completed_executions == 2 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_executions == 1 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_deferred_no_draw == 1 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_input_batches == 1 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_input_vertices == 3 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_input_records == 1 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_output_vertices == 0 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_draws == 0 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_reached_boundary == 1 &&
            metal_texture_live_count() == shadow_initial_live_count,
        "top-only MSCALF6 remains accepted, exact-boundary, and no-draw without bottom access");

  make_shadow_bucket195_chain(true);
  shadow_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(shadow_host, &shadow_metrics) &&
            shadow_metrics.chains == 3 && shadow_metrics.completed_chains == 3 &&
            shadow_metrics.failed_chains == 0 &&
            shadow_metrics.shadow_bucket195_execution.completed_executions == 3 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_ready == 1 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_input_batches == 1 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_input_vertices == 8 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_input_records == 2 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_output_vertices == 12 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_front_triangles == 2 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_back_triangles == 2 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_draws == 4 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_triangles == 8 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_darken_draws == 1 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_lighten_draws == 1 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_unexpected_dma == 0 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_invalid_plan == 0 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_nonfinite_projection == 0 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_overflow == 0 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_pipeline_failures == 0 &&
            shadow_metrics.shadow_bucket195_execution.last_actual_reached_boundary == 1,
        "a Ready bucket-195 plan passes the exact host geometry, draw, and error gate");

  const uint32_t shadow_copied_before_malformed = shadow_metrics.last_copied_bytes;
  make_shadow_bucket195_chain();
  constexpr u32 kTopOnlyIndexHeaderOffset = kShadowCaptureOffset + 416 + 64 + 16;
  static_cast<u8*>(g_ee_main_mem)[kTopOnlyIndexHeaderOffset] = 0;
  shadow_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(shadow_host, &shadow_metrics) &&
            shadow_metrics.chains == 4 && shadow_metrics.completed_chains == 3 &&
            shadow_metrics.failed_chains == 1 &&
            shadow_metrics.last_copied_bytes == shadow_copied_before_malformed &&
            shadow_metrics.shadow_bucket195_execution.completed_executions == 3,
        "malformed live bucket 195 is rejected before copy or renderer mutation");
  goal_jak2_metal_host_destroy(shadow_host);

  goal_jak2_metal_host* warp_texture_host = goal_jak2_metal_host_create();
  goal_gfx_host warp_texture_callbacks = {};
  const std::size_t warp_texture_initial_live_count = metal_texture_live_count();
  check(warp_texture_host &&
            goal_jak2_metal_host_copy_gfx_host(warp_texture_host, &warp_texture_callbacks) &&
            metal_renderer::jak2_metal_bucket_table()[kWarpTextureUploadBucket].behavior ==
                metal_renderer::Jak2MetalBucketBehavior::HostTextureUpload,
        "created a host for typed bucket-316 ordinary upload execution");
  goal_jak2_metal_host_metrics warp_texture_metrics = {};
  make_empty_chain();
  warp_texture_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(warp_texture_host, &warp_texture_metrics) &&
            warp_texture_metrics.chains == 1 && warp_texture_metrics.completed_chains == 1 &&
            warp_texture_metrics.failed_chains == 0 &&
            warp_texture_metrics.warp_texture_upload.observations == 1 &&
            warp_texture_metrics.warp_texture_upload.absent == 1 &&
            warp_texture_metrics.warp_texture_upload.ordinary == 0 &&
            warp_texture_metrics.warp_texture_upload.unclassified == 0 &&
            warp_texture_metrics.warp_texture_upload.last_transfer_count == 1 &&
            warp_texture_metrics.warp_texture_upload.last_upload_count == 0 &&
            warp_texture_metrics.warp_texture_upload.last_payload_bytes == 0 &&
            warp_texture_metrics.warp_texture_upload_executions == 0,
        "bucket 316 dispatches its canonical empty plan without execution");

  make_warp_texture_upload_chain(1);
  warp_texture_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(warp_texture_host, &warp_texture_metrics) &&
            warp_texture_metrics.chains == 2 && warp_texture_metrics.completed_chains == 2 &&
            warp_texture_metrics.failed_chains == 0 &&
            warp_texture_metrics.warp_texture_upload.observations == 2 &&
            warp_texture_metrics.warp_texture_upload.absent == 1 &&
            warp_texture_metrics.warp_texture_upload.ordinary == 1 &&
            warp_texture_metrics.warp_texture_upload.unclassified == 0 &&
            warp_texture_metrics.warp_texture_upload.last_upload_count == 1 &&
            warp_texture_metrics.warp_texture_upload.last_transfer_count == 6 &&
            warp_texture_metrics.warp_texture_upload.last_payload_bytes == 208 &&
            warp_texture_metrics.warp_texture_upload.last_semantic_fingerprint != 0 &&
            warp_texture_metrics.warp_texture_upload_executions == 1 &&
            warp_texture_metrics.texture_uploads == 0 &&
            metal_texture_live_count() == warp_texture_initial_live_count,
        "bucket 316 executes its exact one-group six-transfer plan once");
  const uint32_t warp_texture_valid_copied_bytes = warp_texture_metrics.last_copied_bytes;

  make_warp_texture_upload_chain(1, -2);
  warp_texture_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(warp_texture_host, &warp_texture_metrics) &&
            warp_texture_metrics.chains == 3 && warp_texture_metrics.completed_chains == 2 &&
            warp_texture_metrics.failed_chains == 1 &&
            warp_texture_metrics.warp_texture_upload.observations == 3 &&
            warp_texture_metrics.warp_texture_upload.unclassified == 1 &&
            warp_texture_metrics.warp_texture_upload_executions == 1 &&
            warp_texture_metrics.last_copied_bytes == warp_texture_valid_copied_bytes &&
            warp_texture_metrics.texture_uploads == 0 &&
            metal_texture_live_count() == warp_texture_initial_live_count,
        "a malformed bucket-316 descriptor rejects before copying or a second upload");

  make_warp_texture_mixed_chain();
  warp_texture_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(warp_texture_host, &warp_texture_metrics) &&
            warp_texture_metrics.chains == 4 && warp_texture_metrics.completed_chains == 2 &&
            warp_texture_metrics.failed_chains == 2 &&
            warp_texture_metrics.warp_texture_upload.observations == 4 &&
            warp_texture_metrics.warp_texture_upload.unclassified == 2 &&
            warp_texture_metrics.warp_texture_upload_executions == 1 &&
            warp_texture_metrics.last_copied_bytes == warp_texture_valid_copied_bytes &&
            warp_texture_metrics.texture_uploads == 0 &&
            metal_texture_live_count() == warp_texture_initial_live_count,
        "a mixed bucket-316 animator form fails closed before texture mutation");
  goal_jak2_metal_host_destroy(warp_texture_host);

  const std::size_t gmerc_warp_global_live_count = metal_texture_live_count();
  goal_jak2_metal_host* gmerc_warp_host = goal_jak2_metal_host_create();
  goal_gfx_host gmerc_warp_callbacks = {};
  const std::size_t gmerc_warp_initial_live_count = metal_texture_live_count();
  check(gmerc_warp_host &&
            goal_jak2_metal_host_copy_gfx_host(gmerc_warp_host, &gmerc_warp_callbacks) &&
            metal_renderer::jak2_metal_bucket_table()[kWarpTextureUploadBucket].behavior ==
                metal_renderer::Jak2MetalBucketBehavior::HostTextureUpload &&
            metal_renderer::jak2_metal_bucket_table()[kGmercWarpBucket].behavior ==
                metal_renderer::Jak2MetalBucketBehavior::Warp,
        "created a host with source-ordered bucket 316 upload and bucket 317 warp rendering");
  goal_jak2_metal_host_metrics gmerc_warp_metrics = {};
  make_empty_chain();
  gmerc_warp_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(gmerc_warp_host, &gmerc_warp_metrics) &&
            gmerc_warp_metrics.chains == 1 && gmerc_warp_metrics.completed_chains == 1 &&
            gmerc_warp_metrics.failed_chains == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.observations == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.absent == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.setup_only == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.fragments == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.malformed == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_transfer_count == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_payload_bytes == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_semantic_fingerprint != 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_variant ==
                static_cast<uint8_t>(metal_renderer::Jak2GmercWarpBucket317Variant::Absent) &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.callback_dispatches == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.completed_executions == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_copies == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_draws == 0 &&
            gmerc_warp_metrics.texture_uploads == 0 &&
            gmerc_warp_metrics.last_generic_draw_buckets == 0 &&
            gmerc_warp_metrics.last_generic_draws == 0 &&
            gmerc_warp_metrics.last_generic_triangles == 0 &&
            metal_texture_live_count() == gmerc_warp_initial_live_count,
        "bucket 317 executes its exact empty plan without a snapshot or draw");

  make_gmerc_warp_chain(0, true);
  gmerc_warp_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(gmerc_warp_host, &gmerc_warp_metrics) &&
            gmerc_warp_metrics.chains == 2 && gmerc_warp_metrics.completed_chains == 2 &&
            gmerc_warp_metrics.failed_chains == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.observations == 2 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.setup_only == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_transfer_count == 4 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_fragment_count == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_payload_bytes == 192 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_variant ==
                static_cast<uint8_t>(metal_renderer::Jak2GmercWarpBucket317Variant::SetupOnly) &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.callback_dispatches == 2 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.completed_executions == 2 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_copies == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_draws == 0 &&
            gmerc_warp_metrics.texture_uploads == 0 &&
            gmerc_warp_metrics.last_generic_draw_buckets == 0 &&
            gmerc_warp_metrics.last_generic_draws == 0 &&
            metal_texture_live_count() == gmerc_warp_initial_live_count,
        "bucket 317 consumes the exact short setup fixture without a snapshot or draw");

  make_gmerc_warp_chain(0);
  gmerc_warp_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(gmerc_warp_host, &gmerc_warp_metrics) &&
            gmerc_warp_metrics.chains == 3 && gmerc_warp_metrics.completed_chains == 3 &&
            gmerc_warp_metrics.failed_chains == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.observations == 3 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.setup_only == 2 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_transfer_count == 7 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_fragment_count == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_payload_bytes == 352 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.callback_dispatches == 3 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.completed_executions == 3 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_copies == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_draws == 0 &&
            gmerc_warp_metrics.texture_uploads == 0 &&
            gmerc_warp_metrics.last_generic_draw_buckets == 0 &&
            gmerc_warp_metrics.last_generic_draws == 0 &&
            metal_texture_live_count() == gmerc_warp_initial_live_count,
        "bucket 317 consumes the exact terminated setup fixture without a snapshot or draw");

  make_gmerc_warp_chain(1);
  gmerc_warp_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(gmerc_warp_host, &gmerc_warp_metrics) &&
            gmerc_warp_metrics.chains == 4 && gmerc_warp_metrics.completed_chains == 4 &&
            gmerc_warp_metrics.failed_chains == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.observations == 4 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.fragments == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_transfer_count == 8 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_fragment_count == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_continued_fragment_count == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_vertex_count == 4 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_adgif_count == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_payload_bytes == 656 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_semantic_fingerprint != 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_variant ==
                static_cast<uint8_t>(metal_renderer::Jak2GmercWarpBucket317Variant::Fragments) &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.callback_dispatches == 4 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.completed_executions == 4 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_expected_fragments == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_fragments == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_vertices == 4 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_adgifs == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_draw_buckets == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_draws == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_triangles == 2 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_publications == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_copies == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_allocations == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_replacements == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_failures == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_texture != 0 &&
            gmerc_warp_metrics.texture_uploads == 0 &&
            metal_texture_live_count() == gmerc_warp_initial_live_count + 1,
        "bucket 317 snapshots the framebuffer and renders its exact one-fragment plan");

  const u64 gmerc_warp_snapshot_handle =
      gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_texture;

  make_gmerc_warp_chain(2);
  gmerc_warp_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(gmerc_warp_host, &gmerc_warp_metrics) &&
            gmerc_warp_metrics.chains == 5 && gmerc_warp_metrics.completed_chains == 5 &&
            gmerc_warp_metrics.failed_chains == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.observations == 5 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.fragments == 2 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_transfer_count == 10 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_fragment_count == 2 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_continued_fragment_count == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_vertex_count == 8 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_adgif_count == 2 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_payload_bytes == 976 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_semantic_fingerprint != 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.callback_dispatches == 5 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.completed_executions == 5 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_expected_fragments == 2 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution
                    .last_expected_continued_fragments == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_fragments == 2 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution
                    .last_actual_continued_fragments == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_vertices == 8 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_adgifs == 2 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_draw_buckets > 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_draws ==
                gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_draw_buckets &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_actual_triangles > 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_publications == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_copies == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_allocations == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_replacements == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_failures == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.last_snapshot_texture ==
                gmerc_warp_snapshot_handle &&
            gmerc_warp_metrics.texture_uploads == 0 &&
            metal_texture_live_count() == gmerc_warp_initial_live_count + 1,
        "bucket 317 renders its continued-fragment plan from one stable framebuffer snapshot");

  const uint64_t gmerc_warp_valid_copied_bytes = gmerc_warp_metrics.last_copied_bytes;

  make_malformed_gmerc_warp_chain();
  gmerc_warp_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(gmerc_warp_host, &gmerc_warp_metrics) &&
            gmerc_warp_metrics.chains == 6 && gmerc_warp_metrics.completed_chains == 5 &&
            gmerc_warp_metrics.failed_chains == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.observations == 6 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.malformed == 1 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_transfer_count == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_fragment_count == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_continued_fragment_count == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_vertex_count == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_adgif_count == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_payload_bytes == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_semantic_fingerprint == 0 &&
            gmerc_warp_metrics.gmerc_warp_bucket317.last_variant == 3 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.callback_dispatches == 5 &&
            gmerc_warp_metrics.gmerc_warp_bucket317_execution.completed_executions == 5 &&
            gmerc_warp_metrics.last_copied_bytes == gmerc_warp_valid_copied_bytes &&
            gmerc_warp_metrics.texture_uploads == 0 &&
            metal_texture_live_count() == gmerc_warp_initial_live_count + 1,
        "malformed bucket 317 fails before copy, callback, snapshot, or draw mutation");
  goal_jak2_metal_host_destroy(gmerc_warp_host);
  check(metal_texture_live_count() == gmerc_warp_global_live_count,
        "destroying the host releases the stable bucket-317 snapshot texture");

  const std::size_t common_water_global_live_count = metal_texture_live_count();
  goal_jak2_metal_host* common_water_host = goal_jak2_metal_host_create();
  goal_gfx_host common_water_callbacks = {};
  check(common_water_host &&
            goal_jak2_metal_host_configure_level_art(common_water_host, fr3_directory.c_str()) &&
            goal_jak2_metal_host_copy_gfx_host(common_water_host, &common_water_callbacks) &&
            metal_renderer::jak2_metal_bucket_table()[kCommonWaterBucket].behavior ==
                metal_renderer::Jak2MetalBucketBehavior::HostTextureUpload,
        "created a host for planned common-water bucket-306 execution");
  const std::size_t common_water_textures_before = metal_texture_live_count();
  make_empty_chain();
  common_water_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  goal_jak2_metal_host_metrics common_water_metrics = {};
  check(goal_jak2_metal_host_get_metrics(common_water_host, &common_water_metrics) &&
            common_water_metrics.chains == 1 && common_water_metrics.completed_chains == 1 &&
            common_water_metrics.failed_chains == 0 &&
            texture_capture_is_empty(common_water_metrics.common_water_texture_upload,
                                     kCommonWaterBucket) &&
            common_water_metrics.common_water_texture_upload.executions == 0 &&
            metal_texture_live_count() == common_water_textures_before,
        "empty bucket 306 dispatches its exact absent plan without texture mutation");

  write_empty_texture_page(kLiveBombTexturePageOffset, kTexturePageId);
  make_common_water_bomb_chain();
  common_water_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(common_water_host, &common_water_metrics) &&
            common_water_metrics.chains == 2 && common_water_metrics.completed_chains == 2 &&
            common_water_metrics.failed_chains == 0 &&
            common_water_metrics.common_water_texture_upload.captures == 2 &&
            common_water_metrics.common_water_texture_upload.present_captures == 1 &&
            common_water_metrics.common_water_texture_upload.transfers == 10 &&
            common_water_metrics.common_water_texture_upload.payload_bytes == 512 &&
            common_water_metrics.common_water_texture_upload.animator_payload_bytes == 336 &&
            common_water_metrics.common_water_texture_upload.opcode_counts[28] == 1 &&
            common_water_metrics.common_water_texture_upload.executions == 1 &&
            common_water_metrics.texture_uploads == 0 &&
            metal_texture_live_count() == common_water_textures_before + 1,
        "common GAME art executes the exact first-frame bucket-306 bomb form");

  write_empty_texture_page(kTexturePageOffset, kTexturePageId);
  make_common_water_environment_chain(false);
  common_water_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(common_water_host, &common_water_metrics) &&
            common_water_metrics.chains == 3 && common_water_metrics.completed_chains == 3 &&
            common_water_metrics.failed_chains == 0 &&
            common_water_metrics.common_water_texture_upload.captures == 3 &&
            common_water_metrics.common_water_texture_upload.present_captures == 2 &&
            common_water_metrics.common_water_texture_upload.transfers == 19 &&
            common_water_metrics.common_water_texture_upload.payload_bytes == 1024 &&
            common_water_metrics.common_water_texture_upload.animator_payload_bytes == 672 &&
            common_water_metrics.common_water_texture_upload.opcode_counts[28] == 1 &&
            common_water_metrics.common_water_texture_upload.opcode_counts[30] == 1 &&
            common_water_metrics.common_water_texture_upload.executions == 2 &&
            common_water_metrics.texture_uploads == 0 &&
            metal_texture_live_count() == common_water_textures_before + 2,
        "common GAME art preserves the exact opcode-30 security-environment variant");

  make_common_water_environment_chain(true);
  common_water_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  const char* common_water_error = goal_jak2_metal_host_last_error(common_water_host);
  check(goal_jak2_metal_host_get_metrics(common_water_host, &common_water_metrics) &&
            common_water_metrics.chains == 4 && common_water_metrics.completed_chains == 3 &&
            common_water_metrics.failed_chains == 1 &&
            common_water_metrics.common_water_texture_upload.captures == 4 &&
            common_water_metrics.common_water_texture_upload.present_captures == 3 &&
            common_water_metrics.common_water_texture_upload.executions == 2 &&
            common_water_metrics.texture_uploads == 0 &&
            metal_texture_live_count() == common_water_textures_before + 2 && common_water_error &&
            std::strstr(common_water_error, "common-water texture plan rejected bucket 306"),
        "dot-only opcode 30 fails closed before additional common-water texture mutation");
  goal_jak2_metal_host_destroy(common_water_host);
  check(metal_texture_live_count() == common_water_global_live_count,
        "common-water host teardown releases its bomb and environment publications");

  goal_jak2_metal_host* subtitle_host = goal_jak2_metal_host_create();
  goal_gfx_host subtitle_callbacks = {};
  const std::size_t subtitle_initial_live_count = metal_texture_live_count();
  check(subtitle_host && goal_jak2_metal_host_copy_gfx_host(subtitle_host, &subtitle_callbacks) &&
            metal_renderer::jak2_metal_bucket_table()[kSubtitleBucket].behavior ==
                metal_renderer::Jak2MetalBucketBehavior::DeferredSkip,
        "created a host while bucket 322 remains DeferredSkip");
  goal_jak2_metal_host_metrics subtitle_metrics = {};
  make_empty_chain();
  subtitle_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(subtitle_host, &subtitle_metrics) &&
            subtitle_metrics.chains == 1 && subtitle_metrics.completed_chains == 1 &&
            subtitle_metrics.failed_chains == 0 && subtitle_metrics.subtitle_capture.bucket_id ==
                kSubtitleBucket &&
            subtitle_metrics.subtitle_capture.captures == 1 &&
            subtitle_metrics.subtitle_capture.present_captures == 0 &&
            subtitle_metrics.subtitle_capture.executions == 0 &&
            subtitle_metrics.subtitle_capture.classifications[static_cast<std::size_t>(
                metal_renderer::Jak2CommonTfragTextureUploadClass::Absent)] == 1,
        "bucket 322 observes the exact empty form without execution");

  make_subtitle_mixed_capture_chain();
  subtitle_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(subtitle_host, &subtitle_metrics) &&
            subtitle_metrics.chains == 2 && subtitle_metrics.completed_chains == 2 &&
            subtitle_metrics.failed_chains == 0 && subtitle_metrics.subtitle_capture.captures == 2 &&
            subtitle_metrics.subtitle_capture.present_captures == 1 &&
            subtitle_metrics.subtitle_capture.executions == 0 &&
            subtitle_metrics.subtitle_capture.ordinary_descriptors == 1 &&
            subtitle_metrics.subtitle_capture.animator_arrays == 1 &&
            subtitle_metrics.subtitle_capture.animator_body_transfers == 1 &&
            subtitle_metrics.subtitle_capture.direct_setup_transfers == 1 &&
            subtitle_metrics.subtitle_capture.classifications[static_cast<std::size_t>(
                metal_renderer::Jak2CommonTfragTextureUploadClass::OrdinaryAndAnimator)] == 1 &&
            subtitle_metrics.texture_uploads == 0 &&
            metal_texture_live_count() == subtitle_initial_live_count,
        "bucket 322 records mixed animator/upload/Direct metadata without a route or mutation");

  make_subtitle_direct_only_chain();
  subtitle_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(subtitle_host, &subtitle_metrics) &&
            subtitle_metrics.chains == 3 && subtitle_metrics.completed_chains == 3 &&
            subtitle_metrics.failed_chains == 0 && subtitle_metrics.subtitle_capture.captures == 3 &&
            subtitle_metrics.subtitle_capture.present_captures == 2 &&
            subtitle_metrics.subtitle_capture.executions == 0 &&
            subtitle_metrics.subtitle_capture.direct_setup_transfers == 2 &&
            subtitle_metrics.subtitle_capture.classifications[static_cast<std::size_t>(
                metal_renderer::Jak2CommonTfragTextureUploadClass::EyeOrOther)] == 1 &&
            subtitle_metrics.texture_uploads == 0 &&
            metal_texture_live_count() == subtitle_initial_live_count,
        "unclassified bucket-322 Direct-like metadata stays passive and non-fatal");

  make_subtitle_capture_malformed_chain();
  subtitle_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(subtitle_host, &subtitle_metrics) &&
            subtitle_metrics.chains == 4 && subtitle_metrics.completed_chains == 4 &&
            subtitle_metrics.failed_chains == 0 && subtitle_metrics.subtitle_capture.captures == 4 &&
            subtitle_metrics.subtitle_capture.executions == 0 &&
            subtitle_metrics.subtitle_capture.malformed_transfers == 1 &&
            subtitle_metrics.subtitle_capture.classifications[static_cast<std::size_t>(
                metal_renderer::Jak2CommonTfragTextureUploadClass::Malformed)] == 1 &&
            subtitle_metrics.texture_uploads == 0 &&
            metal_texture_live_count() == subtitle_initial_live_count,
        "capture-malformed bucket-322 metadata stays deferred and does not fail the chain");
  goal_jak2_metal_host_destroy(subtitle_host);

  goal_jak2_metal_host* security_host = goal_jak2_metal_host_create();
  goal_gfx_host security_callbacks = {};
  const std::string security_directory = (fixture_root / "security").string();
  check(security_host &&
            goal_jak2_metal_host_configure_level_art(security_host,
                                                     security_directory.c_str()) &&
            goal_jak2_metal_host_copy_gfx_host(security_host, &security_callbacks),
        "created a host with synthetic common security input");
  write_empty_texture_page(kTexturePageOffset, kTexturePageId);
  make_water_security_chain();
  security_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  goal_jak2_metal_host_metrics security_metrics = {};
  const char* security_error = goal_jak2_metal_host_last_error(security_host);
  check(goal_jak2_metal_host_get_metrics(security_host, &security_metrics) &&
            security_metrics.chains == 1 && security_metrics.completed_chains == 0 &&
            security_metrics.failed_chains == 1 &&
            security_metrics.water_texture_uploads[0].executions == 0 && security_error &&
            std::strstr(security_error, "ctywide level art is unavailable"),
        "security preparation fails closed before its ctywide owner is loaded");

  const char* ctywide[] = {"ctywide"};
  security_callbacks.set_levels(ctywide, 1);
  security_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(security_host, &security_metrics) &&
            security_metrics.chains == 2 && security_metrics.completed_chains == 1 &&
            security_metrics.failed_chains == 1 &&
            security_metrics.water_texture_uploads[0].executions == 1,
        "loaded ctywide inputs prepare and publish the exact security water bucket");

  make_common_water_environment_chain(false);
  security_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(security_host, &security_metrics) &&
            security_metrics.chains == 3 && security_metrics.completed_chains == 2 &&
            security_metrics.failed_chains == 1 &&
            security_metrics.water_texture_uploads[0].executions == 1 &&
            security_metrics.common_water_texture_upload.captures == 3 &&
            security_metrics.common_water_texture_upload.present_captures == 1 &&
            security_metrics.common_water_texture_upload.classifications[static_cast<std::size_t>(
                metal_renderer::Jak2CommonTfragTextureUploadClass::OrdinaryAndAnimator)] == 1 &&
            security_metrics.common_water_texture_upload.transfers == 11 &&
            security_metrics.common_water_texture_upload.payload_bytes == 512 &&
            security_metrics.common_water_texture_upload.ordinary_descriptors == 1 &&
            security_metrics.common_water_texture_upload.direct_setup_transfers == 1 &&
            security_metrics.common_water_texture_upload.animator_arrays == 1 &&
            security_metrics.common_water_texture_upload.animator_body_transfers == 1 &&
            security_metrics.common_water_texture_upload.animator_payload_bytes == 336 &&
            security_metrics.common_water_texture_upload.opcode_counts[12] == 1 &&
            security_metrics.common_water_texture_upload.opcode_counts[13] == 1 &&
            security_metrics.common_water_texture_upload.opcode_counts[30] == 1 &&
            security_metrics.common_water_texture_upload.executions == 1,
        "bucket 306 executes one ordinary upload and only the security-environment prefix");
  goal_jak2_metal_host_destroy(security_host);
  check(metal_level_data::level_count() == initial_level_count &&
            metal_merc_models().level_count() == initial_merc_level_count &&
            metal_merc_models().model_count() == initial_merc_model_count &&
            metal_texture_live_count() == initial_texture_count,
        "security host teardown releases common, ctywide, and both publications");

  @autoreleasepool {
    CAMetalLayer* recovery_layer = [CAMetalLayer layer];
    recovery_layer.drawableSize = CGSizeMake(64, 64);
    goal_jak2_metal_host* recovery_host = goal_jak2_metal_host_create_presenting(recovery_layer);
    goal_gfx_host recovery_callbacks = {};
    check(recovery_host && goal_jak2_metal_host_copy_gfx_host(recovery_host, &recovery_callbacks),
          "created a layer-backed host for pre-render rejection recovery");

    make_sprite_texture_upload_chain(1, -2);
    recovery_callbacks.send_chain(g_ee_main_mem, kChainOffset);
    goal_jak2_metal_host_metrics recovery_metrics = {};
    check(goal_jak2_metal_host_get_metrics(recovery_host, &recovery_metrics),
          "copied layer-backed metrics after a pre-render planner rejection");
    const char* recovery_error = goal_jak2_metal_host_last_error(recovery_host);
    const std::string first_recovery_error = recovery_error ? recovery_error : "";
    check(recovery_metrics.chains == 1 && recovery_metrics.completed_chains == 0 &&
              recovery_metrics.failed_chains == 1 &&
              recovery_metrics.command_buffers_committed == 0 &&
              recovery_metrics.drawables_acquired == 0 && recovery_metrics.submissions == 0 &&
              first_recovery_error.find("bucket 312 texture-upload plan rejected malformed DMA") !=
                  std::string::npos,
          "malformed layer-backed work fails before host mutation or renderer submission");

    write_texture_page();
    make_sprite_texture_upload_chain();
    recovery_callbacks.send_chain(g_ee_main_mem, kChainOffset);
    make_empty_chain();
    recovery_callbacks.send_chain(g_ee_main_mem, kChainOffset);
    recovery_error = goal_jak2_metal_host_last_error(recovery_host);
    check(goal_jak2_metal_host_get_metrics(recovery_host, &recovery_metrics) &&
              recovery_metrics.chains == 3 && recovery_metrics.completed_chains == 2 &&
              recovery_metrics.failed_chains == 1 && recovery_metrics.sprite_texture_uploads == 1 &&
              recovery_metrics.last_buckets_dispatched == kBucketCount &&
              recovery_metrics.command_buffers_committed == 2 &&
              recovery_metrics.drawables_acquired == 2 && recovery_metrics.drawable_misses == 0 &&
              recovery_metrics.submissions == 2 && recovery_metrics.late_present_submissions == 0 &&
              recovery_metrics.command_buffer_errors == 0 && recovery_error &&
              first_recovery_error == recovery_error,
          "valid presenting chains recover after rejection with exact renderer counters and first "
          "error");
    check(!goal_jak2_metal_host_metrics_pass_frame_gate(&recovery_metrics, 0),
          "recovery does not weaken the cumulative no-error frame gate");
    goal_jak2_metal_host_destroy(recovery_host);
  }
  check(metal_texture_live_count() == initial_texture_count,
        "layer-backed recovery released its mutated host texture state");

  constexpr u32 kBucket4FixtureBase = 0x300000;
  auto bucket4_fixture =
      metal_renderer::make_jak2_bucket4_texture_upload_fixture(kBucket4FixtureBase);
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + kBucket4FixtureBase,
              bucket4_fixture.ee_memory.data() + kBucket4FixtureBase,
              bucket4_fixture.ee_memory.size() - kBucket4FixtureBase);
  goal_jak2_metal_host* capture_host = goal_jak2_metal_host_create();
  goal_gfx_host capture_callbacks = {};
  check(capture_host && goal_jak2_metal_host_copy_gfx_host(capture_host, &capture_callbacks),
        "created a host for bucket-4 capture integration");
  if (capture_callbacks.send_chain) {
    capture_callbacks.send_chain(g_ee_main_mem, bucket4_fixture.chain_offset);
  }
  goal_jak2_metal_host_metrics capture_metrics = {};
  check(capture_host && goal_jak2_metal_host_get_metrics(capture_host, &capture_metrics),
        "copied metrics after valid bucket-4 capture");
  check(capture_metrics.chains == 1 && capture_metrics.completed_chains == 1 &&
            capture_metrics.failed_chains == 0 &&
            capture_metrics.last_bucket4_texture_upload.valid == 1 &&
            capture_metrics.last_bucket4_texture_upload.present == 1 &&
            capture_metrics.last_bucket4_texture_upload.total_payload_bytes == 416 &&
            capture_metrics.last_bucket4_texture_upload.dma_transfers == 16 &&
            capture_metrics.bucket4_ordinary_uploads == 1 &&
            capture_metrics.bucket4_mixed_executions == 1 &&
            capture_metrics.bucket4_cloud_publications == 1 &&
            capture_metrics.bucket4_fog_publications == 1 &&
            capture_metrics.bucket4_cloud_texture != 0 &&
            capture_metrics.bucket4_fog_texture != 0 &&
            capture_metrics.bucket4_cloud_texture != capture_metrics.bucket4_fog_texture &&
            capture_metrics.skipped_bucket_bytes == 0,
        "valid bucket 4 executes the ordinary page and both stable animator publications");
  const uint64_t first_cloud_texture = capture_metrics.bucket4_cloud_texture;
  const uint64_t first_fog_texture = capture_metrics.bucket4_fog_texture;
  const uint32_t first_copied_bytes = capture_metrics.last_copied_bytes;

  auto ordinary_fixture =
      metal_renderer::make_jak2_bucket4_ordinary_only_texture_upload_fixture(kBucket4FixtureBase);
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + kBucket4FixtureBase,
              ordinary_fixture.ee_memory.data() + kBucket4FixtureBase,
              ordinary_fixture.ee_memory.size() - kBucket4FixtureBase);
  capture_callbacks.send_chain(g_ee_main_mem, ordinary_fixture.chain_offset);
  check(goal_jak2_metal_host_get_metrics(capture_host, &capture_metrics) &&
            capture_metrics.chains == 2 && capture_metrics.completed_chains == 2 &&
            capture_metrics.failed_chains == 0 &&
            capture_metrics.bucket4_ordinary_uploads == 2 &&
            capture_metrics.bucket4_mixed_executions == 1 &&
            capture_metrics.bucket4_cloud_publications == 1 &&
            capture_metrics.bucket4_fog_publications == 1 &&
            capture_metrics.bucket4_cloud_texture == first_cloud_texture &&
            capture_metrics.bucket4_fog_texture == first_fog_texture &&
            capture_metrics.skipped_bucket_bytes == 0,
        "ordinary-only bucket 4 executes without republishing animator textures");

  std::memcpy(static_cast<u8*>(g_ee_main_mem) + kBucket4FixtureBase,
              bucket4_fixture.ee_memory.data() + kBucket4FixtureBase,
              bucket4_fixture.ee_memory.size() - kBucket4FixtureBase);
  capture_callbacks.send_chain(g_ee_main_mem, bucket4_fixture.chain_offset);
  check(goal_jak2_metal_host_get_metrics(capture_host, &capture_metrics) &&
            capture_metrics.chains == 3 && capture_metrics.completed_chains == 3 &&
            capture_metrics.failed_chains == 0 &&
            capture_metrics.bucket4_ordinary_uploads == 3 &&
            capture_metrics.bucket4_mixed_executions == 2 &&
            capture_metrics.bucket4_cloud_publications == 2 &&
            capture_metrics.bucket4_fog_publications == 2 &&
            capture_metrics.bucket4_cloud_texture == first_cloud_texture &&
            capture_metrics.bucket4_fog_texture == first_fog_texture &&
            capture_metrics.skipped_bucket_bytes == 0,
        "repeated mixed bucket 4 keeps stable registry handles while replacing texture objects");

  const u32 missing_finish = 0;
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + bucket4_fixture.first_finish_tag_offset + 8,
              &missing_finish, sizeof(missing_finish));
  if (capture_callbacks.send_chain) {
    capture_callbacks.send_chain(g_ee_main_mem, bucket4_fixture.chain_offset);
  }
  check(capture_host && goal_jak2_metal_host_get_metrics(capture_host, &capture_metrics),
        "copied metrics after malformed bucket-4 capture");
  const char* capture_error = goal_jak2_metal_host_last_error(capture_host);
  check(capture_metrics.chains == 4 && capture_metrics.completed_chains == 3 &&
            capture_metrics.failed_chains == 1 &&
            capture_metrics.last_bucket4_texture_upload.valid == 0 &&
            capture_metrics.last_bucket4_texture_upload.present == 1 &&
            capture_metrics.bucket4_ordinary_uploads == 3 &&
            capture_metrics.bucket4_mixed_executions == 2 &&
            capture_metrics.last_copied_bytes == first_copied_bytes &&
            capture_metrics.skipped_bucket_bytes == 0 && capture_error &&
            std::strstr(capture_error,
                        "bucket 4 texture-upload capture rejected malformed DMA"),
        "malformed bucket 4 fails before mutation, copying, or dispatch");
  goal_jak2_metal_host_destroy(capture_host);

  goal_jak2_metal_host* pris_upload_host = goal_jak2_metal_host_create();
  goal_gfx_host pris_upload_callbacks = {};
  check(pris_upload_host &&
            goal_jak2_metal_host_copy_gfx_host(pris_upload_host, &pris_upload_callbacks),
        "created a host for ordinary-only per-level PRIS integration");
  write_empty_texture_page(kTexturePageOffset, kTexturePageId);
  make_pris_ordinary_only_chain();
  pris_upload_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  goal_jak2_metal_host_metrics pris_upload_metrics = {};
  check(pris_upload_host &&
            goal_jak2_metal_host_get_metrics(pris_upload_host, &pris_upload_metrics),
        "copied metrics after the ordinary-only PRIS chain");
  check(pris_upload_metrics.chains == 1 && pris_upload_metrics.completed_chains == 1 &&
            pris_upload_metrics.failed_chains == 0 &&
            pris_upload_metrics.last_buckets_dispatched == kBucketCount &&
            pris_upload_metrics.last_pris_eye_dispatches ==
                GOAL_JAK2_PRIS_TEXTURE_UPLOAD_BUCKET_COUNT +
                    metal_renderer::kJak2Pris2TextureUploadBuckets.size() &&
            pris_upload_metrics.last_pris_eye_present_dispatches == 1 &&
            pris_upload_metrics.last_pris_eye_chunks == 0 &&
            pris_upload_metrics.pris_texture_uploads[1].bucket_id == kPrisOrdinaryBucket &&
            pris_upload_metrics.pris_texture_uploads[1].present_captures == 1 &&
            pris_upload_metrics.pris_texture_uploads[1].transfers == 5 &&
            pris_upload_metrics.pris_texture_uploads[1].payload_bytes == 176 &&
            pris_upload_metrics.pris_texture_uploads[1].ordinary_descriptors == 1 &&
            pris_upload_metrics.pris_texture_uploads[1].direct_setup_transfers == 1 &&
            pris_upload_metrics.pris_texture_uploads[1].executions == 1 &&
            pris_upload_metrics.last_eye_composed == 0 &&
            pris_upload_metrics.last_eye_draws == 0 &&
            pris_upload_metrics.last_eye_triangles == 0 &&
            pris_upload_metrics.last_eye_missing_textures == 0 &&
            pris_upload_metrics.last_eye_unexpected_dma == 0 &&
            pris_upload_metrics.last_eye_duplicate_slot_writes == 0 &&
            pris_upload_metrics.last_eye_command_buffers_committed == 0 &&
            pris_upload_metrics.last_eye_command_buffers_completed == 0 &&
            pris_upload_metrics.last_eye_command_buffer_errors == 0 &&
            pris_upload_metrics.skipped_bucket_bytes == 0,
        "all six PRIS and six empty PRIS2 callbacks run, with one ordinary upload and no eyes");
  goal_jak2_metal_host_destroy(pris_upload_host);

  goal_jak2_metal_host* pris2_host = goal_jak2_metal_host_create();
  goal_gfx_host pris2_callbacks = {};
  check(pris2_host && goal_jak2_metal_host_copy_gfx_host(pris2_host, &pris2_callbacks),
        "created a host for all six typed PRIS2 texture buckets");
  write_empty_texture_page(kTexturePageOffset, kTexturePageId);
  for (const u32 bucket_id : metal_renderer::kJak2Pris2TextureUploadBuckets) {
    make_pris2_ordinary_only_chain(bucket_id);
    pris2_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  }
  goal_jak2_metal_host_metrics pris2_metrics = {};
  check(goal_jak2_metal_host_get_metrics(pris2_host, &pris2_metrics) &&
            pris2_metrics.chains == metal_renderer::kJak2Pris2TextureUploadBuckets.size() &&
            pris2_metrics.completed_chains ==
                metal_renderer::kJak2Pris2TextureUploadBuckets.size() &&
            pris2_metrics.failed_chains == 0 &&
            pris2_metrics.last_buckets_dispatched == kBucketCount &&
            pris2_metrics.last_pris_eye_dispatches ==
                GOAL_JAK2_PRIS_TEXTURE_UPLOAD_BUCKET_COUNT +
                    metal_renderer::kJak2Pris2TextureUploadBuckets.size() &&
            pris2_metrics.last_pris_eye_present_dispatches == 1 &&
            pris2_metrics.last_pris_eye_chunks == 0 &&
            pris2_metrics.pris2_bucket_captures[0].bucket_id == kPris2Bucket228 &&
            pris2_metrics.pris2_bucket_captures[0].captures ==
                metal_renderer::kJak2Pris2TextureUploadBuckets.size() &&
            pris2_metrics.pris2_bucket_captures[0].present_captures == 1 &&
            pris2_metrics.pris2_bucket_captures[0].executions == 1 &&
            pris2_metrics.pris2_bucket_captures[1].bucket_id == 229 &&
            pris2_metrics.pris2_bucket_captures[1].executions == 0 &&
            pris2_metrics.last_eye_composed == 0 &&
            pris2_metrics.last_eye_command_buffers_committed == 0 &&
            pris2_metrics.last_eye_command_buffer_errors == 0 &&
            pris2_metrics.skipped_bucket_bytes == 0,
        "all six PRIS2 texture slots execute their ordinary form without resizing passive ABI "
        "metrics");
  goal_jak2_metal_host_destroy(pris2_host);

  goal_jak2_metal_host* missing_prison_host = goal_jak2_metal_host_create();
  goal_gfx_host missing_prison_callbacks = {};
  check(missing_prison_host &&
            goal_jak2_metal_host_copy_gfx_host(missing_prison_host,
                                               &missing_prison_callbacks),
        "created a host without common prison CLUT inputs");
  write_empty_texture_page(kTexturePageOffset, kTexturePageId);
  make_prison_clut_chain(0.5f);
  missing_prison_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  goal_jak2_metal_host_metrics missing_prison_metrics = {};
  const char* missing_prison_error =
      goal_jak2_metal_host_last_error(missing_prison_host);
  check(goal_jak2_metal_host_get_metrics(missing_prison_host,
                                         &missing_prison_metrics) &&
            missing_prison_metrics.chains == 1 &&
            missing_prison_metrics.completed_chains == 0 &&
            missing_prison_metrics.failed_chains == 1 &&
            missing_prison_metrics.pris_texture_uploads[2].executions == 0 &&
            missing_prison_metrics.prison_clut_preparations == 0 &&
            missing_prison_metrics.prison_clut_publications == 0 &&
            missing_prison_error &&
            std::strstr(missing_prison_error, "common level art is unavailable"),
        "prison CLUT preparation fails before the ordinary upload or registry mutation");
  goal_jak2_metal_host_destroy(missing_prison_host);

  goal_jak2_metal_host* prison_host = goal_jak2_metal_host_create();
  goal_gfx_host prison_callbacks = {};
  const std::string prison_directory = (fixture_root / "prison").string();
  check(prison_host &&
            goal_jak2_metal_host_configure_level_art(prison_host,
                                                     prison_directory.c_str()) &&
            goal_jak2_metal_host_copy_gfx_host(prison_host, &prison_callbacks),
        "created a host with synthetic GAME.fr3 prison CLUT IndexTextures");
  make_prison_clut_chain(0.5f);
  prison_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  goal_jak2_metal_host_metrics prison_metrics = {};
  check(goal_jak2_metal_host_get_metrics(prison_host, &prison_metrics),
        "copied metrics after the prison CLUT PRIS chain");
  constexpr std::array<u64, 6> kExpectedTbps = {0x1000, 0x1010, 0x1030,
                                                0x1040, 0x1050, 0x1060};
  constexpr std::array<u32, 6> kExpectedSlots = {4, 5, 7, 8, 9, 10};
  std::array<u64, 6> first_prison_handles = {};
  bool exact_prison_outputs = true;
  for (std::size_t i = 0; i < first_prison_handles.size(); ++i) {
    first_prison_handles[i] = prison_metrics.prison_clut_textures[i];
    exact_prison_outputs = exact_prison_outputs && first_prison_handles[i] != 0 &&
                           prison_metrics.prison_clut_destination_tbps[i] ==
                               kExpectedTbps[i] &&
                           prison_metrics.prison_clut_anim_slots[i] == kExpectedSlots[i];
  }
  check(prison_metrics.chains == 1 && prison_metrics.completed_chains == 1 &&
            prison_metrics.failed_chains == 0 &&
            prison_metrics.last_pris_eye_dispatches ==
                GOAL_JAK2_PRIS_TEXTURE_UPLOAD_BUCKET_COUNT +
                    metal_renderer::kJak2Pris2TextureUploadBuckets.size() &&
            prison_metrics.last_pris_eye_present_dispatches == 1 &&
            prison_metrics.pris_texture_uploads[2].bucket_id == kPrisonClutBucket &&
            prison_metrics.pris_texture_uploads[2].transfers == 9 &&
            prison_metrics.pris_texture_uploads[2].payload_bytes == 224 &&
            prison_metrics.pris_texture_uploads[2].ordinary_descriptors == 1 &&
            prison_metrics.pris_texture_uploads[2].animator_arrays == 1 &&
            prison_metrics.pris_texture_uploads[2].opcode_counts[23] == 1 &&
            prison_metrics.pris_texture_uploads[2].executions == 1 &&
            prison_metrics.prison_clut_preparations == 1 &&
            prison_metrics.prison_clut_publications == 1 && exact_prison_outputs,
        "bucket 204 runs one ordinary upload then publishes six exact CLUT outputs");

  make_prison_clut_chain(0.25f);
  prison_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(prison_host, &prison_metrics),
        "copied metrics after the repeated prison CLUT PRIS chain");
  bool stable_prison_handles = true;
  for (std::size_t i = 0; i < first_prison_handles.size(); ++i) {
    stable_prison_handles = stable_prison_handles &&
                            prison_metrics.prison_clut_textures[i] ==
                                first_prison_handles[i];
  }
  check(prison_metrics.chains == 2 && prison_metrics.completed_chains == 2 &&
            prison_metrics.failed_chains == 0 &&
            prison_metrics.pris_texture_uploads[2].executions == 2 &&
            prison_metrics.prison_clut_preparations == 2 &&
            prison_metrics.prison_clut_publications == 2 && stable_prison_handles,
        "a later morph reuses all six stable host-owned registry handles");

  make_prison_clut_chain(0.75f, kOtherPrisAnimatorBucket, 0x100);
  prison_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(prison_host, &prison_metrics),
        "copied metrics after a source-valid animator in bucket 208");
  constexpr std::array<u32, 6> kExpectedMovedTbps = {0x1100, 0x1110, 0x1130,
                                                     0x1140, 0x1150, 0x1160};
  bool stable_moved_prison_outputs = true;
  for (std::size_t i = 0; i < first_prison_handles.size(); ++i) {
    stable_moved_prison_outputs = stable_moved_prison_outputs &&
                                  prison_metrics.prison_clut_textures[i] ==
                                      first_prison_handles[i] &&
                                  prison_metrics.prison_clut_destination_tbps[i] ==
                                      kExpectedMovedTbps[i];
  }
  check(prison_metrics.chains == 3 && prison_metrics.completed_chains == 3 &&
            prison_metrics.failed_chains == 0 &&
            prison_metrics.pris_texture_uploads[3].bucket_id == kOtherPrisAnimatorBucket &&
            prison_metrics.pris_texture_uploads[3].animator_arrays == 1 &&
            prison_metrics.pris_texture_uploads[3].executions == 1 &&
            prison_metrics.prison_clut_preparations == 3 &&
            prison_metrics.prison_clut_publications == 3 && stable_moved_prison_outputs &&
            first_pixel(first_prison_handles[0]) == std::array<u8, 4>{16, 17, 18, 255},
        "the moved bucket 208 animator updates data and TBP telemetry through stable handles");

  const std::size_t live_textures_before_duplicate = metal_texture_live_count();
  const auto first_pixel_before_duplicate = first_pixel(first_prison_handles[0]);
  make_duplicate_prison_clut_chain();
  prison_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  const char* duplicate_prison_error = goal_jak2_metal_host_last_error(prison_host);
  check(goal_jak2_metal_host_get_metrics(prison_host, &prison_metrics),
        "copied metrics after duplicate prison animators");
  check(prison_metrics.chains == 4 && prison_metrics.completed_chains == 3 &&
            prison_metrics.failed_chains == 1 &&
            prison_metrics.prison_clut_preparations == 3 &&
            prison_metrics.prison_clut_publications == 3 &&
            prison_metrics.pris_texture_uploads[2].executions == 2 &&
            prison_metrics.pris_texture_uploads[3].executions == 1 &&
            metal_texture_live_count() == live_textures_before_duplicate &&
            first_pixel(first_prison_handles[0]) == first_pixel_before_duplicate &&
            duplicate_prison_error &&
            std::strstr(duplicate_prison_error, "multiple PRIS buckets"),
        "duplicate prison animators fail before preparation, render, or texture mutation");
  goal_jak2_metal_host_destroy(prison_host);
  check(metal_level_data::level_count() == initial_level_count &&
            metal_merc_models().level_count() == initial_merc_level_count &&
            metal_merc_models().model_count() == initial_merc_model_count &&
            metal_texture_live_count() == initial_texture_count,
        "prison CLUT host teardown releases GAME.fr3 and all six publications");

  goal_jak2_metal_host* sprite_upload_host = goal_jak2_metal_host_create();
  goal_gfx_host sprite_upload_callbacks = {};
  check(sprite_upload_host &&
            goal_jak2_metal_host_copy_gfx_host(sprite_upload_host, &sprite_upload_callbacks),
        "created a host for bucket-312 sprite texture-upload integration");
  write_texture_page();
  make_sprite_texture_upload_chain();
  sprite_upload_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  goal_jak2_metal_host_metrics sprite_upload_metrics = {};
  check(sprite_upload_host &&
            goal_jak2_metal_host_get_metrics(sprite_upload_host, &sprite_upload_metrics),
        "copied metrics after a valid bucket-312 upload");
  check(sprite_upload_metrics.chains == 1 &&
            sprite_upload_metrics.completed_chains == 1 &&
            sprite_upload_metrics.failed_chains == 0 &&
            sprite_upload_metrics.sprite_texture_uploads == 1 &&
            sprite_upload_metrics.last_sprite_texture_upload.valid == 1 &&
            sprite_upload_metrics.last_sprite_texture_upload.present == 1 &&
            sprite_upload_metrics.last_sprite_texture_upload.upload_count == 1 &&
            sprite_upload_metrics.last_sprite_texture_upload.pages[0] == kTexturePageOffset &&
            sprite_upload_metrics.last_sprite_texture_upload.modes[0] == -1 &&
            sprite_upload_metrics.skipped_bucket_bytes == 0,
        "bucket 312 dispatches its exact ordered ordinary upload through the host callback");

  write_empty_texture_page(kTexturePageOffset + kTexturePageStride, kTexturePageId + 1);
  write_empty_texture_page(kTexturePageOffset + 2 * kTexturePageStride, kTexturePageId + 2);
  make_sprite_texture_upload_chain(3);
  sprite_upload_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(sprite_upload_host, &sprite_upload_metrics) &&
            sprite_upload_metrics.chains == 2 &&
            sprite_upload_metrics.completed_chains == 2 &&
            sprite_upload_metrics.failed_chains == 0 &&
            sprite_upload_metrics.sprite_texture_uploads == 4 &&
            sprite_upload_metrics.last_sprite_texture_upload.valid == 1 &&
            sprite_upload_metrics.last_sprite_texture_upload.present == 1 &&
            sprite_upload_metrics.last_sprite_texture_upload.upload_count == 3 &&
            sprite_upload_metrics.last_sprite_texture_upload.pages[0] == kTexturePageOffset &&
            sprite_upload_metrics.last_sprite_texture_upload.pages[1] ==
                kTexturePageOffset + kTexturePageStride &&
            sprite_upload_metrics.last_sprite_texture_upload.pages[2] ==
                kTexturePageOffset + 2 * kTexturePageStride &&
            sprite_upload_metrics.last_sprite_texture_upload.modes[0] == -1 &&
            sprite_upload_metrics.last_sprite_texture_upload.modes[1] == -1 &&
            sprite_upload_metrics.last_sprite_texture_upload.modes[2] == -1 &&
            sprite_upload_metrics.skipped_bucket_bytes == 0,
        "three source-ordered bucket-312 uploads execute and remain attributed in order");

  make_empty_chain();
  sprite_upload_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(sprite_upload_host, &sprite_upload_metrics) &&
            sprite_upload_metrics.chains == 3 &&
            sprite_upload_metrics.completed_chains == 3 &&
            sprite_upload_metrics.failed_chains == 0 &&
            sprite_upload_metrics.sprite_texture_uploads == 4 &&
            sprite_upload_metrics.last_sprite_texture_upload.valid == 1 &&
            sprite_upload_metrics.last_sprite_texture_upload.present == 0 &&
            sprite_upload_metrics.last_sprite_texture_upload.upload_count == 0,
        "a strict-empty bucket 312 still dispatches its host marker without another upload");
  const uint32_t sprite_empty_copied_bytes = sprite_upload_metrics.last_copied_bytes;

  make_sprite_texture_upload_chain(1, -2);
  sprite_upload_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  const char* sprite_upload_error = goal_jak2_metal_host_last_error(sprite_upload_host);
  check(goal_jak2_metal_host_get_metrics(sprite_upload_host, &sprite_upload_metrics) &&
            sprite_upload_metrics.chains == 4 &&
            sprite_upload_metrics.completed_chains == 3 &&
            sprite_upload_metrics.failed_chains == 1 &&
            sprite_upload_metrics.sprite_texture_uploads == 4 &&
            sprite_upload_metrics.last_sprite_texture_upload.valid == 0 &&
            sprite_upload_metrics.last_copied_bytes == sprite_empty_copied_bytes &&
            sprite_upload_error &&
            std::strstr(sprite_upload_error,
                        "bucket 312 texture-upload plan rejected malformed DMA"),
        "malformed bucket 312 fails before upload execution, copying, or dispatch");

  make_sprite_texture_upload_chain();
  sprite_upload_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(sprite_upload_host, &sprite_upload_metrics) &&
            sprite_upload_metrics.chains == 5 &&
            sprite_upload_metrics.completed_chains == 4 &&
            sprite_upload_metrics.failed_chains == 1 &&
            sprite_upload_metrics.sprite_texture_uploads == 5,
        "a pre-mutation bucket-312 rejection leaves the host usable by a repaired chain");
  goal_jak2_metal_host_destroy(sprite_upload_host);

  goal_jak2_metal_host* sky_post_host = goal_jak2_metal_host_create();
  goal_gfx_host sky_post_callbacks = {};
  check(sky_post_host &&
            goal_jak2_metal_host_copy_gfx_host(sky_post_host, &sky_post_callbacks),
        "created a host for exact bucket-309 sky-post texture uploads");
  write_texture_page();
  make_sky_post_texture_upload_chain();
  sky_post_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  goal_jak2_metal_host_metrics sky_post_metrics = {};
  check(goal_jak2_metal_host_get_metrics(sky_post_host, &sky_post_metrics) &&
            sky_post_metrics.chains == 1 && sky_post_metrics.completed_chains == 1 &&
            sky_post_metrics.failed_chains == 0 &&
            sky_post_metrics.sky_post_texture_upload_executions == 1 &&
            sky_post_metrics.skipped_bucket_bytes == 0,
        "bucket 309 executes its exact source-shaped ordinary upload once at bucket entry");

  make_empty_chain();
  sky_post_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(sky_post_host, &sky_post_metrics) &&
            sky_post_metrics.chains == 2 && sky_post_metrics.completed_chains == 2 &&
            sky_post_metrics.failed_chains == 0 &&
            sky_post_metrics.sky_post_texture_upload_executions == 1,
        "an absent bucket 309 still dispatches its marker without another upload");
  const uint32_t sky_post_empty_copied_bytes = sky_post_metrics.last_copied_bytes;

  make_sky_post_texture_upload_chain(-2);
  sky_post_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  const char* sky_post_error = goal_jak2_metal_host_last_error(sky_post_host);
  check(goal_jak2_metal_host_get_metrics(sky_post_host, &sky_post_metrics) &&
            sky_post_metrics.chains == 3 && sky_post_metrics.completed_chains == 2 &&
            sky_post_metrics.failed_chains == 1 &&
            sky_post_metrics.sky_post_texture_upload_executions == 1 &&
            sky_post_metrics.last_copied_bytes == sky_post_empty_copied_bytes &&
            sky_post_error && std::strstr(sky_post_error, "sky-post texture plan rejected"),
        "malformed bucket 309 fails before copying, mutation, or upload execution");
  goal_jak2_metal_host_destroy(sky_post_host);

  goal_jak2_metal_host* map_upload_host = goal_jak2_metal_host_create();
  goal_gfx_host map_upload_callbacks = {};
  check(map_upload_host &&
            goal_jak2_metal_host_configure_level_art(map_upload_host, fr3_directory.c_str()) &&
            goal_jak2_metal_host_copy_gfx_host(map_upload_host, &map_upload_callbacks),
        "created a configured host for bucket-319 map upload and PROGRESS integration");
  write_texture_page();
  make_map_texture_upload_and_progress_chain();
  map_upload_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  goal_jak2_metal_host_metrics map_upload_metrics = {};
  check(map_upload_host &&
            goal_jak2_metal_host_get_metrics(map_upload_host, &map_upload_metrics),
        "copied metrics after a map upload followed by textured PROGRESS drawing");
  check(map_upload_metrics.chains == 1 && map_upload_metrics.completed_chains == 1 &&
            map_upload_metrics.failed_chains == 0 &&
            map_upload_metrics.map_texture_uploads == 1 &&
            map_upload_metrics.last_map_texture_upload.valid == 1 &&
            map_upload_metrics.last_map_texture_upload.present == 1 &&
            map_upload_metrics.last_map_texture_upload.upload_count == 1 &&
            map_upload_metrics.last_map_texture_upload.pages[0] == kTexturePageOffset &&
            map_upload_metrics.last_map_texture_upload.modes[0] == -1 &&
            map_upload_metrics.last_progress_draws == 1 &&
            map_upload_metrics.last_progress_triangles == 1 &&
            map_upload_metrics.last_progress_textured_draws == 1 &&
            map_upload_metrics.last_progress_missing_texture_draws == 0 &&
            map_upload_metrics.skipped_bucket_bytes == 0,
        "bucket 319 uploads the map page before the dependent textured PROGRESS draw");

  make_map_descriptor_first_upload_and_progress_chain();
  map_upload_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(map_upload_host, &map_upload_metrics) &&
            map_upload_metrics.chains == 2 && map_upload_metrics.completed_chains == 2 &&
            map_upload_metrics.failed_chains == 0 &&
            map_upload_metrics.map_texture_uploads == 2 &&
            map_upload_metrics.last_map_texture_upload.valid == 1 &&
            map_upload_metrics.last_map_texture_upload.present == 1 &&
            map_upload_metrics.last_map_texture_upload.upload_count == 1 &&
            map_upload_metrics.last_map_texture_upload.pages[0] == kTexturePageOffset &&
            map_upload_metrics.last_map_texture_upload.modes[0] == -1 &&
            map_upload_metrics.last_progress_draws == 1 &&
            map_upload_metrics.last_progress_triangles == 1 &&
            map_upload_metrics.last_progress_textured_draws == 1 &&
            map_upload_metrics.last_progress_missing_texture_draws == 0 &&
            map_upload_metrics.skipped_bucket_bytes == 0,
        "descriptor-first bucket 319 uploads before PROGRESS without a Direct prefix or tail");

  write_empty_texture_page(kTexturePageOffset + kTexturePageStride, kTexturePageId + 1);
  make_map_descriptor_prefix_legacy_upload_and_progress_chain();
  map_upload_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(map_upload_host, &map_upload_metrics) &&
            map_upload_metrics.chains == 3 && map_upload_metrics.completed_chains == 3 &&
            map_upload_metrics.failed_chains == 0 &&
            map_upload_metrics.map_texture_uploads == 4 &&
            map_upload_metrics.last_map_texture_upload.valid == 1 &&
            map_upload_metrics.last_map_texture_upload.present == 1 &&
            map_upload_metrics.last_map_texture_upload.upload_count == 2 &&
            map_upload_metrics.last_map_texture_upload.pages[0] == kTexturePageOffset &&
            map_upload_metrics.last_map_texture_upload.pages[1] ==
                kTexturePageOffset + kTexturePageStride &&
            map_upload_metrics.last_map_texture_upload.modes[0] == -1 &&
            map_upload_metrics.last_map_texture_upload.modes[1] == -1 &&
            map_upload_metrics.last_progress_draws == 1 &&
            map_upload_metrics.last_progress_triangles == 1 &&
            map_upload_metrics.last_progress_textured_draws == 1 &&
            map_upload_metrics.last_progress_missing_texture_draws == 0 &&
            map_upload_metrics.skipped_bucket_bytes == 0,
        "descriptor-prefix uploads stay ordered through the legacy group before PROGRESS");
  const uint32_t map_copied_bytes = map_upload_metrics.last_copied_bytes;

  make_map_texture_upload_and_progress_chain(-2);
  map_upload_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  const char* map_upload_error = goal_jak2_metal_host_last_error(map_upload_host);
  check(goal_jak2_metal_host_get_metrics(map_upload_host, &map_upload_metrics) &&
            map_upload_metrics.chains == 4 && map_upload_metrics.completed_chains == 3 &&
            map_upload_metrics.failed_chains == 1 &&
            map_upload_metrics.map_texture_uploads == 4 &&
            map_upload_metrics.last_map_texture_upload.valid == 0 &&
            map_upload_metrics.last_copied_bytes == map_copied_bytes && map_upload_error &&
            std::strstr(map_upload_error,
                        "bucket 319 texture-upload plan rejected malformed DMA") &&
            std::strstr(map_upload_error, "stage=ordinary-contents") &&
            std::strstr(map_upload_error, "transfers=3 rejected=2") &&
            std::strstr(map_upload_error,
                        "failed=CNT:q1:b16:v0=0x08000000:v1=0x00000003:spr0") &&
            std::strstr(map_upload_error, "trace=0@"),
        "malformed bucket 319 fails before upload execution, copying, or dispatch");

  make_map_texture_upload_and_progress_chain();
  map_upload_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(map_upload_host, &map_upload_metrics) &&
            map_upload_metrics.chains == 5 && map_upload_metrics.completed_chains == 4 &&
            map_upload_metrics.failed_chains == 1 &&
            map_upload_metrics.map_texture_uploads == 5 &&
            map_upload_metrics.last_progress_draws == 1 &&
            map_upload_metrics.last_progress_missing_texture_draws == 0,
        "a pre-mutation bucket-319 rejection leaves map upload and PROGRESS usable");
  goal_jak2_metal_host_destroy(map_upload_host);

  goal_jak2_metal_host* raw_image_host = goal_jak2_metal_host_create();
  goal_gfx_host raw_image_callbacks = {};
  check(raw_image_host &&
            goal_jak2_metal_host_copy_gfx_host(raw_image_host, &raw_image_callbacks),
        "created a host for bucket-318 raw-image upload integration");
  goal_jak2_metal_host_metrics raw_image_metrics = {};
  const auto send_raw_image_fixture = [&](const auto& fixture) {
    std::memcpy(static_cast<u8*>(g_ee_main_mem) + kChainOffset,
                fixture.ee_memory.data() + kChainOffset,
                fixture.ee_memory.size() - kChainOffset);
    raw_image_callbacks.send_chain(g_ee_main_mem, fixture.chain_offset);
  };

  auto raw_image_fixture =
      metal_renderer::make_jak2_raw_image_direct_only_fixture(kChainOffset);
  send_raw_image_fixture(raw_image_fixture);
  check(raw_image_host &&
            goal_jak2_metal_host_get_metrics(raw_image_host, &raw_image_metrics),
        "copied metrics after the public synthetic Direct-only chain");
  check(raw_image_metrics.chains == 1 && raw_image_metrics.completed_chains == 1 &&
            raw_image_metrics.failed_chains == 0 &&
            raw_image_metrics.raw_image_publications == 0 &&
            raw_image_metrics.last_debug_no_zbuf1_draws == 1 &&
            raw_image_metrics.last_debug_no_zbuf1_triangles == 2 &&
            raw_image_metrics.last_debug_no_zbuf1_textured_draws == 0 &&
            raw_image_metrics.skipped_bucket_bytes == 0,
        "bucket 318 accepts a Direct-only overlay without publishing a raw image");

  raw_image_fixture =
      metal_renderer::make_jak2_raw_image_direct_before_upload_fixture(kChainOffset);
  send_raw_image_fixture(raw_image_fixture);
  check(goal_jak2_metal_host_get_metrics(raw_image_host, &raw_image_metrics) &&
            raw_image_metrics.chains == 2 && raw_image_metrics.completed_chains == 2 &&
            raw_image_metrics.failed_chains == 0 &&
            raw_image_metrics.raw_image_publications == 1 &&
            raw_image_metrics.raw_image_texture != 0 &&
            raw_image_metrics.raw_image_pixels ==
                static_cast<u64>(metal_renderer::kJak2RawImageWidth) *
                    metal_renderer::kJak2RawImageHeight &&
            raw_image_metrics.last_debug_no_zbuf1_draws == 1 &&
            raw_image_metrics.last_debug_no_zbuf1_triangles == 2 &&
            raw_image_metrics.last_debug_no_zbuf1_textured_draws == 0 &&
            raw_image_metrics.skipped_bucket_bytes == 0,
        "bucket 318 preserves Direct-before work and then publishes at PC_PORT 12");

  raw_image_fixture =
      metal_renderer::make_jak2_raw_image_upload_before_direct_fixture(kChainOffset);
  send_raw_image_fixture(raw_image_fixture);
  check(goal_jak2_metal_host_get_metrics(raw_image_host, &raw_image_metrics) &&
            raw_image_metrics.chains == 3 && raw_image_metrics.completed_chains == 3 &&
            raw_image_metrics.failed_chains == 0 &&
            raw_image_metrics.raw_image_publications == 2 &&
            raw_image_metrics.last_debug_no_zbuf1_draws == 2 &&
            raw_image_metrics.last_debug_no_zbuf1_triangles == 2 &&
            raw_image_metrics.last_debug_no_zbuf1_textured_draws == 2 &&
            raw_image_metrics.last_debug_no_zbuf1_missing_texture_draws == 0 &&
            raw_image_metrics.skipped_bucket_bytes == 0,
        "bucket 318 publishes before a trailing textured Direct overlay resolves TBP0");

  raw_image_fixture =
      metal_renderer::make_jak2_raw_image_mixed_overlay_fixture(kChainOffset);
  send_raw_image_fixture(raw_image_fixture);
  check(goal_jak2_metal_host_get_metrics(raw_image_host, &raw_image_metrics) &&
            raw_image_metrics.chains == 4 && raw_image_metrics.completed_chains == 4 &&
            raw_image_metrics.failed_chains == 0 &&
            raw_image_metrics.raw_image_publications == 3 &&
            raw_image_metrics.last_debug_no_zbuf1_draws == 3 &&
            raw_image_metrics.last_debug_no_zbuf1_triangles == 4 &&
            raw_image_metrics.last_debug_no_zbuf1_textured_draws == 2 &&
            raw_image_metrics.last_debug_no_zbuf1_missing_texture_draws == 0 &&
            raw_image_metrics.skipped_bucket_bytes == 0,
        "bucket 318 accepts Direct on both sides of one raw-image publication marker");

  make_empty_chain();
  raw_image_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(raw_image_host, &raw_image_metrics) &&
            raw_image_metrics.chains == 5 && raw_image_metrics.completed_chains == 5 &&
            raw_image_metrics.failed_chains == 0 &&
            raw_image_metrics.raw_image_publications == 3 &&
            raw_image_metrics.last_debug_no_zbuf1_draws == 0 &&
            raw_image_metrics.last_debug_no_zbuf1_triangles == 0,
        "a strict-empty bucket 318 neither republishes nor draws");
  const uint32_t raw_image_empty_copied_bytes = raw_image_metrics.last_copied_bytes;

  raw_image_fixture = metal_renderer::make_jak2_raw_image_upload_fixture(kChainOffset);
  raw_image_fixture.ee_memory[raw_image_fixture.upload_data_offset + 13] = 0;
  send_raw_image_fixture(raw_image_fixture);
  const char* raw_image_error = goal_jak2_metal_host_last_error(raw_image_host);
  check(goal_jak2_metal_host_get_metrics(raw_image_host, &raw_image_metrics) &&
            raw_image_metrics.chains == 6 && raw_image_metrics.completed_chains == 5 &&
            raw_image_metrics.failed_chains == 1 &&
            raw_image_metrics.raw_image_publications == 3 &&
            raw_image_metrics.last_copied_bytes == raw_image_empty_copied_bytes &&
            raw_image_error &&
            std::strstr(raw_image_error,
                        "bucket 318 raw-image upload plan rejected malformed DMA"),
        "malformed bucket 318 fails before publication, copying, or dispatch");

  raw_image_fixture = metal_renderer::make_jak2_raw_image_upload_fixture(kChainOffset);
  constexpr u32 kDuplicateRawImageOffset = kChainOffset + 0x7000;
  metal_renderer::jak2_raw_image_fixture_detail::put_tag(
      raw_image_fixture.ee_memory, raw_image_fixture.final_boundary_offset,
      DmaTag::Kind::NEXT, 0, kDuplicateRawImageOffset);
  std::memcpy(raw_image_fixture.ee_memory.data() + kDuplicateRawImageOffset,
              raw_image_fixture.ee_memory.data() + raw_image_fixture.start_tag_offset, 64);
  metal_renderer::jak2_raw_image_fixture_detail::put_tag(
      raw_image_fixture.ee_memory, kDuplicateRawImageOffset + 64, DmaTag::Kind::NEXT, 0,
      raw_image_fixture.bucket_offset + 16);
  send_raw_image_fixture(raw_image_fixture);
  raw_image_error = goal_jak2_metal_host_last_error(raw_image_host);
  check(goal_jak2_metal_host_get_metrics(raw_image_host, &raw_image_metrics) &&
            raw_image_metrics.chains == 7 && raw_image_metrics.completed_chains == 5 &&
            raw_image_metrics.failed_chains == 2 &&
            raw_image_metrics.raw_image_publications == 3 &&
            raw_image_metrics.last_copied_bytes == raw_image_empty_copied_bytes &&
            raw_image_error &&
            std::strstr(raw_image_error,
                        "bucket 318 raw-image upload plan rejected malformed DMA"),
        "duplicate bucket-318 publication markers fail before mutation or dispatch");

  raw_image_fixture = metal_renderer::make_jak2_raw_image_upload_fixture(kChainOffset);
  send_raw_image_fixture(raw_image_fixture);
  check(goal_jak2_metal_host_get_metrics(raw_image_host, &raw_image_metrics) &&
            raw_image_metrics.chains == 8 && raw_image_metrics.completed_chains == 6 &&
            raw_image_metrics.failed_chains == 2 &&
            raw_image_metrics.raw_image_publications == 4 &&
            raw_image_metrics.last_debug_no_zbuf1_textured_draws == 2,
        "a pre-mutation bucket-318 rejection leaves publication and Direct drawing usable");
  goal_jak2_metal_host_destroy(raw_image_host);

  goal_jak2_metal_host* common_pris_capture_host = goal_jak2_metal_host_create();
  goal_gfx_host common_pris_capture_callbacks = {};
  check(common_pris_capture_host &&
            goal_jak2_metal_host_configure_level_art(common_pris_capture_host,
                                                     prison_directory.c_str()) &&
            goal_jak2_metal_host_copy_gfx_host(common_pris_capture_host,
                                               &common_pris_capture_callbacks),
        "created a configured host for common PRIS texture execution");
  write_empty_texture_page(kTexturePageOffset, kTexturePageId);
  make_common_pris_opcode22_capture_chain();
  const std::size_t common_pris_textures_before = metal_texture_live_count();
  common_pris_capture_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  goal_jak2_metal_host_metrics common_pris_capture_metrics = {};
  const auto& common_pris = common_pris_capture_metrics.common_pris_texture_upload;
  check(goal_jak2_metal_host_get_metrics(common_pris_capture_host,
                                         &common_pris_capture_metrics) &&
            common_pris_capture_metrics.chains == 1 &&
            common_pris_capture_metrics.completed_chains == 1 &&
            common_pris_capture_metrics.failed_chains == 0 &&
            common_pris.bucket_id == kCommonPrisBucket && common_pris.captures == 1 &&
            common_pris.present_captures == 1 && common_pris.executions == 1 &&
            common_pris.classifications[static_cast<std::size_t>(
                metal_renderer::Jak2CommonTfragTextureUploadClass::OrdinaryAndAnimator)] == 1 &&
            common_pris.transfers == 9 && common_pris.payload_bytes == 208 &&
            common_pris.inert_transfers == 4 && common_pris.ordinary_descriptors == 1 &&
            common_pris.gs_setup_transfers == 0 && common_pris.direct_setup_transfers == 1 &&
            common_pris.animator_arrays == 1 && common_pris.animator_body_transfers == 1 &&
            common_pris.animator_payload_bytes == 32 && common_pris.opcode_counts[12] == 1 &&
            common_pris.opcode_counts[13] == 1 && common_pris.opcode_counts[22] == 1 &&
            common_pris.other_transfers == 0 && common_pris.malformed_transfers == 0 &&
            common_pris_capture_metrics.texture_uploads == 0 &&
            metal_texture_live_count() == common_pris_textures_before,
        "common PRIS updates the four defaults through their stable handles");
  make_common_pris_opcode22_capture_chain();
  common_pris_capture_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(common_pris_capture_host,
                                         &common_pris_capture_metrics) &&
            common_pris_capture_metrics.chains == 2 &&
            common_pris_capture_metrics.completed_chains == 2 &&
            common_pris_capture_metrics.failed_chains == 0 &&
            common_pris_capture_metrics.common_pris_texture_upload.executions == 2 &&
            metal_texture_live_count() == common_pris_textures_before,
        "a later common PRIS frame updates the same four stable registry textures");
  goal_jak2_metal_host_destroy(common_pris_capture_host);
  check(metal_texture_live_count() == initial_texture_count,
        "common PRIS host teardown releases common art and all four Dark Jak outputs");

  goal_jak2_metal_host* replacement = goal_jak2_metal_host_create();
  check(replacement != nullptr, "host ownership can be re-established after destruction");
  check(replacement &&
            goal_jak2_metal_host_configure_level_art(replacement, fr3_directory.c_str()),
        "a recreated host can own the same FR3 directory after exact teardown");
  goal_jak2_metal_host_destroy(replacement);

  goal_jak2_metal_host* basename_host = goal_jak2_metal_host_create();
  goal_gfx_host basename_callbacks = {};
  check(basename_host &&
            goal_jak2_metal_host_configure_level_art(basename_host, fr3_directory.c_str()) &&
            goal_jak2_metal_host_copy_gfx_host(basename_host, &basename_callbacks),
        "created a configured host for basename validation");
  const char* escaped_level[] = {"../outside"};
  if (basename_callbacks.set_levels) {
    basename_callbacks.set_levels(escaped_level, 1);
  }
  const char* basename_error = goal_jak2_metal_host_last_error(basename_host);
  check(basename_error && std::strstr(basename_error, "non-basename"),
        "set-levels rejects path traversal instead of leaving the FR3 directory");
  goal_jak2_metal_host_destroy(basename_host);

  goal_jak2_metal_host* late_host = goal_jak2_metal_host_create();
  goal_gfx_host late_callbacks = {};
  check(late_host && goal_jak2_metal_host_copy_gfx_host(late_host, &late_callbacks),
        "created a callback-published host for late-configuration rejection");
  check(late_host &&
            !goal_jak2_metal_host_configure_level_art(late_host, fr3_directory.c_str()),
        "rejected level-art configuration after callbacks were published");
  goal_jak2_metal_host_destroy(late_host);
  check(metal_level_data::level_count() == initial_level_count &&
            metal_merc_models().level_count() == initial_merc_level_count &&
            metal_merc_models().model_count() == initial_merc_model_count &&
            metal_texture_live_count() == initial_texture_count,
        "host recreation and late rejection left no global Metal handles");

  goal_kernel_core_shutdown();
  std::filesystem::remove_all(fixture_root, fixture_error);

  if (failures) {
    std::printf("FAIL: %d Jak 2 Metal host lifecycle checks failed\n", failures);
    return 1;
  }
  std::printf("PASS: Jak 2 external Metal host copied, dispatched, synchronized, and released\n");
  return 0;
}
