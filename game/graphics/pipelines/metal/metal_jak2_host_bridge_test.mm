#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/util/FileUtil.h"
#include "common/util/compress.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_fixture.h"
#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"
#include "game/graphics/pipelines/metal/metal_jak2_effects_bucket315_capture.h"
#include "game/graphics/pipelines/metal/metal_eye_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_pris2_bucket228_plan.h"
#include "game/graphics/pipelines/metal/metal_jak2_raw_image_upload_fixture.h"
#include "game/graphics/pipelines/metal/metal_jak2_sky_post_texture_upload_plan.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/pipelines/metal/metal_merc_model_pool.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/kernel/core/kernel_core.h"
#include "game/runtime.h"

#import <QuartzCore/CAMetalLayer.h>

namespace {

static_assert(offsetof(goal_jak2_metal_host_metrics,
                       sky_post_texture_upload_executions) +
                  sizeof(uint64_t) ==
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
constexpr u32 kCommonPrisDescriptorOffset = kChainOffset + 0x16800;
constexpr u32 kCommonPrisAnimatorOffset = kChainOffset + 0x16a00;
constexpr u32 kCommonPrisDirectOffset = kChainOffset + 0x16b00;
constexpr u32 kPris2Bucket228 = metal_renderer::kJak2Pris2TextureUploadBucket;
constexpr u32 kPris2Bucket228DescriptorOffset = kChainOffset + 0x16c00;
constexpr u32 kPris2Bucket228DirectOffset = kChainOffset + 0x16d00;
constexpr u32 kEffectsLightningPayloadOffset = kChainOffset + 0x17000;
constexpr u32 kSkyPostBucket = metal_renderer::kJak2SkyPostTextureUploadBucket;
constexpr u32 kSkyPostGroupOffset = kChainOffset + 0x17400;
constexpr u32 kSkyPostDirectOffset = kChainOffset + 0x17500;
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

void make_empty_chain() {
  static_assert(kBucketCount == 327);
  std::memset(static_cast<u8*>(g_ee_main_mem) + kChainOffset, 0, (kBucketCount + 1) * 16);
  for (u32 bucket = 0; bucket < kBucketCount; bucket++) {
    put_tag(kChainOffset + bucket * 16, DmaTag::Kind::CNT);
  }
  put_tag(kChainOffset + kBucketCount * 16, DmaTag::Kind::END);
}

void make_effects_lightning_chain() {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  constexpr u32 kMark = static_cast<u32>(VifCode::Kind::MARK) << 24;
  constexpr u32 kStcycl = static_cast<u32>(VifCode::Kind::STCYCL) << 24;
  constexpr u32 kUnpackV432 = static_cast<u32>(VifCode::Kind::UNPACK_V4_32) << 24;
  constexpr u32 kMscalf = static_cast<u32>(VifCode::Kind::MSCALF) << 24;
  constexpr u32 kStmod = static_cast<u32>(VifCode::Kind::STMOD) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  const u32 bucket_offset = kChainOffset + kEffectsBucket * 16;
  std::memset(ee + kEffectsLightningPayloadOffset, 0, 0x400);
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kEffectsLightningPayloadOffset, kMark);
  u32 cursor = kEffectsLightningPayloadOffset;
  put_tag(cursor, DmaTag::Kind::CNT, 2, 0, 0, kDirect | 2);
  cursor += 48;
  put_tag(cursor, DmaTag::Kind::CNT, 8, 0, kStcycl, kUnpackV432);
  cursor += 144;
  put_tag(cursor, DmaTag::Kind::CNT, 2, 0, kMscalf, kStmod);
  cursor += 48;
  put_tag(cursor, DmaTag::Kind::CNT);
  cursor += 16;
  put_tag(cursor, DmaTag::Kind::CNT, 10, 0, kFlusha, kDirect | 10);
  cursor += 176;
  put_tag(cursor, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
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

void make_pris2_bucket228_ordinary_only_chain() {
  make_empty_chain();
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr s64 kMode = -1;
  constexpr u64 kPageOffset = kTexturePageOffset;
  const u32 bucket_offset = kChainOffset + kPris2Bucket228 * 16;

  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kPris2Bucket228DescriptorOffset);
  put_tag(kPris2Bucket228DescriptorOffset, DmaTag::Kind::CNT, 1, 0, kPcPort, 3);
  std::memcpy(ee + kPris2Bucket228DescriptorOffset + 16, &kPageOffset, sizeof(kPageOffset));
  std::memcpy(ee + kPris2Bucket228DescriptorOffset + 24, &kMode, sizeof(kMode));
  put_tag(kPris2Bucket228DescriptorOffset + 32, DmaTag::Kind::NEXT, 0,
          kPris2Bucket228DirectOffset);
  put_tag(kPris2Bucket228DirectOffset, DmaTag::Kind::CNT, 10, 0, kFlusha, kDirect | 10);
  std::memset(ee + kPris2Bucket228DirectOffset + 16, 0x52, 160);
  put_tag(kPris2Bucket228DirectOffset + 176, DmaTag::Kind::NEXT, 0, bucket_offset + 16);
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
                metal_renderer::Jak2MetalBucketBehavior::DeferredSkip &&
            metal_renderer::jak2_metal_host_policy_table_is_audited(),
        "bucket 315 remains DeferredSkip while the host audits its live DMA");

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
                initial_texture_count + 6 + METAL_NUM_EYE_PAIRS * 2,
        "common art, placeholder, OCEAN targets, and detached eye targets are resident");
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
  check(metrics.last_pris_eye_dispatches == kPrisBuckets.size() + 1 &&
            metrics.last_pris_eye_present_dispatches == 0 &&
            metrics.last_pris_eye_chunks == 0 && metrics.last_eye_composed == 0 &&
            metrics.last_eye_command_buffers_committed == 0 &&
            metrics.last_eye_command_buffers_completed == 0 &&
            metrics.last_eye_command_buffer_errors == 0,
        "all six per-level PRIS callbacks plus empty bucket 228 run once without eye execution");
  check(texture_captures_are_empty(metrics.water_texture_uploads, kWaterBuckets),
        "the host records all six empty source-identical water texture upload buckets");
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

  const char* missing_level[] = {"missing-level"};
  callbacks.set_levels(missing_level, 1);
  check(metal_level_data::level_count() == initial_level_count + 2 &&
            metal_merc_models().level_count() == initial_merc_level_count + 2 &&
            metal_merc_models().model_count() == initial_merc_model_count,
        "a missing requested FR3 leaves neither loader partially resident");
  make_empty_chain();
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(host, &metrics) && metrics.chains == 6 &&
            metrics.completed_chains == 5 && metrics.failed_chains == 1,
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
        "created a host for the passive bucket-315 live capture");
  make_effects_lightning_chain();
  effects_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  goal_jak2_metal_host_metrics effects_metrics = {};
  check(goal_jak2_metal_host_get_metrics(effects_host, &effects_metrics) &&
            effects_metrics.chains == 1 && effects_metrics.completed_chains == 1 &&
            effects_metrics.failed_chains == 0 && effects_metrics.effects_bucket315.captures == 1 &&
            effects_metrics.effects_bucket315.valid_captures == 1 &&
            effects_metrics.effects_bucket315.lightning_captures == 1 &&
            effects_metrics.effects_bucket315.malformed_captures == 0 &&
            effects_metrics.effects_bucket315.other_captures == 0 &&
            effects_metrics.effects_bucket315.last_transfer_count == 7 &&
            effects_metrics.effects_bucket315.last_payload_bytes == 352 &&
            effects_metrics.effects_bucket315.last_classification ==
                static_cast<uint8_t>(metal_renderer::Jak2EffectsBucket315CaptureClass::Lightning),
        "bucket 315 is captured from live DMA before copy without execution or promotion");
  goal_jak2_metal_host_destroy(effects_host);

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
                GOAL_JAK2_PRIS_TEXTURE_UPLOAD_BUCKET_COUNT + 1 &&
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
        "all six PRIS callbacks and empty bucket 228 run, with one ordinary upload and no eyes");
  goal_jak2_metal_host_destroy(pris_upload_host);

  goal_jak2_metal_host* pris2_bucket228_host = goal_jak2_metal_host_create();
  goal_gfx_host pris2_bucket228_callbacks = {};
  check(pris2_bucket228_host &&
            goal_jak2_metal_host_copy_gfx_host(pris2_bucket228_host,
                                               &pris2_bucket228_callbacks),
        "created a host for typed PRIS2 bucket 228 execution");
  write_empty_texture_page(kTexturePageOffset, kTexturePageId);
  make_pris2_bucket228_ordinary_only_chain();
  pris2_bucket228_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  goal_jak2_metal_host_metrics pris2_bucket228_metrics = {};
  check(goal_jak2_metal_host_get_metrics(pris2_bucket228_host,
                                         &pris2_bucket228_metrics) &&
            pris2_bucket228_metrics.chains == 1 &&
            pris2_bucket228_metrics.completed_chains == 1 &&
            pris2_bucket228_metrics.failed_chains == 0 &&
            pris2_bucket228_metrics.last_buckets_dispatched == kBucketCount &&
            pris2_bucket228_metrics.last_pris_eye_dispatches ==
                GOAL_JAK2_PRIS_TEXTURE_UPLOAD_BUCKET_COUNT + 1 &&
            pris2_bucket228_metrics.last_pris_eye_present_dispatches == 1 &&
            pris2_bucket228_metrics.last_pris_eye_chunks == 0 &&
            pris2_bucket228_metrics.pris2_bucket_captures[0].bucket_id == kPris2Bucket228 &&
            pris2_bucket228_metrics.pris2_bucket_captures[0].present_captures == 1 &&
            pris2_bucket228_metrics.pris2_bucket_captures[0].transfers == 5 &&
            pris2_bucket228_metrics.pris2_bucket_captures[0].payload_bytes == 176 &&
            pris2_bucket228_metrics.pris2_bucket_captures[0].executions == 1 &&
            pris2_bucket228_metrics.pris2_bucket_captures[1].bucket_id == 229 &&
            pris2_bucket228_metrics.pris2_bucket_captures[1].executions == 0 &&
            pris2_bucket228_metrics.last_eye_composed == 0 &&
            pris2_bucket228_metrics.last_eye_command_buffers_committed == 0 &&
            pris2_bucket228_metrics.last_eye_command_buffer_errors == 0 &&
            pris2_bucket228_metrics.skipped_bucket_bytes == 0,
        "bucket 228 ordinary form uploads once while bucket 229 executes zero times");
  goal_jak2_metal_host_destroy(pris2_bucket228_host);

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
                GOAL_JAK2_PRIS_TEXTURE_UPLOAD_BUCKET_COUNT + 1 &&
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
            metal_texture_live_count() == common_pris_textures_before + 4,
        "common PRIS executes the exact nine-transfer form and publishes four outputs");
  make_common_pris_opcode22_capture_chain();
  common_pris_capture_callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(common_pris_capture_host,
                                         &common_pris_capture_metrics) &&
            common_pris_capture_metrics.chains == 2 &&
            common_pris_capture_metrics.completed_chains == 2 &&
            common_pris_capture_metrics.failed_chains == 0 &&
            common_pris_capture_metrics.common_pris_texture_upload.executions == 2 &&
            metal_texture_live_count() == common_pris_textures_before + 4,
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
