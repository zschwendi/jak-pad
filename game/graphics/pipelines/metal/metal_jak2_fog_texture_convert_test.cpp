#include "game/graphics/pipelines/metal/metal_jak2_fog_texture_convert.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

std::array<u8, metal_renderer::kJak2FogIndexedPixelCount> make_indices() {
  std::array<u8, metal_renderer::kJak2FogIndexedPixelCount> indices = {};
  for (std::size_t i = 0; i < indices.size(); i++) {
    indices[i] = static_cast<u8>((i * 73 + 19) & 0xff);
  }
  return indices;
}

std::array<u32, metal_renderer::kJak2FogPsmct32ClutEntryCount> make_clut() {
  std::array<u32, metal_renderer::kJak2FogPsmct32ClutEntryCount> clut = {};
  for (std::size_t i = 0; i < clut.size(); i++) {
    const u32 r = static_cast<u32>((i * 29 + 3) & 0xff);
    const u32 g = static_cast<u32>((i * 47 + 5) & 0xff);
    const u32 b = static_cast<u32>((i * 61 + 7) & 0xff);
    const u32 a = static_cast<u32>(0x80 | (i & 0x7f));
    clut[i] = (a << 24) | (b << 16) | (g << 8) | r;
  }
  return clut;
}

u64 hash_rgba_words(const metal_renderer::Jak2FogRgbaPixels& rgba) {
  u64 hash = 14695981039346656037ull;
  for (const u32 word : rgba) {
    for (u32 shift = 0; shift < 32; shift += 8) {
      hash ^= (word >> shift) & 0xff;
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

void test_ps2_clut_scramble_boundaries() {
  std::array<u8, metal_renderer::kJak2FogIndexedPixelCount> indices = {};
  std::array<u32, metal_renderer::kJak2FogPsmct32ClutEntryCount> clut = {};
  for (std::size_t i = 0; i < clut.size(); i++) {
    clut[i] = 0x5a000000u | static_cast<u32>(i);
  }

  constexpr std::array<u8, 12> kProbeIndices = {0, 7, 8, 15, 16, 23, 24, 31, 40, 48, 127, 255};
  constexpr std::array<u32, 12> kExpectedClutAddresses = {0,  7,  16, 23, 8,   15,
                                                          24, 31, 48, 40, 127, 255};
  for (std::size_t i = 0; i < kProbeIndices.size(); i++) {
    indices[i] = kProbeIndices[i];
  }

  const auto result = metal_renderer::convert_jak2_fog_psmt8_to_rgba(indices.data(), indices.size(),
                                                                     clut.data(), clut.size());
  check(result.has_value(), "an exact 256-index and 16x16 CLUT input converts");
  for (std::size_t i = 0; i < kExpectedClutAddresses.size(); i++) {
    check((*result)[i] == (0x5a000000u | kExpectedClutAddresses[i]),
          "PSMT8 indices use TextureAnimator's exact CSM1 CLUT mapping");
  }
}

void test_raw_rgba_word_is_unchanged() {
  std::array<u8, metal_renderer::kJak2FogIndexedPixelCount> indices = {};
  std::array<u32, metal_renderer::kJak2FogPsmct32ClutEntryCount> clut = {};
  indices[0] = 8;
  clut[16] = 0x44332211u;

  const auto result = metal_renderer::convert_jak2_fog_psmt8_to_rgba(indices.data(), indices.size(),
                                                                     clut.data(), clut.size());
  check(result.has_value() && (*result)[0] == 0x44332211u,
        "PSMCT32 values remain raw 0xAABBGGRR words without a channel transform");
}

void test_stable_full_output_hash() {
  const auto indices = make_indices();
  const auto clut = make_clut();
  const auto result = metal_renderer::convert_jak2_fog_psmt8_to_rgba(indices.data(), indices.size(),
                                                                     clut.data(), clut.size());
  check(result.has_value(), "the deterministic full conversion succeeds");
  check(hash_rgba_words(*result) == 0x7d20e54ce93e3b25ull,
        "the complete converted RGBA output retains its stable hash");
}

void test_invalid_inputs_fail_closed() {
  const auto indices = make_indices();
  const auto clut = make_clut();

  check(!metal_renderer::convert_jak2_fog_psmt8_to_rgba(nullptr, indices.size(), clut.data(),
                                                        clut.size()),
        "a null index pointer is rejected");
  check(!metal_renderer::convert_jak2_fog_psmt8_to_rgba(indices.data(), indices.size(), nullptr,
                                                        clut.size()),
        "a null CLUT pointer is rejected");
  check(!metal_renderer::convert_jak2_fog_psmt8_to_rgba(indices.data(), indices.size() - 1,
                                                        clut.data(), clut.size()),
        "an undersized index buffer is rejected");
  check(!metal_renderer::convert_jak2_fog_psmt8_to_rgba(indices.data(), indices.size() + 1,
                                                        clut.data(), clut.size()),
        "an oversized index buffer is rejected");
  check(!metal_renderer::convert_jak2_fog_psmt8_to_rgba(indices.data(), indices.size(), clut.data(),
                                                        clut.size() - 1),
        "an undersized CLUT is rejected");
  check(!metal_renderer::convert_jak2_fog_psmt8_to_rgba(indices.data(), indices.size(), clut.data(),
                                                        clut.size() + 1),
        "an oversized CLUT is rejected");
}

}  // namespace

int main() {
  test_ps2_clut_scramble_boundaries();
  test_raw_rgba_word_is_unchanged();
  test_stable_full_output_hash();
  test_invalid_inputs_fail_closed();
  std::printf("metal_jak2_fog_texture_convert_test: PASS\n");
  return 0;
}
