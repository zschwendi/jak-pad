#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "common/dma/dma.h"

namespace {

constexpr u32 kChainOffset = 0x100;
constexpr u32 kOrdinaryOffset = 0x4000;
constexpr u32 kAnimatorOffset = 0x5000;
constexpr u32 kDirectSetupOffset = 0x6000;
constexpr u32 kExtraTransferOffset = 0x6800;
constexpr u32 kTexturePageOffset = 0x7000;
constexpr u32 kAnimatorBodyTagOffset = kAnimatorOffset + 16;
constexpr u32 kAnimatorBodyOffset = kAnimatorBodyTagOffset + 16;
constexpr u32 kAnimatorFinishOffset = kAnimatorBodyOffset + 496;
constexpr u32 kAnimatorNextOffset = kAnimatorFinishOffset + 16;
constexpr u32 kSecurityAnimatorFinishOffset = kAnimatorBodyOffset + 832;
constexpr u32 kSecurityAnimatorNextOffset = kSecurityAnimatorFinishOffset + 16;
constexpr std::size_t kMemorySize = 0x10000;
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

std::vector<u8> make_normal_ordinary_fixture(u32 bucket_id, s64 mode = -1) {
  std::vector<u8> packet(kMemorySize);
  const u32 end_offset = bucket_offset(bucket_id) + 16;
  put_tag(&packet, bucket_offset(bucket_id), DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0);
  put_tag(&packet, kOrdinaryOffset, DmaTag::Kind::CNT, 1, 0, kPcPortVif, 3);
  put_u64(&packet, kOrdinaryOffset + 16, kTexturePageOffset);
  put_u64(&packet, kOrdinaryOffset + 24, static_cast<u64>(mode));
  put_tag(&packet, kOrdinaryOffset + 32, DmaTag::Kind::NEXT, 0, kDirectSetupOffset, 0, 0);
  put_tag(&packet, kDirectSetupOffset, DmaTag::Kind::CNT, 10, 0,
          static_cast<u32>(VifCode::Kind::FLUSHA) << 24, kDirectVif | 10);
  std::fill_n(packet.begin() + kDirectSetupOffset + 16, 160, 0x52);
  put_tag(&packet, kDirectSetupOffset + 176, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
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

void put_layer_values(std::vector<u8>* packet, u32 offset, float base, u8 padding) {
  for (u32 i = 0; i < 18; ++i) {
    put_float(packet, offset + i * sizeof(float), base + static_cast<float>(i) * 0.25f);
  }
  std::fill_n(packet->begin() + offset + 72, 8, padding);
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

  for (const u32 bucket_id : metal_renderer::kJak2WaterTextureUploadBuckets) {
    const auto result = capture(make_empty_fixture(bucket_id), bucket_id);
    check(result.valid && !result.present && result.classification == Classification::Absent,
          "each source-identical water texture bucket accepts an exact empty chain");
  }

  const auto packet = make_empty_fixture();
  check(!metal_renderer::capture_jak2_tfrag_texture_upload(
             packet.data(), packet.size(), kChainOffset, 8)
             .valid,
        "an unaudited bucket cannot enter the TFRAG texture classifier");
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
  check(capture(packet).valid &&
            !metal_renderer::plan_jak2_common_tfrag_texture_upload(
                 packet.data(), packet.size(), kChainOffset, packet.data(), packet.size())
                 .has_value(),
        "the legacy alternate finish metadata stays capturable but not executable by this plan");

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
  check(result.valid && result.classification == Classification::AnimatorOnly,
        "the source-defined legacy PC_PORT finish VIF is accepted");

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

}  // namespace

int main() {
  test_exact_empty_and_ordinary_metadata();
  test_texture_bucket_allowlist();
  test_normal_tfrag_execution_plan();
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
  std::puts("PASS: Jak II common-tfrag texture-upload metadata capture");
  return 0;
}
