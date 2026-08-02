#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "common/common_types.h"

namespace metal_merc_transform_trace {

constexpr std::size_t kMatrixLaneCount = 28;
constexpr std::size_t kBoneSlotCount = 128;

inline u32 nonfinite_lane_mask(const float* lanes) {
  u32 mask = 0;
  for (std::size_t lane = 0; lane < kMatrixLaneCount; lane++) {
    if (!std::isfinite(lanes[lane])) {
      mask |= 1u << lane;
    }
  }
  return mask;
}

enum DiscontinuityIssue : u8 {
  SCALE_DISCONTINUITY = 1 << 0,
  ASPECT_DISCONTINUITY = 1 << 1,
};

struct Event {
  u8 issue_mask = 0;
  int bone_slot = -1;
  u64 model_name_hash = 0;
  u64 previous_frame_id = 0;
  u64 current_frame_id = 0;
  u64 previous_matrix_hash = 0;
  u64 current_matrix_hash = 0;
  double previous_axis_norm_x = 0.0;
  double previous_axis_norm_y = 0.0;
  double previous_axis_norm_z = 0.0;
  double current_axis_norm_x = 0.0;
  double current_axis_norm_y = 0.0;
  double current_axis_norm_z = 0.0;
  double previous_scale = 0.0;
  double current_scale = 0.0;
  double previous_aspect = 0.0;
  double current_aspect = 0.0;
  u64 previous_source_base = 0;
  u64 current_source_base = 0;

  bool valid() const { return issue_mask != 0; }
};

// Jak's bone transforms may rotate every frame, but a rigid transform preserves the lengths and
// aspect ratio of its basis vectors. This bounded diagnostic only reports conspicuous (2x or more)
// changes between truly consecutive engine frames. It never alters the matrix sent to Metal.
class Tracker {
 public:
  static constexpr double kDiscontinuityRatio = 2.0;

  Event observe(u64 frame_id,
                int bone_slot,
                u64 model_name_hash,
                u64 matrix_hash,
                double axis_norm_x,
                double axis_norm_y,
                double axis_norm_z,
                u64 source_base) {
    Event event;
    if (!frame_id || bone_slot < 0 || bone_slot >= static_cast<int>(m_samples.size())) {
      return event;
    }

    auto& previous = m_samples[bone_slot];
    const Sample current = make_sample(frame_id, matrix_hash, axis_norm_x, axis_norm_y,
                                       axis_norm_z, source_base);
    if (!current.valid) {
      previous = {};
      return event;
    }
    if (!previous.valid || frame_id != previous.frame_id + 1) {
      if (frame_id != previous.frame_id) {
        previous = current;
      }
      return event;
    }

    const double scale_ratio = ratio(previous.scale, current.scale);
    const double aspect_ratio = std::max(
        {ratio(previous.axis_norm_x / previous.scale, current.axis_norm_x / current.scale),
         ratio(previous.axis_norm_y / previous.scale, current.axis_norm_y / current.scale),
         ratio(previous.axis_norm_z / previous.scale, current.axis_norm_z / current.scale)});
    if (scale_ratio >= kDiscontinuityRatio) {
      event.issue_mask |= SCALE_DISCONTINUITY;
    }
    if (aspect_ratio >= kDiscontinuityRatio) {
      event.issue_mask |= ASPECT_DISCONTINUITY;
    }
    if (event.valid()) {
      event.bone_slot = bone_slot;
      event.model_name_hash = model_name_hash;
      event.previous_frame_id = previous.frame_id;
      event.current_frame_id = current.frame_id;
      event.previous_matrix_hash = previous.matrix_hash;
      event.current_matrix_hash = current.matrix_hash;
      event.previous_axis_norm_x = previous.axis_norm_x;
      event.previous_axis_norm_y = previous.axis_norm_y;
      event.previous_axis_norm_z = previous.axis_norm_z;
      event.current_axis_norm_x = current.axis_norm_x;
      event.current_axis_norm_y = current.axis_norm_y;
      event.current_axis_norm_z = current.axis_norm_z;
      event.previous_scale = previous.scale;
      event.current_scale = current.scale;
      event.previous_aspect = previous.aspect;
      event.current_aspect = current.aspect;
      event.previous_source_base = previous.source_base;
      event.current_source_base = current.source_base;
    }
    previous = current;
    return event;
  }

 private:
  struct Sample {
    bool valid = false;
    u64 frame_id = 0;
    u64 matrix_hash = 0;
    double axis_norm_x = 0.0;
    double axis_norm_y = 0.0;
    double axis_norm_z = 0.0;
    double scale = 0.0;
    double aspect = 0.0;
    u64 source_base = 0;
  };

  static Sample make_sample(u64 frame_id,
                            u64 matrix_hash,
                            double axis_norm_x,
                            double axis_norm_y,
                            double axis_norm_z,
                            u64 source_base) {
    Sample out;
    const double minimum = std::min({axis_norm_x, axis_norm_y, axis_norm_z});
    const double maximum = std::max({axis_norm_x, axis_norm_y, axis_norm_z});
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum <= 0.0) {
      return out;
    }
    out.valid = true;
    out.frame_id = frame_id;
    out.matrix_hash = matrix_hash;
    out.axis_norm_x = axis_norm_x;
    out.axis_norm_y = axis_norm_y;
    out.axis_norm_z = axis_norm_z;
    out.scale = std::cbrt(axis_norm_x * axis_norm_y * axis_norm_z);
    out.aspect = maximum / minimum;
    out.source_base = source_base;
    if (!std::isfinite(out.scale) || !std::isfinite(out.aspect) || out.scale <= 0.0) {
      return {};
    }
    return out;
  }

  static double ratio(double a, double b) {
    const double minimum = std::min(a, b);
    return minimum > 0.0 ? std::max(a, b) / minimum : 0.0;
  }

  std::array<Sample, kBoneSlotCount> m_samples = {};
};

}  // namespace metal_merc_transform_trace
