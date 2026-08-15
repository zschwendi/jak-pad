#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_jak2_subtitle_bucket322_fixture.h"

[[noreturn]] void private_assert_failed(const char*, const char*, int, const char*, const char*) {
  std::abort();
}

namespace {

using Fixture = metal_renderer::Jak2SubtitleBucket322Fixture;
using Layout = metal_renderer::Jak2SubtitleBucket322FixtureLayout;
using Plan = metal_renderer::Jak2SubtitleBucket322Plan;
using RejectReason = metal_renderer::Jak2SubtitleBucket322RejectReason;
using Variant = metal_renderer::Jak2SubtitleBucket322Variant;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

std::optional<Plan> plan(const Fixture& fixture,
                         RejectReason* rejection = nullptr,
                         u32* rejection_transfer_index = nullptr) {
  return metal_renderer::plan_jak2_subtitle_bucket322(
      fixture.ee_memory.data(), fixture.ee_memory.size(), fixture.chain_offset, rejection,
      rejection_transfer_index);
}

void test_policy_stays_deferred() {
  static_assert(metal_renderer::kJak2SubtitleBucket322 ==
                static_cast<u32>(jak2::BucketId::SUBTITLE));
  const auto& descriptor =
      metal_renderer::jak2_metal_bucket_table()[metal_renderer::kJak2SubtitleBucket322];
  check(
      descriptor.behavior == metal_renderer::Jak2MetalBucketBehavior::DeferredSkip &&
          metal_renderer::jak2_metal_direct_batch_size(metal_renderer::kJak2SubtitleBucket322) == 0,
      "the passive plan does not promote bucket 322 to execution");
}

void test_empty_forms() {
  auto fixture = metal_renderer::make_jak2_subtitle_bucket322_strict_empty_fixture();
  const auto before = fixture.ee_memory;
  auto result = plan(fixture);
  check(result && fixture.ee_memory == before && result->variant == Variant::Absent &&
            result->transfer_count == 1 && result->linker_transfers == 1 &&
            result->direct_transfers == 0 && result->image_upload_count == 0 &&
            result->semantic_fingerprint != 0,
        "a strict-empty bucket produces an immutable absent plan");

  metal_renderer::jak2_subtitle_bucket322_fixture_detail::put_tag(
      fixture.ee_memory, fixture.bucket_offset, DmaTag::Kind::NEXT, 0, fixture.bucket_offset + 16);
  result = plan(fixture);
  check(result && result->variant == Variant::Absent && result->transfer_count == 1 &&
            result->linker_transfers == 1,
        "the source bucket-link empty form produces an absent plan");
}

void test_opaque_direct_family() {
  const auto fixture = metal_renderer::make_jak2_subtitle_bucket322_fixture(Layout::OpaqueDirect);
  const auto result = plan(fixture);
  check(result && result->variant == Variant::OpaqueDirect &&
            result->opaque_direct_transfers == 1 && result->direct_transfers == 1 &&
            result->direct_payload_bytes == 48 && result->hud_sprite_pairs == 0 &&
            result->image_upload_count == 0,
        "an unresolved draw-string-style Direct envelope stays typed but passive");
}

void test_intro_hud_sprite_family() {
  auto fixture = metal_renderer::make_jak2_subtitle_bucket322_fixture(Layout::IntroHudSprite);
  auto result = plan(fixture);
  check(result && result->variant == Variant::HudSpriteDirect && result->hud_sprite_pairs == 1 &&
            result->direct_transfers == 2 && result->opaque_direct_transfers == 0 &&
            result->image_upload_count == 0,
        "the source hud-sprite qwc-6/qwc-13 pair is classified exactly");

  fixture = metal_renderer::make_jak2_subtitle_bucket322_fixture(Layout::IntroTwoHudSprites);
  result = plan(fixture);
  check(result && result->variant == Variant::HudSpriteDirect && result->hud_sprite_pairs == 2 &&
            result->direct_transfers == 4 && result->opaque_direct_transfers == 0,
        "the intro's source-valid two-sprite form stays one passive family");
}

void test_subtitle_image_family() {
  const auto fixture = metal_renderer::make_jak2_subtitle_bucket322_fixture(Layout::SubtitleImage);
  const auto before = fixture.ee_memory;
  const auto result = plan(fixture);
  check(result && fixture.ee_memory == before && result->variant == Variant::SubtitleImage &&
            result->image_upload_count == 1 && result->direct_transfers == 3 &&
            result->opaque_direct_transfers == 0 && result->hud_sprite_pairs == 0 &&
            result->image_uploads[0].clut_source_offset == fixture.clut_source_offset &&
            result->image_uploads[0].image_source_offset == fixture.image_source_offset &&
            result->image_uploads[0].width == 128 && result->image_uploads[0].height == 64 &&
            result->image_uploads[0].start_relative_tag_offset > 0,
        "the exact 12/16/16/13 image upload and qwc-7/qwc-6/qwc-6 draw are owned as metadata");
}

void test_mixed_source_families() {
  const auto fixture = metal_renderer::make_jak2_subtitle_bucket322_fixture(Layout::Mixed);
  const auto result = plan(fixture);
  check(result && result->variant == Variant::Mixed && result->image_upload_count == 1 &&
            result->hud_sprite_pairs == 1 && result->opaque_direct_transfers == 1 &&
            result->direct_transfers == 6 && result->linker_transfers == 3,
        "linked text, image, and intro-sprite producers compose only as passive metadata");
}

void test_semantic_match() {
  const auto fixture = metal_renderer::make_jak2_subtitle_bucket322_fixture(Layout::Mixed);
  const auto live = plan(fixture);
  check(live && metal_renderer::jak2_subtitle_bucket322_plans_match(*live, *live),
        "an identical typed subtitle plan has matching semantics");

  auto copied = *live;
  copied.semantic_fingerprint++;
  check(metal_renderer::jak2_subtitle_bucket322_plans_match(*live, copied),
        "a relocation-sensitive packet fingerprint does not change typed semantics");

  copied.image_uploads[0].image_source_offset++;
  check(!metal_renderer::jak2_subtitle_bucket322_plans_match(*live, copied),
        "a typed subtitle image-source change fails semantic matching");
}

void test_rejections() {
  {
    auto fixture = metal_renderer::make_jak2_subtitle_bucket322_fixture(Layout::SubtitleImage);
    const u16 wrong_clut_width = 3;
    std::memcpy(fixture.ee_memory.data() + fixture.first_upload_data_offset + 4, &wrong_clut_width,
                sizeof(wrong_clut_width));
    RejectReason rejection = RejectReason::None;
    check(!plan(fixture, &rejection) && rejection == RejectReason::UploadMetadata,
          "non-source clut metadata is rejected");
  }
  {
    auto fixture = metal_renderer::make_jak2_subtitle_bucket322_fixture(Layout::SubtitleImage);
    fixture.ee_memory[fixture.second_upload_data_offset + 13] = 0;
    RejectReason rejection = RejectReason::None;
    check(!plan(fixture, &rejection) && rejection == RejectReason::UploadMetadata,
          "a CPU-only subtitle image publication is rejected");
  }
  {
    auto fixture = metal_renderer::make_jak2_subtitle_bucket322_fixture(Layout::SubtitleImage);
    metal_renderer::jak2_subtitle_bucket322_fixture_detail::put_u64(
        fixture.ee_memory, fixture.image_setup_tag_offset + 16, 0);
    RejectReason rejection = RejectReason::None;
    check(!plan(fixture, &rejection) && rejection == RejectReason::ImageDrawGrammar,
          "a transposed subtitle-image draw packet is rejected");
  }
  {
    auto fixture = metal_renderer::make_jak2_subtitle_bucket322_fixture(Layout::OpaqueDirect);
    metal_renderer::jak2_subtitle_bucket322_fixture_detail::put_tag(
        fixture.ee_memory, fixture.payload_offset, DmaTag::Kind::CNT, 3, 0,
        metal_renderer::jak2_subtitle_bucket322_fixture_detail::vif(VifCode::Kind::PC_PORT, 22), 0);
    RejectReason rejection = RejectReason::None;
    check(!plan(fixture, &rejection) && rejection == RejectReason::UploadGrammar,
          "an unproved animator opcode fails closed");
  }
  {
    auto fixture = metal_renderer::make_jak2_subtitle_bucket322_fixture(Layout::OpaqueDirect);
    constexpr u32 kReferencedPayloadOffset = 0x28000;
    std::memcpy(fixture.ee_memory.data() + kReferencedPayloadOffset,
                fixture.ee_memory.data() + fixture.payload_offset + 16, 48);
    metal_renderer::jak2_subtitle_bucket322_fixture_detail::put_tag(
        fixture.ee_memory, fixture.payload_offset, DmaTag::Kind::REF, 3, kReferencedPayloadOffset,
        0, metal_renderer::jak2_subtitle_bucket322_fixture_detail::vif(VifCode::Kind::DIRECT, 3));
    metal_renderer::jak2_subtitle_bucket322_fixture_detail::put_tag(
        fixture.ee_memory, fixture.payload_offset + 16, DmaTag::Kind::NEXT, 0,
        fixture.bucket_offset + 16);
    RejectReason rejection = RejectReason::None;
    check(!plan(fixture, &rejection) && rejection == RejectReason::TransferEnvelope,
          "a structurally valid but unproved REF Direct form is rejected");
  }
  {
    auto fixture = metal_renderer::make_jak2_subtitle_bucket322_fixture(Layout::OpaqueDirect);
    metal_renderer::jak2_subtitle_bucket322_fixture_detail::put_tag(
        fixture.ee_memory, fixture.bucket_offset, DmaTag::Kind::NEXT, 0,
        static_cast<u32>(fixture.ee_memory.size() + 16));
    RejectReason rejection = RejectReason::None;
    check(!plan(fixture, &rejection) && rejection == RejectReason::Chain,
          "an invalid outer DMA chain is rejected before classification");
  }
}

}  // namespace

int main() {
  test_policy_stays_deferred();
  test_empty_forms();
  test_opaque_direct_family();
  test_intro_hud_sprite_family();
  test_subtitle_image_family();
  test_mixed_source_families();
  test_semantic_match();
  test_rejections();
  std::puts("PASS: Jak II bucket-322 passive subtitle planner");
  return 0;
}
