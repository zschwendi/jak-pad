#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/dma/dma.h"

namespace {

constexpr u32 kChainOffset = 0x100;
constexpr u32 kOrdinaryOffset = 0x4000;
constexpr u32 kAnimatorOffset = 0x5000;
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

u32 bucket_offset() {
  return kChainOffset + metal_renderer::kJak2CommonTfragTextureUploadBucket * 16;
}

void put_u32(std::vector<u8>* memory, u32 offset, u32 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_u64(std::vector<u8>* memory, u32 offset, u64 value) {
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

Capture capture(const std::vector<u8>& packet) {
  return metal_renderer::capture_jak2_common_tfrag_texture_upload(
      packet.data(), packet.size(), kChainOffset);
}

std::vector<u8> make_empty_fixture() {
  std::vector<u8> packet(kMemorySize);
  put_tag(&packet, bucket_offset(), DmaTag::Kind::CNT, 0, 0, 0, 0);
  return packet;
}

std::vector<u8> make_ordinary_fixture() {
  std::vector<u8> packet(kMemorySize);
  const u32 end_offset = bucket_offset() + 16;
  put_tag(&packet, bucket_offset(), DmaTag::Kind::NEXT, 0, kOrdinaryOffset, 0, 0);
  put_tag(&packet, kOrdinaryOffset, DmaTag::Kind::CNT, 1, 0, kPcPortVif, 3);
  std::fill_n(packet.begin() + kOrdinaryOffset + 16, 16, 0x31);
  put_tag(&packet, kOrdinaryOffset + 32, DmaTag::Kind::NEXT, 0, end_offset, 0, 0);
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

bool metadata_matches(const Capture& lhs, const Capture& rhs) {
  if (lhs.valid != rhs.valid || lhs.present != rhs.present ||
      lhs.classification != rhs.classification || lhs.transfer_count != rhs.transfer_count ||
      lhs.total_payload_bytes != rhs.total_payload_bytes ||
      lhs.inert_transfers != rhs.inert_transfers ||
      lhs.ordinary_descriptors != rhs.ordinary_descriptors ||
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
            result.eye_markers == 0 && result.other_transfers == 1,
        "unclassified work remains visible and cannot look ordinary-only");

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
