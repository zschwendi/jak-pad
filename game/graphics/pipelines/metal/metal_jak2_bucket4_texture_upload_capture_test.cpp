#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_capture.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_fixture.h"

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

}  // namespace

int main() {
  test_exact_observed_grammar();
  test_missing_finish_fails_closed();
  test_bad_direct_fails_closed();
  test_bad_fixed_packet_state_fails_closed();
  test_noncanonical_fog_shape_fails_closed();
  test_noncanonical_inert_boundary_fails_closed();
  test_bad_embedded_pointers_fail_closed();
  test_bad_dma_pointer_fails_closed();
  test_unsupported_opcode_is_counted();
  test_empty_bucket_is_valid_and_absent();
  std::puts("PASS: Jak II bucket-4 texture-upload capture");
  return 0;
}
