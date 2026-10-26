#include "game/graphics/pipelines/metal/metal_jak2_pris2_bucket228_plan.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/dma/dma.h"
#include "common/dma/gs.h"

namespace {

constexpr u32 kChainOffset = 0x100;
constexpr u32 kOrdinaryOffset = 0x8000;
constexpr u32 kFirstEyeOffset = 0x9000;
constexpr u32 kSecondEyeOffset = 0xa000;
constexpr u32 kDirectOffset = 0xb000;
constexpr u32 kExtraOffset = 0xd000;
constexpr u32 kPageOffset = 0x18000;
constexpr std::size_t kMemorySize = 0x24000;
constexpr u32 kPcPortVif = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
constexpr u32 kDirectVif = static_cast<u32>(VifCode::Kind::DIRECT) << 24;

using Capture = metal_renderer::Jak2Pris2TextureUploadCapture;
using RejectReason = metal_renderer::Jak2PrisEyeTextureUploadRejectReason;
using Variant = metal_renderer::Jak2Pris2Bucket228Variant;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

u32 bucket_offset(u32 bucket_id = metal_renderer::kJak2Pris2TextureUploadBucket) {
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
  const std::array<u64, 10> adgif = {
      tex0,
      static_cast<u64>(GsRegisterAddress::TEX0_1),
      0x101,
      static_cast<u64>(GsRegisterAddress::TEX1_1) | 0x70c00700ull,
      0x202,
      static_cast<u64>(GsRegisterAddress::MIPTBP1_1) | 0x123400ull,
      clamp,
      static_cast<u64>(GsRegisterAddress::CLAMP_1),
      alpha,
      static_cast<u64>(GsRegisterAddress::ALPHA_1)};
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

struct EyeChunkSpec {
  bool eye64 = false;
  u32 pair_index = 0;
};

u32 put_eye_chunk(std::vector<u8>* packet,
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
  cursor = put_eye_sprite(packet, cursor, true, 0, eye_width * 3 * 16,
                          y0 * 16, eye_width * 2 * 16,
                          (y0 + eye_width) * 16, false);

  cursor = put_display_reset(packet, cursor);
  return put_gs_set(packet, cursor, GsRegisterAddress::ALPHA_1, 0x44);
}

struct Fixture {
  std::vector<u8> packet;
  u32 first_eye_offset = 0;
  u32 linker_offset = 0;
};

Fixture make_fixture(const std::vector<EyeChunkSpec>& chunks,
                     u32 dma_relocation = 0,
                     u32 bucket_id = metal_renderer::kJak2Pris2TextureUploadBucket) {
  Fixture fixture{std::vector<u8>(kMemorySize), kFirstEyeOffset + dma_relocation, 0};
  const u32 ordinary_offset = kOrdinaryOffset + dma_relocation;
  const u32 first_offset = kFirstEyeOffset + dma_relocation;
  const u32 second_offset = kSecondEyeOffset + dma_relocation;
  const u32 direct_offset = kDirectOffset + dma_relocation;
  const u32 end_offset = bucket_offset(bucket_id) + 16;

  put_tag(&fixture.packet, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0,
          ordinary_offset, 0, 0);
  put_tag(&fixture.packet, ordinary_offset, DmaTag::Kind::CNT, 1, 0, kPcPortVif, 3);
  put_u64(&fixture.packet, ordinary_offset + 16, kPageOffset);
  put_u64(&fixture.packet, ordinary_offset + 24, static_cast<u64>(-1));
  put_tag(&fixture.packet, ordinary_offset + 32, DmaTag::Kind::NEXT, 0,
          chunks.empty() ? direct_offset : first_offset, 0, 0);

  if (!chunks.empty()) {
    fixture.linker_offset = put_eye_chunk(&fixture.packet, first_offset, chunks.at(0));
    if (chunks.size() == 2) {
      put_tag(&fixture.packet, fixture.linker_offset, DmaTag::Kind::NEXT, 0,
              second_offset, 0, 0);
      fixture.linker_offset = put_eye_chunk(&fixture.packet, second_offset, chunks.at(1));
    }
    put_tag(&fixture.packet, fixture.linker_offset, DmaTag::Kind::NEXT, 0,
            direct_offset, 0, 0);
  }
  put_tag(&fixture.packet, direct_offset, DmaTag::Kind::CNT, 10, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 10);
  std::fill_n(fixture.packet.begin() + direct_offset + 16, 160, 0x52);
  put_tag(&fixture.packet, direct_offset + 176, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  fixture.packet[kPageOffset + 8] = 0x44;
  return fixture;
}

std::optional<metal_renderer::Jak2Pris2Bucket228Plan> plan(
    const Fixture& fixture,
    Capture* capture = nullptr,
    metal_renderer::Jak2PrisEyeTextureUploadRejection* rejection = nullptr,
    u32 bucket_id = metal_renderer::kJak2Pris2TextureUploadBucket) {
  return metal_renderer::plan_jak2_pris2_texture_upload(
      fixture.packet.data(), fixture.packet.size(), kChainOffset, bucket_id,
      fixture.packet.data(), fixture.packet.size(), capture, rejection);
}

void test_observed_forms() {
  Fixture absent{std::vector<u8>(kMemorySize), 0, 0};
  put_tag(&absent.packet, bucket_offset(), DmaTag::Kind::CNT, 0, 0, 0, 0);
  const auto absent_plan = plan(absent);
  check(absent_plan && absent_plan->variant == Variant::Absent &&
            absent_plan->eye_slot_mask == 0,
        "bucket 228 accepts an exact absent slot as a structural no-op");
  const auto absent_renderer_plan =
      metal_renderer::adapt_jak2_pris2_bucket228_to_pris_eye_plan(*absent_plan);
  check(!absent_renderer_plan.present &&
            absent_renderer_plan.bucket_id == metal_renderer::kJak2Pris2TextureUploadBucket,
        "the exact absent bucket-228 slot adapts to a renderer no-op");

  auto ordinary = make_fixture({});
  Capture capture;
  const auto ordinary_plan = plan(ordinary, &capture);
  check(ordinary_plan && ordinary_plan->bucket_id == 228 &&
            ordinary_plan->variant == Variant::OrdinaryOnly &&
            ordinary_plan->ordinary.page_offset == kPageOffset &&
            ordinary_plan->ordinary.mode == -1 &&
            ordinary_plan->ordinary.page_header[8] == 0x44 &&
            ordinary_plan->eye_slot_mask == 0 &&
            ordinary_plan->direct_reset_transfer_index == 3 &&
            ordinary_plan->terminal_transfer_index == 4 &&
            ordinary_plan->semantic_fingerprint != 0 && capture.valid &&
            capture.transfer_count == 5 && capture.total_payload_bytes == 176 &&
            capture.inert_transfers == 3 && capture.ordinary_descriptors == 1 &&
            capture.direct_setup_transfers == 1 && capture.gs_setup_transfers == 0 &&
            capture.eye_markers == 0 && capture.other_transfers == 0,
        "bucket 228 accepts only the exact ordinary descriptor/reset form");
  const auto ordinary_renderer_plan =
      metal_renderer::adapt_jak2_pris2_bucket228_to_pris_eye_plan(*ordinary_plan);
  check(ordinary_renderer_plan.present && ordinary_renderer_plan.chunk_count == 0 &&
            ordinary_renderer_plan.ordinary.page_offset == kPageOffset &&
            ordinary_renderer_plan.direct_reset_transfer_index == 3 &&
            ordinary_renderer_plan.terminal_transfer_index == 4,
        "the ordinary bucket-228 plan adapts to the shared renderer without eye work");
  ordinary.packet[kPageOffset + 8] = 0;
  check(ordinary_plan->ordinary.page_header[8] == 0x44,
        "the bucket-228 plan owns its bounded page header");

  auto eye = make_fixture({{false, 2}});
  const auto eye_plan = plan(eye, &capture);
  check(eye_plan && eye_plan->bucket_id == 228 &&
            eye_plan->variant == Variant::OneEyeChunk &&
            eye_plan->ordinary.page_offset == kPageOffset &&
            eye_plan->chunk_count == 1 &&
            eye_plan->chunks[0].resolution == metal_renderer::Jak2PrisEyeResolution::Eye32 &&
            eye_plan->chunks[0].pair_index == 2 && eye_plan->eye_slot_mask == 0x30 &&
            eye_plan->chunks[0].transfer_count ==
                metal_renderer::kJak2PrisEyeChunkTransferCount &&
            eye_plan->chunks[0].payload_bytes ==
                metal_renderer::kJak2PrisEyeChunkPayloadBytes &&
            eye_plan->chunks[0].semantic_fingerprint != 0 &&
            eye_plan->direct_reset_transfer_index == 30 &&
            eye_plan->terminal_transfer_index == 31 &&
            eye_plan->semantic_fingerprint != 0 && capture.valid &&
            capture.transfer_count == 32 && capture.total_payload_bytes == 2032 &&
            capture.inert_transfers == 4 && capture.ordinary_descriptors == 1 &&
            capture.direct_setup_transfers == 1 && capture.gs_setup_transfers == 11 &&
            capture.eye_markers == 2 && capture.other_transfers == 13 &&
            capture.malformed_transfers == 0,
        "bucket 228 accepts the exact live one-eye-chunk envelope");
  const auto eye_renderer_plan =
      metal_renderer::adapt_jak2_pris2_bucket228_to_pris_eye_plan(*eye_plan);
  check(eye_renderer_plan.present && eye_renderer_plan.bucket_id == 228 &&
            eye_renderer_plan.chunk_count == 1 &&
            eye_renderer_plan.chunks[0].pair_index == eye_plan->chunks[0].pair_index &&
            eye_renderer_plan.eye_slot_mask == eye_plan->eye_slot_mask &&
            eye_renderer_plan.semantic_fingerprint == eye_plan->semantic_fingerprint,
        "the one-eye bucket-228 plan adapts exactly to the shared PRIS eye renderer");

  auto two_eyes = make_fixture({{false, 0}, {true, 4}});
  const auto two_eye_plan = plan(two_eyes, &capture);
  check(two_eye_plan && two_eye_plan->variant == Variant::TwoEyeChunks &&
            two_eye_plan->chunk_count == 2 && two_eye_plan->chunks[0].pair_index == 0 &&
            two_eye_plan->chunks[1].pair_index == 4 &&
            two_eye_plan->eye_slot_mask == ((3ull << 0) | (3ull << 8)) &&
            capture.transfer_count == 59 && capture.total_payload_bytes == 3888,
        "PRIS2 accepts the source-valid two-eye-chunk envelope");
}

void test_live_copy_matching() {
  auto live = make_fixture({{false, 2}});
  auto copied = make_fixture({{false, 2}}, 0x2000);
  const auto live_plan = plan(live);
  auto copied_plan = plan(copied);
  check(live_plan && copied_plan &&
            live_plan->chunks[0].start_relative_tag_offset !=
                copied_plan->chunks[0].start_relative_tag_offset &&
            metal_renderer::jak2_pris2_texture_upload_plans_match(*live_plan, *copied_plan),
        "relocated bucket-228 plans match by owned semantics");

  const u64 copied_tex0 = get_u64(copied.packet, copied.first_eye_offset + 224);
  put_u64(&copied.packet, copied.first_eye_offset + 224,
          (copied_tex0 & ~0x3fffull) | 0x321);
  copied_plan = plan(copied);
  check(copied_plan && copied_plan->semantic_fingerprint !=
                             live_plan->semantic_fingerprint &&
            !metal_renderer::jak2_pris2_texture_upload_plans_match(*live_plan, *copied_plan),
        "a valid source mutation between live and copied plans fails matching");
}

void test_source_ordered_eye_slot_ownership() {
  std::array<metal_renderer::Jak2PrisEyeTextureUploadPlan, 6> per_level;
  for (std::size_t i = 0; i < per_level.size(); ++i) {
    per_level[i].bucket_id = metal_renderer::kJak2PrisTextureUploadBuckets[i];
  }
  metal_renderer::Jak2CommonPrisTextureUploadPlan common;
  std::array<metal_renderer::Jak2Pris2Bucket228Plan, 6> pris2;
  for (std::size_t i = 0; i < pris2.size(); ++i) {
    pris2[i].bucket_id = metal_renderer::kJak2Pris2TextureUploadBuckets[i];
  }

  auto add_pair = [](auto* plan, u32 pair) {
    auto& chunk = plan->chunks[plan->chunk_count++];
    chunk.pair_index = pair;
    chunk.eye_slot_mask = 3ull << (pair * 2);
    plan->eye_slot_mask |= chunk.eye_slot_mask;
  };
  per_level[1].present = true;
  add_pair(&per_level[1], 12);
  per_level[2].present = true;
  add_pair(&per_level[2], 0);
  add_pair(&per_level[2], 12);
  check(metal_renderer::jak2_pris_eye_plan_sequence_is_valid(
            per_level.data(), per_level.size(), common, pris2.data(), pris2.size()),
        "Build18 bucket 200 pair 12 may be reused by bucket 204 after its Merc consumer");

  per_level[2].chunks[1].pair_index = 0;
  per_level[2].chunks[1].eye_slot_mask = 0x3;
  check(!metal_renderer::jak2_pris_eye_plan_sequence_is_valid(
             per_level.data(), per_level.size(), common, pris2.data(), pris2.size()),
        "duplicate slots within one producer remain fail-closed");
  per_level[2].chunks[1].pair_index = 12;
  per_level[2].chunks[1].eye_slot_mask = 0x03000000;

  per_level[2].bucket_id = 208;
  check(!metal_renderer::jak2_pris_eye_plan_sequence_is_valid(
             per_level.data(), per_level.size(), common, pris2.data(), pris2.size()),
        "a reordered per-level producer sequence is rejected");
  per_level[2].bucket_id = 204;

  per_level[1].eye_slot_mask ^= 1;
  check(!metal_renderer::jak2_pris_eye_plan_sequence_is_valid(
             per_level.data(), per_level.size(), common, pris2.data(), pris2.size()),
        "a producer mask not exactly owned by its chunks is rejected");

  check(metal_renderer::jak2_pris_eye_producer_precedes(200, 204) &&
            metal_renderer::jak2_pris_eye_producer_precedes(220, 224) &&
            !metal_renderer::jak2_pris_eye_producer_precedes(204, 200) &&
            !metal_renderer::jak2_pris_eye_producer_precedes(200, 200) &&
            !metal_renderer::jak2_pris_eye_producer_precedes(199, 204),
        "only distinct audited PRIS producers are ordered for generation reuse");
}

void test_source_grammar_and_rejections() {
  auto clipped = make_fixture({{false, 2}});
  put_u32(&clipped.packet, clipped.first_eye_offset + 576, 0);
  put_u32(&clipped.packet, clipped.first_eye_offset + 608, 16);
  put_u32(&clipped.packet, clipped.first_eye_offset + 1440, 0);
  put_u32(&clipped.packet, clipped.first_eye_offset + 1472, 16);
  put_u32(&clipped.packet, clipped.first_eye_offset + 1764, 0);
  put_u32(&clipped.packet, clipped.first_eye_offset + 1796, 16);
  check(plan(clipped).has_value(),
        "source-valid dynamic clipping remains accepted for bucket 228");

  auto bad_shape = make_fixture({{false, 2}});
  put_u64(&bad_shape.packet, bad_shape.first_eye_offset + 16,
          make_gif_tag_word(7, false, 0, 1));
  metal_renderer::Jak2PrisEyeTextureUploadRejection rejection;
  check(!plan(bad_shape, nullptr, &rejection) &&
            rejection.reason == RejectReason::SetupTag,
        "a transposed source GS-set GIF shape is rejected");

  auto bad_selector = make_fixture({{false, 2}});
  const u32 tex1_address_offset = bad_selector.first_eye_offset + 248;
  const u64 tex1_address = get_u64(bad_selector.packet, tex1_address_offset);
  put_u64(&bad_selector.packet, tex1_address_offset,
          (tex1_address & ~0xffull) | static_cast<u8>(GsRegisterAddress::TEX1_2));
  check(!plan(bad_selector, nullptr, &rejection) &&
            rejection.reason == RejectReason::BodyAdgif && rejection.chunk_index == 0 &&
            rejection.body_index == 0,
        "an A+D selector with the wrong low byte is rejected");

  auto extra = make_fixture({{false, 2}});
  put_tag(&extra.packet, extra.linker_offset, DmaTag::Kind::NEXT, 0, kExtraOffset, 0, 0);
  put_tag(&extra.packet, kExtraOffset, DmaTag::Kind::CNT, 0, 0, 0, 0);
  put_tag(&extra.packet, kExtraOffset + 16, DmaTag::Kind::NEXT, 0, kDirectOffset, 0, 0);
  check(!plan(extra).has_value(), "an otherwise inert unknown extension is rejected");

  auto malformed = make_fixture({{false, 2}});
  put_tag(&malformed.packet, bucket_offset(), DmaTag::Kind::NEXT, 0,
          kOrdinaryOffset, 0, 0, true);
  check(!plan(malformed).has_value(), "an SPR-routed envelope is rejected as malformed");

  auto valid = make_fixture({{false, 2}});
  rejection = {};
  check(!metal_renderer::plan_jak2_pris2_bucket228(
             valid.packet.data(), valid.packet.size(), kChainOffset, 229,
             valid.packet.data(), valid.packet.size(), nullptr, &rejection) &&
            rejection.reason == RejectReason::UnsupportedBucket,
        "bucket 229 is outside this producer's ownership");
  check(!metal_renderer::plan_jak2_pris2_bucket228(
             valid.packet.data(), valid.packet.size(), kChainOffset, 220,
             valid.packet.data(), valid.packet.size()),
        "common PRIS bucket 220 is rejected before alias parsing");

  for (std::size_t i = 0; i < metal_renderer::kJak2Pris2TextureUploadBuckets.size(); ++i) {
    const u32 bucket_id = metal_renderer::kJak2Pris2TextureUploadBuckets[i];
    std::vector<EyeChunkSpec> chunks;
    if (i % 3 == 1) {
      chunks.push_back({false, 2});
    } else if (i % 3 == 2) {
      chunks.push_back({false, 0});
      chunks.push_back({true, 4});
    }
    auto slot_fixture = make_fixture(chunks, 0, bucket_id);
    const auto slot_plan = plan(slot_fixture, nullptr, nullptr, bucket_id);
    check(slot_plan && slot_plan->bucket_id == bucket_id &&
              slot_plan->chunk_count == chunks.size(),
          "all six dynamic PRIS2 texture slots accept their exact zero/one/two-chunk grammar");
  }
  for (const u32 bucket_id : metal_renderer::kJak2Pris2MercBuckets) {
    auto wrong_consumer = make_fixture({}, 0, metal_renderer::kJak2Pris2TextureUploadBucket);
    check(!plan(wrong_consumer, nullptr, nullptr, bucket_id),
          "PRIS2 Merc consumers cannot be parsed as texture producers");
  }
}

constexpr u32 kManyFirstEyeOffset = 0x30000;
constexpr u32 kManyEyeStride = 0x1000;
constexpr u32 kManyDirectOffset = 0xa0000;
constexpr std::size_t kManyMemorySize = 0x100000;
constexpr u32 kEyeTex0Offset = 224;
constexpr u32 kEyeFirstScissorValueOffset = 336;

struct ManyFixture {
  std::vector<u8> packet;
  std::vector<u32> chunk_offsets;
  std::vector<u32> linker_offsets;
};

// Source-shaped PRIS2 envelope with any number of chunks, each in its own update-eyes DMA block.
ManyFixture make_many_fixture(const std::vector<EyeChunkSpec>& chunks, u32 dma_relocation = 0) {
  ManyFixture fixture{std::vector<u8>(kManyMemorySize), {}, {}};
  const u32 ordinary_offset = kOrdinaryOffset + dma_relocation;
  const u32 direct_offset = kManyDirectOffset + dma_relocation;
  const u32 end_offset = bucket_offset() + 16;
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    fixture.chunk_offsets.push_back(kManyFirstEyeOffset + dma_relocation +
                                    static_cast<u32>(i) * kManyEyeStride);
  }
  put_tag(&fixture.packet, bucket_offset(), DmaTag::Kind::NEXT, 0, ordinary_offset, 0, 0);
  put_tag(&fixture.packet, ordinary_offset, DmaTag::Kind::CNT, 1, 0, kPcPortVif, 3);
  put_u64(&fixture.packet, ordinary_offset + 16, kPageOffset);
  put_u64(&fixture.packet, ordinary_offset + 24, static_cast<u64>(-1));
  put_tag(&fixture.packet, ordinary_offset + 32, DmaTag::Kind::NEXT, 0,
          chunks.empty() ? direct_offset : fixture.chunk_offsets[0], 0, 0);
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    const u32 linker = put_eye_chunk(&fixture.packet, fixture.chunk_offsets[i], chunks[i]);
    fixture.linker_offsets.push_back(linker);
    put_tag(&fixture.packet, linker, DmaTag::Kind::NEXT, 0,
            i + 1 < chunks.size() ? fixture.chunk_offsets[i + 1] : direct_offset, 0, 0);
  }
  put_tag(&fixture.packet, direct_offset, DmaTag::Kind::CNT, 10, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 10);
  std::fill_n(fixture.packet.begin() + direct_offset + 16, 160, 0x52);
  put_tag(&fixture.packet, direct_offset + 176, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
  fixture.packet[kPageOffset + 8] = 0x44;
  return fixture;
}

std::optional<metal_renderer::Jak2Pris2Bucket228Plan> plan_many(
    const ManyFixture& fixture,
    Capture* capture = nullptr,
    metal_renderer::Jak2PrisEyeTextureUploadRejection* rejection = nullptr) {
  return metal_renderer::plan_jak2_pris2_texture_upload(
      fixture.packet.data(), fixture.packet.size(), kChainOffset,
      metal_renderer::kJak2Pris2TextureUploadBucket, fixture.packet.data(),
      fixture.packet.size(), capture, rejection);
}

void mutate_tex0(std::vector<u8>* packet, u32 chunk_offset) {
  const u64 tex0 = get_u64(*packet, chunk_offset + kEyeTex0Offset);
  put_u64(packet, chunk_offset + kEyeTex0Offset, (tex0 & ~0x3fffull) | 0x321);
}

bool sequence_accepts_pris2(const metal_renderer::Jak2Pris2Bucket228Plan& plan) {
  std::array<metal_renderer::Jak2PrisEyeTextureUploadPlan, 6> per_level;
  for (std::size_t i = 0; i < per_level.size(); ++i) {
    per_level[i].bucket_id = metal_renderer::kJak2PrisTextureUploadBuckets[i];
  }
  metal_renderer::Jak2CommonPrisTextureUploadPlan common;
  std::array<metal_renderer::Jak2Pris2Bucket228Plan, 6> pris2;
  for (std::size_t i = 0; i < pris2.size(); ++i) {
    pris2[i].bucket_id = metal_renderer::kJak2Pris2TextureUploadBuckets[i];
  }
  pris2[1] = plan;
  return metal_renderer::jak2_pris_eye_plan_sequence_is_valid(
      per_level.data(), per_level.size(), common, pris2.data(), pris2.size());
}

void test_source_bounded_chunk_count() {
  auto three = make_many_fixture({{false, 0}, {false, 1}, {true, 8}});
  Capture capture;
  const auto three_plan = plan_many(three, &capture);
  check(three_plan && three_plan->variant == Variant::ManyEyeChunks &&
            three_plan->chunk_count == 3 && three_plan->chunks[0].pair_index == 0 &&
            three_plan->chunks[1].pair_index == 1 && three_plan->chunks[2].pair_index == 8 &&
            three_plan->chunks[2].resolution == metal_renderer::Jak2PrisEyeResolution::Eye64 &&
            three_plan->chunks[0].start_transfer_index == 3 &&
            three_plan->chunks[1].start_transfer_index == 30 &&
            three_plan->chunks[2].start_transfer_index == 57 &&
            three_plan->eye_slot_mask == (0x3ull | (0x3ull << 2) | (0x3ull << 16)) &&
            three_plan->direct_reset_transfer_index == 84 &&
            three_plan->terminal_transfer_index == 85 && capture.valid &&
            capture.transfer_count == 86 && capture.total_payload_bytes == 5744 &&
            capture.inert_transfers == 6 && capture.eye_markers == 6 &&
            capture.gs_setup_transfers == 33 && capture.other_transfers == 39 &&
            capture.malformed_transfers == 0,
        "PRIS2 owns the live third eye chunk that overflowed the generic 64-transfer capture");
  const auto three_renderer =
      metal_renderer::adapt_jak2_pris2_to_pris_eye_plan(*three_plan);
  check(three_renderer.present && three_renderer.chunk_count == 3 &&
            three_renderer.chunks[2].semantic_fingerprint ==
                three_plan->chunks[2].semantic_fingerprint &&
            three_renderer.chunks[2].start_relative_tag_offset ==
                three_plan->chunks[2].start_relative_tag_offset &&
            three_renderer.semantic_fingerprint == three_plan->semantic_fingerprint,
        "the renderer adapter keeps every PRIS2 chunk in DMA order");
  check(sequence_accepts_pris2(*three_plan),
        "the producer sequence accepts an owned three-chunk PRIS2 plan");

  std::vector<EyeChunkSpec> maximum;
  for (std::size_t i = 0; i < metal_renderer::kJak2Pris2EyeMaximumChunks; ++i) {
    maximum.push_back({false, static_cast<u32>(i % 16)});
  }
  auto maximum_fixture = make_many_fixture(maximum);
  const auto maximum_plan = plan_many(maximum_fixture, &capture);
  check(maximum_plan && maximum_plan->variant == Variant::ManyEyeChunks &&
            maximum_plan->chunk_count == 96 && maximum_plan->eye_slot_mask == 0xffffffffull &&
            capture.transfer_count == metal_renderer::kJak2Pris2TextureUploadMaximumTransfers &&
            sequence_accepts_pris2(*maximum_plan),
        "PRIS2 accepts the update-eyes 6x16 emission bound with repeated same-resolution pairs");

  maximum.push_back({false, 0});
  auto overflow = make_many_fixture(maximum);
  metal_renderer::Jak2PrisEyeTextureUploadRejection rejection;
  check(!plan_many(overflow, &capture, &rejection) &&
            rejection.reason == RejectReason::CaptureInvalid && !capture.valid &&
            capture.malformed_transfers == 1 &&
            capture.transfer_count == metal_renderer::kJak2Pris2TextureUploadMaximumTransfers,
        "a 97th chunk is rejected explicitly at the source-derived capture bound");
}

void test_same_resolution_last_writer() {
  // Live build-250 tick 11972: two Eye64 chunks from different actors both target hi-res row 0.
  auto live = make_many_fixture({{true, 0}, {true, 1}});
  metal_renderer::Jak2PrisEyeTextureUploadRejection rejection;
  const auto plan = plan_many(live, nullptr, &rejection);
  check(plan && rejection.reason == RejectReason::None &&
            plan->variant == Variant::TwoEyeChunks && plan->chunk_count == 2 &&
            plan->chunks[0].pair_index == 0 && plan->chunks[1].pair_index == 0 &&
            plan->chunks[0].resolution == metal_renderer::Jak2PrisEyeResolution::Eye64 &&
            plan->chunks[1].resolution == metal_renderer::Jak2PrisEyeResolution::Eye64 &&
            plan->chunks[0].semantic_fingerprint != plan->chunks[1].semantic_fingerprint &&
            plan->eye_slot_mask == 0x3 && sequence_accepts_pris2(*plan),
        "same-resolution hi-res row reuse in one PRIS2 producer is planned as ordered writes");

  auto swapped = make_many_fixture({{true, 1}, {true, 0}});
  const auto swapped_plan = plan_many(swapped);
  check(swapped_plan && swapped_plan->semantic_fingerprint != plan->semantic_fingerprint &&
            !metal_renderer::jak2_pris2_texture_upload_plans_match(*plan, *swapped_plan),
        "the final writer is order-sensitive in the plan fingerprint");

  auto identical = make_many_fixture({{false, 3}, {false, 3}, {false, 3}});
  const auto identical_plan = plan_many(identical);
  check(identical_plan && identical_plan->variant == Variant::ManyEyeChunks &&
            identical_plan->chunk_count == 3 && identical_plan->eye_slot_mask == (0x3ull << 6),
        "repeated lo-res writes of one pair remain owned per chunk");

  const std::vector<std::vector<EyeChunkSpec>> mixed = {
      {{false, 0}, {true, 0}}, {{true, 4}, {false, 4}}, {{true, 0}, {true, 1}, {false, 0}}};
  const std::array<u8, 3> mixed_chunk = {1, 1, 2};
  for (std::size_t i = 0; i < mixed.size(); ++i) {
    auto fixture = make_many_fixture(mixed[i]);
    rejection = {};
    const bool rejected = !plan_many(fixture, nullptr, &rejection);
    const auto& later = mixed[i][mixed_chunk[i]];
    check(rejected && rejection.reason == RejectReason::MixedResolutionEyeSlots &&
              rejection.chunk_index == mixed_chunk[i] &&
              rejection.duplicate_resolution == static_cast<u8>(later.eye64) &&
              rejection.previous_resolution == static_cast<u8>(!later.eye64) &&
              rejection.duplicate_pair_index == rejection.previous_pair_index,
          "mixed-resolution writes of one PRIS2 eye pair remain rejected");
  }
}

void test_many_chunk_live_copy_and_corruption() {
  const std::vector<EyeChunkSpec> specs = {{false, 0}, {true, 0}, {true, 1}, {false, 9}};
  auto live = make_many_fixture(specs);
  auto copied = make_many_fixture(specs, 0x4000);
  const auto live_plan = plan_many(live);
  auto copied_plan = plan_many(copied);
  check(!live_plan, "a lo-res pair 0 followed by hi-res row 0 is mixed and rejected");

  const std::vector<EyeChunkSpec> valid_specs = {{false, 0}, {true, 4}, {true, 5}, {false, 9}};
  live = make_many_fixture(valid_specs);
  copied = make_many_fixture(valid_specs, 0x4000);
  const auto valid_live = plan_many(live);
  copied_plan = plan_many(copied);
  check(valid_live && copied_plan && valid_live->chunk_count == 4 &&
            valid_live->chunks[3].start_relative_tag_offset !=
                copied_plan->chunks[3].start_relative_tag_offset &&
            metal_renderer::jak2_pris2_texture_upload_plans_match(*valid_live, *copied_plan),
        "relocated four-chunk PRIS2 plans match by owned semantics");
  for (const std::size_t chunk : {std::size_t(2), std::size_t(3)}) {
    auto mutated = make_many_fixture(valid_specs, 0x4000);
    mutate_tex0(&mutated.packet, mutated.chunk_offsets[chunk]);
    const auto mutated_plan = plan_many(mutated);
    check(mutated_plan && mutated_plan->chunks[chunk].semantic_fingerprint !=
                              valid_live->chunks[chunk].semantic_fingerprint &&
              !metal_renderer::jak2_pris2_texture_upload_plans_match(*valid_live, *mutated_plan),
          "a copied mutation in a later or rewritten chunk fails live/copy matching");
  }

  const std::vector<EyeChunkSpec> three = {{false, 0}, {false, 1}, {false, 2}};
  auto bad_scissor = make_many_fixture(three);
  put_u64(&bad_scissor.packet, bad_scissor.chunk_offsets[2] + kEyeFirstScissorValueOffset,
          make_scissor(0, 63, 65, 96));
  metal_renderer::Jak2PrisEyeTextureUploadRejection rejection;
  check(!plan_many(bad_scissor, nullptr, &rejection) &&
            rejection.reason == RejectReason::BodyScissor && rejection.chunk_index == 2 &&
            rejection.body_index == 1,
        "a corrupt third-chunk scissor is rejected with its chunk index");

  auto skipped_setup = make_many_fixture(three);
  put_tag(&skipped_setup.packet, skipped_setup.linker_offsets[1], DmaTag::Kind::NEXT, 0,
          skipped_setup.chunk_offsets[2] + 144, 0, 0);
  rejection = {};
  check(!plan_many(skipped_setup, nullptr, &rejection) &&
            rejection.reason == RejectReason::Counts,
        "a truncated later chunk cannot be counted as a complete chunk");

  auto cycle = make_many_fixture(three);
  put_tag(&cycle.packet, cycle.linker_offsets[2], DmaTag::Kind::NEXT, 0, cycle.chunk_offsets[0],
          0, 0);
  rejection = {};
  Capture capture;
  check(!plan_many(cycle, &capture, &rejection) &&
            rejection.reason == RejectReason::CaptureInvalid && capture.malformed_transfers == 1,
        "a looping PRIS2 chain is rejected before the capture bound");

  auto extra = make_many_fixture(three);
  put_tag(&extra.packet, extra.linker_offsets[1], DmaTag::Kind::NEXT, 0, kExtraOffset, 0, 0);
  put_tag(&extra.packet, kExtraOffset, DmaTag::Kind::CNT, 0, 0, 0, 0);
  put_tag(&extra.packet, kExtraOffset + 16, DmaTag::Kind::NEXT, 0, extra.chunk_offsets[2], 0, 0);
  rejection = {};
  check(!plan_many(extra, nullptr, &rejection) && rejection.reason == RejectReason::Counts,
        "an inert extension between later chunks is rejected");
}

void test_pris2_sequence_contract() {
  auto three = make_many_fixture({{false, 0}, {false, 1}, {false, 2}});
  auto plan = *plan_many(three);
  auto forged = plan;
  forged.variant = Variant::TwoEyeChunks;
  check(!sequence_accepts_pris2(forged), "a PRIS2 variant must agree with its chunk count");
  forged = plan;
  forged.chunk_count = metal_renderer::kJak2Pris2EyeMaximumChunks + 1;
  check(!sequence_accepts_pris2(forged), "a PRIS2 chunk count beyond the source bound fails");
  forged = plan;
  forged.chunks[2].pair_index = 0;
  forged.chunks[2].eye_slot_mask = 0x3;
  forged.eye_slot_mask = 0x3 | (0x3ull << 2);
  check(sequence_accepts_pris2(forged), "a same-resolution PRIS2 rewrite is owned");
  forged.chunks[2].resolution = metal_renderer::Jak2PrisEyeResolution::Eye64;
  check(!sequence_accepts_pris2(forged), "a mixed-resolution PRIS2 rewrite is not owned");

  check(metal_renderer::jak2_pris_eye_producer_allows_same_resolution_rewrite(224) &&
            metal_renderer::jak2_pris_eye_producer_allows_same_resolution_rewrite(228) &&
            metal_renderer::jak2_pris_eye_producer_allows_same_resolution_rewrite(244) &&
            !metal_renderer::jak2_pris_eye_producer_allows_same_resolution_rewrite(196) &&
            !metal_renderer::jak2_pris_eye_producer_allows_same_resolution_rewrite(220) &&
            !metal_renderer::jak2_pris_eye_producer_allows_same_resolution_rewrite(229) &&
            !metal_renderer::jak2_pris_eye_producer_allows_same_resolution_rewrite(0),
        "only PRIS2 texture producers may rewrite their own eye pairs");
}

}  // namespace

int main() {
  test_observed_forms();
  test_live_copy_matching();
  test_source_ordered_eye_slot_ownership();
  test_source_grammar_and_rejections();
  test_source_bounded_chunk_count();
  test_same_resolution_last_writer();
  test_many_chunk_live_copy_and_corruption();
  test_pris2_sequence_contract();
  std::puts("metal_jak2_pris2_bucket228_plan_test: PASS");
  return 0;
}
