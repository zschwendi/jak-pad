#include "game/graphics/pipelines/metal/metal_jak2_warp_texture_upload_plan.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr u32 kChainOffset = 0x100;
constexpr u32 kFirstGroupOffset = 0x4000;
constexpr u32 kGroupStride = 0x100;
constexpr u32 kTailOffset = 0x6000;
constexpr u32 kExtraOffset = 0x7000;
constexpr u32 kFirstPageOffset = 0x9000;
constexpr u32 kAlternatePageOffset = 0xb000;
constexpr u32 kPageStride = 0x200;
constexpr std::size_t kMemorySize = 0x10000;
constexpr u8 kDmaCnt = 1;
constexpr u8 kDmaNext = 2;
constexpr u8 kDmaCall = 5;
constexpr u32 kPcPortVif = 8u << 24;
constexpr u32 kFlushaVif = 19u << 24;
constexpr u32 kDirectVif = 80u << 24;

using Plan = metal_renderer::Jak2WarpTextureUploadPlan;
using Variant = metal_renderer::Jak2WarpTextureUploadVariant;

enum class AbsentForm {
  ZeroCnt,
  ZeroNext,
};

struct Fixture {
  std::vector<u8> packet = std::vector<u8>(kMemorySize);
  std::vector<u8> live = std::vector<u8>(kMemorySize);
  std::array<u32, 8> direct_offsets = {};
  std::array<u32, 8> descriptor_offsets = {};
  std::array<u32, 9> boundary_offsets = {};
  u32 chain_offset = kChainOffset;
  u32 tail_offset = 0;
  u32 final_boundary_offset = 0;
  u32 upload_count = 0;
};

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

u32 bucket_offset(u32 chain_offset) {
  return chain_offset + metal_renderer::kJak2WarpTextureUploadBucket * 16;
}

u32 page_offset(u32 index) {
  return kFirstPageOffset + index * kPageStride;
}

void put_u32(std::vector<u8>* memory, u32 offset, u32 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_u64(std::vector<u8>* memory, u32 offset, u64 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_tag(std::vector<u8>* memory,
             u32 offset,
             u8 kind,
             u16 qwc,
             u32 address,
             u32 vif0,
             u32 vif1) {
  const u64 tag =
      static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) | (static_cast<u64>(address) << 32);
  put_u64(memory, offset, tag);
  put_u32(memory, offset + 8, vif0);
  put_u32(memory, offset + 12, vif1);
}

void put_page_header(std::vector<u8>* memory, u32 offset, u8 seed) {
  for (std::size_t i = 0; i < metal_renderer::kJak2Bucket4OrdinaryPageHeaderBytes; ++i) {
    (*memory)[offset + i] = static_cast<u8>(seed + i);
  }
  put_u32(memory, offset + 12, 1);
  put_u32(memory, offset + metal_renderer::kJak2Bucket4OrdinaryPageHeaderBytes, offset + 0x180);
}

Fixture make_present_fixture(u32 upload_count, u32 relocation = 0) {
  check(upload_count <= 8, "the fixture remains bounded to one excess upload group");
  Fixture fixture;
  fixture.chain_offset = kChainOffset + relocation;
  fixture.upload_count = upload_count;
  fixture.tail_offset = kTailOffset + relocation;
  fixture.final_boundary_offset = fixture.tail_offset + 176;
  const u32 first_group = kFirstGroupOffset + relocation;
  const u32 start = bucket_offset(fixture.chain_offset);

  put_tag(&fixture.packet, start, kDmaNext, 0,
          upload_count == 0 ? fixture.tail_offset : first_group, 0, 0);
  fixture.boundary_offsets[0] = start;

  for (u32 i = 0; i < upload_count; ++i) {
    const u32 group = first_group + i * kGroupStride;
    const u32 next = i + 1 == upload_count ? fixture.tail_offset : group + kGroupStride;
    fixture.direct_offsets[i] = group;
    put_tag(&fixture.packet, group, kDmaCnt, 2, 0, 0, kDirectVif | 2);
    std::fill_n(fixture.packet.begin() + group + 16, 32, static_cast<u8>(0x20 + i));

    fixture.descriptor_offsets[i] = group + 48;
    put_tag(&fixture.packet, group + 48, kDmaCnt, 1, 0, kPcPortVif, 3);
    put_u64(&fixture.packet, group + 64, page_offset(i));
    put_u64(&fixture.packet, group + 72, static_cast<u64>(-1));

    fixture.boundary_offsets[i + 1] = group + 80;
    put_tag(&fixture.packet, group + 80, kDmaNext, 0, next, 0, 0);
    put_page_header(&fixture.live, page_offset(i), static_cast<u8>(0x40 + i * 7));
  }

  put_tag(&fixture.packet, fixture.tail_offset, kDmaCnt, 10, 0, kFlushaVif, kDirectVif | 10);
  std::fill_n(fixture.packet.begin() + fixture.tail_offset + 16, 160, 0x9a);
  put_tag(&fixture.packet, fixture.final_boundary_offset, kDmaNext, 0, start + 16, 0, 0);
  put_page_header(&fixture.live, kAlternatePageOffset, 0xc0);
  return fixture;
}

Fixture make_absent_fixture(AbsentForm form, u32 relocation = 0) {
  Fixture fixture;
  fixture.chain_offset = kChainOffset + relocation;
  const u32 start = bucket_offset(fixture.chain_offset);
  if (form == AbsentForm::ZeroCnt) {
    put_tag(&fixture.packet, start, kDmaCnt, 0, 0, 0, 0);
  } else {
    put_tag(&fixture.packet, start, kDmaNext, 0, start + 16, 0, 0);
  }
  return fixture;
}

std::optional<Plan> parse(const Fixture& fixture,
                          u32 bucket_id = metal_renderer::kJak2WarpTextureUploadBucket) {
  return metal_renderer::plan_jak2_warp_texture_upload(fixture.packet.data(), fixture.packet.size(),
                                                       fixture.chain_offset, bucket_id,
                                                       fixture.live.data(), fixture.live.size());
}

void test_exact_present_and_absent_envelopes() {
  for (const u32 upload_count : {1u, 2u, 4u, 7u}) {
    auto fixture = make_present_fixture(upload_count);
    const auto result = parse(fixture);
    check(result && result->variant == Variant::Ordinary && result->upload_count == upload_count &&
              result->transfer_count == 3 * upload_count + 3 &&
              result->total_payload_bytes == 160 + 48 * upload_count,
          "one-through-seven groups produce the exact bounded warp-upload envelope");
    for (u32 i = 0; i < upload_count; ++i) {
      check(result->uploads[i].page_offset == page_offset(i) && result->uploads[i].mode == -1 &&
                result->uploads[i].page_header[0] == static_cast<u8>(0x40 + i * 7),
            "ordered descriptors and texture-page headers are retained by value");
    }

    const auto owned_headers = result->uploads;
    std::fill(fixture.packet.begin(), fixture.packet.end(), 0xa5);
    std::fill(fixture.live.begin(), fixture.live.end(), 0x5a);
    for (u32 i = 0; i < upload_count; ++i) {
      check(result->uploads[i].page_header == owned_headers[i].page_header,
            "source-memory reuse cannot change an owned warp-upload plan");
    }
  }

  const auto absent_cnt = parse(make_absent_fixture(AbsentForm::ZeroCnt));
  check(absent_cnt && absent_cnt->variant == Variant::Absent && absent_cnt->transfer_count == 1 &&
            absent_cnt->upload_count == 0 && absent_cnt->total_payload_bytes == 0 &&
            absent_cnt->transfers[0].tag_kind == kDmaCnt,
        "the exact zero-CNT empty bucket entry produces an absent plan");

  const auto absent_next = parse(make_absent_fixture(AbsentForm::ZeroNext));
  check(absent_next && absent_next->variant == Variant::Absent &&
            absent_next->transfer_count == 1 && absent_next->upload_count == 0 &&
            absent_next->total_payload_bytes == 0 && absent_next->transfers[0].tag_kind == kDmaNext,
        "the exact zero-NEXT link to the bucket boundary produces an absent plan");
}

void test_live_and_copied_semantic_match() {
  const auto live_fixture = make_present_fixture(3);
  const auto copied_fixture = make_present_fixture(3, 0x1000);
  const auto live = parse(live_fixture);
  const auto copied = parse(copied_fixture);
  check(live && copied && metal_renderer::jak2_warp_texture_upload_plans_match(*live, *copied),
        "relocated live and copied packets retain identical owned semantics");

  const auto absent_cnt_live = parse(make_absent_fixture(AbsentForm::ZeroCnt));
  const auto absent_cnt_copy = parse(make_absent_fixture(AbsentForm::ZeroCnt, 0x1000));
  const auto absent_next_live = parse(make_absent_fixture(AbsentForm::ZeroNext));
  const auto absent_next_copy = parse(make_absent_fixture(AbsentForm::ZeroNext, 0x1000));
  check(absent_cnt_live && absent_cnt_copy && absent_next_live && absent_next_copy &&
            metal_renderer::jak2_warp_texture_upload_plans_match(*absent_cnt_live,
                                                                 *absent_cnt_copy) &&
            metal_renderer::jak2_warp_texture_upload_plans_match(*absent_next_live,
                                                                 *absent_next_copy),
        "both relocated exact empty forms match their own semantics");
  check(metal_renderer::jak2_warp_texture_upload_plans_match(*absent_cnt_live, *absent_next_copy) &&
            metal_renderer::jak2_warp_texture_upload_plans_match(*absent_next_live,
                                                                 *absent_cnt_copy) &&
            absent_cnt_live->semantic_fingerprint == absent_next_live->semantic_fingerprint,
        "zero-CNT and boundary-linked zero-NEXT are equivalent absence semantics");
  check(!metal_renderer::jak2_warp_texture_upload_plans_match(*live, *absent_cnt_live),
        "present and absent plans cannot match");

  auto changed_direct_fixture = make_present_fixture(3, 0x1000);
  changed_direct_fixture.packet[changed_direct_fixture.direct_offsets[1] + 16] ^= 1;
  const auto changed_direct = parse(changed_direct_fixture);
  check(changed_direct &&
            !metal_renderer::jak2_warp_texture_upload_plans_match(*live, *changed_direct),
        "a changed inert DIRECT payload fails live/copy semantic matching");

  auto changed_reset_fixture = make_present_fixture(3, 0x1000);
  changed_reset_fixture.packet[changed_reset_fixture.tail_offset + 16] ^= 1;
  const auto changed_reset = parse(changed_reset_fixture);
  check(
      changed_reset && !metal_renderer::jak2_warp_texture_upload_plans_match(*live, *changed_reset),
      "a changed reset payload fails live/copy semantic matching");

  auto changed_page_fixture = make_present_fixture(3, 0x1000);
  changed_page_fixture.live[page_offset(1)] ^= 1;
  const auto changed_page = parse(changed_page_fixture);
  check(changed_page && !metal_renderer::jak2_warp_texture_upload_plans_match(*live, *changed_page),
        "a changed owned page-header byte fails live/copy semantic matching");

  auto changed_descriptor_fixture = make_present_fixture(3, 0x1000);
  put_u64(&changed_descriptor_fixture.packet, changed_descriptor_fixture.descriptor_offsets[1] + 16,
          kAlternatePageOffset);
  const auto changed_descriptor = parse(changed_descriptor_fixture);
  check(changed_descriptor &&
            !metal_renderer::jak2_warp_texture_upload_plans_match(*live, *changed_descriptor),
        "a changed valid ordinary descriptor fails live/copy semantic matching");
}

void test_unsupported_forms_fail_closed() {
  auto animator = make_present_fixture(1);
  put_tag(&animator.packet, animator.direct_offsets[0], kDmaCnt, 0, 0, kPcPortVif | 12, 0);
  check(!parse(animator), "a PC_PORT-12 animator form is rejected");

  auto mixed = make_present_fixture(1);
  put_tag(&mixed.packet, mixed.boundary_offsets[1], kDmaNext, 0, kExtraOffset, 0, 0);
  put_tag(&mixed.packet, kExtraOffset, kDmaCnt, 0, 0, kPcPortVif | 12, 0);
  put_tag(&mixed.packet, kExtraOffset + 16, kDmaNext, 0, mixed.tail_offset, 0, 0);
  check(!parse(mixed), "an ordinary-plus-animator mixed form is rejected");

  auto extra = make_present_fixture(1);
  put_tag(&extra.packet, extra.final_boundary_offset, kDmaNext, 0, kExtraOffset, 0, 0);
  put_tag(&extra.packet, kExtraOffset, kDmaNext, 0, bucket_offset(extra.chain_offset) + 16, 0, 0);
  check(!parse(extra), "an extra transfer after the reset is rejected");

  check(!parse(make_present_fixture(0)), "a reset with zero ordinary groups is rejected");
  check(!parse(make_present_fixture(8)), "an eighth ordinary group is rejected");

  const auto correct = make_present_fixture(1);
  check(!parse(correct, 315) && !parse(correct, 317),
        "bucket IDs other than exact TEX_ALL_WARP 316 are rejected");
}

void test_malformed_shapes_and_contents_fail_closed() {
  auto fixture = make_present_fixture(1);
  put_u32(&fixture.packet, fixture.boundary_offsets[0] + 12, 1);
  check(!parse(fixture), "a non-inert initial boundary is rejected");

  fixture = make_present_fixture(1);
  put_tag(&fixture.packet, fixture.direct_offsets[0], kDmaCnt, 3, 0, 0, kDirectVif | 2);
  check(!parse(fixture), "an incorrect setup QWC is rejected");

  fixture = make_present_fixture(1);
  put_u32(&fixture.packet, fixture.direct_offsets[0] + 8, kFlushaVif);
  check(!parse(fixture), "an incorrect setup VIF0 is rejected");

  fixture = make_present_fixture(1);
  put_u32(&fixture.packet, fixture.descriptor_offsets[0] + 8, kPcPortVif | 1);
  check(!parse(fixture), "a noncanonical descriptor PC_PORT opcode is rejected");

  fixture = make_present_fixture(1);
  put_u32(&fixture.packet, fixture.descriptor_offsets[0] + 12, 0);
  check(!parse(fixture), "a descriptor without vif1 equal to three is rejected");

  fixture = make_present_fixture(1);
  put_u64(&fixture.packet, fixture.descriptor_offsets[0] + 24, static_cast<u64>(-2));
  check(!parse(fixture), "an ordinary mode other than negative one is rejected");

  fixture = make_present_fixture(1);
  put_u64(&fixture.packet, fixture.descriptor_offsets[0] + 16, 0);
  check(!parse(fixture), "a null page pointer is rejected");

  fixture = make_present_fixture(1);
  put_u32(&fixture.live, page_offset(0) + 12, 0xffffffff);
  check(!parse(fixture), "a negative texture-page length is rejected");

  fixture = make_present_fixture(1);
  put_u32(&fixture.live, page_offset(0) + 12, 0x40000000);
  check(!parse(fixture), "an out-of-range texture pointer table is rejected");

  fixture = make_present_fixture(1);
  put_tag(&fixture.packet, fixture.boundary_offsets[1], kDmaCnt, 0, 0, 0, 0);
  check(!parse(fixture), "a non-NEXT group boundary is rejected");

  fixture = make_present_fixture(1);
  put_u32(&fixture.packet, fixture.tail_offset + 8, 0);
  check(!parse(fixture), "a reset without FLUSHA is rejected");

  fixture = make_present_fixture(1);
  put_u32(&fixture.packet, fixture.final_boundary_offset + 12, 1);
  check(!parse(fixture), "a non-inert final boundary is rejected");

  fixture = make_present_fixture(1);
  put_tag(&fixture.packet, fixture.boundary_offsets[0], kDmaNext, 0, fixture.boundary_offsets[0], 0,
          0);
  check(!parse(fixture), "a cyclic DMA link is rejected");

  fixture = make_present_fixture(1);
  put_tag(&fixture.packet, fixture.direct_offsets[0], kDmaCall, 0, fixture.tail_offset, 0, 0);
  check(!parse(fixture), "an unsupported DMA tag kind is rejected");

  fixture = make_absent_fixture(AbsentForm::ZeroCnt);
  put_u32(&fixture.packet, bucket_offset(fixture.chain_offset) + 12, 1);
  check(!parse(fixture), "an otherwise empty bucket with VIF state is rejected");

  fixture = make_absent_fixture(AbsentForm::ZeroNext);
  put_tag(&fixture.packet, bucket_offset(fixture.chain_offset), kDmaNext, 0, kExtraOffset, 0, 0);
  put_tag(&fixture.packet, kExtraOffset, kDmaCnt, 0, 0, 0, 0);
  check(!parse(fixture), "a zero-NEXT that does not link directly to the boundary is not absent");

  fixture = make_absent_fixture(AbsentForm::ZeroNext);
  put_tag(&fixture.packet, bucket_offset(fixture.chain_offset), kDmaNext, 1,
          bucket_offset(fixture.chain_offset) + 16, 0, 0);
  check(!parse(fixture), "a NEXT with payload cannot represent absence");

  fixture = make_present_fixture(1);
  check(!metal_renderer::plan_jak2_warp_texture_upload(
            fixture.packet.data(), fixture.packet.size(), fixture.chain_offset + 1,
            metal_renderer::kJak2WarpTextureUploadBucket, fixture.live.data(), fixture.live.size()),
        "an unaligned chain offset is rejected");
  check(!metal_renderer::plan_jak2_warp_texture_upload(
            fixture.packet.data(), fixture.packet.size(), fixture.chain_offset,
            metal_renderer::kJak2WarpTextureUploadBucket, nullptr, 0),
        "a present upload without live EE memory is rejected");
  check(!metal_renderer::plan_jak2_warp_texture_upload(
            nullptr, fixture.packet.size(), fixture.chain_offset,
            metal_renderer::kJak2WarpTextureUploadBucket, fixture.live.data(), fixture.live.size()),
        "a null DMA snapshot is rejected");
  check(!metal_renderer::plan_jak2_warp_texture_upload(
            fixture.packet.data(), bucket_offset(fixture.chain_offset) + 15, fixture.chain_offset,
            metal_renderer::kJak2WarpTextureUploadBucket, fixture.live.data(), fixture.live.size()),
        "a snapshot truncated inside the bucket entry is rejected");

  fixture = make_present_fixture(1);
  put_tag(&fixture.packet, fixture.boundary_offsets[0], kDmaNext, 0, kMemorySize + 16, 0, 0);
  check(!parse(fixture), "an out-of-range DMA link is rejected");
}

}  // namespace

int main() {
  test_exact_present_and_absent_envelopes();
  test_live_and_copied_semantic_match();
  test_unsupported_forms_fail_closed();
  test_malformed_shapes_and_contents_fail_closed();
  std::puts("PASS: Jak 2 warp bucket-316 typed upload plan");
  return 0;
}
