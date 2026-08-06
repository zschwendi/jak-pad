#pragma once

#include <array>
#include <vector>

#include "common/common_types.h"

namespace metal_renderer {

constexpr int kJak2Opcode41CloudSize = 128;
constexpr int kJak2Opcode41CloudLayerCount = 4;
constexpr int kJak2Opcode41CloudPixelCount = kJak2Opcode41CloudSize * kJak2Opcode41CloudSize;

struct Jak2Opcode41CloudInput {
  float cloud_min = 0.f;
  float cloud_max = 0.f;
  // These are opcode-41 SkyInput times[1] through times[4].
  std::array<float, kJak2Opcode41CloudLayerCount> times = {};
  std::array<float, kJak2Opcode41CloudLayerCount> max_times = {};
  std::array<float, kJak2Opcode41CloudLayerCount> scales = {};
};

/*! Mirrors tex_anim.frag's cloud_lookup scalar. */
float jak2_opcode41_cloud_lookup(float value, float minimum, float maximum);

/*!
 * Deterministic CPU implementation of Jak II's 128x128 opcode-41 cloud texture.
 * Pixels are owned by this object and packed as 0xAABBGGRR.
 */
class Jak2Opcode41CloudCpu {
 public:
  Jak2Opcode41CloudCpu();

  void reset();
  bool generate(const Jak2Opcode41CloudInput& input);

  const std::array<u32, kJak2Opcode41CloudPixelCount>& rgba() const { return m_rgba; }

 private:
  static constexpr int kRandomTableRows = 8;
  static constexpr int kRandomRowBytes = 16;

  using RandomRow = std::array<u8, kRandomRowBytes>;
  using RandomTable = std::array<RandomRow, kRandomTableRows>;

  struct NoiseLayer {
    int dimension = 0;
    float last_time = 0.f;
    std::vector<u8> old_noise;
    std::vector<u8> new_noise;
  };

  bool input_is_valid(const Jak2Opcode41CloudInput& input) const;
  void make_noise(std::vector<u8>* destination, int dimension);
  float sample_linear_repeat(const std::vector<u8>& noise,
                             int dimension,
                             int output_x,
                             int output_y) const;

  RandomTable m_random_table = {};
  int m_random_index = 0;
  std::array<NoiseLayer, kJak2Opcode41CloudLayerCount> m_layers = {};
  std::array<u32, kJak2Opcode41CloudPixelCount> m_rgba = {};
};

}  // namespace metal_renderer
