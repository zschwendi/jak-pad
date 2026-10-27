#include "game/graphics/pipelines/metal/metal_jak2_fog_texture_convert.h"
#include "game/graphics/pipelines/metal/metal_jak2_map_layer_convert.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

using metal_renderer::Jak2MapLayerRejection;
using metal_renderer::kJak2MapLayerHeight;
using metal_renderer::kJak2MapLayerPixelCount;
using metal_renderer::kJak2MapLayerWidth;

constexpr std::size_t W = kJak2MapLayerWidth;
constexpr std::size_t H = kJak2MapLayerHeight;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

// Independent oracle: CSM1 PSMT8 lookup is a swap of index bits 3 and 4.
std::size_t oracle_clut_address(u8 index) {
  const u32 b3 = (index >> 3) & 1;
  const u32 b4 = (index >> 4) & 1;
  return (index & ~0x18u) | (b3 << 4) | (b4 << 3);
}

std::vector<u32> make_clut(u32 seed) {
  static const u8 kAlphaEdges[] = {0, 1, 127, 128, 255};
  std::vector<u32> clut(256);
  for (u32 i = 0; i < 256; i++) {
    const u32 r = (i * 29 + seed * 3 + 1) & 0xff;
    const u32 g = (i * 47 + seed * 5 + 2) & 0xff;
    const u32 b = (i * 61 + seed * 7 + 3) & 0xff;
    const u32 a = kAlphaEdges[(i + seed) % 5];
    clut[i] = (a << 24) | (b << 16) | (g << 8) | r;
  }
  return clut;
}

// Row/column asymmetric indices; row 0 contains all 256 values at least twice.
std::vector<u8> make_indices(u32 seed) {
  std::vector<u8> indices(kJak2MapLayerPixelCount);
  for (std::size_t r = 0; r < H; r++) {
    for (std::size_t c = 0; c < W; c++) {
      indices[c + r * W] = static_cast<u8>((c + r * (37 + seed * 2) + seed * 11 + (r * r) % 7) & 0xff);
    }
  }
  indices[0] = static_cast<u8>(0x01 + seed);
  indices[W - 1] = static_cast<u8>(0x80 + seed);
  indices[(H - 1) * W] = static_cast<u8>(0x10 + seed);
  indices[H * W - 1] = static_cast<u8>(0xF7 - seed);
  return indices;
}

bool matches_oracle(const std::vector<u32>& rgba,
                    const std::vector<u8>& indices,
                    const std::vector<u32>& clut) {
  if (rgba.size() != kJak2MapLayerPixelCount) {
    return false;
  }
  for (std::size_t i = 0; i < rgba.size(); i++) {
    if (rgba[i] != clut[oracle_clut_address(indices[i])]) {
      return false;
    }
  }
  return true;
}

void test_csm1_permutation() {
  // Hand boundary probes.
  check(oracle_clut_address(7) == 7, "probe 7");
  check(oracle_clut_address(8) == 16, "probe 8 -> 16");
  check(oracle_clut_address(15) == 23, "probe 15 -> 23");
  check(oracle_clut_address(16) == 8, "probe 16 -> 8");
  check(oracle_clut_address(24) == 24, "probe 24");
  check(oracle_clut_address(32) == 32, "probe 32");
  check(oracle_clut_address(255) == 255, "probe 255");

  // Identity-ish palette exposes the address; every index maps through the oracle.
  std::vector<u32> clut(256);
  for (u32 i = 0; i < 256; i++) {
    clut[i] = 0xA5000000u | (i << 8) | (255 - i);
  }
  std::vector<u8> indices(kJak2MapLayerPixelCount);
  for (std::size_t i = 0; i < indices.size(); i++) {
    indices[i] = static_cast<u8>(i & 0xff);
  }
  const auto out = metal_renderer::convert_jak2_map_psmt8_to_rgba(indices, clut, W, H);
  check(out.valid && out.rejection == Jak2MapLayerRejection::None, "permutation valid");
  std::array<bool, 256> seen = {};
  for (u32 i = 0; i < 256; i++) {
    const std::size_t addr = oracle_clut_address(static_cast<u8>(i));
    check(out.rgba[i] == clut[addr], "oracle permutation");
    check(!seen[addr], "oracle bijective");
    seen[addr] = true;
  }

  // Baseline fog converter parity on every 256-pixel block of a valid map.
  const auto map_clut = make_clut(3);
  const auto map_indices = make_indices(3);
  const auto map = metal_renderer::convert_jak2_map_psmt8_to_rgba(map_indices, map_clut, W, H);
  check(map.valid, "map valid for fog parity");
  for (std::size_t block = 0; block < kJak2MapLayerPixelCount / 256; block++) {
    const auto fog = metal_renderer::convert_jak2_fog_psmt8_to_rgba(
        map_indices.data() + block * 256, 256, map_clut.data(), map_clut.size());
    check(fog.has_value(), "fog block valid");
    for (std::size_t i = 0; i < 256; i++) {
      check((*fog)[i] == map.rgba[block * 256 + i], "fog parity");
    }
  }
}

void test_four_layers_and_ownership() {
  std::vector<std::vector<u8>> indices;
  std::vector<std::vector<u32>> cluts;
  std::vector<metal_renderer::Jak2MapLayerRgba> outs;
  for (u32 layer = 0; layer < 4; layer++) {
    indices.push_back(make_indices(layer));
    cluts.push_back(make_clut(layer));
  }
  for (u32 layer = 0; layer < 4; layer++) {
    outs.push_back(metal_renderer::convert_jak2_map_psmt8_to_rgba(indices[layer], cluts[layer], W, H));
    check(outs[layer].valid, "layer valid");
    check(matches_oracle(outs[layer].rgba, indices[layer], cluts[layer]), "layer oracle");

    // Every alpha edge value survives verbatim.
    for (u32 a : {0u, 1u, 127u, 128u, 255u}) {
      bool found = false;
      for (const u32 word : outs[layer].rgba) {
        found |= (word >> 24) == a;
      }
      check(found, "alpha edge preserved");
    }

    // Negative controls: reversed row order and the un-swizzled lookup must both differ.
    std::vector<u32> reversed(kJak2MapLayerPixelCount);
    for (std::size_t r = 0; r < H; r++) {
      for (std::size_t c = 0; c < W; c++) {
        reversed[c + r * W] = outs[layer].rgba[c + (H - 1 - r) * W];
      }
    }
    check(!matches_oracle(reversed, indices[layer], cluts[layer]), "row reverse control");
    std::vector<u32> linear(kJak2MapLayerPixelCount);
    for (std::size_t i = 0; i < linear.size(); i++) {
      linear[i] = cluts[layer][indices[layer][i]];
    }
    check(!matches_oracle(linear, indices[layer], cluts[layer]), "no-bitswap control");
    std::vector<u32> swapped = outs[layer].rgba;
    for (u32& word : swapped) {
      word = (word & 0xff00ff00u) | ((word & 0xffu) << 16) | ((word >> 16) & 0xffu);
    }
    check(!matches_oracle(swapped, indices[layer], cluts[layer]), "red/blue swap control");

    // Mutate inputs after decode; earlier outputs must retain exact words.
    const std::vector<u32> snapshot = outs[layer].rgba;
    for (auto& v : indices[layer]) {
      v = static_cast<u8>(~v);
    }
    for (auto& v : cluts[layer]) {
      v ^= 0xFFFFFFFFu;
    }
    for (u32 prev = 0; prev <= layer; prev++) {
      check(outs[prev].rgba.size() == kJak2MapLayerPixelCount, "prior output size");
    }
    check(outs[layer].rgba == snapshot, "output survives input mutation");
  }

  // Distinct layers, distinct storage.
  for (u32 a = 0; a < 4; a++) {
    for (u32 b = a + 1; b < 4; b++) {
      check(outs[a].rgba != outs[b].rgba, "layers distinct");
      check(outs[a].rgba.data() != outs[b].rgba.data(), "distinct storage");
    }
  }

  // Destroy inputs; recompute expectations from fresh deterministic inputs.
  indices.clear();
  indices.shrink_to_fit();
  cluts.clear();
  cluts.shrink_to_fit();
  for (u32 layer = 0; layer < 4; layer++) {
    check(matches_oracle(outs[layer].rgba, make_indices(layer), make_clut(layer)),
          "output owned after input destruction");
  }

  // Copy and move behave as ordinary vectors.
  auto copy = outs[0];
  check(copy.valid && copy.rgba == outs[0].rgba && copy.rgba.data() != outs[0].rgba.data(),
        "copy is deep");
  const u32* storage = outs[1].rgba.data();
  auto moved = std::move(outs[1]);
  check(moved.valid && moved.rgba.data() == storage, "move transfers storage");
  check(matches_oracle(moved.rgba, make_indices(1), make_clut(1)), "moved contents");
}

void expect_reject(const metal_renderer::Jak2MapLayerRgba& out,
                   Jak2MapLayerRejection rejection,
                   const char* message) {
  check(!out.valid && out.rejection == rejection && out.rgba.empty() && out.rgba.capacity() == 0,
        message);
}

void test_rejections() {
  const auto indices = make_indices(0);
  const auto clut = make_clut(0);
  const std::size_t max = SIZE_MAX;
  const std::pair<std::size_t, std::size_t> bad_dims[] = {
      {0, 0},   {0, H},       {W, 0},       {H, W},     {W + 1, H}, {W, H + 1},
      {W - 1, H}, {W, H - 1}, {1024, 104}, {256, 416}, {max, H},   {W, max},
      {max, max}, {max / H + 1, H}};
  for (const auto& [w, h] : bad_dims) {
    expect_reject(metal_renderer::convert_jak2_map_psmt8_to_rgba(indices, clut, w, h),
                  Jak2MapLayerRejection::BadDimensions, "bad dimensions");
  }

  const std::vector<u8> long_indices(kJak2MapLayerPixelCount + 1, 0);
  expect_reject(metal_renderer::convert_jak2_map_psmt8_to_rgba(
                    std::span<const u8>(indices.data(), indices.size() - 1), clut, W, H),
                Jak2MapLayerRejection::BadIndexCount, "short indices");
  expect_reject(metal_renderer::convert_jak2_map_psmt8_to_rgba(long_indices, clut, W, H),
                Jak2MapLayerRejection::BadIndexCount, "long indices");
  expect_reject(metal_renderer::convert_jak2_map_psmt8_to_rgba({}, clut, W, H),
                Jak2MapLayerRejection::BadIndexCount, "empty indices");

  const std::vector<u32> long_clut(257, 0);
  expect_reject(metal_renderer::convert_jak2_map_psmt8_to_rgba(
                    indices, std::span<const u32>(clut.data(), 255), W, H),
                Jak2MapLayerRejection::BadClutEntryCount, "short clut");
  expect_reject(metal_renderer::convert_jak2_map_psmt8_to_rgba(indices, long_clut, W, H),
                Jak2MapLayerRejection::BadClutEntryCount, "long clut");
  expect_reject(metal_renderer::convert_jak2_map_psmt8_to_rgba(indices, {}, W, H),
                Jak2MapLayerRejection::BadClutEntryCount, "empty clut");
  expect_reject(metal_renderer::convert_jak2_map_psmt8_to_rgba({}, {}, 0, 0),
                Jak2MapLayerRejection::BadDimensions, "all empty");
}

}  // namespace

int main() {
  test_csm1_permutation();
  test_four_layers_and_ownership();
  test_rejections();
  std::printf("metal_jak2_map_layer_convert_test: PASS\n");
  return 0;
}
