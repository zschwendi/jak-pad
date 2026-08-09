#include "game/graphics/pipelines/metal/metal_vis_data.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {

using metal_renderer::MetalVisibilityFrame;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

u64 vif_tag(u32 vif0, VifCode::Kind vif1) {
  return static_cast<u64>(vif0) | (static_cast<u64>(vif1) << 56);
}

struct Fixture {
  static constexpr std::size_t kLevels = 6;
  std::array<std::array<u8, metal_renderer::kMetalVisibilityBytes>, kLevels> visibility = {};
  std::array<u8, metal_renderer::kMetalBackgroundFallbackBytes> fallback = {};
  std::array<DmaTransfer, kLevels * 2 + 2> transfers = {};

  Fixture() {
    for (std::size_t level = 0; level < kLevels; level++) {
      for (std::size_t byte = 0; byte < visibility[level].size(); byte++) {
        visibility[level][byte] = static_cast<u8>((level * 37 + byte * 13) & 0xff);
      }
      transfers[level * 2].data = visibility[level].data();
      transfers[level * 2].size_bytes =
          level == 0 || level == 4 ? metal_renderer::kMetalVisibilityBytes
                                   : metal_renderer::kMetalInactiveVisibilityBytes;
      transfers[level * 2].transferred_tag = vif_tag(0x44332211, VifCode::Kind::PC_PORT);
    }
    for (std::size_t byte = 0; byte < fallback.size(); byte++) {
      fallback[byte] = static_cast<u8>((byte * 29 + 7) & 0xff);
    }
    transfers[kLevels * 2].data = fallback.data();
    transfers[kLevels * 2].size_bytes = fallback.size();
    transfers[kLevels * 2].transferred_tag = vif_tag(0, VifCode::Kind::PC_PORT);
  }
};

void test_owned_mixed_frame_with_fallback() {
  Fixture fixture;
  MetalVisibilityFrame frame;
  check(metal_renderer::decode_metal_visibility_frame(
            fixture.transfers.data(), fixture.transfers.size(), Fixture::kLevels, &frame),
        "the exact six-level mixed frame decodes");
  check(frame.level_count == Fixture::kLevels && frame.fog_vif0 == 0x44332211,
        "the frame owns the requested level count and final fog word");
  for (std::size_t level = 0; level < Fixture::kLevels; level++) {
    const bool expected_valid = level == 0 || level == 4;
    check(frame.levels[level].valid == expected_valid,
          "2048-byte slots are active and 16-byte slots are inactive");
    if (expected_valid) {
      check(frame.levels[level].data == fixture.visibility[level],
            "active visibility bytes are copied exactly");
    }
  }
  check(frame.has_fallback && frame.fallback == fixture.fallback,
        "the optional 400-byte fallback is copied exactly");

  const u8 first_vis = frame.levels[0].data[0];
  const u8 first_fallback = frame.fallback[0];
  fixture.visibility[0][0] ^= 0xff;
  fixture.fallback[0] ^= 0xff;
  check(frame.levels[0].data[0] == first_vis && frame.fallback[0] == first_fallback,
        "the decoded frame retains no DMA payload pointers");
}

void test_visibility_only_frame() {
  Fixture fixture;
  MetalVisibilityFrame frame;
  check(metal_renderer::decode_metal_visibility_frame(
            fixture.transfers.data(), Fixture::kLevels * 2, Fixture::kLevels, &frame),
        "a visibility frame without fallback data decodes");
  check(!frame.has_fallback, "the absent fallback remains explicitly absent");
}

void test_malformed_input_is_atomic() {
  Fixture fixture;
  MetalVisibilityFrame frame;
  frame.level_count = 9;
  frame.fog_vif0 = 0xaabbccdd;
  frame.levels[3].valid = true;
  frame.levels[3].data[7] = 0x5a;
  const auto unchanged = [&frame]() {
    return frame.level_count == 9 && frame.fog_vif0 == 0xaabbccdd &&
           frame.levels[3].valid && frame.levels[3].data[7] == 0x5a;
  };

  fixture.transfers[4].size_bytes = 32;
  check(!metal_renderer::decode_metal_visibility_frame(
            fixture.transfers.data(), fixture.transfers.size(), Fixture::kLevels, &frame),
        "an unexpected visibility payload size is rejected");
  check(unchanged(),
        "a malformed late slot leaves the destination unchanged");

  Fixture boundary_fixture;
  boundary_fixture.transfers[3].size_bytes = 16;
  check(!metal_renderer::decode_metal_visibility_frame(
            boundary_fixture.transfers.data(), boundary_fixture.transfers.size(),
            Fixture::kLevels, &frame),
        "a non-empty level boundary is rejected");
  check(unchanged(),
        "a malformed boundary leaves the destination unchanged");

  Fixture vif_fixture;
  vif_fixture.transfers[8].transferred_tag = 0;
  check(!metal_renderer::decode_metal_visibility_frame(
            vif_fixture.transfers.data(), vif_fixture.transfers.size(), Fixture::kLevels, &frame),
        "a visibility payload without PC_PORT in vif1 is rejected");
  check(unchanged(),
        "a malformed VIF tag leaves the destination unchanged");

  check(!metal_renderer::decode_metal_visibility_frame(
            fixture.transfers.data(), fixture.transfers.size(),
            metal_renderer::kMetalMaxVisibilityLevels + 1, &frame),
        "a level count beyond the owned frame capacity is rejected");
  check(!metal_renderer::decode_metal_visibility_frame(
            fixture.transfers.data(), fixture.transfers.size() - 1, Fixture::kLevels, &frame),
        "an incomplete fallback pair is rejected");
  check(!metal_renderer::decode_metal_visibility_frame(
            fixture.transfers.data(), fixture.transfers.size(), Fixture::kLevels, nullptr),
        "a null destination is rejected");
}

}  // namespace

int main() {
  test_owned_mixed_frame_with_fallback();
  test_visibility_only_frame();
  test_malformed_input_is_atomic();
  std::puts("metal_vis_data_test: PASS");
  return 0;
}
