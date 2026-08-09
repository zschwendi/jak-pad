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
    const metal_renderer::Jak2RawImageUploadFixture& fixture) {
  return metal_renderer::plan_jak2_raw_image_upload(
      fixture.ee_memory.data(), fixture.ee_memory.size(), fixture.chain_offset,
      fixture.ee_memory.data(), fixture.ee_memory.size());
}

void test_exact_owned_plan() {
  auto fixture = metal_renderer::make_jak2_raw_image_upload_fixture();
  auto result = plan(fixture);
  check(result && result->present &&
            result->width == metal_renderer::kJak2RawImageWidth &&
            result->height == metal_renderer::kJak2RawImageHeight &&
            result->destination == metal_renderer::kJak2RawImageDestination &&
            result->format == metal_renderer::kJak2RawImagePsmct32 &&
            result->force_to_gpu == 1 &&
            result->rgba.size() == static_cast<std::size_t>(metal_renderer::kJak2RawImageWidth) *
                                       metal_renderer::kJak2RawImageHeight &&
            result->rgba.front() == 0xff0000ffu && result->rgba.back() == 0xff0000ffu,
        "the exact public 512x416 raw-image grammar produces an owning RGBA plan");

  std::memset(fixture.ee_memory.data() + fixture.source_offset, 0,
              result->rgba.size() * sizeof(u32));
  check(result->rgba.front() == 0xff0000ffu && result->rgba.back() == 0xff0000ffu,
        "reusing live EE source memory cannot change the planned image");
}

void test_strict_empty_plan() {
  auto fixture = metal_renderer::make_jak2_raw_image_upload_fixture();
  metal_renderer::jak2_raw_image_fixture_detail::put_tag(
      fixture.ee_memory, fixture.bucket_offset, DmaTag::Kind::CNT);
  auto result = plan(fixture);
  check(result && !result->present && result->rgba.empty(),
        "a canonical strict-empty bucket produces an absent plan");
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
    const u64 wrong_state_address = static_cast<u64>(GsRegisterAddress::TEX0_2);
    std::memcpy(fixture.ee_memory.data() + fixture.state_tag_offset + 16 + 2 * 16 + 8,
                &wrong_state_address, sizeof(wrong_state_address));
    check(!plan(fixture), "a non-source GS register sequence is rejected");
  }
}

}  // namespace

int main() {
  test_exact_owned_plan();
  test_strict_empty_plan();
  test_rejections();
  std::puts("PASS: Jak II bucket-318 raw-image upload planner");
  return 0;
}
