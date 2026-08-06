#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <variant>

#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_capture.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_fixture.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_plan.h"

namespace {

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

metal_renderer::Jak2Bucket4TextureUploadCapture capture(
    const metal_renderer::Jak2Bucket4TextureUploadFixture& fixture) {
  return metal_renderer::capture_jak2_bucket4_texture_upload(
      fixture.ee_memory.data(), fixture.ee_memory.size(), fixture.chain_offset);
}

void put_u32(std::vector<u8>* memory, u32 offset, u32 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_u64(std::vector<u8>* memory, u32 offset, u64 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void test_exact_observed_grammar() {
  const auto fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  const auto result = capture(fixture);
  check(result.valid && result.present, "the exact mixed packet is present and valid");
  check(result.total_payload_bytes == 416 && result.dma_transfers == 16 &&
            result.payload_transfers == 7,
        "the exact payload and transfer totals are retained");
  check(result.inert_transfers == 4 && result.inert_cnt_transfers == 0 &&
            result.inert_next_transfers == 4,
        "all four synthetic boundary NEXT transfers are classified as inert");
  const u32 expected_inert_states = (1u << 0) | (1u << 2) | (1u << 5) | (1u << 12);
  check(result.inert_state_mask == expected_inert_states,
        "inert boundary provenance records its four parser states");
  check(result.ordinary_descriptors == 1 && result.ordinary_page == 0x6000 &&
            result.ordinary_mode == -1,
        "the ordinary page descriptor scalars are captured");
  check(result.animator_arrays == 2 && result.animator_bytes == 368 &&
            result.opcode_counts[12] == 2 && result.opcode_counts[13] == 2 &&
            result.opcode_counts[14] == 1 && result.opcode_counts[15] == 1 &&
            result.opcode_counts[16] == 1 && result.opcode_counts[41] == 1,
        "the two animator arrays and exact opcode counts are captured");
  check(result.cloud_destination == 0x1234, "the cloud destination is captured");
  check(result.erase_width == 16 && result.erase_height == 16 &&
            result.erase_destination == 0x1200 && result.erase_test == 0x11 &&
            result.erase_alpha == 0x22 && result.erase_clamp == 0x1 &&
            result.erase_clear[0] == 17 &&
            result.erase_clear[1] == 34 && result.erase_clear[2] == 51 &&
            result.erase_clear[3] == 68,
        "the erase target, dimensions, and clear state are captured");
  check(result.generic_source == 0x8000 && result.generic_width == 256 &&
            result.generic_height == 1 && result.generic_destination == 0x1300 &&
            result.generic_format == 19 && result.generic_force_to_gpu == 1,
        "the generic upload descriptor scalars are captured");
  check(result.clut_source == 0xa000 && result.clut_destination == 0x1200 &&
            result.finishes == 2,
        "the CLUT upload and both array finishes are captured");
  check(result.malformed_transfers == 0 && result.unsupported_transfers == 0,
        "the exact grammar has no malformed or unsupported payload");
}

void test_exact_ordinary_only_grammar() {
  const auto fixture =
      metal_renderer::make_jak2_bucket4_ordinary_only_texture_upload_fixture();
  const auto result = capture(fixture);
  check(result.valid && result.present,
        "the exact ordinary-only title packet is present and valid");
  check(result.total_payload_bytes == 48 && result.dma_transfers == 4 &&
            result.payload_transfers == 2 && result.inert_transfers == 2 &&
            result.inert_cnt_transfers == 0 && result.inert_next_transfers == 2 &&
            result.inert_state_mask == ((1u << 0) | (1u << 2)),
        "the ordinary-only variant retains its exact transfer and boundary grammar");
  check(result.ordinary_descriptors == 1 && result.ordinary_page == 0x6000 &&
            result.ordinary_mode == -1 && result.animator_arrays == 0 &&
            result.animator_bytes == 0 && result.finishes == 0,
        "the ordinary-only variant captures one page descriptor and no animator work");
  for (const u32 count : result.opcode_counts) {
    check(count == 0, "the ordinary-only variant captures no animator opcodes");
  }

  auto near_miss =
      metal_renderer::make_jak2_bucket4_ordinary_only_texture_upload_fixture();
  const u32 closing_boundary = near_miss.ordinary_descriptor_data_offset + 16;
  const u32 extra_boundary = near_miss.chain_offset + 0x1800;
  const u32 bucket_end = near_miss.chain_offset + 5 * 16;
  const u64 next_to_extra = (static_cast<u64>(DmaTag::Kind::NEXT) << 28) |
                            (static_cast<u64>(extra_boundary) << 32);
  const u64 next_to_end = (static_cast<u64>(DmaTag::Kind::NEXT) << 28) |
                          (static_cast<u64>(bucket_end) << 32);
  put_u64(&near_miss.ee_memory, closing_boundary, next_to_extra);
  put_u64(&near_miss.ee_memory, extra_boundary, next_to_end);
  const auto near_miss_result = capture(near_miss);
  check(!near_miss_result.valid && near_miss_result.present &&
            near_miss_result.dma_transfers == 5 && near_miss_result.inert_transfers == 3,
        "an extra ordinary-only boundary transfer cannot loosen the exact variant");
}

void test_missing_finish_fails_closed() {
  auto fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  put_u32(&fixture.ee_memory, fixture.first_finish_tag_offset + 8, 0);
  const auto result = capture(fixture);
  check(!result.valid && result.present && result.finishes == 0,
        "a missing first finish cannot be accepted as a complete array");
  check(result.unsupported_transfers == 1,
        "the first later semantic transfer is retained as unsupported");
}

void test_bad_direct_fails_closed() {
  auto fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  put_u32(&fixture.ee_memory, fixture.erase_setup_tag_offset + 12,
          (static_cast<u32>(VifCode::Kind::DIRECT) << 24) | 9);
  const auto result = capture(fixture);
  check(!result.valid && result.malformed_transfers == 1 && result.malformed_bytes == 160,
        "an erase setup with the wrong DIRECT size is malformed");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  put_u32(&fixture.ee_memory, fixture.erase_clear_tag_offset + 12,
          (static_cast<u32>(VifCode::Kind::DIRECT) << 24) | 3);
  const auto clear_result = capture(fixture);
  check(!clear_result.valid && clear_result.malformed_bytes == 64,
        "an erase clear with the wrong DIRECT size is malformed");
}

void test_bad_fixed_packet_state_fails_closed() {
  auto fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  const u64 bad_outer_gif = 2;
  std::memcpy(fixture.ee_memory.data() + fixture.outer_direct_tag_offset + 16, &bad_outer_gif,
              sizeof(bad_outer_gif));
  const auto outer_result = capture(fixture);
  check(!outer_result.valid && outer_result.malformed_bytes == 32,
        "a noncanonical outer TEXFLUSH GIF tag is rejected");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  const s64 bad_mode = 0;
  std::memcpy(fixture.ee_memory.data() + fixture.ordinary_descriptor_data_offset + 8, &bad_mode,
              sizeof(bad_mode));
  const auto mode_result = capture(fixture);
  check(!mode_result.valid && mode_result.malformed_transfers == 1,
        "an ordinary descriptor outside the observed mode fails the exact grammar");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  const u64 bad_xyoffset = 0;
  std::memcpy(fixture.ee_memory.data() + fixture.erase_setup_tag_offset + 16 + 16 + 16,
              &bad_xyoffset, sizeof(bad_xyoffset));
  const auto erase_result = capture(fixture);
  check(!erase_result.valid && erase_result.malformed_bytes == 160,
        "an erase setup with noncanonical fixed GS state is rejected");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  constexpr u64 kFrameMaskBit = 1ull << 32;
  constexpr u64 kFixtureFrame = 0x1200 / 32 | (1ull << 16);
  put_u64(&fixture.ee_memory, fixture.erase_setup_tag_offset + 64,
          kFixtureFrame | kFrameMaskBit);
  const auto frame_mask_result = capture(fixture);
  check(!frame_mask_result.valid && frame_mask_result.malformed_bytes == 160,
        "an erase setup with nonzero FRAME FBMSK is rejected");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  put_u64(&fixture.ee_memory, fixture.erase_setup_tag_offset + 112, 0x8);
  const auto clamp_result = capture(fixture);
  check(!clamp_result.valid && clamp_result.malformed_bytes == 160,
        "an erase setup with CLAMP bits outside 0x5 is rejected");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  constexpr u64 kTexturedSpritePrimBit = 1ull << 50;
  put_u64(&fixture.ee_memory, fixture.erase_clear_tag_offset + 16,
          1 | (1ull << 15) | (1ull << 46) |
              (static_cast<u64>(GsPrim::Kind::SPRITE) << 47) | kTexturedSpritePrimBit |
              (3ull << 60));
  const auto prim_result = capture(fixture);
  check(!prim_result.valid && prim_result.malformed_bytes == 64,
        "a SPRITE with extra primitive state is rejected by full-value equality");
}

void test_noncanonical_fog_shape_fails_closed() {
  auto fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  constexpr u64 kBadScissor = (31ull << 16) | (15ull << 48);
  put_u64(&fixture.ee_memory, fixture.erase_setup_tag_offset + 16 + 16, kBadScissor);
  const auto erase_result = capture(fixture);
  check(!erase_result.valid && erase_result.malformed_bytes == 160,
        "a non-16x16 fog erase is rejected");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  const u16 bad_width = 255;
  std::memcpy(fixture.ee_memory.data() + fixture.generic_upload_data_offset + 4, &bad_width,
              sizeof(bad_width));
  const auto dimensions_result = capture(fixture);
  check(!dimensions_result.valid && dimensions_result.malformed_bytes == 16,
        "a fog upload outside exact 256x1 dimensions is rejected");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  fixture.ee_memory[fixture.generic_upload_data_offset + 12] = 20;
  const auto format_result = capture(fixture);
  check(!format_result.valid && format_result.malformed_bytes == 16,
        "a fog upload outside exact PSMT8 format 19 is rejected");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  fixture.ee_memory[fixture.generic_upload_data_offset + 13] = 0;
  const auto force_result = capture(fixture);
  check(!force_result.valid && force_result.malformed_bytes == 16,
        "a fog upload without force-to-GPU is rejected");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  put_u32(&fixture.ee_memory, fixture.clut_upload_data_offset + 8, 0x1400);
  const auto destination_result = capture(fixture);
  check(!destination_result.valid && destination_result.malformed_bytes == 16,
        "a CLUT destination that differs from the erase target is rejected");
}

void test_noncanonical_inert_boundary_fails_closed() {
  auto fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  const u32 final_boundary = fixture.second_finish_tag_offset + 16;
  const u64 inert_refs = static_cast<u64>(DmaTag::Kind::REFS) << 28;
  put_u64(&fixture.ee_memory, final_boundary, inert_refs);
  const auto result = capture(fixture);
  check(!result.valid && result.present && result.malformed_transfers == 1,
        "an inert boundary outside exact NEXT grammar is rejected");
}

void test_bad_embedded_pointers_fail_closed() {
  auto fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  put_u32(&fixture.ee_memory, fixture.generic_upload_data_offset,
          static_cast<u32>(fixture.ee_memory.size() - 16));
  const auto generic_result = capture(fixture);
  check(!generic_result.valid && generic_result.malformed_bytes == 16,
        "an out-of-range generic upload source is rejected before dereference");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  put_u32(&fixture.ee_memory, fixture.clut_upload_data_offset,
          static_cast<u32>(fixture.ee_memory.size() - 16));
  const auto clut_result = capture(fixture);
  check(!clut_result.valid && clut_result.malformed_bytes == 16,
        "an out-of-range CLUT source is rejected before dereference");
}

void test_bad_dma_pointer_fails_closed() {
  auto fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  const u32 bucket_offset = fixture.chain_offset + 4 * 16;
  const u64 bad_next = (static_cast<u64>(DmaTag::Kind::NEXT) << 28) |
                       (static_cast<u64>(fixture.ee_memory.size() + 16) << 32);
  std::memcpy(fixture.ee_memory.data() + bucket_offset, &bad_next, sizeof(bad_next));
  const auto result = capture(fixture);
  check(!result.valid && !result.present && result.malformed_transfers == 1,
        "an out-of-range DMA NEXT is rejected without following it");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  const u64 spr_next = (static_cast<u64>(DmaTag::Kind::NEXT) << 28) |
                       (static_cast<u64>(0x4000) << 32) | (1ull << 63);
  put_u64(&fixture.ee_memory, bucket_offset, spr_next);
  const auto spr_result = capture(fixture);
  check(!spr_result.valid && !spr_result.present && spr_result.malformed_transfers == 1,
        "an SPR DMA tag is rejected before following it");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  const u64 misaligned_next = (static_cast<u64>(DmaTag::Kind::NEXT) << 28) |
                              (static_cast<u64>(0x4001) << 32);
  put_u64(&fixture.ee_memory, bucket_offset, misaligned_next);
  const auto misaligned_result = capture(fixture);
  check(!misaligned_result.valid && !misaligned_result.present &&
            misaligned_result.malformed_transfers == 1,
        "a misaligned DMA NEXT is rejected before following it");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  const u64 self_next = (static_cast<u64>(DmaTag::Kind::NEXT) << 28) |
                        (static_cast<u64>(bucket_offset) << 32);
  put_u64(&fixture.ee_memory, bucket_offset, self_next);
  const auto cycle_result = capture(fixture);
  check(!cycle_result.valid && !cycle_result.present && cycle_result.malformed_transfers == 1,
        "a self-cycle is rejected without rereading the DMA tag");
}

void test_unsupported_opcode_is_counted() {
  auto fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  const u32 cloud_tag = fixture.first_finish_tag_offset - 128;
  put_u32(&fixture.ee_memory, cloud_tag + 8,
          (static_cast<u32>(VifCode::Kind::PC_PORT) << 24) | 42);
  const auto result = capture(fixture);
  check(!result.valid && result.unsupported_transfers == 1 && result.unsupported_bytes == 112,
        "an unsupported animator opcode retains only its scalar byte count");
}

void test_nonpositive_cloud_max_time_is_malformed() {
  auto fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  constexpr float kNegativeMaxTime = -1.f;
  std::memcpy(fixture.ee_memory.data() + fixture.sky_input_data_offset + 56,
              &kNegativeMaxTime, sizeof(kNegativeMaxTime));
  const auto result = capture(fixture);
  check(!result.valid && result.malformed_transfers == 1 && result.malformed_bytes == 112,
        "a nonpositive cloud max time is rejected before the CPU generator");
}

void test_empty_bucket_is_valid_and_absent() {
  std::vector<u8> memory(0x1000);
  constexpr u32 chain_offset = 0x100;
  const u32 bucket_offset = chain_offset + 4 * 16;
  const u64 empty = static_cast<u64>(DmaTag::Kind::CNT) << 28;
  std::memcpy(memory.data() + bucket_offset, &empty, sizeof(empty));
  const auto result = metal_renderer::capture_jak2_bucket4_texture_upload(
      memory.data(), memory.size(), chain_offset);
  check(result.valid && !result.present && result.dma_transfers == 1 &&
            result.inert_cnt_transfers == 1,
        "an empty bucket remains a valid absent diagnostic");
}

void test_plan_uses_separate_packet_and_live_domains_and_owns_animator_bytes() {
  const auto fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  auto packet = fixture.ee_memory;
  auto live = fixture.ee_memory;
  std::array<u8, metal_renderer::kJak2Bucket4OrdinaryPageHeaderBytes> expected_page = {};
  std::array<u8, metal_renderer::kJak2Bucket4SkyInputBytes> expected_sky = {};
  std::array<u8, metal_renderer::kJak2Bucket4FogIndexBytes> expected_indices = {};
  std::array<u8, metal_renderer::kJak2Bucket4ClutBytes> expected_clut = {};
  std::copy_n(live.begin() + fixture.ordinary_page_offset, expected_page.size(),
              expected_page.begin());
  std::copy_n(packet.begin() + fixture.sky_input_data_offset, expected_sky.size(),
              expected_sky.begin());
  std::copy_n(live.begin() + fixture.generic_source_offset, expected_indices.size(),
              expected_indices.begin());
  std::copy_n(live.begin() + fixture.clut_source_offset, expected_clut.size(),
              expected_clut.begin());

  std::fill(packet.begin() + fixture.ordinary_page_offset,
            packet.begin() + fixture.ordinary_page_offset +
                metal_renderer::kJak2Bucket4OrdinaryPageHeaderBytes,
            0xee);
  std::fill(packet.begin() + fixture.generic_source_offset,
            packet.begin() + fixture.generic_source_offset +
                metal_renderer::kJak2Bucket4FogIndexBytes,
            0xee);
  std::fill(packet.begin() + fixture.clut_source_offset,
            packet.begin() + fixture.clut_source_offset + metal_renderer::kJak2Bucket4ClutBytes,
            0xee);
  std::fill(live.begin() + fixture.chain_offset,
            live.begin() + fixture.chain_offset + 328 * 16, 0xdd);
  std::fill(live.begin() + fixture.outer_direct_tag_offset,
            live.begin() + fixture.outer_direct_tag_offset + 0x600, 0xdd);

  auto plan = metal_renderer::plan_jak2_bucket4_texture_upload(
      packet.data(), packet.size(), fixture.chain_offset, live.data(), live.size());
  check(plan.has_value(), "the exact mixed packet produces an execution plan");
  check(std::holds_alternative<metal_renderer::Jak2Bucket4MixedPlan>(*plan),
        "the mixed packet has the Mixed discriminant");
  const auto& mixed = std::get<metal_renderer::Jak2Bucket4MixedPlan>(*plan);
  check(mixed.ordinary.page_offset == fixture.ordinary_page_offset &&
            mixed.ordinary.mode == -1 && mixed.ordinary.page_header == expected_page,
        "the ordinary descriptor uses the live EE page domain without retaining a host pointer");
  check(mixed.sky.fog_height == 1.f && mixed.sky.cloud_min == 2.f &&
            mixed.sky.cloud_max == 3.f && mixed.sky.times.front() == 4.f &&
            mixed.sky.times.back() == 14.f && mixed.sky.max_times.front() == 15.f &&
            mixed.sky.max_times.back() == 20.f && mixed.sky.scales.front() == 21.f &&
            mixed.sky.scales.back() == 26.f && mixed.sky.cloud_destination == 0x1234,
        "all meaningful SkyInput scalars are owned by the plan");
  check(mixed.sky.bytes == expected_sky,
        "the exact SkyInput bytes are owned by the plan");
  check(mixed.erase.width == 16 && mixed.erase.height == 16 &&
            mixed.erase.destination == 0x1200 && mixed.erase.test == 0x11 &&
            mixed.erase.alpha == 0x22 && mixed.erase.clamp == 0x1 &&
            mixed.erase.setup_values[1] == (0x8000ull | (0x8000ull << 32)) &&
            mixed.erase.setup_values[8] == 0 && mixed.erase.clear[0] == 17 &&
            mixed.erase.clear[3] == 68,
        "the exact erase setup and clear scalars are owned by the plan");
  check(mixed.fog.width == 256 && mixed.fog.height == 1 &&
            mixed.fog.destination == 0x1300 && mixed.fog.format == 19 &&
            mixed.fog.force_to_gpu == 1 && mixed.fog.indices.front() == 0x5a &&
            mixed.fog.indices.back() == static_cast<u8>(255 ^ 0x5a) &&
            mixed.fog.clut_destination == 0x1200 && mixed.fog.clut.front() == 7 &&
            mixed.fog.clut.back() == static_cast<u8>(1023 * 5 + 7) &&
            mixed.fog.indices == expected_indices && mixed.fog.clut == expected_clut,
        "the exact fog-index and CLUT bytes come from the live EE domain");

  std::fill(packet.begin(), packet.end(), 0xa5);
  std::fill(live.begin() + fixture.generic_source_offset,
            live.begin() + fixture.generic_source_offset +
                metal_renderer::kJak2Bucket4FogIndexBytes,
            0xa5);
  std::fill(live.begin() + fixture.clut_source_offset,
            live.begin() + fixture.clut_source_offset + metal_renderer::kJak2Bucket4ClutBytes,
            0xa5);
  check(mixed.sky.bytes == expected_sky && mixed.sky.fog_height == 1.f &&
            mixed.sky.cloud_destination == 0x1234 && mixed.fog.indices == expected_indices &&
            mixed.fog.clut == expected_clut,
        "packet reuse and live-source poisoning cannot change owned plan bytes");
}

void test_plan_accepts_exact_ordinary_only_and_absent_shapes() {
  const auto ordinary_fixture =
      metal_renderer::make_jak2_bucket4_ordinary_only_texture_upload_fixture();
  auto ordinary_plan = metal_renderer::plan_jak2_bucket4_texture_upload(
      ordinary_fixture.ee_memory.data(), ordinary_fixture.ee_memory.size(),
      ordinary_fixture.chain_offset, ordinary_fixture.ee_memory.data(),
            ordinary_fixture.ee_memory.size());
  check(ordinary_plan.has_value() &&
            std::holds_alternative<metal_renderer::Jak2Bucket4OrdinaryOnlyPlan>(*ordinary_plan),
        "the exact ordinary-only packet has the OrdinaryOnly discriminant");
  const auto& ordinary =
      std::get<metal_renderer::Jak2Bucket4OrdinaryOnlyPlan>(*ordinary_plan).ordinary;
  check(ordinary.page_offset == ordinary_fixture.ordinary_page_offset && ordinary.mode == -1 &&
            ordinary.page_header[8] == 1 && ordinary.page_header[12] == 0,
        "the ordinary-only plan retains its validated live page offset, mode, and header");

  std::vector<u8> packet(0x1000);
  constexpr u32 chain_offset = 0x100;
  const u32 bucket_offset = chain_offset + 4 * 16;
  const u64 empty = static_cast<u64>(DmaTag::Kind::CNT) << 28;
  std::memcpy(packet.data() + bucket_offset, &empty, sizeof(empty));
  auto absent_plan = metal_renderer::plan_jak2_bucket4_texture_upload(
      packet.data(), packet.size(), chain_offset, nullptr, 0);
  check(absent_plan.has_value() &&
            std::holds_alternative<metal_renderer::Jak2Bucket4AbsentPlan>(*absent_plan),
        "an exact empty bucket has the Absent discriminant without a live EE dependency");
}

void test_late_malformed_packet_produces_no_plan() {
  auto fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  put_u32(&fixture.ee_memory, fixture.second_finish_tag_offset + 12, 0);
  const auto plan = metal_renderer::plan_jak2_bucket4_texture_upload(
      fixture.ee_memory.data(), fixture.ee_memory.size(), fixture.chain_offset,
      fixture.ee_memory.data(), fixture.ee_memory.size());
  check(!plan.has_value(), "a malformed final animator finish cannot leak a partial plan");

  fixture = metal_renderer::make_jak2_bucket4_texture_upload_fixture();
  const auto short_live_plan = metal_renderer::plan_jak2_bucket4_texture_upload(
      fixture.ee_memory.data(), fixture.ee_memory.size(), fixture.chain_offset,
      fixture.ee_memory.data(), fixture.generic_source_offset + 128);
  check(!short_live_plan.has_value(),
        "embedded sources are validated against the live EE domain rather than packet size");
}

}  // namespace

int main() {
  test_exact_observed_grammar();
  test_exact_ordinary_only_grammar();
  test_missing_finish_fails_closed();
  test_bad_direct_fails_closed();
  test_bad_fixed_packet_state_fails_closed();
  test_noncanonical_fog_shape_fails_closed();
  test_noncanonical_inert_boundary_fails_closed();
  test_bad_embedded_pointers_fail_closed();
  test_bad_dma_pointer_fails_closed();
  test_unsupported_opcode_is_counted();
  test_nonpositive_cloud_max_time_is_malformed();
  test_empty_bucket_is_valid_and_absent();
  test_plan_uses_separate_packet_and_live_domains_and_owns_animator_bytes();
  test_plan_accepts_exact_ordinary_only_and_absent_shapes();
  test_late_malformed_packet_produces_no_plan();
  std::puts("PASS: Jak II bucket-4 texture-upload capture");
  return 0;
}
