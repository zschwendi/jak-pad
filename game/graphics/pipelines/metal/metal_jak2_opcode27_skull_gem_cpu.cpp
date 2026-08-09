#include "game/graphics/pipelines/metal/metal_jak2_opcode27_skull_gem_cpu.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace metal_renderer {
namespace {

constexpr float kAnimationEndTime = 300.f;
constexpr double kPi = 3.14159265358979323846;

struct Vec2 {
  float x = 0.f;
  float y = 0.f;
};

struct LayerGeometry {
  std::array<Vec2, 4> positions;
  std::array<Vec2, 4> uvs;
  std::array<float, 4> modulation;
};

template <std::size_t Size>
bool all_finite(const std::array<float, Size>& values) {
  for (const float value : values) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  return true;
}

bool all_finite(const Jak2Opcode27LayerValues& values) {
  return all_finite(values.color) && all_finite(values.scale) && all_finite(values.offset) &&
         all_finite(values.st_scale) && all_finite(values.st_offset) && all_finite(values.qs) &&
         std::isfinite(values.rot) && std::isfinite(values.st_rot);
}

bool valid_source(const Jak2Opcode27RgbaSource& source) {
  if (source.width == 0 || source.height == 0 ||
      source.width > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
      source.height > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
      source.width > std::numeric_limits<std::size_t>::max() / source.height) {
    return false;
  }
  const std::size_t pixel_count = source.width * source.height;
  return pixel_count <= std::numeric_limits<std::size_t>::max() / 4 &&
         source.rgba.size() == pixel_count * 4;
}

template <std::size_t Size>
bool interpolate_array(float interpolation,
                       const std::array<float, Size>& start,
                       const std::array<float, Size>& end,
                       std::array<float, Size>* result) {
  for (std::size_t i = 0; i < Size; ++i) {
    (*result)[i] = start[i] + (end[i] - start[i]) * interpolation;
    if (!std::isfinite((*result)[i])) {
      return false;
    }
  }
  return true;
}

bool interpolate_values(float interpolation,
                        const Jak2Opcode27LayerTransition& transition,
                        Jak2Opcode27LayerValues* result) {
  if (!interpolate_array(interpolation, transition.start.color, transition.end.color,
                         &result->color) ||
      !interpolate_array(interpolation, transition.start.scale, transition.end.scale,
                         &result->scale) ||
      !interpolate_array(interpolation, transition.start.offset, transition.end.offset,
                         &result->offset) ||
      !interpolate_array(interpolation, transition.start.st_scale, transition.end.st_scale,
                         &result->st_scale) ||
      !interpolate_array(interpolation, transition.start.st_offset, transition.end.st_offset,
                         &result->st_offset) ||
      !interpolate_array(interpolation, transition.start.qs, transition.end.qs, &result->qs)) {
    return false;
  }
  result->rot = transition.start.rot +
                (transition.end.rot - transition.start.rot) * interpolation;
  result->st_rot = transition.start.st_rot +
                   (transition.end.st_rot - transition.start.st_rot) * interpolation;
  return std::isfinite(result->rot) && std::isfinite(result->st_rot);
}

Vec2 rotate_texture_anim_corner(const Vec2& corner, float rotation) {
  const float radians =
      static_cast<float>(2.0 * kPi * static_cast<double>(rotation) / 65536.0);
  const float sine = std::sin(radians);
  const float cosine = std::cos(radians);
  const Vec2 vx{sine, cosine};
  const Vec2 vy{cosine, -sine};
  return {vx.x * corner.x + vy.x * corner.y, vx.y * corner.x + vy.y * corner.y};
}

bool quantize_modulation(const std::array<float, 4>& color,
                         std::array<float, 4>* modulation) {
  for (std::size_t channel = 0; channel < color.size(); ++channel) {
    const float scaled = color[channel] * 128.f;
    if (!std::isfinite(scaled) || scaled < 0.f ||
        static_cast<double>(scaled) > std::numeric_limits<u32>::max()) {
      return false;
    }
    (*modulation)[channel] = static_cast<float>(static_cast<u32>(scaled)) / 128.f;
  }
  return true;
}

bool build_layer_geometry(const Jak2Opcode27LayerValues& values, LayerGeometry* geometry) {
  constexpr std::array<Vec2, 4> kCorners = {
      Vec2{-0.5f, -0.5f}, Vec2{0.5f, -0.5f}, Vec2{-0.5f, 0.5f}, Vec2{0.5f, 0.5f}};
  if (!quantize_modulation(values.color, &geometry->modulation)) {
    return false;
  }

  const Vec2 position_scale{values.scale[0] * kJak2Opcode27SkullGemSize,
                            values.scale[1] * kJak2Opcode27SkullGemSize};
  const Vec2 position_offset{2048.f + values.offset[0] * kJak2Opcode27SkullGemSize,
                             2048.f + values.offset[1] * kJak2Opcode27SkullGemSize};

  for (std::size_t i = 0; i < kCorners.size(); ++i) {
    Vec2 position_corner = kCorners[i];
    if (values.rot != 0.f) {
      position_corner = rotate_texture_anim_corner(position_corner, values.rot);
    }
    const float packed_x =
        (position_corner.x * position_scale.x + position_offset.x) * 16.f;
    const float packed_y =
        (position_corner.y * position_scale.y + position_offset.y) * 16.f;
    if (!std::isfinite(packed_x) || !std::isfinite(packed_y) || packed_x < 0.f ||
        packed_y < 0.f || static_cast<double>(packed_x) > std::numeric_limits<u32>::max() ||
        static_cast<double>(packed_y) > std::numeric_limits<u32>::max()) {
      return false;
    }
    const u32 gs_x = static_cast<u32>(packed_x);
    const u32 gs_y = static_cast<u32>(packed_y);
    geometry->positions[i] = {
        ((static_cast<float>(gs_x) / 16.f) - 2048.f) / kJak2Opcode27SkullGemSize,
        ((static_cast<float>(gs_y) / 16.f) - 2048.f) / kJak2Opcode27SkullGemSize};

    Vec2 texture_corner{kCorners[i].x * values.st_scale[0],
                        kCorners[i].y * values.st_scale[1]};
    if (values.st_rot != 0.f) {
      texture_corner = rotate_texture_anim_corner(texture_corner, values.st_rot);
    }
    geometry->uvs[i] = {values.st_offset[0] + texture_corner.x,
                        values.st_offset[1] + texture_corner.y};
    if (!std::isfinite(geometry->uvs[i].x) || !std::isfinite(geometry->uvs[i].y)) {
      return false;
    }
  }
  return true;
}

float cross(const Vec2& a, const Vec2& b) {
  return a.x * b.y - a.y * b.x;
}

Vec2 subtract(const Vec2& a, const Vec2& b) {
  return {a.x - b.x, a.y - b.y};
}

bool interpolate_triangle(const Vec2& point,
                          const Vec2& position0,
                          const Vec2& position1,
                          const Vec2& position2,
                          const Vec2& uv0,
                          const Vec2& uv1,
                          const Vec2& uv2,
                          Vec2* uv) {
  const Vec2 edge01 = subtract(position1, position0);
  const Vec2 edge02 = subtract(position2, position0);
  const float denominator = cross(edge01, edge02);
  if (denominator == 0.f || !std::isfinite(denominator)) {
    return false;
  }

  const Vec2 from0 = subtract(point, position0);
  const float weight1 = cross(from0, edge02) / denominator;
  const float weight2 = cross(edge01, from0) / denominator;
  const float weight0 = 1.f - weight1 - weight2;
  constexpr float kEdgeTolerance = 1e-6f;
  if (weight0 < -kEdgeTolerance || weight1 < -kEdgeTolerance || weight2 < -kEdgeTolerance) {
    return false;
  }
  uv->x = uv0.x * weight0 + uv1.x * weight1 + uv2.x * weight2;
  uv->y = uv0.y * weight0 + uv1.y * weight1 + uv2.y * weight2;
  return std::isfinite(uv->x) && std::isfinite(uv->y);
}

bool texture_coordinate_at_pixel(const LayerGeometry& geometry, int x, int y, Vec2* uv) {
  const Vec2 point{(static_cast<float>(x) + 0.5f) / kJak2Opcode27SkullGemSize,
                   (static_cast<float>(y) + 0.5f) / kJak2Opcode27SkullGemSize};
  if (interpolate_triangle(point, geometry.positions[0], geometry.positions[1],
                           geometry.positions[2], geometry.uvs[0], geometry.uvs[1],
                           geometry.uvs[2], uv)) {
    return true;
  }
  return interpolate_triangle(point, geometry.positions[2], geometry.positions[1],
                              geometry.positions[3], geometry.uvs[2], geometry.uvs[1],
                              geometry.uvs[3], uv);
}

std::int64_t wrap_coordinate(std::int64_t coordinate, std::int64_t dimension) {
  const std::int64_t remainder = coordinate % dimension;
  return remainder < 0 ? remainder + dimension : remainder;
}

bool sample_linear_repeat(const Jak2Opcode27RgbaSource& source,
                          const Vec2& uv,
                          std::array<float, 4>* sample) {
  const float source_x = uv.x * static_cast<float>(source.width) - 0.5f;
  const float source_y = uv.y * static_cast<float>(source.height) - 0.5f;
  constexpr float kInt64Limit = static_cast<float>(std::numeric_limits<std::int64_t>::max());
  if (!std::isfinite(source_x) || !std::isfinite(source_y) || source_x <= -kInt64Limit ||
      source_y <= -kInt64Limit || source_x >= kInt64Limit || source_y >= kInt64Limit) {
    return false;
  }

  const std::int64_t x0 = static_cast<std::int64_t>(std::floor(source_x));
  const std::int64_t y0 = static_cast<std::int64_t>(std::floor(source_y));
  const std::int64_t width = static_cast<std::int64_t>(source.width);
  const std::int64_t height = static_cast<std::int64_t>(source.height);
  const float fraction_x = source_x - static_cast<float>(x0);
  const float fraction_y = source_y - static_cast<float>(y0);

  const auto texel = [&](std::int64_t x, std::int64_t y, std::size_t channel) {
    const std::size_t wrapped_x = static_cast<std::size_t>(wrap_coordinate(x, width));
    const std::size_t wrapped_y = static_cast<std::size_t>(wrap_coordinate(y, height));
    const std::size_t offset = (wrapped_y * source.width + wrapped_x) * 4 + channel;
    return static_cast<float>(source.rgba[offset]) / 255.f;
  };

  for (std::size_t channel = 0; channel < sample->size(); ++channel) {
    const float row0 = texel(x0, y0, channel) +
                       (texel(x0 + 1, y0, channel) - texel(x0, y0, channel)) * fraction_x;
    const float row1 = texel(x0, y0 + 1, channel) +
                       (texel(x0 + 1, y0 + 1, channel) - texel(x0, y0 + 1, channel)) *
                           fraction_x;
    (*sample)[channel] = row0 + (row1 - row0) * fraction_y;
  }
  return true;
}

u8 to_unorm8(float value) {
  const float clamped = std::clamp(value, 0.f, 1.f);
  return static_cast<u8>(std::floor(clamped * 255.f + 0.5f));
}

bool draw_layer(const LayerGeometry& geometry,
                const Jak2Opcode27RgbaSource& source,
                Jak2Opcode27SkullGemRgba* output) {
  for (int y = 0; y < kJak2Opcode27SkullGemSize; ++y) {
    for (int x = 0; x < kJak2Opcode27SkullGemSize; ++x) {
      Vec2 uv;
      if (!texture_coordinate_at_pixel(geometry, x, y, &uv)) {
        continue;
      }

      std::array<float, 4> sample;
      if (!sample_linear_repeat(source, uv, &sample)) {
        return false;
      }
      std::array<float, 4> source_color;
      for (std::size_t channel = 0; channel < source_color.size(); ++channel) {
        source_color[channel] =
            std::clamp(sample[channel] * geometry.modulation[channel], 0.f, 1.f);
      }

      const std::size_t output_offset =
          (static_cast<std::size_t>(y) * kJak2Opcode27SkullGemSize + x) * 4;
      const float source_alpha_for_rgb = std::clamp(source_color[3] * 2.f, 0.f, 1.f);
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const float destination = static_cast<float>((*output)[output_offset + channel]) / 255.f;
        (*output)[output_offset + channel] =
            to_unorm8(destination + source_color[channel] * source_alpha_for_rgb);
      }
      (*output)[output_offset + 3] = to_unorm8(source_color[3]);
    }
  }
  return true;
}

}  // namespace

bool compose_jak2_opcode27_skull_gem_cpu(
    const Jak2Opcode27SkullGemPlan& plan,
    const std::array<Jak2Opcode27RgbaSource, kJak2Opcode27SkullGemLayerCount>& sources,
    Jak2Opcode27SkullGemRgba* output) {
  if (!output || !std::isfinite(plan.time) || plan.time < 0.f ||
      plan.time > kAnimationEndTime) {
    return false;
  }
  for (std::size_t layer = 0; layer < sources.size(); ++layer) {
    if (!valid_source(sources[layer]) || !all_finite(plan.layers[layer].start) ||
        !all_finite(plan.layers[layer].end)) {
      return false;
    }
  }

  const float interpolation = plan.time / kAnimationEndTime;
  std::array<LayerGeometry, kJak2Opcode27SkullGemLayerCount> geometries;
  for (std::size_t layer = 0; layer < geometries.size(); ++layer) {
    Jak2Opcode27LayerValues values;
    if (!interpolate_values(interpolation, plan.layers[layer], &values) ||
        !build_layer_geometry(values, &geometries[layer])) {
      return false;
    }
  }

  Jak2Opcode27SkullGemRgba composed;
  for (std::size_t pixel = 0; pixel < composed.size(); pixel += 4) {
    composed[pixel + 0] = 0;
    composed[pixel + 1] = 0;
    composed[pixel + 2] = 0;
    composed[pixel + 3] = 255;
  }
  for (std::size_t layer = 0; layer < geometries.size(); ++layer) {
    if (!draw_layer(geometries[layer], sources[layer], &composed)) {
      return false;
    }
  }

  *output = composed;
  return true;
}

}  // namespace metal_renderer
