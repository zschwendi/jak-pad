#include "game/graphics/pipelines/metal/metal_jak2_gmerc_warp_bucket317_plan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/dma/dma.h"

namespace {

constexpr u32 kChainOffset = 0x100;
constexpr u32 kDataOffset = 0x4000;
constexpr std::size_t kMemorySize = 0x40000;

using Plan = metal_renderer::Jak2GmercWarpBucket317Plan;
using Variant = metal_renderer::Jak2GmercWarpBucket317Variant;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

u32 vif(VifCode::Kind kind, u16 immediate = 0, u8 num = 0) {
  return (static_cast<u32>(kind) << 24) | (static_cast<u32>(num) << 16) | immediate;
}

u32 vif_stcycl(u16 cl, u16 wl) {
  return vif(VifCode::Kind::STCYCL, cl | (wl << 8));
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
  put_u64(memory, offset, static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                              (static_cast<u64>(address) << 32));
  put_u32(memory, offset + 8, vif0);
  put_u32(memory, offset + 12, vif1);
}

void append_u32(std::vector<u8>* bytes, u32 value) {
  const auto offset = bytes->size();
  bytes->resize(offset + 4);
  std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

std::vector<u8> make_fragment() {
  // Seven-qword Generic2 header and one five-qword adgif.
  std::vector<u8> bytes(192, 0);
  append_u32(&bytes, vif_stcycl(3, 1));
  append_u32(&bytes, vif(VifCode::Kind::UNPACK_V3_32, 0, 4));
  bytes.resize(bytes.size() + 4 * 12, 0x21);
  append_u32(&bytes, vif(VifCode::Kind::UNPACK_V4_8, 0, 4));
  bytes.resize(bytes.size() + 4 * 4, 0x43);
  append_u32(&bytes, vif(VifCode::Kind::UNPACK_V2_16, 0, 4));
  bytes.resize(bytes.size() + 4 * 4, 0x65);
  append_u32(&bytes, vif_stcycl(4, 4));
  append_u32(&bytes, vif(VifCode::Kind::MSCAL, 0x24));
  while (bytes.size() % 16) {
    append_u32(&bytes, 0);
  }
  return bytes;
}

std::vector<u8> make_two_fragment_transfer(u8 continued_vertices = 4) {
  auto bytes = make_fragment();
  append_u32(&bytes, vif_stcycl(4, 4));
  append_u32(&bytes, vif(VifCode::Kind::UNPACK_V4_32, 0, 12));
  while (bytes.size() % 16) {
    append_u32(&bytes, 0);
  }
  bytes.resize(bytes.size() + 192, 0);
  append_u32(&bytes, vif_stcycl(3, 1));
  append_u32(&bytes, vif(VifCode::Kind::UNPACK_V4_8, 0, continued_vertices));
  bytes.resize(bytes.size() + continued_vertices * 4, 0x87);
  append_u32(&bytes, vif(VifCode::Kind::UNPACK_V2_16, 0, continued_vertices));
  bytes.resize(bytes.size() + continued_vertices * 4, 0xa9);
  append_u32(&bytes, vif_stcycl(4, 4));
  append_u32(&bytes, vif(VifCode::Kind::MSCAL, 0x24));
  while (bytes.size() % 16) {
    append_u32(&bytes, 0);
  }
  return bytes;
}

struct Fixture {
  std::vector<u8> memory;
  u32 chain_offset = 0;
  u32 data_offset = 0;
  u32 constants_payload_offset = 0;
  u32 fragment_payload_offset = 0;
  u32 final_tag_offset = 0;
};

u32 bucket_offset(u32 chain_offset) {
  return chain_offset + metal_renderer::kJak2GmercWarpBucket * 16;
}

Fixture make_chain(u32 fragments,
                   bool short_setup = false,
                   u32 chain_offset = kChainOffset,
                   u32 data_offset = kDataOffset,
                   u32 leading_nops = 1,
                   u8 continued_vertices = 4,
                   u32 continued_position_bytes = 0) {
  Fixture fixture{std::vector<u8>(kMemorySize), chain_offset, data_offset};
  const u32 next_bucket = chain_offset + (metal_renderer::kJak2GmercWarpBucket + 1) * 16;
  put_tag(&fixture.memory, bucket_offset(chain_offset), DmaTag::Kind::NEXT, 0, data_offset,
          short_setup ? 0 : vif(VifCode::Kind::MARK), 0);
  u32 cursor = data_offset;
  auto append = [&](const std::vector<u8>& payload, u32 vif0, u32 vif1,
                    DmaTag::Kind kind = DmaTag::Kind::CNT, u32 address = 0) {
    put_tag(&fixture.memory, cursor, kind, static_cast<u16>(payload.size() / 16), address, vif0,
            vif1);
    if (!payload.empty()) {
      std::memcpy(fixture.memory.data() + cursor + 16, payload.data(), payload.size());
    }
    const u32 tag_offset = cursor;
    cursor += 16 + static_cast<u32>(payload.size());
    return tag_offset;
  };

  append(std::vector<u8>(32, 0x12), 0, vif(VifCode::Kind::DIRECT, 2));
  const u32 constants_tag =
      append(std::vector<u8>(128, 0x34), vif_stcycl(4, 4),
             vif(VifCode::Kind::UNPACK_V4_32, 0x381, 8));
  fixture.constants_payload_offset = constants_tag + 16;
  if (short_setup) {
    append(std::vector<u8>(32, 0x56), vif(VifCode::Kind::MSCALF),
           vif(VifCode::Kind::STMOD), DmaTag::Kind::NEXT, next_bucket);
    return fixture;
  }
  append(std::vector<u8>(32, 0x56), vif(VifCode::Kind::MSCALF),
         vif(VifCode::Kind::STMOD));
  for (u32 i = 0; i < leading_nops; ++i) {
    append({}, 0, 0);
  }
  if (fragments == 1) {
    const auto fragment = make_fragment();
    const u32 tag = append(fragment, vif_stcycl(4, 4),
                           vif(VifCode::Kind::UNPACK_V4_32, 0x8000, 12));
    fixture.fragment_payload_offset = tag + 16;
  } else if (fragments == 2) {
    const auto combined = make_two_fragment_transfer(continued_vertices);
    const u32 tag = append(combined, vif_stcycl(4, 4),
                           vif(VifCode::Kind::UNPACK_V4_32, 0x8000, 12));
    fixture.fragment_payload_offset = tag + 16;
    if (continued_position_bytes == 0) {
      continued_position_bytes = (continued_vertices * 12 + 15) & ~15u;
    }
    append(std::vector<u8>(continued_position_bytes, 0xbc), 0,
           vif(VifCode::Kind::UNPACK_V3_32, 0, continued_vertices));
    append({}, 0, vif(VifCode::Kind::MSCAL, 0x24));
  }
  append(std::vector<u8>(160, 0xde), vif(VifCode::Kind::FLUSHA),
         vif(VifCode::Kind::DIRECT, 10));
  fixture.final_tag_offset = cursor;
  append({}, 0, 0, DmaTag::Kind::NEXT, next_bucket);
  return fixture;
}

std::optional<Plan> plan(const Fixture& fixture,
                         u32 bucket_id = metal_renderer::kJak2GmercWarpBucket) {
  return metal_renderer::plan_jak2_gmerc_warp_bucket317(
      fixture.memory.data(), fixture.memory.size(), fixture.chain_offset, bucket_id);
}

void test_absent_and_setup_only() {
  Fixture absent{std::vector<u8>(kMemorySize), kChainOffset, kDataOffset};
  put_tag(&absent.memory, bucket_offset(kChainOffset), DmaTag::Kind::CNT, 0, 0, 0, 0);
  const auto absent_plan = plan(absent);
  check(absent_plan && absent_plan->variant == Variant::Absent &&
            absent_plan->transfer_count == 1 && absent_plan->payload_bytes == 0 &&
            absent_plan->semantic_fingerprint != 0,
        "the synthetic zero-CNT empty bucket is classified as absent");

  Fixture source_absent{std::vector<u8>(kMemorySize), kChainOffset, kDataOffset};
  const u32 next_bucket = kChainOffset + (metal_renderer::kJak2GmercWarpBucket + 1) * 16;
  put_tag(&source_absent.memory, bucket_offset(kChainOffset), DmaTag::Kind::NEXT, 0, next_bucket,
          0, 0);
  const auto source_absent_plan = plan(source_absent);
  check(source_absent_plan && source_absent_plan->variant == Variant::Absent &&
            source_absent_plan->transfer_count == 1 && source_absent_plan->payload_bytes == 0,
        "the source-shaped zero-NEXT empty bucket is classified as absent");

  const auto short_plan = plan(make_chain(0, true));
  check(short_plan && short_plan->variant == Variant::SetupOnly &&
            short_plan->transfer_count == 4 && short_plan->payload_bytes == 192,
        "the source short setup-only envelope is classified without execution");

  const auto terminated_plan = plan(make_chain(0));
  check(terminated_plan && terminated_plan->variant == Variant::SetupOnly &&
            terminated_plan->transfer_count == 7 && terminated_plan->payload_bytes == 352 &&
            terminated_plan->fragment_count == 0,
        "the setup-plus-terminator envelope is classified separately from fragments");
}

void test_fragments_and_relocation() {
  const auto one_fixture = make_chain(1);
  const auto one = plan(one_fixture);
  check(one && one->variant == Variant::Fragments && one->transfer_count == 8 &&
            one->fragment_count == 1 && one->continued_fragment_count == 0 &&
            one->vertex_count == 4 && one->adgif_count == 1 && one->payload_bytes == 656,
        "one source-shaped Generic2 fragment produces bounded typed counts");

  const auto two = plan(make_chain(2));
  check(two && two->variant == Variant::Fragments && two->transfer_count == 10 &&
            two->fragment_count == 2 && two->continued_fragment_count == 1 &&
            two->vertex_count == 8 && two->adgif_count == 2 && two->payload_bytes == 976,
        "the source parser's continued-fragment form is captured without drawing");

  const auto one_vertex_continued = plan(make_chain(2, false, kChainOffset, kDataOffset, 1, 1));
  check(one_vertex_continued && one_vertex_continued->variant == Variant::Fragments &&
            one_vertex_continued->fragment_count == 2 &&
            one_vertex_continued->continued_fragment_count == 1 &&
            one_vertex_continued->vertex_count == 5,
        "a one-vertex continued fragment accepts its 16-byte DMA-qword payload");

  const auto two_vertex_continued =
      plan(make_chain(2, false, kChainOffset, kDataOffset, 1, 2));
  check(two_vertex_continued && two_vertex_continued->variant == Variant::Fragments &&
            two_vertex_continued->fragment_count == 2 &&
            two_vertex_continued->continued_fragment_count == 1 &&
            two_vertex_continued->vertex_count == 6,
        "a two-vertex continued fragment accepts its 32-byte DMA-qword payload");

  const auto relocated_fixture = make_chain(1, false, kChainOffset + 0x1000,
                                             kDataOffset + 0x8000);
  const auto relocated = plan(relocated_fixture);
  check(relocated && metal_renderer::jak2_gmerc_warp_bucket317_plans_match(*one, *relocated),
        "relocated live and copied plans match by owned semantics");

  auto changed_fixture = relocated_fixture;
  changed_fixture.memory[changed_fixture.constants_payload_offset + 7] ^= 1;
  const auto changed = plan(changed_fixture);
  check(changed && changed->semantic_fingerprint != one->semantic_fingerprint &&
            !metal_renderer::jak2_gmerc_warp_bucket317_plans_match(*one, *changed),
        "a non-structural payload mutation invalidates live/copy semantic equality");
}

void test_malformed_rejections() {
  check(!plan(make_chain(1), metal_renderer::kJak2GmercWarpBucket - 1),
        "every other bucket ID is rejected");

  auto cycle = make_chain(1);
  put_u64(&cycle.memory, bucket_offset(cycle.chain_offset),
          static_cast<u64>(DmaTag::Kind::NEXT) << 28 |
              (static_cast<u64>(bucket_offset(cycle.chain_offset)) << 32));
  check(!plan(cycle), "a cyclic DMA chain fails closed");

  auto wrong_setup = make_chain(1);
  put_u32(&wrong_setup.memory, wrong_setup.data_offset + 12, vif(VifCode::Kind::NOP));
  check(!plan(wrong_setup), "an unknown fixed-setup VIF shape fails closed");

  auto wrong_fragment = make_chain(1);
  put_u32(&wrong_fragment.memory, wrong_fragment.fragment_payload_offset + 192,
          vif_stcycl(4, 4));
  check(!plan(wrong_fragment), "a fragment with the wrong vertex STCYCL fails closed");

  check(!plan(make_chain(2, false, kChainOffset, kDataOffset, 1, 1, 32)),
        "a continued-position payload with the wrong floor-divided vertex count fails closed");

  auto trailing = make_chain(1);
  const u32 next_bucket =
      trailing.chain_offset + (metal_renderer::kJak2GmercWarpBucket + 1) * 16;
  put_tag(&trailing.memory, trailing.final_tag_offset, DmaTag::Kind::CNT, 0, 0, 0, 0);
  put_tag(&trailing.memory, trailing.final_tag_offset + 16, DmaTag::Kind::NEXT, 0, next_bucket,
          0, 0);
  check(!plan(trailing), "a transfer after the final boundary NOP is rejected");

  check(!plan(make_chain(0, false, kChainOffset, kDataOffset,
                         metal_renderer::kJak2GmercWarpMaximumTransfers)),
        "the passive transfer budget fails closed before retaining unbounded input");
}

void test_source_capacity_boundaries() {
  check(metal_renderer::kJak2GmercWarpMaximumVertices == 499999,
        "the passive vertex bound preserves the renderers' strict less-than-500000 contract");
}

}  // namespace

int main() {
  test_absent_and_setup_only();
  test_fragments_and_relocation();
  test_malformed_rejections();
  test_source_capacity_boundaries();
  std::puts("jak2 GMERC_WARP bucket-317 plan tests passed");
  return 0;
}
