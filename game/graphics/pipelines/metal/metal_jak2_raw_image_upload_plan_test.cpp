#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/graphics/pipelines/metal/metal_jak2_raw_image_upload_fixture.h"

[[noreturn]] void private_assert_failed(const char*,
                                        const char*,
                                        int,
                                        const char*,
                                        const char*) {
  std::abort();
}

namespace {

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

std::optional<metal_renderer::Jak2RawImageUploadPlan> plan(
    const metal_renderer::Jak2RawImageUploadFixture& fixture,
    const u8* live_memory = nullptr,
    std::size_t live_memory_size = 0) {
  if (!live_memory) {
    live_memory = fixture.ee_memory.data();
    live_memory_size = fixture.ee_memory.size();
  }
  return metal_renderer::plan_jak2_raw_image_upload(
      fixture.ee_memory.data(), fixture.ee_memory.size(), fixture.chain_offset,
      live_memory, live_memory_size);
}

void test_live_source_owned_plan() {
  auto fixture = metal_renderer::make_jak2_raw_image_upload_fixture();
  auto live_memory = fixture.ee_memory;
  constexpr u32 kLiveGreen = 0xff00ff00u;
  constexpr u32 kLaterBlue = 0xffff0000u;
  const std::size_t pixel_count =
      static_cast<std::size_t>(metal_renderer::kJak2RawImageWidth) *
      metal_renderer::kJak2RawImageHeight;
  for (std::size_t i = 0; i < pixel_count; ++i) {
    metal_renderer::jak2_raw_image_fixture_detail::put_u32(
        live_memory, fixture.source_offset + static_cast<u32>(i * sizeof(u32)),
        kLiveGreen);
  }

  auto result = plan(fixture, live_memory.data(), live_memory.size());
  check(result && result->present &&
            result->width == metal_renderer::kJak2RawImageWidth &&
            result->height == metal_renderer::kJak2RawImageHeight &&
            result->destination == metal_renderer::kJak2RawImageDestination &&
            result->format == metal_renderer::kJak2RawImagePsmct32 &&
            result->force_to_gpu == 1 &&
            result->rgba.size() == pixel_count && result->rgba.front() == kLiveGreen &&
            result->rgba.back() == kLiveGreen,
        "raw-image metadata from the snapshot copies pixels from distinct live EE memory");

  std::memset(fixture.ee_memory.data() + fixture.source_offset, 0,
              result->rgba.size() * sizeof(u32));
  for (std::size_t i = 0; i < pixel_count; ++i) {
    metal_renderer::jak2_raw_image_fixture_detail::put_u32(
        live_memory, fixture.source_offset + static_cast<u32>(i * sizeof(u32)),
        kLaterBlue);
  }
  check(result->rgba.front() == kLiveGreen && result->rgba.back() == kLiveGreen,
        "later snapshot and live-memory reuse cannot change the owned plan");
}

void test_supported_layouts() {
  auto direct_only = metal_renderer::make_jak2_raw_image_direct_only_fixture();
  auto result = metal_renderer::plan_jak2_raw_image_upload(
      direct_only.ee_memory.data(), direct_only.ee_memory.size(), direct_only.chain_offset,
      nullptr, 0);
  check(result && !result->present && result->rgba.empty(),
        "a structurally valid Direct-only bucket produces an absent upload plan");

  auto direct_before = metal_renderer::make_jak2_raw_image_direct_before_upload_fixture();
  result = plan(direct_before);
  check(result && result->present,
        "Direct transfers before the raw-image marker are accepted");

  auto upload_before = metal_renderer::make_jak2_raw_image_upload_before_direct_fixture();
  result = plan(upload_before);
  check(result && result->present,
        "Direct transfers after the raw-image marker are accepted");

  auto mixed = metal_renderer::make_jak2_raw_image_mixed_overlay_fixture();
  result = plan(mixed);
  check(result && result->present,
        "Direct transfers on both sides of one raw-image marker are accepted");
}

void test_strict_empty_plan() {
  auto fixture = metal_renderer::make_jak2_raw_image_upload_fixture();
  metal_renderer::jak2_raw_image_fixture_detail::put_tag(
      fixture.ee_memory, fixture.bucket_offset, DmaTag::Kind::CNT);
  auto result = plan(fixture);
  check(result && !result->present && result->rgba.empty(),
        "a canonical strict-empty bucket produces an absent plan");
}

void test_copied_marker_divergence() {
  auto absent_source = metal_renderer::make_jak2_raw_image_direct_only_fixture();
  const auto absent_plan = plan(absent_source);
  auto unexpected_copied = metal_renderer::make_jak2_raw_image_upload_fixture();
  check(absent_plan && !absent_plan->present &&
            !metal_renderer::copied_jak2_raw_image_upload_markers_match_plan(
                unexpected_copied.ee_memory.data(), unexpected_copied.ee_memory.size(),
                unexpected_copied.chain_offset, absent_plan->present),
        "an absent live plan detects an unexpected copied publication marker");

  auto present_source = metal_renderer::make_jak2_raw_image_upload_fixture();
  const auto present_plan = plan(present_source);
  auto missing_copied = metal_renderer::make_jak2_raw_image_direct_only_fixture();
  check(present_plan && present_plan->present &&
            !metal_renderer::copied_jak2_raw_image_upload_markers_match_plan(
                missing_copied.ee_memory.data(), missing_copied.ee_memory.size(),
                missing_copied.chain_offset, present_plan->present),
        "a present live plan detects a missing copied publication marker");
}

void test_rejections() {
  {
    auto fixture = metal_renderer::make_jak2_raw_image_upload_fixture();
    const u16 wrong_width = metal_renderer::kJak2RawImageWidth - 1;
    std::memcpy(fixture.ee_memory.data() + fixture.upload_data_offset + 4, &wrong_width,
                sizeof(wrong_width));
    check(!plan(fixture), "an untracked raw-image width is rejected");
  }
  {
    auto fixture = metal_renderer::make_jak2_raw_image_upload_fixture();
    fixture.ee_memory[fixture.upload_data_offset + 13] = 0;
    check(!plan(fixture), "a CPU-only generic upload is rejected");
  }
  {
    auto fixture = metal_renderer::make_jak2_raw_image_upload_fixture();
    const u32 outside = static_cast<u32>(fixture.ee_memory.size() - 16);
    std::memcpy(fixture.ee_memory.data() + fixture.upload_data_offset, &outside,
                sizeof(outside));
    check(!plan(fixture), "an out-of-range raw image source is rejected");
  }
  {
    auto fixture = metal_renderer::make_jak2_raw_image_upload_fixture();
    metal_renderer::jak2_raw_image_fixture_detail::put_u32(
        fixture.ee_memory, fixture.finish_tag_offset + 8,
        metal_renderer::jak2_raw_image_fixture_detail::vif(VifCode::Kind::PC_PORT, 12));
    check(!plan(fixture), "a missing finish opcode is rejected");
  }
  {
    auto fixture = metal_renderer::make_jak2_raw_image_upload_fixture();
    constexpr u32 kDuplicateOffset = 0x6000;
    metal_renderer::jak2_raw_image_fixture_detail::put_tag(
        fixture.ee_memory, fixture.final_boundary_offset, DmaTag::Kind::NEXT, 0,
        kDuplicateOffset);
    std::memcpy(fixture.ee_memory.data() + kDuplicateOffset,
                fixture.ee_memory.data() + fixture.start_tag_offset, 64);
    metal_renderer::jak2_raw_image_fixture_detail::put_tag(
        fixture.ee_memory, kDuplicateOffset + 64, DmaTag::Kind::NEXT, 0,
        fixture.bucket_offset + 16);
    check(!plan(fixture), "a second exact raw-image marker sequence is rejected");
  }
}

}  // namespace

int main() {
  test_live_source_owned_plan();
  test_supported_layouts();
  test_strict_empty_plan();
  test_copied_marker_divergence();
  test_rejections();
  std::puts("PASS: Jak II bucket-318 raw-image upload planner");
  return 0;
}
