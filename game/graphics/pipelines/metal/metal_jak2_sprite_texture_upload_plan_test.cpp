#include "game/graphics/pipelines/metal/metal_jak2_sprite_texture_upload_plan.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/dma/dma.h"

namespace {

constexpr u32 kChainOffset = 0x100;
constexpr u32 kFirstGroupOffset = 0x2000;
constexpr u32 kGroupStride = 0x100;
constexpr u32 kTailOffset = 0x3000;
constexpr u32 kFirstPageOffset = 0x6000;
constexpr u32 kPageStride = 0x200;
constexpr std::size_t kMemorySize = 0x10000;
constexpr std::size_t kFixtureMaximumGroups =
    metal_renderer::kJak2MapTextureUploadMaximumGroups + 1;
constexpr u32 kDirectVif = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
constexpr u32 kPcPortVif = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
constexpr u32 kFlushaVif = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;

struct Fixture {
  std::vector<u8> packet = std::vector<u8>(kMemorySize);
  std::vector<u8> live = std::vector<u8>(kMemorySize);
  std::array<u32, kFixtureMaximumGroups> direct_tag_offsets = {};
  std::array<u32, kFixtureMaximumGroups> descriptor_tag_offsets = {};
  std::array<u32, kFixtureMaximumGroups + 1> group_boundary_offsets = {};
  u32 upload_count = 0;
  u32 tail_tag_offset = 0;
  u32 final_boundary_offset = 0;
};

constexpr u32 group_offset(u32 index) {
  return kFirstGroupOffset + index * kGroupStride;
}

constexpr u32 page_offset(u32 index) {
  return kFirstPageOffset + index * kPageStride;
}

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
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
             u32 vif1) {
  const u64 tag =
      static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) | (static_cast<u64>(address) << 32);
  put_u64(memory, offset, tag);
  put_u32(memory, offset + 8, vif0);
  put_u32(memory, offset + 12, vif1);
}

void put_page_header(std::vector<u8>* memory, u32 page_offset, u32 id, s32 length = 0) {
  put_u32(memory, page_offset + 8, id);
  put_u32(memory, page_offset + 12, static_cast<u32>(length));
  for (u32 i = 0; i < metal_renderer::kJak2Bucket4OrdinaryPageHeaderBytes; ++i) {
    if (i != 8 && i != 9 && i != 10 && i != 11 && i != 12 && i != 13 && i != 14 && i != 15) {
      (*memory)[page_offset + i] = static_cast<u8>((id * 17 + i) & 0xff);
    }
  }
}

u32 put_upload_group(Fixture* fixture, u32 offset, u32 index, u32 page_offset, u32 next_offset) {
  fixture->direct_tag_offsets[index] = offset;
  put_tag(&fixture->packet, offset, DmaTag::Kind::CNT, 2, 0, 0, kDirectVif | 2);
  std::fill_n(fixture->packet.begin() + offset + 16, 32, static_cast<u8>(0x40 + index));
  offset += 48;

  fixture->descriptor_tag_offsets[index] = offset;
  put_tag(&fixture->packet, offset, DmaTag::Kind::CNT, 1, 0, kPcPortVif, 3);
  put_u64(&fixture->packet, offset + 16, page_offset);
  put_u64(&fixture->packet, offset + 24, static_cast<u64>(-1));
  offset += 32;

  fixture->group_boundary_offsets[index + 1] = offset;
  put_tag(&fixture->packet, offset, DmaTag::Kind::NEXT, 0, next_offset, 0, 0);
  return offset;
}

Fixture make_fixture(
    u32 upload_count,
    u32 bucket_id = metal_renderer::kJak2SpriteTextureUploadBucket) {
  check(upload_count <= kFixtureMaximumGroups,
        "the synthetic fixture stays within its bounded upload storage");
  Fixture fixture;
  fixture.upload_count = upload_count;
  for (u32 i = 0; i < upload_count; ++i) {
    put_page_header(&fixture.live, page_offset(i), 0x101 + i, 2 + i);
  }

  const u32 bucket_offset = kChainOffset + bucket_id * 16;
  fixture.group_boundary_offsets[0] = bucket_offset;
  put_tag(&fixture.packet, bucket_offset, DmaTag::Kind::NEXT, 0,
          upload_count == 0 ? kTailOffset : group_offset(0), 0, 0);

  for (u32 i = 0; i < upload_count; ++i) {
    const u32 next_offset = i + 1 == upload_count ? kTailOffset : group_offset(i + 1);
    put_upload_group(&fixture, group_offset(i), i, page_offset(i), next_offset);
  }

  fixture.tail_tag_offset = kTailOffset;
  put_tag(&fixture.packet, kTailOffset, DmaTag::Kind::CNT, 10, 0, kFlushaVif, kDirectVif | 10);
  std::fill_n(fixture.packet.begin() + kTailOffset + 16, 160, 0x9a);
  fixture.final_boundary_offset = kTailOffset + 176;
  const u32 bucket_end = bucket_offset + 16;
  put_tag(&fixture.packet, fixture.final_boundary_offset, DmaTag::Kind::NEXT, 0, bucket_end, 0, 0);
  return fixture;
}

std::optional<metal_renderer::Jak2SpriteTextureUploadPlan> plan(const Fixture& fixture) {
  return metal_renderer::plan_jak2_sprite_texture_upload(fixture.packet.data(),
                                                         fixture.packet.size(), kChainOffset,
                                                         fixture.live.data(), fixture.live.size());
}

std::optional<metal_renderer::Jak2MapTextureUploadPlan> plan_map(
    const Fixture& fixture,
    metal_renderer::Jak2MapTextureUploadDiagnostic* diagnostic = nullptr) {
  return metal_renderer::plan_jak2_map_texture_upload(
      fixture.packet.data(), fixture.packet.size(), kChainOffset, fixture.live.data(),
      fixture.live.size(), diagnostic);
}

void test_source_bounded_upload_grammars() {
  for (const u32 upload_count : {1u, 2u, 3u, 7u}) {
    auto fixture = make_fixture(upload_count);
    const auto result = plan(fixture);
    check(result.has_value() && result->present && result->upload_count == upload_count,
          "the exact one-through-seven source-bounded forms produce matching plans");

    std::array<std::array<u8, metal_renderer::kJak2Bucket4OrdinaryPageHeaderBytes>,
               metal_renderer::kJak2SpriteTextureUploadMaximumGroups>
        expected_headers = {};
    for (u32 i = 0; i < upload_count; ++i) {
      check(result->uploads[i].page_offset == page_offset(i) && result->uploads[i].mode == -1 &&
                result->uploads[i].page_header[8] == static_cast<u8>(0x101 + i) &&
                result->uploads[i].page_header[12] == static_cast<u8>(2 + i),
            "each ordered ordinary descriptor and page header is owned");
      expected_headers[i] = result->uploads[i].page_header;
    }

    std::fill(fixture.packet.begin(), fixture.packet.end(), 0xa5);
    std::fill(fixture.live.begin(), fixture.live.end(), 0x5a);
    for (u32 i = 0; i < upload_count; ++i) {
      check(result->uploads[i].page_header == expected_headers[i],
            "packet and live-memory reuse cannot change the owning plan");
    }
  }
}

void test_map_source_bounded_upload_grammars() {
  for (const u32 upload_count : {1u, 2u, 3u, 7u, 8u}) {
    auto fixture = make_fixture(upload_count, metal_renderer::kJak2MapTextureUploadBucket);
    const auto result = plan_map(fixture);
    check(result.has_value() && result->present && result->upload_count == upload_count,
          "the exact one-through-eight map forms produce matching plans");
    for (u32 i = 0; i < upload_count; ++i) {
      check(result->uploads[i].page_offset == page_offset(i) && result->uploads[i].mode == -1,
            "each ordered map descriptor is retained");
    }
  }

  const auto nine = make_fixture(9, metal_renderer::kJak2MapTextureUploadBucket);
  check(!plan_map(nine).has_value(), "a ninth map upload group is rejected");
}

void test_map_rejection_diagnostic() {
  auto fixture = make_fixture(1, metal_renderer::kJak2MapTextureUploadBucket);
  put_tag(&fixture.packet, fixture.direct_tag_offsets[0], DmaTag::Kind::CNT, 3, 0, 0,
          kDirectVif | 3);
  metal_renderer::Jak2MapTextureUploadDiagnostic diagnostic;
  check(!plan_map(fixture, &diagnostic).has_value(),
        "a non-group Direct transfer remains rejected without capture evidence");
  check(diagnostic.rejection_stage ==
                metal_renderer::Jak2MapTextureUploadRejectionStage::GroupOrTail &&
            diagnostic.failure_offset == fixture.direct_tag_offsets[0] &&
            diagnostic.transfer_count == 2 && diagnostic.rejected_transfer == 1,
        "the rejection identifies the exact parser stage and failing transfer");
  check(diagnostic.transfers[0].tag_offset == fixture.group_boundary_offsets[0] &&
            diagnostic.transfers[0].tag_kind == static_cast<u8>(DmaTag::Kind::NEXT) &&
            diagnostic.transfers[0].qwc == 0 && diagnostic.transfers[0].payload_bytes == 0 &&
            diagnostic.transfers[0].vif0 == 0 && diagnostic.transfers[0].vif1 == 0 &&
            !diagnostic.transfers[0].spr &&
            diagnostic.transfers[1].tag_offset == fixture.direct_tag_offsets[0] &&
            diagnostic.transfers[1].tag_kind == static_cast<u8>(DmaTag::Kind::CNT) &&
            diagnostic.transfers[1].qwc == 3 && diagnostic.transfers[1].payload_bytes == 48 &&
            diagnostic.transfers[1].vif0 == 0 &&
            diagnostic.transfers[1].vif1 == (kDirectVif | 3) &&
            !diagnostic.transfers[1].spr,
        "the bounded trace owns tag kinds, QWC, byte counts, and raw VIF codes");
  check(std::strcmp(metal_renderer::jak2_map_texture_upload_rejection_stage_name(
                        diagnostic.rejection_stage),
                    "group-or-tail") == 0,
        "the rejection stage has a stable log spelling");

  fixture = make_fixture(1, metal_renderer::kJak2MapTextureUploadBucket);
  put_u64(&fixture.packet, fixture.descriptor_tag_offsets[0] + 24, static_cast<u64>(-2));
  diagnostic = {};
  check(!plan_map(fixture, &diagnostic).has_value() &&
            diagnostic.rejection_stage ==
                metal_renderer::Jak2MapTextureUploadRejectionStage::OrdinaryContents &&
            diagnostic.failure_offset == fixture.descriptor_tag_offsets[0] &&
            diagnostic.transfer_count == 3 && diagnostic.rejected_transfer == 2,
        "a rejected descriptor records its later failure position deterministically");
}

void test_strict_empty_bucket_is_absent() {
  Fixture fixture;
  const u32 bucket_offset = kChainOffset + metal_renderer::kJak2SpriteTextureUploadBucket * 16;
  put_tag(&fixture.packet, bucket_offset, DmaTag::Kind::CNT, 0, 0, 0, 0);
  const auto result = metal_renderer::plan_jak2_sprite_texture_upload(
      fixture.packet.data(), fixture.packet.size(), kChainOffset, nullptr, 0);
  check(result.has_value() && !result->present && result->upload_count == 0,
        "a strict empty bucket-table entry produces an absent plan without live EE memory");

  put_u32(&fixture.packet, bucket_offset + 12, 1);
  check(!metal_renderer::plan_jak2_sprite_texture_upload(
             fixture.packet.data(), fixture.packet.size(), kChainOffset, nullptr, 0)
             .has_value(),
        "an otherwise empty bucket with nonzero VIF state is rejected");
}

void test_direct_payloads_are_inert() {
  auto fixture = make_fixture(2);
  for (u32 i = 0; i < fixture.upload_count; ++i) {
    std::fill_n(fixture.packet.begin() + fixture.direct_tag_offsets[i] + 16, 32, 0xff);
  }
  std::fill_n(fixture.packet.begin() + fixture.tail_tag_offset + 16, 160, 0x00);
  check(plan(fixture).has_value(),
        "Direct payload bytes stay uninterpreted when the GL handler has add_direct disabled");
}

void test_packet_and_live_domains_are_separate() {
  auto fixture = make_fixture(2);
  std::fill_n(fixture.packet.begin() + page_offset(0),
              metal_renderer::kJak2Bucket4OrdinaryPageHeaderBytes, 0xee);
  std::fill_n(fixture.packet.begin() + page_offset(1),
              metal_renderer::kJak2Bucket4OrdinaryPageHeaderBytes, 0xee);
  std::fill_n(fixture.live.begin() + fixture.group_boundary_offsets[0], 16, 0xdd);
  std::fill_n(fixture.live.begin() + group_offset(0), 96, 0xdd);
  std::fill_n(fixture.live.begin() + group_offset(1), 96, 0xdd);
  std::fill_n(fixture.live.begin() + kTailOffset, 192, 0xdd);
  const auto result = plan(fixture);
  check(result.has_value() && result->uploads[0].page_header[8] == 0x01 &&
            result->uploads[1].page_header[8] == 0x02,
        "DMA comes from the snapshot while page headers come from live EE memory");
}

void test_zero_or_more_than_seven_groups_fail_closed() {
  const auto zero = make_fixture(0);
  check(!plan(zero).has_value(), "a tail without an ordinary upload group is rejected");

  const auto eight = make_fixture(8);
  check(!plan(eight).has_value(), "an eighth ordinary upload group is rejected");
}

void test_malformed_transfer_shapes_fail_closed() {
  auto fixture = make_fixture(1);
  put_u32(&fixture.packet, fixture.group_boundary_offsets[0] + 12, 1);
  check(!plan(fixture).has_value(), "an initial NEXT with nonzero VIF state is rejected");

  fixture = make_fixture(1);
  put_u32(&fixture.packet, fixture.direct_tag_offsets[0] + 8, kFlushaVif);
  check(!plan(fixture).has_value(), "a DIRECT2 transfer with nonzero vif0 is rejected");

  fixture = make_fixture(1);
  put_u32(&fixture.packet, fixture.direct_tag_offsets[0] + 12, kDirectVif | 1);
  check(!plan(fixture).has_value(), "a DIRECT2 immediate mismatch is rejected");

  fixture = make_fixture(1);
  put_tag(&fixture.packet, fixture.direct_tag_offsets[0], DmaTag::Kind::CNT, 1, 0,
          kPcPortVif | 12, 0);
  check(!plan(fixture).has_value(), "texture-animator packets remain unsupported");

  fixture = make_fixture(1);
  put_tag(&fixture.packet, fixture.direct_tag_offsets[0], DmaTag::Kind::CNT, 8, 0, 0, 0);
  check(!plan(fixture).has_value(), "eye packets remain unsupported");

  fixture = make_fixture(1);
  put_tag(&fixture.packet, fixture.direct_tag_offsets[0], DmaTag::Kind::CALL, 0, kTailOffset, 0, 0);
  check(!plan(fixture).has_value(), "CALL packets remain unsupported");

  fixture = make_fixture(1);
  put_u32(&fixture.packet, fixture.descriptor_tag_offsets[0] + 8, kPcPortVif | 1);
  check(!plan(fixture).has_value(), "a noncanonical PC_PORT opcode is rejected");

  fixture = make_fixture(1);
  put_u32(&fixture.packet, fixture.descriptor_tag_offsets[0] + 12, 0);
  check(!plan(fixture).has_value(), "an ordinary descriptor without vif1 equal to 3 is rejected");

  fixture = make_fixture(1);
  put_tag(&fixture.packet, fixture.group_boundary_offsets[1], DmaTag::Kind::CNT, 0, 0, 0, 0);
  check(!plan(fixture).has_value(), "a group separator outside exact NEXT grammar is rejected");

  fixture = make_fixture(1);
  put_u32(&fixture.packet, fixture.tail_tag_offset + 8, 0);
  check(!plan(fixture).has_value(), "a DIRECT10 tail without FLUSHA in vif0 is rejected");

  fixture = make_fixture(1);
  put_u32(&fixture.packet, fixture.tail_tag_offset + 12, kDirectVif | 9);
  check(!plan(fixture).has_value(), "a DIRECT10 immediate mismatch is rejected");

  fixture = make_fixture(1);
  put_tag(&fixture.packet, fixture.tail_tag_offset, DmaTag::Kind::CNT, 9, 0, kFlushaVif,
          kDirectVif | 10);
  check(!plan(fixture).has_value(), "a DIRECT10 transfer with qwc other than 10 is rejected");

  fixture = make_fixture(1);
  put_tag(&fixture.packet, fixture.final_boundary_offset, DmaTag::Kind::CNT, 0, 0, 0, 0);
  check(!plan(fixture).has_value(), "a final boundary outside exact NEXT grammar is rejected");

  fixture = make_fixture(1);
  constexpr u32 kExtraBoundaryOffset = 0x3c00;
  const u32 bucket_end = kChainOffset + (metal_renderer::kJak2SpriteTextureUploadBucket + 1) * 16;
  put_tag(&fixture.packet, fixture.final_boundary_offset, DmaTag::Kind::NEXT, 0,
          kExtraBoundaryOffset, 0, 0);
  put_tag(&fixture.packet, kExtraBoundaryOffset, DmaTag::Kind::NEXT, 0, bucket_end, 0, 0);
  check(!plan(fixture).has_value(), "an extra final boundary cannot loosen the exact grammar");
}

void test_bad_dma_and_page_ranges_fail_closed() {
  auto fixture = make_fixture(1);
  const u32 bucket_offset = kChainOffset + metal_renderer::kJak2SpriteTextureUploadBucket * 16;
  put_tag(&fixture.packet, bucket_offset, DmaTag::Kind::NEXT, 0,
          static_cast<u32>(fixture.packet.size() + 16), 0, 0);
  check(!plan(fixture).has_value(), "an out-of-range NEXT is rejected before following it");

  fixture = make_fixture(1);
  put_tag(&fixture.packet, bucket_offset, DmaTag::Kind::NEXT, 0, 0, 0, 0);
  check(!plan(fixture).has_value(), "a null NEXT target is rejected before following it");

  fixture = make_fixture(1);
  put_tag(&fixture.packet, bucket_offset, DmaTag::Kind::NEXT, 0, bucket_offset, 0, 0);
  check(!plan(fixture).has_value(), "a DMA cycle is rejected");

  fixture = make_fixture(1);
  put_u64(&fixture.packet, fixture.descriptor_tag_offsets[0] + 16, 0);
  check(!plan(fixture).has_value(), "a null texture-page offset is rejected");

  fixture = make_fixture(1);
  put_u64(&fixture.packet, fixture.descriptor_tag_offsets[0] + 24, static_cast<u64>(-2));
  check(!plan(fixture).has_value(), "an upload mode other than the observed -1 is rejected");

  fixture = make_fixture(1);
  put_u32(&fixture.live, kFirstPageOffset + 12, static_cast<u32>(-1));
  check(!plan(fixture).has_value(), "a negative texture-page length is rejected");

  fixture = make_fixture(1);
  put_u32(&fixture.live, kFirstPageOffset + 12, 0x4000);
  check(!plan(fixture).has_value(), "an out-of-range texture pointer table is rejected");

  fixture = make_fixture(1);
  check(!metal_renderer::plan_jak2_sprite_texture_upload(fixture.packet.data(), bucket_offset + 15,
                                                         kChainOffset, fixture.live.data(),
                                                         fixture.live.size())
             .has_value(),
        "a snapshot shorter than the bucket entry is rejected");
  check(!metal_renderer::plan_jak2_sprite_texture_upload(
             fixture.packet.data(), fixture.packet.size(), kChainOffset, nullptr, 0)
             .has_value(),
        "missing live EE memory is rejected before copying a page header");
}

}  // namespace

int main() {
  test_source_bounded_upload_grammars();
  test_map_source_bounded_upload_grammars();
  test_map_rejection_diagnostic();
  test_strict_empty_bucket_is_absent();
  test_direct_payloads_are_inert();
  test_packet_and_live_domains_are_separate();
  test_zero_or_more_than_seven_groups_fail_closed();
  test_malformed_transfer_shapes_fail_closed();
  test_bad_dma_and_page_ranges_fail_closed();
  std::puts("PASS: Jak II grouped texture-upload plans");
  return 0;
}
