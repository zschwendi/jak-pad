#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/dma/gs.h"
#include "common/util/fnv.h"

#include "game/graphics/pipelines/metal/metal_eye_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"
#include "game/graphics/pipelines/metal/metal_jak2_pris2_bucket228_plan.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr u32 kSourceTbp = 100;
constexpr u32 kSecondSourceTbp = 101;
constexpr u32 kPlaceholderSourceTbp = 102;
constexpr u64 kEyeHash = 0x123456789abcdef0ull;
constexpr u32 kPrisBucket = 200;
constexpr u32 kPrisBucketOffset = kPrisBucket * 16;
constexpr u32 kCommonPrisBucket = metal_renderer::kJak2CommonPrisTextureUploadBucket;
constexpr u32 kCommonPrisBucketOffset = kCommonPrisBucket * 16;
constexpr u32 kPrisOrdinaryOffset = 0x4000;
constexpr u32 kPrisAnimatorOffset = 0x4800;
constexpr u32 kPrisChunkOffset = 0x5000;

int failures = 0;

void check(bool condition, const char* message) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
  if (!condition) {
    failures++;
  }
}

void append_qword(std::vector<u8>* data, u64 lo, u64 hi) {
  const std::size_t offset = data->size();
  data->resize(offset + 16);
  std::memcpy(data->data() + offset, &lo, sizeof(lo));
  std::memcpy(data->data() + offset + 8, &hi, sizeof(hi));
}

void append_transfer(std::vector<u8>* chain,
                     const std::vector<u8>& payload,
                     VifCode::Kind vif0,
                     VifCode::Kind vif1,
                     u16 vif0_immediate = 0,
                     u16 vif1_immediate = 0) {
  const u64 tag = payload.size() / 16 |
                  (static_cast<u64>(DmaTag::Kind::CNT) << 28);
  const u64 transferred_tag = vif0_immediate | (static_cast<u64>(vif0) << 24) |
                              (static_cast<u64>(vif1_immediate) << 32) |
                              (static_cast<u64>(vif1) << 56);
  append_qword(chain, tag, transferred_tag);
  chain->insert(chain->end(), payload.begin(), payload.end());
}

std::vector<u8> make_adgif(u32 tbp) {
  std::vector<u8> data;
  const u64 tag = 5 | (1ull << 15) | (1ull << 60);
  append_qword(&data, tag, static_cast<u64>(GifTag::RegisterDescriptor::AD));
  AdGifData adgif = {};
  adgif.tex0_data = tbp;
  adgif.tex0_addr = static_cast<u64>(GsRegisterAddress::TEX0_1);
  adgif.tex1_addr = static_cast<u64>(GsRegisterAddress::TEX1_1);
  adgif.mip_addr = static_cast<u64>(GsRegisterAddress::MIPTBP1_1);
  adgif.clamp_addr = static_cast<u64>(GsRegisterAddress::CLAMP_1);
  adgif.alpha_addr = static_cast<u64>(GsRegisterAddress::ALPHA_1);
  const std::size_t offset = data.size();
  data.resize(offset + sizeof(adgif));
  std::memcpy(data.data() + offset, &adgif, sizeof(adgif));
  return data;
}

std::vector<u8> make_scissor(u32 pair) {
  std::vector<u8> data;
  const u64 tag = 1 | (1ull << 15) | (1ull << 60);
  append_qword(&data, tag, static_cast<u64>(GifTag::RegisterDescriptor::AD));
  const u64 y0 = pair * 32;
  const u64 y1 = y0 + 31;
  const u64 scissor = 31ull << 16 | y0 << 32 | y1 << 48;
  append_qword(&data, scissor, static_cast<u64>(GsRegisterAddress::SCISSOR_1));
  return data;
}

std::vector<u8> make_sprite(u64 hash, u32 pair) {
  std::vector<u8> data;
  const u64 tag = 1 | (1ull << 15) | (1ull << 46) | (5ull << 60);
  const u64 regs = static_cast<u64>(GifTag::RegisterDescriptor::RGBAQ) |
                   (static_cast<u64>(GifTag::RegisterDescriptor::UV) << 4) |
                   (static_cast<u64>(GifTag::RegisterDescriptor::XYZ2) << 8) |
                   (static_cast<u64>(GifTag::RegisterDescriptor::UV) << 12) |
                   (static_cast<u64>(GifTag::RegisterDescriptor::XYZ2) << 16);
  append_qword(&data, tag, regs);

  std::array<u8, 16> color = {};
  color[0] = 128;
  color[4] = 128;
  color[8] = 128;
  color[12] = 128;
  data.insert(data.end(), color.begin(), color.end());
  append_qword(&data, hash, 0);
  const u64 y0 = static_cast<u64>((pair + 1) * 32 * 16);
  const u64 y1 = y0 + 32 * 16;
  append_qword(&data, 512ull | (y0 << 32), 0xffffffull << 4);
  append_qword(&data, 0, 0);
  append_qword(&data, 1024ull | (y1 << 32), 0xffffffull << 4);
  return data;
}

void append_eye_draw(std::vector<u8>* chain, u64 hash, u32 pair) {
  append_transfer(chain, make_scissor(pair), VifCode::Kind::NOP, VifCode::Kind::DIRECT, 0, 2);
  append_transfer(chain, make_sprite(hash, pair), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 6);
}

std::vector<u8> make_eye_chain_with_sources(u32 pair,
                                            u32 iris_tbp,
                                            u32 pupil_tbp,
                                            u32 lid_tbp) {
  std::vector<u8> chain;
  append_transfer(&chain, std::vector<u8>(128), VifCode::Kind::FLUSHA,
                  VifCode::Kind::DIRECT, 0, 8);
  append_transfer(&chain, std::vector<u8>(32), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 2);

  append_transfer(&chain, make_adgif(iris_tbp), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 6);
  append_eye_draw(&chain, kEyeHash + pair, pair);  // pair/hash metadata
  append_eye_draw(&chain, 0, pair);                 // left iris
  append_transfer(&chain, make_adgif(iris_tbp), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 6);
  append_eye_draw(&chain, 0, pair);  // right iris

  append_transfer(&chain, std::vector<u8>(32), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 2);
  append_transfer(&chain, make_adgif(pupil_tbp), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 6);
  append_eye_draw(&chain, 0, pair);  // left pupil
  append_transfer(&chain, make_adgif(pupil_tbp), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 6);
  append_eye_draw(&chain, 0, pair);  // right pupil

  append_transfer(&chain, std::vector<u8>(32), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 2);
  append_transfer(&chain, make_adgif(lid_tbp), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 6);
  append_eye_draw(&chain, 0, pair);  // left lid
  append_transfer(&chain, make_adgif(lid_tbp), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 6);
  append_eye_draw(&chain, 0, pair);  // right lid

  // get_draws stops before the fixed GS-state restore transfer.
  append_transfer(&chain, std::vector<u8>(128), VifCode::Kind::FLUSHA,
                  VifCode::Kind::DIRECT, 0, 8);
  append_transfer(&chain, std::vector<u8>(32), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 2);
  return chain;
}

std::vector<u8> make_eye_chain(u32 pair = 0, u32 source_tbp = kSourceTbp) {
  return make_eye_chain_with_sources(pair, source_tbp, source_tbp, source_tbp);
}

void put_tag(std::vector<u8>* data,
             u32 offset,
             DmaTag::Kind kind,
             u16 qwc,
             u32 address,
             u32 vif0,
             u32 vif1) {
  const u64 raw = qwc | (static_cast<u64>(kind) << 28) |
                  (static_cast<u64>(address) << 32);
  std::memcpy(data->data() + offset, &raw, sizeof(raw));
  std::memcpy(data->data() + offset + 8, &vif0, sizeof(vif0));
  std::memcpy(data->data() + offset + 12, &vif1, sizeof(vif1));
}

struct PrisFixture {
  std::vector<u8> data;
  metal_renderer::Jak2PrisEyeTextureUploadPlan plan;
};

PrisFixture make_pris_fixture(std::size_t chunk_count,
                              bool prison_jak_animator = false,
                              u32 bucket_id = kPrisBucket) {
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr u64 kPageOffset = 0x200000;
  constexpr s64 kMode = -1;
  const u32 bucket_offset = bucket_id * 16;
  PrisFixture fixture;
  fixture.data.resize(0x8000);
  fixture.plan.bucket_id = bucket_id;
  fixture.plan.present = true;
  fixture.plan.ordinary.page_offset = kPageOffset;
  fixture.plan.ordinary.mode = kMode;
  fixture.plan.chunk_count = chunk_count;
  fixture.plan.has_prison_jak_animator = prison_jak_animator;

  put_tag(&fixture.data, bucket_offset, DmaTag::Kind::NEXT, 0,
          kPrisOrdinaryOffset, 0, 0);
  put_tag(&fixture.data, kPrisOrdinaryOffset, DmaTag::Kind::CNT, 1, 0,
          kPcPort, 3);
  std::memcpy(fixture.data.data() + kPrisOrdinaryOffset + 16, &kPageOffset,
              sizeof(kPageOffset));
  std::memcpy(fixture.data.data() + kPrisOrdinaryOffset + 24, &kMode, sizeof(kMode));

  u32 chunk_offset = kPrisChunkOffset;
  if (prison_jak_animator) {
    constexpr std::array<u32, metal_renderer::kJak2PrisPrisonJakAnimatorTbpCount> kTbps = {
        0x1000, metal_renderer::kJak2PrisPrisonJakAnimatorMissingTbp, 0x1020, 0x1030,
        0x1040, 0x1050, 0x1060};
    auto& animator = fixture.plan.prison_jak_animator;
    animator.opcode = metal_renderer::kJak2PrisPrisonJakAnimatorOpcode;
    animator.destination_tbp_count = static_cast<u8>(kTbps.size());
    animator.source_padding_size = static_cast<u8>(
        12 + metal_renderer::kJak2PrisPrisonJakAnimatorBodyBytes -
        (16 + kTbps.size() * sizeof(u32)));
    animator.morph = 0.5f;
    animator.destination_tbps = kTbps;
    for (std::size_t i = 0; i < animator.source_padding.size(); ++i) {
      animator.source_padding[i] = static_cast<u8>(0xc0 + i);
    }
    animator.semantic_fingerprint = 1;
    animator.start_transfer_index = 3;
    animator.start_relative_tag_offset = kPrisAnimatorOffset - bucket_offset;
    animator.body_transfer_index = 4;
    animator.body_relative_tag_offset = animator.start_relative_tag_offset + 16;
    animator.finish_transfer_index = 5;
    animator.finish_relative_tag_offset = animator.body_relative_tag_offset + 16 +
                                          metal_renderer::kJak2PrisPrisonJakAnimatorBodyBytes;
    animator.linker_transfer_index = 6;
    animator.linker_relative_tag_offset = animator.finish_relative_tag_offset + 16;
    put_tag(&fixture.data, kPrisOrdinaryOffset + 32, DmaTag::Kind::NEXT, 0,
            kPrisAnimatorOffset, 0, 0);
    put_tag(&fixture.data, kPrisAnimatorOffset, DmaTag::Kind::CNT, 0, 0,
            kPcPort | metal_renderer::kJak2PrisPrisonJakAnimatorStartOpcode, 0);
    put_tag(&fixture.data, kPrisAnimatorOffset + 16, DmaTag::Kind::CNT,
            metal_renderer::kJak2PrisPrisonJakAnimatorBodyBytes / 16, 0,
            kPcPort | metal_renderer::kJak2PrisPrisonJakAnimatorOpcode, 0);
    const u32 body_offset = kPrisAnimatorOffset + 32;
    std::memcpy(fixture.data.data() + body_offset, &animator.morph, sizeof(animator.morph));
    for (std::size_t i = 0; i < kTbps.size(); ++i) {
      std::memcpy(fixture.data.data() + body_offset + 16 + i * sizeof(u32), &kTbps[i],
                  sizeof(kTbps[i]));
    }
    std::memcpy(fixture.data.data() + body_offset + 4, animator.source_padding.data(), 12);
    std::memcpy(fixture.data.data() + body_offset + 44, animator.source_padding.data() + 12, 4);
    const u32 finish_offset = kPrisAnimatorOffset + 32 +
                              metal_renderer::kJak2PrisPrisonJakAnimatorBodyBytes;
    put_tag(&fixture.data, finish_offset, DmaTag::Kind::CNT, 0, 0,
            kPcPort | metal_renderer::kJak2PrisPrisonJakAnimatorFinishOpcode, 0);
    put_tag(&fixture.data, finish_offset + 16, DmaTag::Kind::NEXT, 0, kPrisChunkOffset, 0, 0);
  } else {
    put_tag(&fixture.data, kPrisOrdinaryOffset + 32, DmaTag::Kind::NEXT, 0,
            chunk_offset, 0, 0);
  }

  for (std::size_t i = 0; i < chunk_count; ++i) {
    const auto chunk_data = make_eye_chain(static_cast<u32>(i));
    std::memcpy(fixture.data.data() + chunk_offset, chunk_data.data(), chunk_data.size());
    const u32 linker_offset = chunk_offset + static_cast<u32>(chunk_data.size());
    put_tag(&fixture.data, linker_offset, DmaTag::Kind::NEXT, 0, linker_offset + 16, 0, 0);

    auto& chunk = fixture.plan.chunks[i];
    chunk.resolution = metal_renderer::Jak2PrisEyeResolution::Eye32;
    chunk.pair_index = static_cast<u32>(i);
    chunk.start_transfer_index = (prison_jak_animator ? 7 : 3) + static_cast<u32>(i) * 27;
    chunk.start_relative_tag_offset = chunk_offset - bucket_offset;
    chunk.linker_transfer_index = chunk.start_transfer_index +
                                  metal_renderer::kJak2PrisEyeChunkTransferCount;
    chunk.linker_relative_tag_offset = linker_offset - bucket_offset;
    chunk.transfer_count = metal_renderer::kJak2PrisEyeChunkTransferCount;
    chunk.payload_bytes = metal_renderer::kJak2PrisEyeChunkPayloadBytes;
    chunk.eye_slot_mask = 3ull << (i * 2);
    fixture.plan.eye_slot_mask |= chunk.eye_slot_mask;
    chunk_offset = linker_offset + 16;
  }

  fixture.plan.direct_reset_transfer_index = (prison_jak_animator ? 7 : 3) +
                                             static_cast<u32>(chunk_count) * 27;
  fixture.plan.direct_reset_relative_tag_offset = chunk_offset - bucket_offset;
  put_tag(&fixture.data, chunk_offset, DmaTag::Kind::CNT, 10, 0, kFlusha,
          kDirect | 10);
  const u32 terminal_offset = chunk_offset + 176;
  fixture.plan.terminal_transfer_index = fixture.plan.direct_reset_transfer_index + 1;
  fixture.plan.terminal_relative_tag_offset = terminal_offset - bucket_offset;
  put_tag(&fixture.data, terminal_offset, DmaTag::Kind::NEXT, 0,
          bucket_offset + 16, 0, 0);
  return fixture;
}

metal_renderer::Jak2Pris2Bucket228Plan make_pris2_plan(const PrisFixture& fixture) {
  metal_renderer::Jak2Pris2Bucket228Plan plan;
  plan.bucket_id = fixture.plan.bucket_id;
  plan.variant = fixture.plan.chunk_count == 0
                     ? metal_renderer::Jak2Pris2Bucket228Variant::OrdinaryOnly
                     : fixture.plan.chunk_count == 1
                           ? metal_renderer::Jak2Pris2Bucket228Variant::OneEyeChunk
                           : metal_renderer::Jak2Pris2Bucket228Variant::TwoEyeChunks;
  plan.ordinary = fixture.plan.ordinary;
  plan.chunks = fixture.plan.chunks;
  plan.chunk_count = fixture.plan.chunk_count;
  plan.direct_reset_transfer_index = fixture.plan.direct_reset_transfer_index;
  plan.direct_reset_relative_tag_offset = fixture.plan.direct_reset_relative_tag_offset;
  plan.terminal_transfer_index = fixture.plan.terminal_transfer_index;
  plan.terminal_relative_tag_offset = fixture.plan.terminal_relative_tag_offset;
  plan.eye_slot_mask = fixture.plan.eye_slot_mask;
  plan.semantic_fingerprint = 1;
  return plan;
}

struct CommonPrisFixture {
  std::vector<u8> data;
  metal_renderer::Jak2CommonPrisTextureUploadPlan plan;
};

CommonPrisFixture make_common_pris_fixture(std::size_t chunk_count) {
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  auto base = make_pris_fixture(chunk_count);
  CommonPrisFixture fixture{std::move(base.data), {}};
  fixture.plan.present = true;
  fixture.plan.ordinary = base.plan.ordinary;
  fixture.plan.chunk_count = chunk_count;

  put_tag(&fixture.data, kCommonPrisBucketOffset, DmaTag::Kind::NEXT, 0,
          kPrisOrdinaryOffset, 0, 0);
  put_tag(&fixture.data, kPrisOrdinaryOffset + 32, DmaTag::Kind::NEXT, 0,
          kPrisAnimatorOffset, 0, 0);
  auto& animator = fixture.plan.dark_jak_animator;
  animator.morph = 0.5f;
  animator.destination_tbps = {0x1200, 0x1210, 0x1220, 0x1230};
  for (std::size_t i = 0; i < animator.source_padding.size(); ++i) {
    animator.source_padding[i] = static_cast<u8>(0xa0 + i);
  }
  animator.semantic_fingerprint = 1;
  animator.start_transfer_index = 3;
  animator.start_relative_tag_offset = kPrisAnimatorOffset - kCommonPrisBucketOffset;
  animator.body_transfer_index = 4;
  animator.body_relative_tag_offset = animator.start_relative_tag_offset + 16;
  animator.finish_transfer_index = 5;
  animator.finish_relative_tag_offset = animator.body_relative_tag_offset + 16 +
                                        metal_renderer::kJak2CommonPrisDarkJakAnimatorBodyBytes;
  animator.linker_transfer_index = 6;
  animator.linker_relative_tag_offset = animator.finish_relative_tag_offset + 16;
  put_tag(&fixture.data, kPrisAnimatorOffset, DmaTag::Kind::CNT, 0, 0,
          kPcPort | metal_renderer::kJak2PrisPrisonJakAnimatorStartOpcode, 0);
  put_tag(&fixture.data, kPrisAnimatorOffset + 16, DmaTag::Kind::CNT,
          metal_renderer::kJak2CommonPrisDarkJakAnimatorBodyBytes / 16, 0,
          kPcPort | metal_renderer::kJak2CommonPrisDarkJakAnimatorOpcode, 0);
  const u32 body_offset = kPrisAnimatorOffset + 32;
  std::memcpy(fixture.data.data() + body_offset, &animator.morph, sizeof(animator.morph));
  std::memcpy(fixture.data.data() + body_offset + 4, animator.source_padding.data(),
              animator.source_padding.size());
  std::memcpy(fixture.data.data() + body_offset + 16, animator.destination_tbps.data(),
              sizeof(animator.destination_tbps));
  const u32 finish_offset = body_offset + metal_renderer::kJak2CommonPrisDarkJakAnimatorBodyBytes;
  put_tag(&fixture.data, finish_offset, DmaTag::Kind::CNT, 0, 0,
          kPcPort | metal_renderer::kJak2PrisPrisonJakAnimatorFinishOpcode, 0);
  put_tag(&fixture.data, finish_offset + 16, DmaTag::Kind::NEXT, 0,
          kPrisChunkOffset, 0, 0);

  for (std::size_t i = 0; i < chunk_count; ++i) {
    auto chunk = base.plan.chunks[i];
    const u32 start_offset = kPrisBucketOffset + chunk.start_relative_tag_offset;
    const u32 linker_offset = kPrisBucketOffset + chunk.linker_relative_tag_offset;
    chunk.start_transfer_index += 4;
    chunk.linker_transfer_index += 4;
    chunk.start_relative_tag_offset = start_offset - kCommonPrisBucketOffset;
    chunk.linker_relative_tag_offset = linker_offset - kCommonPrisBucketOffset;
    fixture.plan.chunks[i] = chunk;
    fixture.plan.eye_slot_mask |= chunk.eye_slot_mask;
  }
  fixture.plan.direct_reset_transfer_index = 7 + static_cast<u32>(chunk_count) * 27;
  const u32 reset_offset = kPrisBucketOffset + base.plan.direct_reset_relative_tag_offset;
  fixture.plan.direct_reset_relative_tag_offset = reset_offset - kCommonPrisBucketOffset;
  fixture.plan.terminal_transfer_index = fixture.plan.direct_reset_transfer_index + 1;
  const u32 terminal_offset = kPrisBucketOffset + base.plan.terminal_relative_tag_offset;
  fixture.plan.terminal_relative_tag_offset = terminal_offset - kCommonPrisBucketOffset;
  put_tag(&fixture.data, terminal_offset, DmaTag::Kind::NEXT, 0,
          kCommonPrisBucketOffset + 16, 0, 0);
  return fixture;
}

struct HostBucketCounter {
  u32 calls = 0;
  u32 bucket_id = 0;
};

void count_host_bucket(void* opaque, u32 bucket_id) {
  auto* counter = static_cast<HostBucketCounter*>(opaque);
  counter->calls++;
  counter->bucket_id = bucket_id;
}

struct EyeConsumerVertex {
  float pos[3];
  float uv[2];
  float color[4];
  float use_texture;
};
static_assert(sizeof(EyeConsumerVertex) == 40);

id<MTLTexture> make_eye_consumer_target(id<MTLDevice> device) {
  auto* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:4
                                  height:4
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  return [device newTextureWithDescriptor:descriptor];
}

bool encode_eye_consumer(id<MTLCommandBuffer> commands,
                         id<MTLTexture> target,
                         u64 source_handle,
                         MetalPsoCache* pso_cache,
                         MetalSamplerCache* sampler_cache) {
  id<MTLTexture> source = metal_texture_lookup(source_handle);
  MetalPsoKey pso_key;
  pso_key.shader = MetalShaderId::SAMPLE;
  pso_key.color_format = MTLPixelFormatRGBA8Unorm;
  MetalSamplerKey sampler_key;
  sampler_key.min_filter = MTLSamplerMinMagFilterNearest;
  sampler_key.mag_filter = MTLSamplerMinMagFilterNearest;
  id<MTLRenderPipelineState> pso = pso_cache->get_pipeline(pso_key);
  id<MTLSamplerState> sampler = sampler_cache->get(sampler_key);
  if (!commands || !target || !source || !pso || !sampler) {
    return false;
  }

  const auto vertex = [](float x, float y, float u, float v) {
    EyeConsumerVertex result = {};
    result.pos[0] = x;
    result.pos[1] = y;
    result.uv[0] = u;
    result.uv[1] = v;
    result.use_texture = 1.f;
    return result;
  };
  const std::array<EyeConsumerVertex, 6> vertices = {
      vertex(-1.f, 1.f, 0.f, 0.f), vertex(1.f, 1.f, 1.f, 0.f),
      vertex(1.f, -1.f, 1.f, 1.f), vertex(-1.f, 1.f, 0.f, 0.f),
      vertex(1.f, -1.f, 1.f, 1.f), vertex(-1.f, -1.f, 0.f, 1.f)};

  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
  id<MTLRenderCommandEncoder> encoder =
      [commands renderCommandEncoderWithDescriptor:pass];
  if (!encoder) {
    return false;
  }
  [encoder setRenderPipelineState:pso];
  [encoder setVertexBytes:vertices.data() length:sizeof(vertices) atIndex:0];
  [encoder setFragmentTexture:source atIndex:0];
  [encoder setFragmentSamplerState:sampler atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertices.size()];
  [encoder endEncoding];
  return true;
}

u32 read_eye_consumer_center(id<MTLTexture> texture) {
  u32 pixel = 0;
  [texture getBytes:&pixel
        bytesPerRow:sizeof(pixel)
         fromRegion:MTLRegionMake2D(2, 2, 1, 1)
        mipmapLevel:0];
  return pixel;
}

u32 read_eye_consumer_pixel(id<MTLTexture> texture, u32 x, u32 y) {
  u32 pixel = 0;
  [texture getBytes:&pixel
        bytesPerRow:sizeof(pixel)
         fromRegion:MTLRegionMake2D(x, y, 1, 1)
        mipmapLevel:0];
  return pixel;
}

struct EyeVertexStorageRun {
  bool initialized = false;
  bool readback_completed = false;
  MetalEyeRenderer::Stats stats;
  std::array<u64, 2> eye_hashes = {};
  std::vector<u8> eye_pixels;
  bool consumer_readback_completed = false;
  std::array<u32, 4> consumer_corners = {};
};

u32 read_eye_pixel(const EyeVertexStorageRun& run, u32 eye, u32 x, u32 y) {
  constexpr std::size_t kBytesPerEye = METAL_EYE_TEX_SIZE * METAL_EYE_TEX_SIZE * 4;
  const std::size_t offset = eye * kBytesPerEye +
                             (y * METAL_EYE_TEX_SIZE + x) * sizeof(u32);
  u32 pixel = 0;
  if (offset + sizeof(pixel) <= run.eye_pixels.size()) {
    std::memcpy(&pixel, run.eye_pixels.data() + offset, sizeof(pixel));
  }
  return pixel;
}

EyeVertexStorageRun run_eye_vertex_storage_case(
    bool dedicated_vertex_buffer,
    id<MTLDevice> device,
    id<MTLCommandQueue> queue,
    TexturePool* texture_pool,
    MetalPsoCache* pso_cache,
    MetalSamplerCache* sampler_cache,
    const std::vector<u8>& chain,
    bool cancel_shader_y_negation = false) {
  constexpr const char* kDiagnosticVariable =
      "GOALPAD_JAK2_DIAGNOSTIC_EYE_DEDICATED_VERTEX_BUFFER";
  constexpr const char* kCancelYVariable =
      "GOALPAD_JAK2_DIAGNOSTIC_EYE_CANCEL_SHADER_Y_NEGATION";
  constexpr const char* kOutputReadbackVariable =
      "GOALPAD_JAK2_DIAGNOSTIC_EYE_OUTPUT_READBACK";
  if (dedicated_vertex_buffer) {
    setenv(kDiagnosticVariable, "1", 1);
  } else {
    unsetenv(kDiagnosticVariable);
  }
  if (cancel_shader_y_negation) {
    setenv(kCancelYVariable, "1", 1);
  } else {
    unsetenv(kCancelYVariable);
  }
  setenv(kOutputReadbackVariable, "1", 1);

  EyeVertexStorageRun result;
  MetalStreamBuffer stream;
  stream.init(device);
  id<MTLBuffer> prefix_buffer = nil;
  u32 prefix_offset = 0;
  std::memset(stream.alloc(32, &prefix_buffer, &prefix_offset), 0xa5, 32);

  MetalEyeRenderer renderer("jak2-eye-vertex-storage-ab", 0, device, queue);
  unsetenv(kDiagnosticVariable);
  unsetenv(kCancelYVariable);
  unsetenv(kOutputReadbackVariable);
  result.initialized = prefix_buffer != nil && prefix_offset == 0 &&
                       renderer.init_textures(*texture_pool, GameVersion::Jak2);
  if (!result.initialized) {
    return result;
  }

  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.texture_pool = texture_pool;
  MetalFrameContext context;
  context.pso_cache = pso_cache;
  context.sampler_cache = sampler_cache;
  context.stream = &stream;
  DmaFollower dma(chain.data(), 0, chain.size());
  renderer.render_from_texture_bucket(dma, &state, context);
  result.stats = renderer.stats();

  const auto left_handle = renderer.lookup_eye_texture(0);
  const auto right_handle = renderer.lookup_eye_texture(1);
  id<MTLTexture> left_texture =
      left_handle ? metal_texture_lookup(*left_handle) : nil;
  id<MTLTexture> right_texture =
      right_handle ? metal_texture_lookup(*right_handle) : nil;
  if (!left_texture || !right_texture) {
    return result;
  }

  constexpr std::size_t kBytesPerRow = METAL_EYE_TEX_SIZE * 4;
  constexpr std::size_t kBytesPerEye = kBytesPerRow * METAL_EYE_TEX_SIZE;
  id<MTLBuffer> readback =
      [device newBufferWithLength:kBytesPerEye * 2
                          options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> commands = [queue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
  if (!readback || !commands || !blit) {
    return result;
  }

  const auto copy_eye = [&](id<MTLTexture> texture, std::size_t offset) {
    [blit copyFromTexture:texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(METAL_EYE_TEX_SIZE, METAL_EYE_TEX_SIZE, 1)
                 toBuffer:readback
        destinationOffset:offset
   destinationBytesPerRow:kBytesPerRow
 destinationBytesPerImage:kBytesPerEye
                  options:MTLBlitOptionNone];
  };
  copy_eye(left_texture, 0);
  copy_eye(right_texture, kBytesPerEye);
  [blit endEncoding];
  [commands commit];
  [commands waitUntilCompleted];
  result.readback_completed = commands.status == MTLCommandBufferStatusCompleted;
  if (result.readback_completed) {
    result.eye_pixels.resize(kBytesPerEye * 2);
    std::memcpy(result.eye_pixels.data(), readback.contents, result.eye_pixels.size());
    result.eye_hashes[0] = fnv64(result.eye_pixels.data(), kBytesPerEye);
    result.eye_hashes[1] = fnv64(result.eye_pixels.data() + kBytesPerEye, kBytesPerEye);
  }

  id<MTLTexture> consumer = make_eye_consumer_target(device);
  id<MTLCommandBuffer> consumer_commands = [queue commandBuffer];
  if (consumer && consumer_commands &&
      encode_eye_consumer(consumer_commands, consumer, *left_handle, pso_cache, sampler_cache)) {
    [consumer_commands commit];
    [consumer_commands waitUntilCompleted];
    result.consumer_readback_completed =
        consumer_commands.status == MTLCommandBufferStatusCompleted;
    if (result.consumer_readback_completed) {
      result.consumer_corners = {read_eye_consumer_pixel(consumer, 0, 0),
                                 read_eye_consumer_pixel(consumer, 3, 0),
                                 read_eye_consumer_pixel(consumer, 0, 3),
                                 read_eye_consumer_pixel(consumer, 3, 3)};
    }
  }
  return result;
}

}  // namespace

int main() {
  @autoreleasepool {
    const std::size_t initial_live_textures = metal_texture_live_count();
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    check(device != nil, "a default Metal device is available");
    if (!device) {
      return 1;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    dispatch_data_t library_data = dispatch_data_create(
        g_goalpad_metallib, g_goalpad_metallib_size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError* library_error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:library_data error:&library_error];
    check(queue != nil && library != nil, "initialized Metal and the eye shader library");
    if (!queue || !library) {
      if (library_error) {
        std::printf("Metal library error: %s\n", library_error.localizedDescription.UTF8String);
      }
      return 1;
    }

    MetalPsoCache pso_cache;
    MetalSamplerCache sampler_cache;
    check(pso_cache.init(device, library), "initialized the eye pipeline cache");
    sampler_cache.init(device);
    MetalStreamBuffer stream;
    stream.init(device);
    id<MTLBuffer> prefix_buffer = nil;
    u32 prefix_offset = 0;
    std::memset(stream.alloc(32, &prefix_buffer, &prefix_offset), 0xa5, 32);
    check(prefix_buffer != nil && prefix_offset == 0,
          "reserved a non-eye prefix in the merged transient vertex stream");

    TexturePool texture_pool(GameVersion::Jak2);
    const std::array<u32, 4> source_pixels = {
        0xffffffff, 0xff2020ff, 0xff20ff20, 0xffff2020};
    const u64 source_handle = metal_upload_texture_rgba8(
        device, queue, reinterpret_cast<const u8*>(source_pixels.data()), 2, 2);
    const std::array<u32, 4> second_source_pixels = {
        0xff00ff00, 0xff00ff00, 0xff00ff00, 0xff00ff00};
    const u64 second_source_handle = metal_upload_texture_rgba8(
        device, queue, reinterpret_cast<const u8*>(second_source_pixels.data()), 2, 2);
    const u64 placeholder_handle = metal_upload_texture_rgba8(
        device, queue, reinterpret_cast<const u8*>(texture_pool.placeholder_data().data()), 16,
        16);
    PcTextureId source_id;
    PcTextureId second_source_id;
    {
      std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
      TextureInput input;
      input.debug_page_name = "PC-EYE-TEST";
      input.debug_name = "synthetic-eye-source";
      input.id = texture_pool.allocate_pc_port_texture(GameVersion::Jak2);
      input.gpu_texture = source_handle;
      input.src_data = reinterpret_cast<const u8*>(source_pixels.data());
      input.w = 2;
      input.h = 2;
      source_id = input.id;
      texture_pool.give_texture_and_load_to_vram(input, kSourceTbp);

      input.debug_name = "synthetic-eye-source-second";
      input.id = texture_pool.allocate_pc_port_texture(GameVersion::Jak2);
      input.gpu_texture = second_source_handle;
      input.src_data = reinterpret_cast<const u8*>(second_source_pixels.data());
      second_source_id = input.id;
      texture_pool.give_texture_and_load_to_vram(input, kSecondSourceTbp);
    }
    check(source_handle != 0 && second_source_handle != 0 && placeholder_handle != 0,
          "published synthetic real and placeholder eye source textures");

    const auto eye_chain = make_eye_chain();
    const auto stream_vertex_run = run_eye_vertex_storage_case(
        false, device, queue, &texture_pool, &pso_cache, &sampler_cache, eye_chain);
    const auto dedicated_vertex_run = run_eye_vertex_storage_case(
        true, device, queue, &texture_pool, &pso_cache, &sampler_cache, eye_chain);
    const auto no_shader_y_negation_run = [&]() {
      TexturePool no_y_texture_pool(GameVersion::Jak2);
      {
        std::lock_guard<std::mutex> pool_lock(no_y_texture_pool.mutex());
        TextureInput input;
        input.debug_page_name = "PC-EYE-TEST";
        input.debug_name = "synthetic-eye-source";
        input.id = no_y_texture_pool.allocate_pc_port_texture(GameVersion::Jak2);
        input.gpu_texture = source_handle;
        input.src_data = reinterpret_cast<const u8*>(source_pixels.data());
        input.w = 2;
        input.h = 2;
        no_y_texture_pool.give_texture_and_load_to_vram(input, kSourceTbp);

        input.debug_name = "synthetic-eye-source-second";
        input.id = no_y_texture_pool.allocate_pc_port_texture(GameVersion::Jak2);
        input.gpu_texture = second_source_handle;
        input.src_data = reinterpret_cast<const u8*>(second_source_pixels.data());
        no_y_texture_pool.give_texture_and_load_to_vram(input, kSecondSourceTbp);
      }
      return run_eye_vertex_storage_case(false, device, queue, &no_y_texture_pool, &pso_cache,
                                         &sampler_cache, eye_chain, true);
    }();
    check(stream_vertex_run.initialized && dedicated_vertex_run.initialized,
          "initialized both eye vertex-storage A/B renderers");
    check(stream_vertex_run.stats.eyes == 2 &&
              stream_vertex_run.stats.draw_calls == 8 &&
              stream_vertex_run.stats.triangles == 16 &&
              stream_vertex_run.stats.command_buffers_completed == 1 &&
              stream_vertex_run.stats.command_buffer_errors == 0 &&
              stream_vertex_run.stats.last_vertex_buffer_offset == 32 &&
              dedicated_vertex_run.stats.eyes == 2 &&
              dedicated_vertex_run.stats.draw_calls == 8 &&
              dedicated_vertex_run.stats.triangles == 16 &&
              dedicated_vertex_run.stats.command_buffers_completed == 1 &&
              dedicated_vertex_run.stats.command_buffer_errors == 0 &&
              dedicated_vertex_run.stats.last_vertex_buffer_offset == 0 &&
              stream_vertex_run.stats.last_vertex_fingerprint != 0 &&
              stream_vertex_run.stats.last_vertex_fingerprint ==
                  dedicated_vertex_run.stats.last_vertex_fingerprint,
          "both A/B paths submit identical vertices from distinct buffer offsets");
    check(stream_vertex_run.readback_completed &&
              dedicated_vertex_run.readback_completed,
          "both A/B eye targets completed full GPU readback");
    std::printf(
        "eye vertex A/B hashes: stream=%016llx/%016llx dedicated=%016llx/%016llx\n",
        static_cast<unsigned long long>(stream_vertex_run.eye_hashes[0]),
        static_cast<unsigned long long>(stream_vertex_run.eye_hashes[1]),
        static_cast<unsigned long long>(dedicated_vertex_run.eye_hashes[0]),
        static_cast<unsigned long long>(dedicated_vertex_run.eye_hashes[1]));
    check(!stream_vertex_run.eye_pixels.empty() &&
              stream_vertex_run.eye_pixels == dedicated_vertex_run.eye_pixels &&
              stream_vertex_run.eye_hashes == dedicated_vertex_run.eye_hashes,
          "default stream and dedicated vertex storage produce byte-identical eye textures");
    check(read_eye_pixel(stream_vertex_run, 0, 8, 8) == source_pixels[0] &&
              read_eye_pixel(stream_vertex_run, 0, 120, 8) == source_pixels[1] &&
              read_eye_pixel(stream_vertex_run, 0, 8, 120) == source_pixels[2] &&
              read_eye_pixel(stream_vertex_run, 0, 120, 120) == source_pixels[3],
          "asymmetric source corners retain their expected composed-eye orientation");
    check(stream_vertex_run.stats.diagnostic_readbacks == 1 &&
              stream_vertex_run.stats.diagnostic_readback_errors == 0 &&
              stream_vertex_run.stats.diagnostic_eye_slot == 0 &&
              stream_vertex_run.stats.diagnostic_output_hash ==
                  stream_vertex_run.eye_hashes[0] &&
              stream_vertex_run.stats.diagnostic_output_corners == source_pixels &&
              stream_vertex_run.stats.diagnostic_iris_source_hash ==
                  fnv64(source_pixels.data(), sizeof(source_pixels)) &&
              stream_vertex_run.stats.diagnostic_iris_source_width == 2 &&
              stream_vertex_run.stats.diagnostic_iris_source_height == 2 &&
              stream_vertex_run.stats.diagnostic_iris_source_corners == source_pixels &&
              stream_vertex_run.stats.diagnostic_lid_source_hash ==
                  fnv64(source_pixels.data(), sizeof(source_pixels)) &&
              stream_vertex_run.stats.diagnostic_lid_source_width == 2 &&
              stream_vertex_run.stats.diagnostic_lid_source_height == 2 &&
              stream_vertex_run.stats.diagnostic_lid_source_corners == source_pixels,
          "env-gated eye diagnostics hash the full composed output and first real iris/lid "
          "sources");
    check(no_shader_y_negation_run.initialized &&
              no_shader_y_negation_run.readback_completed &&
              no_shader_y_negation_run.consumer_readback_completed &&
              no_shader_y_negation_run.stats.last_source_vertex_fingerprint ==
                  stream_vertex_run.stats.last_source_vertex_fingerprint &&
              no_shader_y_negation_run.stats.last_vertex_fingerprint !=
                  stream_vertex_run.stats.last_vertex_fingerprint &&
              no_shader_y_negation_run.eye_hashes[0] != stream_vertex_run.eye_hashes[0],
          "current and canceled-Y runs use identical decoded vertices but distinct exact clip-Y "
          "inputs and outputs");
    const std::array<u32, 4> vertically_flipped_source = {
        source_pixels[2], source_pixels[3], source_pixels[0], source_pixels[1]};
    check(no_shader_y_negation_run.stats.diagnostic_output_corners ==
                  vertically_flipped_source &&
              read_eye_pixel(no_shader_y_negation_run, 0, 8, 8) == source_pixels[2] &&
              read_eye_pixel(no_shader_y_negation_run, 0, 120, 8) == source_pixels[3] &&
              read_eye_pixel(no_shader_y_negation_run, 0, 8, 120) == source_pixels[0] &&
              read_eye_pixel(no_shader_y_negation_run, 0, 120, 120) == source_pixels[1],
          "canceling the shader Y negation vertically flips the composed eye readback");
    check(stream_vertex_run.consumer_readback_completed &&
              stream_vertex_run.consumer_corners == source_pixels &&
              no_shader_y_negation_run.consumer_corners == vertically_flipped_source,
          "the Metal consumer preserves each composed orientation, so the current shader matches "
          "GL's render-and-sample row identity");
    std::printf(
        "eye Y-sign A/B: current=%016llx corners=%08x/%08x/%08x/%08x "
        "no-negation=%016llx corners=%08x/%08x/%08x/%08x\n",
        static_cast<unsigned long long>(stream_vertex_run.eye_hashes[0]),
        stream_vertex_run.stats.diagnostic_output_corners[0],
        stream_vertex_run.stats.diagnostic_output_corners[1],
        stream_vertex_run.stats.diagnostic_output_corners[2],
        stream_vertex_run.stats.diagnostic_output_corners[3],
        static_cast<unsigned long long>(no_shader_y_negation_run.eye_hashes[0]),
        no_shader_y_negation_run.stats.diagnostic_output_corners[0],
        no_shader_y_negation_run.stats.diagnostic_output_corners[1],
        no_shader_y_negation_run.stats.diagnostic_output_corners[2],
        no_shader_y_negation_run.stats.diagnostic_output_corners[3]);

    EyeVertexStorageRun placeholder_run;
    {
      TexturePool placeholder_pool(GameVersion::Jak2);
      placeholder_pool.set_placeholder(placeholder_handle);
      PcTextureId placeholder_source_id;
      {
        std::lock_guard<std::mutex> pool_lock(placeholder_pool.mutex());
        TextureInput input;
        input.debug_page_name = "PC-EYE-TEST";
        input.debug_name = "synthetic-eye-real-source";
        input.id = placeholder_pool.allocate_pc_port_texture(GameVersion::Jak2);
        input.gpu_texture = source_handle;
        input.src_data = reinterpret_cast<const u8*>(source_pixels.data());
        input.w = 2;
        input.h = 2;
        placeholder_pool.give_texture_and_load_to_vram(input, kSourceTbp);

        input.debug_name = "synthetic-eye-placeholder-source";
        input.id = placeholder_pool.allocate_pc_port_texture(GameVersion::Jak2);
        placeholder_source_id = input.id;
        placeholder_pool.give_texture_and_load_to_vram(input, kPlaceholderSourceTbp);
        placeholder_pool.unload_texture(placeholder_source_id, source_handle);
      }
      const auto placeholder_chain = make_eye_chain_with_sources(
          0, kPlaceholderSourceTbp, kSourceTbp, kPlaceholderSourceTbp);
      placeholder_run = run_eye_vertex_storage_case(
          false, device, queue, &placeholder_pool, &pso_cache, &sampler_cache, placeholder_chain);
    }
    std::printf("placeholder-backed eye hash: %016llx\n",
                static_cast<unsigned long long>(placeholder_run.eye_hashes[0]));
    std::printf(
        "placeholder provenance: total=%d iris=%d pupil=%d lid=%d first=%u/0x%08x/0x%016llx/"
        "%ux%u/0x%08x\n",
        placeholder_run.stats.placeholder_textures,
        placeholder_run.stats.placeholder_iris_textures,
        placeholder_run.stats.placeholder_pupil_textures,
        placeholder_run.stats.placeholder_lid_textures,
        placeholder_run.stats.first_placeholder_component,
        placeholder_run.stats.first_placeholder_tbp,
        static_cast<unsigned long long>(placeholder_run.stats.first_placeholder_handle),
        placeholder_run.stats.first_placeholder_width,
        placeholder_run.stats.first_placeholder_height,
        placeholder_run.stats.first_placeholder_texture_id);
    check(placeholder_run.readback_completed && placeholder_run.stats.eyes == 2 &&
              placeholder_run.stats.draw_calls == 8 &&
              placeholder_run.stats.missing_textures == 0 &&
              placeholder_run.stats.placeholder_textures == 4 &&
              placeholder_run.stats.placeholder_iris_textures == 2 &&
              placeholder_run.stats.placeholder_pupil_textures == 0 &&
              placeholder_run.stats.placeholder_lid_textures == 2 &&
              placeholder_run.stats.first_placeholder_component == 1 &&
              placeholder_run.stats.first_placeholder_tbp == kPlaceholderSourceTbp &&
              placeholder_run.stats.first_placeholder_handle == placeholder_handle &&
              placeholder_run.stats.first_placeholder_width == 2 &&
              placeholder_run.stats.first_placeholder_height == 2 &&
              placeholder_run.stats.first_placeholder_texture_id != 0 &&
              placeholder_run.stats.unexpected_dma == 0 &&
              placeholder_run.stats.command_buffer_errors == 0 &&
              read_eye_pixel(placeholder_run, 0, 8, 8) == 0xff303030 &&
              read_eye_pixel(placeholder_run, 0, 40, 8) == 0xffe0e0e0,
          "placeholder-backed eye sources draw a gray checkerboard with exact source provenance");

    {
      MetalEyeRenderer renderer("jak2-eyes", 0, device, queue);
      check(renderer.init_textures(texture_pool, GameVersion::Jak2),
            "initialized Jak II eye slots at the GL-compatible base");
      check(!renderer.lookup_eye_texture(0) &&
                !renderer.lookup_eye_texture_hash(kEyeHash, false),
            "uncomposed eye targets are not lookup-visible");

      MetalSharedRenderState state;
      state.version = GameVersion::Jak2;
      state.texture_pool = &texture_pool;
      MetalFrameContext context;
      context.pso_cache = &pso_cache;
      context.sampler_cache = &sampler_cache;
      context.stream = &stream;
      const auto chain = make_eye_chain();

      DmaFollower first_dma(chain.data(), 0, chain.size());
      renderer.render_from_texture_bucket(first_dma, &state, context);
      const auto first = renderer.stats();
      check(first.eyes == 2 && first.draw_calls == 8 && first.triangles == 16 &&
                first.duplicate_slot_writes == 0 && first.command_buffers_committed == 1 &&
                first.command_buffers_completed == 1 && first.command_buffer_errors == 0 &&
                first.vertex_stream_uploads == 1 && first.vertex_bytes == 512 &&
                first.last_vertex_buffer_offset == 32 && first.last_vertex_fingerprint != 0 &&
                first.last_command_buffer_status == MTLCommandBufferStatusCompleted,
            "one synthetic eye pair composes from a nonzero transient-stream offset");
      check(renderer.lookup_eye_texture(0) && renderer.lookup_eye_texture(1) &&
                renderer.lookup_eye_texture_hash(kEyeHash, false) &&
                renderer.lookup_eye_texture_hash(kEyeHash, true),
            "both successfully composed eye targets are lookup-visible");

      DmaFollower duplicate_dma(chain.data(), 0, chain.size());
      renderer.render_from_texture_bucket(duplicate_dma, &state, context);
      const auto duplicate = renderer.stats();
      check(duplicate.eyes == 2 && duplicate.duplicate_slot_writes == 2 &&
                duplicate.command_buffers_committed == 1 &&
                duplicate.command_buffers_completed == 1,
            "duplicate same-frame eye slot writes fail before another command buffer");

      renderer.start_frame();
      const auto same_producer_chain = make_eye_chain(12, kSourceTbp);
      DmaFollower same_producer_first(same_producer_chain.data(), 0,
                                      same_producer_chain.size());
      renderer.render_from_texture_bucket(same_producer_first, &state, context, 200);
      DmaFollower same_producer_repeat(same_producer_chain.data(), 0,
                                       same_producer_chain.size());
      renderer.render_from_texture_bucket(same_producer_repeat, &state, context, 200);
      check(renderer.stats().eyes == 2 && renderer.stats().duplicate_slot_writes == 2 &&
                renderer.stats().versioned_slot_writes == 0 &&
                renderer.stats().command_buffers_completed == 1,
            "a repeated write from producer 200 remains fail-closed");

      renderer.start_frame();
      const auto producer_200_chain = make_eye_chain(12, kSourceTbp);
      DmaFollower producer_200_dma(producer_200_chain.data(), 0,
                                   producer_200_chain.size());
      renderer.render_from_texture_bucket(producer_200_dma, &state, context, 200);
      const auto first_pair_12_handle = renderer.lookup_eye_texture(24);
      id<MTLCommandBuffer> frame_commands = [queue commandBuffer];
      id<MTLTexture> first_consumer = make_eye_consumer_target(device);
      id<MTLTexture> second_consumer = make_eye_consumer_target(device);
      const bool first_consumer_encoded =
          first_pair_12_handle &&
          encode_eye_consumer(frame_commands, first_consumer, *first_pair_12_handle,
                              &pso_cache, &sampler_cache);

      const auto producer_204_chain = make_eye_chain(12, kSecondSourceTbp);
      DmaFollower producer_204_dma(producer_204_chain.data(), 0,
                                   producer_204_chain.size());
      renderer.render_from_texture_bucket(producer_204_dma, &state, context, 204);
      const auto second_pair_12_handle = renderer.lookup_eye_texture(24);
      const bool second_consumer_encoded =
          second_pair_12_handle &&
          encode_eye_consumer(frame_commands, second_consumer, *second_pair_12_handle,
                              &pso_cache, &sampler_cache);
      [frame_commands commit];
      [frame_commands waitUntilCompleted];
      const u32 first_consumer_pixel = read_eye_consumer_center(first_consumer);
      const u32 second_consumer_pixel = read_eye_consumer_center(second_consumer);
      const auto versioned = renderer.stats();
      check(first_consumer_encoded && second_consumer_encoded &&
                frame_commands.status == MTLCommandBufferStatusCompleted &&
                first_pair_12_handle && second_pair_12_handle &&
                *first_pair_12_handle != *second_pair_12_handle &&
                metal_texture_lookup(*first_pair_12_handle) != nil &&
                first_consumer_pixel != second_consumer_pixel &&
                second_consumer_pixel == second_source_pixels[0] &&
                versioned.eyes == 4 && versioned.versioned_slot_writes == 2 &&
                versioned.duplicate_slot_writes == 0 &&
                versioned.command_buffers_completed == 2,
            "producer 200, consumer A, overlapping producer 204, and consumer B retain distinct GPU contents");

      renderer.start_frame();
      check(first_pair_12_handle && metal_texture_lookup(*first_pair_12_handle) == nil &&
                second_pair_12_handle && metal_texture_lookup(*second_pair_12_handle) != nil,
            "the prior eye generation retires only at the next frame boundary");
      check(renderer.lookup_eye_texture(0) && renderer.lookup_eye_texture(1),
            "a completed eye composition remains visible across frame reset");
      DmaFollower next_frame_dma(chain.data(), 0, chain.size());
      renderer.render_from_texture_bucket(next_frame_dma, &state, context);
      const auto next_frame = renderer.stats();
      check(next_frame.eyes == 2 && next_frame.duplicate_slot_writes == 0 &&
                next_frame.command_buffers_completed == 1,
            "the same slots can be composed once in the next frame");

      MetalJak2PrisEyeBucketRenderer pris_renderer("jak2-pris-eye-200", kPrisBucket);
      state.eye_renderer = &renderer;
      state.buckets_base = 0;
      state.next_bucket = kPrisBucketOffset + 16;
      state.host_bucket_callback = count_host_bucket;

      auto one_chunk = make_pris_fixture(1);
      HostBucketCounter host_counter;
      state.host_bucket_context = &host_counter;
      state.jak2_pris_eye_plans = &one_chunk.plan;
      state.jak2_pris_eye_plan_count = 1;
      renderer.start_frame();
      DmaFollower one_chunk_dma(one_chunk.data.data(), kPrisBucketOffset,
                                one_chunk.data.size());
      pris_renderer.render(one_chunk_dma, &state, context);
      const auto one_chunk_stats = renderer.stats();
      check(host_counter.calls == 1 && host_counter.bucket_id == kPrisBucket &&
                one_chunk_dma.current_tag_offset() == state.next_bucket &&
                one_chunk_stats.eyes == 2 && one_chunk_stats.draw_calls == 8 &&
                one_chunk_stats.triangles == 16 && one_chunk_stats.missing_textures == 0 &&
                one_chunk_stats.unexpected_dma == 0 &&
                one_chunk_stats.duplicate_slot_writes == 0 &&
                one_chunk_stats.command_buffers_committed == 1 &&
                one_chunk_stats.command_buffers_completed == 1 &&
                one_chunk_stats.command_buffer_errors == 0,
            "the PRIS bucket callback runs once before one exact detached eye chunk completes");

      auto two_chunks = make_pris_fixture(2);
      host_counter = {};
      state.jak2_pris_eye_plans = &two_chunks.plan;
      renderer.start_frame();
      DmaFollower two_chunk_dma(two_chunks.data.data(), kPrisBucketOffset,
                                two_chunks.data.size());
      pris_renderer.render(two_chunk_dma, &state, context);
      const auto two_chunk_stats = renderer.stats();
      check(host_counter.calls == 1 && host_counter.bucket_id == kPrisBucket &&
                two_chunk_dma.current_tag_offset() == state.next_bucket &&
                two_chunk_stats.eyes == 4 && two_chunk_stats.draw_calls == 16 &&
                two_chunk_stats.triangles == 32 && two_chunk_stats.missing_textures == 0 &&
                two_chunk_stats.unexpected_dma == 0 &&
                two_chunk_stats.duplicate_slot_writes == 0 &&
                two_chunk_stats.command_buffers_committed == 2 &&
                two_chunk_stats.command_buffers_completed == 2 &&
                two_chunk_stats.vertex_stream_uploads == 2 &&
                two_chunk_stats.vertex_bytes == 1024 &&
                two_chunk_stats.last_vertex_buffer_offset > 32 &&
                two_chunk_stats.last_vertex_fingerprint != 0 &&
                two_chunk_stats.command_buffer_errors == 0,
            "the PRIS renderer follows both planned chunks and consumes every terminal shape");

      auto animator_one_chunk = make_pris_fixture(1, true);
      host_counter = {};
      state.jak2_pris_eye_plans = &animator_one_chunk.plan;
      renderer.start_frame();
      DmaFollower animator_one_dma(animator_one_chunk.data.data(), kPrisBucketOffset,
                                   animator_one_chunk.data.size());
      pris_renderer.render(animator_one_dma, &state, context);
      const auto animator_one_stats = renderer.stats();
      check(host_counter.calls == 1 && animator_one_dma.current_tag_offset() == state.next_bucket &&
                animator_one_stats.eyes == 2 && animator_one_stats.draw_calls == 8 &&
                animator_one_stats.triangles == 16 && animator_one_stats.missing_textures == 0 &&
                animator_one_stats.unexpected_dma == 0 &&
                animator_one_stats.duplicate_slot_writes == 0 &&
                animator_one_stats.command_buffers_committed == 1 &&
                animator_one_stats.command_buffers_completed == 1 &&
                animator_one_stats.command_buffer_errors == 0,
            "the PRIS renderer validates and consumes a prison-Jak animator before one eye chunk");

      auto animator_two_chunks = make_pris_fixture(2, true);
      host_counter = {};
      state.jak2_pris_eye_plans = &animator_two_chunks.plan;
      renderer.start_frame();
      DmaFollower animator_two_dma(animator_two_chunks.data.data(), kPrisBucketOffset,
                                   animator_two_chunks.data.size());
      pris_renderer.render(animator_two_dma, &state, context);
      const auto animator_two_stats = renderer.stats();
      check(host_counter.calls == 1 && animator_two_dma.current_tag_offset() == state.next_bucket &&
                animator_two_stats.eyes == 4 && animator_two_stats.draw_calls == 16 &&
                animator_two_stats.triangles == 32 && animator_two_stats.missing_textures == 0 &&
                animator_two_stats.unexpected_dma == 0 &&
                animator_two_stats.duplicate_slot_writes == 0 &&
                animator_two_stats.command_buffers_committed == 2 &&
                animator_two_stats.command_buffers_completed == 2 &&
                animator_two_stats.command_buffer_errors == 0,
            "the PRIS renderer keeps both eye chunks executing after the prison-Jak no-op");

      auto animator_only = make_pris_fixture(0, true);
      host_counter = {};
      state.jak2_pris_eye_plans = &animator_only.plan;
      renderer.start_frame();
      DmaFollower animator_only_dma(animator_only.data.data(), kPrisBucketOffset,
                                    animator_only.data.size());
      pris_renderer.render(animator_only_dma, &state, context);
      const auto animator_only_stats = renderer.stats();
      check(host_counter.calls == 1 && animator_only_dma.current_tag_offset() == state.next_bucket &&
                animator_only_stats.eyes == 0 && animator_only_stats.draw_calls == 0 &&
                animator_only_stats.triangles == 0 && animator_only_stats.unexpected_dma == 0 &&
                animator_only_stats.command_buffers_committed == 0 &&
                animator_only_stats.command_buffers_completed == 0 &&
                animator_only_stats.command_buffer_errors == 0,
            "the PRIS renderer consumes an animator-only plan through its terminal reset");

      constexpr u32 kPris2Bucket = metal_renderer::kJak2Pris2TextureUploadBucket;
      constexpr u32 kPris2BucketOffset = kPris2Bucket * 16;
      MetalJak2PrisEyeBucketRenderer pris2_renderer("jak2-pris2-eye-228", kPris2Bucket);
      state.next_bucket = kPris2BucketOffset + 16;

      auto pris2_ordinary = make_pris_fixture(0, false, kPris2Bucket);
      const auto pris2_ordinary_source = make_pris2_plan(pris2_ordinary);
      const auto pris2_ordinary_plan =
          metal_renderer::adapt_jak2_pris2_bucket228_to_pris_eye_plan(
              pris2_ordinary_source);
      host_counter = {};
      state.jak2_pris_eye_plans = &pris2_ordinary_plan;
      renderer.start_frame();
      DmaFollower pris2_ordinary_dma(pris2_ordinary.data.data(), kPris2BucketOffset,
                                     pris2_ordinary.data.size());
      pris2_renderer.render(pris2_ordinary_dma, &state, context);
      const auto pris2_ordinary_stats = renderer.stats();
      check(host_counter.calls == 1 && host_counter.bucket_id == kPris2Bucket &&
                pris2_ordinary_dma.current_tag_offset() == state.next_bucket &&
                pris2_ordinary_stats.eyes == 0 && pris2_ordinary_stats.draw_calls == 0 &&
                pris2_ordinary_stats.triangles == 0 &&
                pris2_ordinary_stats.command_buffers_committed == 0 &&
                pris2_ordinary_stats.command_buffers_completed == 0 &&
                pris2_ordinary_stats.command_buffer_errors == 0,
            "adapted bucket 228 ordinary-only form calls back once and reaches its boundary");

      auto pris2_eye = make_pris_fixture(1, false, kPris2Bucket);
      const auto pris2_eye_source = make_pris2_plan(pris2_eye);
      const auto pris2_eye_plan =
          metal_renderer::adapt_jak2_pris2_bucket228_to_pris_eye_plan(pris2_eye_source);
      host_counter = {};
      state.jak2_pris_eye_plans = &pris2_eye_plan;
      renderer.start_frame();
      DmaFollower pris2_eye_dma(pris2_eye.data.data(), kPris2BucketOffset,
                                pris2_eye.data.size());
      pris2_renderer.render(pris2_eye_dma, &state, context);
      const auto pris2_eye_stats = renderer.stats();
      check(host_counter.calls == 1 && host_counter.bucket_id == kPris2Bucket &&
                pris2_eye_dma.current_tag_offset() == state.next_bucket &&
                pris2_eye_stats.eyes == 2 && pris2_eye_stats.draw_calls == 8 &&
                pris2_eye_stats.triangles == 16 && pris2_eye_stats.missing_textures == 0 &&
                pris2_eye_stats.unexpected_dma == 0 &&
                pris2_eye_stats.duplicate_slot_writes == 0 &&
                pris2_eye_stats.command_buffers_committed == 1 &&
                pris2_eye_stats.command_buffers_completed == 1 &&
                pris2_eye_stats.command_buffer_errors == 0,
            "adapted bucket 228 one-eye form executes 2 eyes, 8 draws, and 16 triangles once");

      constexpr u32 kNonL1Pris2Bucket = metal_renderer::kJak2Pris2TextureUploadBuckets.back();
      constexpr u32 kNonL1Pris2BucketOffset = kNonL1Pris2Bucket * 16;
      MetalJak2PrisEyeBucketRenderer non_l1_pris2_renderer("jak2-pris2-eye-244",
                                                           kNonL1Pris2Bucket);
      auto non_l1_pris2 = make_pris_fixture(2, false, kNonL1Pris2Bucket);
      const auto non_l1_pris2_source = make_pris2_plan(non_l1_pris2);
      const auto non_l1_pris2_plan =
          metal_renderer::adapt_jak2_pris2_to_pris_eye_plan(non_l1_pris2_source);
      host_counter = {};
      state.next_bucket = kNonL1Pris2BucketOffset + 16;
      state.jak2_pris_eye_plans = &non_l1_pris2_plan;
      renderer.start_frame();
      DmaFollower non_l1_pris2_dma(non_l1_pris2.data.data(), kNonL1Pris2BucketOffset,
                                   non_l1_pris2.data.size());
      non_l1_pris2_renderer.render(non_l1_pris2_dma, &state, context);
      const auto non_l1_pris2_stats = renderer.stats();
      check(host_counter.calls == 1 && host_counter.bucket_id == kNonL1Pris2Bucket &&
                non_l1_pris2_dma.current_tag_offset() == state.next_bucket &&
                non_l1_pris2_stats.eyes == 4 && non_l1_pris2_stats.draw_calls == 16 &&
                non_l1_pris2_stats.triangles == 32 &&
                non_l1_pris2_stats.missing_textures == 0 &&
                non_l1_pris2_stats.unexpected_dma == 0 &&
                non_l1_pris2_stats.duplicate_slot_writes == 0 &&
                non_l1_pris2_stats.command_buffers_committed == 2 &&
                non_l1_pris2_stats.command_buffers_completed == 2 &&
                non_l1_pris2_stats.command_buffer_errors == 0,
            "adapted non-L1 PRIS2 two-eye form executes both chunks through its exact boundary");

      MetalJak2CommonPrisBucketRenderer common_pris_renderer(
          "jak2-common-pris", kCommonPrisBucket);
      CommonPrisFixture common_absent;
      common_absent.data.resize(kCommonPrisBucketOffset + 32);
      put_tag(&common_absent.data, kCommonPrisBucketOffset, DmaTag::Kind::CNT, 0, 0, 0, 0);
      host_counter = {};
      state.next_bucket = kCommonPrisBucketOffset + 16;
      state.host_bucket_context = &host_counter;
      state.jak2_common_pris_plan = &common_absent.plan;
      renderer.start_frame();
      DmaFollower common_absent_dma(common_absent.data.data(), kCommonPrisBucketOffset,
                                    common_absent.data.size());
      common_pris_renderer.render(common_absent_dma, &state, context);
      check(host_counter.calls == 1 && host_counter.bucket_id == kCommonPrisBucket &&
                common_absent_dma.current_tag_offset() == state.next_bucket &&
                renderer.stats().eyes == 0 && renderer.stats().command_buffers_committed == 0,
            "absent common PRIS still calls the host once and consumes its strict empty slot");

      auto common_form_a = make_common_pris_fixture(0);
      host_counter = {};
      state.next_bucket = kCommonPrisBucketOffset + 16;
      state.host_bucket_context = &host_counter;
      state.jak2_common_pris_plan = &common_form_a.plan;
      renderer.start_frame();
      DmaFollower common_form_a_dma(common_form_a.data.data(), kCommonPrisBucketOffset,
                                    common_form_a.data.size());
      common_pris_renderer.render(common_form_a_dma, &state, context);
      const auto common_form_a_stats = renderer.stats();
      check(host_counter.calls == 1 && host_counter.bucket_id == kCommonPrisBucket &&
                common_form_a_dma.current_tag_offset() == state.next_bucket &&
                common_form_a_stats.eyes == 0 && common_form_a_stats.draw_calls == 0 &&
                common_form_a_stats.command_buffers_committed == 0 &&
                common_form_a_stats.command_buffers_completed == 0 &&
                common_form_a_stats.command_buffer_errors == 0,
            "common PRIS Form A calls the host once and consumes the Dark Jak/reset envelope");

      auto common_one_eye = make_common_pris_fixture(1);
      host_counter = {};
      state.jak2_common_pris_plan = &common_one_eye.plan;
      renderer.start_frame();
      DmaFollower common_one_eye_dma(common_one_eye.data.data(), kCommonPrisBucketOffset,
                                     common_one_eye.data.size());
      common_pris_renderer.render(common_one_eye_dma, &state, context);
      const auto common_one_eye_stats = renderer.stats();
      check(host_counter.calls == 1 && host_counter.bucket_id == kCommonPrisBucket &&
                common_one_eye_dma.current_tag_offset() == state.next_bucket &&
                common_one_eye_stats.eyes == 2 && common_one_eye_stats.draw_calls == 8 &&
                common_one_eye_stats.triangles == 16 &&
                common_one_eye_stats.missing_textures == 0 &&
                common_one_eye_stats.unexpected_dma == 0 &&
                common_one_eye_stats.duplicate_slot_writes == 0 &&
                common_one_eye_stats.command_buffers_committed == 1 &&
                common_one_eye_stats.command_buffers_completed == 1 &&
                common_one_eye_stats.command_buffer_errors == 0,
            "common PRIS one-eye form calls the host once and completes its exact boundary");

      auto common_form_b = make_common_pris_fixture(2);
      host_counter = {};
      state.jak2_common_pris_plan = &common_form_b.plan;
      renderer.start_frame();
      DmaFollower common_form_b_dma(common_form_b.data.data(), kCommonPrisBucketOffset,
                                    common_form_b.data.size());
      common_pris_renderer.render(common_form_b_dma, &state, context);
      const auto common_form_b_stats = renderer.stats();
      check(host_counter.calls == 1 && host_counter.bucket_id == kCommonPrisBucket &&
                common_form_b_dma.current_tag_offset() == state.next_bucket &&
                common_form_b_stats.eyes == 4 && common_form_b_stats.draw_calls == 16 &&
                common_form_b_stats.triangles == 32 &&
                common_form_b_stats.missing_textures == 0 &&
                common_form_b_stats.unexpected_dma == 0 &&
                common_form_b_stats.duplicate_slot_writes == 0 &&
                common_form_b_stats.command_buffers_committed == 2 &&
                common_form_b_stats.command_buffers_completed == 2 &&
                common_form_b_stats.command_buffer_errors == 0,
            "common PRIS Form B calls the host once then completes both detached eye chunks");
    }

    bool detached_all_eye_slots = true;
    for (u32 slot = METAL_EYE_BASE_BLOCK_JAK1;
         slot < METAL_EYE_BASE_BLOCK_JAK1 + METAL_NUM_EYE_PAIRS * 2; slot++) {
      const auto handle = texture_pool.lookup(slot);
      detached_all_eye_slots &= handle.has_value() && *handle == 0;
    }
    check(detached_all_eye_slots,
          "eye teardown unloads every pool publication while the pool is live");
    check(metal_texture_live_count() == initial_live_textures + 3,
          "eye teardown releases all forty registered render targets");

    {
      std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
      texture_pool.unload_texture(source_id, source_handle);
      texture_pool.unload_texture(second_source_id, second_source_handle);
    }
    metal_texture_release(source_handle);
    metal_texture_release(second_source_handle);
    metal_texture_release(placeholder_handle);
    check(metal_texture_live_count() == initial_live_textures,
          "the synthetic source cleanup restores the texture registry baseline");

    if (failures) {
      std::printf("FAIL: %d Jak II Metal eye renderer checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak II Metal eye publication and lifecycle checks passed\n");
    return 0;
  }
}
