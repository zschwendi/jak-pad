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
constexpr u32 kSecondGroupOffset = 0x2800;
constexpr u32 kTailOffset = 0x3000;
constexpr u32 kFirstPageOffset = 0x6000;
constexpr u32 kSecondPageOffset = 0x7000;
constexpr std::size_t kMemorySize = 0x10000;
constexpr u32 kDirectVif = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
constexpr u32 kPcPortVif = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
constexpr u32 kFlushaVif = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;

struct Fixture {
  std::vector<u8> packet = std::vector<u8>(kMemorySize);
  std::vector<u8> live = std::vector<u8>(kMemorySize);
  std::array<u32, 2> direct_tag_offsets = {};
  std::array<u32, 2> descriptor_tag_offsets = {};
  std::array<u32, 3> group_boundary_offsets = {};
  u32 tail_tag_offset = 0;
  u32 final_boundary_offset = 0;
};

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

Fixture make_fixture(u32 upload_count) {
  check(upload_count == 1 || upload_count == 2,
        "the synthetic fixture supports one or two uploads");
  Fixture fixture;
  put_page_header(&fixture.live, kFirstPageOffset, 0x101, 2);
  put_page_header(&fixture.live, kSecondPageOffset, 0x202, 3);

  const u32 bucket_offset = kChainOffset + metal_renderer::kJak2SpriteTextureUploadBucket * 16;
  fixture.group_boundary_offsets[0] = bucket_offset;
  put_tag(&fixture.packet, bucket_offset, DmaTag::Kind::NEXT, 0, kFirstGroupOffset, 0, 0);

  if (upload_count == 1) {
    put_upload_group(&fixture, kFirstGroupOffset, 0, kFirstPageOffset, kTailOffset);
  } else {
    put_upload_group(&fixture, kFirstGroupOffset, 0, kFirstPageOffset, kSecondGroupOffset);
    put_upload_group(&fixture, kSecondGroupOffset, 1, kSecondPageOffset, kTailOffset);
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

void test_exact_one_and_two_upload_grammars() {
  for (const u32 upload_count : {1u, 2u}) {
    auto fixture = make_fixture(upload_count);
    const auto result = plan(fixture);
    check(result.has_value() && result->present && result->upload_count == upload_count,
          "the exact one- and two-upload live forms produce matching plans");
    check(result->uploads[0].page_offset == kFirstPageOffset && result->uploads[0].mode == -1 &&
              result->uploads[0].page_header[8] == 0x01 &&
              result->uploads[0].page_header[9] == 0x01 && result->uploads[0].page_header[12] == 2,
          "the first ordered ordinary descriptor and page header are owned");
    if (upload_count == 2) {
      check(result->uploads[1].page_offset == kSecondPageOffset && result->uploads[1].mode == -1 &&
                result->uploads[1].page_header[8] == 0x02 &&
                result->uploads[1].page_header[9] == 0x02 &&
                result->uploads[1].page_header[12] == 3,
            "the second ordered ordinary descriptor and page header are owned");
    }

    const auto expected_first_header = result->uploads[0].page_header;
    const auto expected_second_header = result->uploads[1].page_header;
    std::fill(fixture.packet.begin(), fixture.packet.end(), 0xa5);
    std::fill(fixture.live.begin(), fixture.live.end(), 0x5a);
    check(result->uploads[0].page_header == expected_first_header &&
              result->uploads[1].page_header == expected_second_header,
          "packet and live-memory reuse cannot change the owning plan");
  }
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
  for (const u32 offset : fixture.direct_tag_offsets) {
    std::fill_n(fixture.packet.begin() + offset + 16, 32, 0xff);
  }
  std::fill_n(fixture.packet.begin() + fixture.tail_tag_offset + 16, 160, 0x00);
  check(plan(fixture).has_value(),
        "Direct payload bytes stay uninterpreted when the GL handler has add_direct disabled");
}

void test_packet_and_live_domains_are_separate() {
  auto fixture = make_fixture(2);
  std::fill_n(fixture.packet.begin() + kFirstPageOffset,
              metal_renderer::kJak2Bucket4OrdinaryPageHeaderBytes, 0xee);
  std::fill_n(fixture.packet.begin() + kSecondPageOffset,
              metal_renderer::kJak2Bucket4OrdinaryPageHeaderBytes, 0xee);
  std::fill_n(fixture.live.begin() + fixture.group_boundary_offsets[0], 16, 0xdd);
  std::fill_n(fixture.live.begin() + kFirstGroupOffset, 96, 0xdd);
  std::fill_n(fixture.live.begin() + kSecondGroupOffset, 96, 0xdd);
  std::fill_n(fixture.live.begin() + kTailOffset, 192, 0xdd);
  const auto result = plan(fixture);
  check(result.has_value() && result->uploads[0].page_header[8] == 0x01 &&
            result->uploads[1].page_header[8] == 0x02,
        "DMA comes from the snapshot while page headers come from live EE memory");
}

void test_zero_or_more_than_two_groups_fail_closed() {
  auto zero = make_fixture(1);
  put_tag(&zero.packet, zero.direct_tag_offsets[0], DmaTag::Kind::CNT, 10, 0, kFlushaVif,
          kDirectVif | 10);
  check(!plan(zero).has_value(), "a tail without an ordinary upload group is rejected");

  auto three = make_fixture(2);
  constexpr u32 kThirdGroupOffset = 0x3800;
  put_tag(&three.packet, three.group_boundary_offsets[2], DmaTag::Kind::NEXT, 0, kThirdGroupOffset,
          0, 0);
  put_tag(&three.packet, kThirdGroupOffset, DmaTag::Kind::CNT, 2, 0, 0, kDirectVif | 2);
  put_tag(&three.packet, kThirdGroupOffset + 48, DmaTag::Kind::CNT, 1, 0, kPcPortVif, 3);
  put_u64(&three.packet, kThirdGroupOffset + 64, kFirstPageOffset);
  put_u64(&three.packet, kThirdGroupOffset + 72, static_cast<u64>(-1));
  put_tag(&three.packet, kThirdGroupOffset + 80, DmaTag::Kind::NEXT, 0, kTailOffset, 0, 0);
  check(!plan(three).has_value(), "a third ordinary upload group is rejected");
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
  test_exact_one_and_two_upload_grammars();
  test_strict_empty_bucket_is_absent();
  test_direct_payloads_are_inert();
  test_packet_and_live_domains_are_separate();
  test_zero_or_more_than_two_groups_fail_closed();
  test_malformed_transfer_shapes_fail_closed();
  test_bad_dma_and_page_ranges_fail_closed();
  std::puts("PASS: Jak II TEX_ALL_SPRITE texture-upload plan");
  return 0;
}
