#include "game/graphics/pipelines/metal/metal_jak2_opcode41_cloud_cpu.h"

#include <algorithm>
#include <cmath>

namespace metal_renderer {
namespace {

constexpr std::array<std::array<u8, 16>, 8> kInitialRandomTable = {{
    {0x20, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x89, 0x67, 0x45, 0x23, 0x1},
    {0x37, 0x82, 0x87, 0x23, 0x78, 0x87, 0x4, 0x32, 0x97, 0x91, 0x48, 0x98, 0x30, 0x38, 0x89, 0x87},
    {0x62, 0x47, 0x2, 0x62, 0x78, 0x92, 0x28, 0x90, 0x81, 0x47, 0x72, 0x28, 0x83, 0x29, 0x71, 0x68},
    {0x28, 0x61, 0x17, 0x62, 0x87, 0x74, 0x38, 0x12, 0x83, 0x9, 0x78, 0x12, 0x76, 0x31, 0x72, 0x80},
    {0x39, 0x72, 0x98, 0x34, 0x72, 0x98, 0x69, 0x78, 0x65, 0x71, 0x98, 0x83, 0x97, 0x23, 0x98, 0x1},
    {0x97, 0x38, 0x72, 0x98, 0x23, 0x87, 0x23, 0x98, 0x93, 0x72, 0x98, 0x20, 0x81, 0x29, 0x10,
     0x62},
    {0x28, 0x75, 0x38, 0x82, 0x99, 0x30, 0x72, 0x87, 0x83, 0x9, 0x14, 0x98, 0x10, 0x43, 0x87, 0x29},
    {0x87, 0x23, 0x0, 0x87, 0x18, 0x98, 0x12, 0x98, 0x10, 0x98, 0x21, 0x83, 0x90, 0x37, 0x62, 0x71},
}};

u8 to_unorm8(float value) {
  const float clamped = std::clamp(value, 0.f, 1.f);
  return static_cast<u8>(std::floor(clamped * 255.f + 0.5f));
}

int wrap_coordinate(int coordinate, int dimension) {
  const int remainder = coordinate % dimension;
  return remainder < 0 ? remainder + dimension : remainder;
}

}  // namespace

float jak2_opcode41_cloud_lookup(float value, float minimum, float maximum) {
  maximum = std::max(minimum, maximum);
  if (value <= minimum) {
    return 0.f;
  }
  if (value >= maximum) {
    return 1.f;
  }

  const float alpha = (value - minimum) / (maximum - minimum);
  const float sin_alpha = std::sin(alpha * (3.1415926f / 2.f));
  return sin_alpha * sin_alpha;
}

Jak2Opcode41CloudCpu::Jak2Opcode41CloudCpu() {
  reset();
}

void Jak2Opcode41CloudCpu::reset() {
  m_random_table = kInitialRandomTable;
  m_random_index = 0;
  m_rgba.fill(0);

  int dimension = kJak2Opcode41CloudSize >> (kJak2Opcode41CloudLayerCount - 1);
  for (auto& layer : m_layers) {
    layer.dimension = dimension;
    layer.last_time = 0.f;
    make_noise(&layer.new_noise, dimension);
    make_noise(&layer.old_noise, dimension);
    dimension *= 2;
  }

  // TextureAnimator::setup_sky uses the same random table for the regular and hires clouds.
  // Discarding the six hires pairs preserves the exact RNG state seen by later regular rollovers.
  std::vector<u8> discard;
  dimension = 16;
  for (int layer = 0; layer < 6; ++layer) {
    make_noise(&discard, dimension);
    make_noise(&discard, dimension);
    dimension *= 2;
  }
}

bool Jak2Opcode41CloudCpu::input_is_valid(const Jak2Opcode41CloudInput& input) const {
  if (!std::isfinite(input.cloud_min) || !std::isfinite(input.cloud_max)) {
    return false;
  }
  for (int layer = 0; layer < kJak2Opcode41CloudLayerCount; ++layer) {
    if (!std::isfinite(input.times[layer]) || !std::isfinite(input.max_times[layer]) ||
        input.max_times[layer] <= 0.f || !std::isfinite(input.scales[layer])) {
      return false;
    }
  }
  return true;
}

void Jak2Opcode41CloudCpu::make_noise(std::vector<u8>* destination, int dimension) {
  destination->resize(static_cast<std::size_t>(dimension) * dimension);
  const int quadwords_per_row = dimension / kRandomRowBytes;
  for (int row = 0; row < dimension; ++row) {
    for (int quadword = 0; quadword < quadwords_per_row; ++quadword) {
      RandomRow& source0 = m_random_table[(m_random_index + 0) % kRandomTableRows];
      RandomRow& source1 = m_random_table[(m_random_index + 3) % kRandomTableRows];
      RandomRow& source2 = m_random_table[(m_random_index + 5) % kRandomTableRows];
      RandomRow& result = m_random_table[(m_random_index + 7) % kRandomTableRows];
      const int destination_offset = (row * quadwords_per_row + quadword) * kRandomRowBytes;
      for (int byte = 0; byte < kRandomRowBytes; ++byte) {
        result[byte] = static_cast<u8>(source0[byte] + source1[byte] + source2[byte]);
        (*destination)[destination_offset + byte] = result[byte];
      }
      m_random_index = (m_random_index + 1) % kRandomTableRows;
    }
  }
}

float Jak2Opcode41CloudCpu::sample_linear_repeat(const std::vector<u8>& noise,
                                                 int dimension,
                                                 int output_x,
                                                 int output_y) const {
  // run_clouds sets CLAMP_TO_EDGE before it binds each generated noise texture. The textures
  // therefore retain GL_REPEAT; preserve that live bind-order quirk at the bilinear edges.
  const float source_scale = static_cast<float>(dimension) / kJak2Opcode41CloudSize;
  const float source_x = (static_cast<float>(output_x) + 0.5f) * source_scale - 0.5f;
  const float source_y = (static_cast<float>(output_y) + 0.5f) * source_scale - 0.5f;
  const int x0 = static_cast<int>(std::floor(source_x));
  const int y0 = static_cast<int>(std::floor(source_y));
  const int x1 = x0 + 1;
  const int y1 = y0 + 1;
  const float fraction_x = source_x - static_cast<float>(x0);
  const float fraction_y = source_y - static_cast<float>(y0);

  const auto value = [&](int x, int y) {
    const int wrapped_x = wrap_coordinate(x, dimension);
    const int wrapped_y = wrap_coordinate(y, dimension);
    return static_cast<float>(noise[wrapped_y * dimension + wrapped_x]) / 255.f;
  };
  const float top = value(x0, y0) + (value(x1, y0) - value(x0, y0)) * fraction_x;
  const float bottom = value(x0, y1) + (value(x1, y1) - value(x0, y1)) * fraction_x;
  return top + (bottom - top) * fraction_y;
}

bool Jak2Opcode41CloudCpu::generate(const Jak2Opcode41CloudInput& input) {
  if (!input_is_valid(input)) {
    return false;
  }

  for (int layer_index = 0; layer_index < kJak2Opcode41CloudLayerCount; ++layer_index) {
    auto& layer = m_layers[layer_index];
    if (input.times[layer_index] < layer.last_time) {
      std::swap(layer.new_noise, layer.old_noise);
      make_noise(&layer.new_noise, layer.dimension);
    }
    layer.last_time = input.times[layer_index];
  }

  for (int y = 0; y < kJak2Opcode41CloudSize; ++y) {
    for (int x = 0; x < kJak2Opcode41CloudSize; ++x) {
      u8 blended = 0;
      for (int layer_index = 0; layer_index < kJak2Opcode41CloudLayerCount; ++layer_index) {
        const auto& layer = m_layers[layer_index];
        const float interpolation = input.times[layer_index] / input.max_times[layer_index];
        const float new_weight = interpolation * input.scales[layer_index];
        const float old_weight = (1.f - interpolation) * input.scales[layer_index];

        // The GL path stores each additive draw into an RGBA8 target, so preserve its per-draw
        // UNORM quantization instead of accumulating all eight samples in an unbounded float.
        float value = static_cast<float>(blended) / 255.f;
        value += sample_linear_repeat(layer.new_noise, layer.dimension, x, y) * new_weight;
        blended = to_unorm8(value);
        value = static_cast<float>(blended) / 255.f;
        value += sample_linear_repeat(layer.old_noise, layer.dimension, x, y) * old_weight;
        blended = to_unorm8(value);
      }

      const float cloud_value = static_cast<float>(blended) / 255.f;
      const float alpha =
          0.5f * jak2_opcode41_cloud_lookup(cloud_value, input.cloud_min, input.cloud_max);
      const u32 alpha_byte = to_unorm8(alpha);
      m_rgba[y * kJak2Opcode41CloudSize + x] = (alpha_byte << 24) | 0x00808080u;
    }
  }
  return true;
}

}  // namespace metal_renderer
