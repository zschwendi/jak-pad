#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

#include "common/common_types.h"
#include "game/mips2c/jak1_target_control_capture.h"

namespace metal_merc_transform_trace {

constexpr std::size_t kMatrixLaneCount = 28;
constexpr std::size_t kBoneSlotCount = 128;
constexpr std::size_t kBasisComponentCount = 9;
using MatrixSnapshot = std::array<float, 16>;

inline u32 nonfinite_lane_mask(const float* lanes) {
  u32 mask = 0;
  for (std::size_t lane = 0; lane < kMatrixLaneCount; lane++) {
    if (!std::isfinite(lanes[lane])) {
      mask |= 1u << lane;
    }
  }
  return mask;
}

struct BasisSnapshot {
  bool valid = false;
  std::array<double, kBasisComponentCount> components = {};
};

struct FacingSnapshot {
  bool valid = false;
  double x = 0.0;
  double z = 0.0;
};

struct DeformationSnapshot {
  bool valid = false;
  double axis_norm_x = 0.0;
  double axis_norm_y = 0.0;
  double axis_norm_z = 0.0;
  double aspect = 0.0;
  double normalized_abs_determinant = 0.0;
};

inline FacingSnapshot make_facing_snapshot(double x, double z) {
  FacingSnapshot out;
  const double length = std::sqrt(x * x + z * z);
  if (!std::isfinite(length) || length <= 1e-12) {
    return out;
  }
  out.valid = true;
  out.x = x / length;
  out.z = z / length;
  return out;
}

inline double facing_dot(const FacingSnapshot& a, const FacingSnapshot& b) {
  return a.valid && b.valid ? a.x * b.x + a.z * b.z : std::numeric_limits<double>::quiet_NaN();
}

inline DeformationSnapshot make_deformation_snapshot(const float* matrix,
                                                      std::size_t vector_stride = 4) {
  DeformationSnapshot out;
  if (!matrix || vector_stride < 3) {
    return out;
  }
  const auto norm = [](const float* axis) {
    return std::sqrt(static_cast<double>(axis[0]) * axis[0] +
                     static_cast<double>(axis[1]) * axis[1] +
                     static_cast<double>(axis[2]) * axis[2]);
  };
  const float* x = matrix;
  const float* y = matrix + vector_stride;
  const float* z = matrix + vector_stride * 2;
  out.axis_norm_x = norm(x);
  out.axis_norm_y = norm(y);
  out.axis_norm_z = norm(z);
  const double minimum = std::min({out.axis_norm_x, out.axis_norm_y, out.axis_norm_z});
  const double maximum = std::max({out.axis_norm_x, out.axis_norm_y, out.axis_norm_z});
  const double determinant =
      static_cast<double>(x[0]) * (static_cast<double>(y[1]) * z[2] -
                                   static_cast<double>(y[2]) * z[1]) -
      static_cast<double>(y[0]) * (static_cast<double>(x[1]) * z[2] -
                                   static_cast<double>(x[2]) * z[1]) +
      static_cast<double>(z[0]) * (static_cast<double>(x[1]) * y[2] -
                                   static_cast<double>(x[2]) * y[1]);
  const double norm_product = out.axis_norm_x * out.axis_norm_y * out.axis_norm_z;
  if (!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(determinant) ||
      minimum <= 0.0 || norm_product <= 0.0) {
    return out;
  }
  out.aspect = maximum / minimum;
  out.normalized_abs_determinant = std::abs(determinant) / norm_product;
  out.valid = std::isfinite(out.aspect) && std::isfinite(out.normalized_abs_determinant);
  return out;
}

struct TargetControlObservation {
  bool valid = false;
  jak1_target_control_capture::Stage capture_stage =
      jak1_target_control_capture::Stage::NOT_ATTEMPTED;
  jak1_target_control_capture::Result capture_result =
      jak1_target_control_capture::Result::NOT_ATTEMPTED;
  u64 producer_serial = 0;
  u32 source_base = 0;
  u64 camera_hash = 0;
  u32 target_state_id = 0;
  u64 target_attack_id = 0;
  u32 button0_abs = 0;
  u32 button0_rel = 0;
  u8 left_x = 0;
  u8 left_y = 0;
  double stick_direction = 0.0;
  double stick_speed = 0.0;
  double pad_magnitude = 0.0;
  std::array<float, 4> raw_dir_targ = {};
  std::array<float, 4> raw_quat_for_control = {};
  std::array<float, 4> raw_render_quat = {};
  std::array<float, 4> raw_turn_to_target = {};
  FacingSnapshot intent_forward;
  FacingSnapshot desired_forward;
  FacingSnapshot control_forward;
  FacingSnapshot render_forward;
  FacingSnapshot root_forward;
  BasisSnapshot camera_basis;
  DeformationSnapshot input_root_deformation;
  DeformationSnapshot output_deformation;
  double input_root_translation_x = 0.0;
  double input_root_translation_y = 0.0;
  double input_root_translation_z = 0.0;
};

// Extracts the direction of each 3D basis vector while deliberately removing scale. Both the
// world-space bone transforms and Merc matrices store four-float vectors, hence the default stride.
inline BasisSnapshot make_basis_snapshot(const float* matrix, std::size_t vector_stride = 4) {
  BasisSnapshot out;
  if (!matrix || vector_stride < 3) {
    return out;
  }
  for (std::size_t axis = 0; axis < 3; axis++) {
    const double x = matrix[axis * vector_stride];
    const double y = matrix[axis * vector_stride + 1];
    const double z = matrix[axis * vector_stride + 2];
    const double norm = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(norm) || norm <= 1e-12) {
      return {};
    }
    out.components[axis * 3] = x / norm;
    out.components[axis * 3 + 1] = y / norm;
    out.components[axis * 3 + 2] = z / norm;
  }
  out.valid = true;
  return out;
}

inline double basis_distance(const BasisSnapshot& a, const BasisSnapshot& b) {
  if (!a.valid || !b.valid) {
    return std::numeric_limits<double>::infinity();
  }
  double squared_distance = 0.0;
  for (std::size_t component = 0; component < a.components.size(); component++) {
    const double delta = b.components[component] - a.components[component];
    squared_distance += delta * delta;
  }
  return std::sqrt(squared_distance / a.components.size());
}

inline MatrixSnapshot multiply_matrices(const float* left, const float* right) {
  MatrixSnapshot out = {};
  if (!left || !right) {
    return out;
  }
  for (std::size_t column = 0; column < 4; column++) {
    for (std::size_t row = 0; row < 4; row++) {
      out[column * 4 + row] =
          left[row] * right[column * 4] + left[4 + row] * right[column * 4 + 1] +
          left[8 + row] * right[column * 4 + 2] + left[12 + row] * right[column * 4 + 3];
    }
  }
  return out;
}

inline double output_composition_distance(const float* camera,
                                          const float* bone,
                                          const float* bind_pose,
                                          const float* actual_output) {
  const auto bone_bind = multiply_matrices(bone, bind_pose);
  const auto expected_output = multiply_matrices(camera, bone_bind.data());
  return basis_distance(make_basis_snapshot(expected_output.data()),
                        make_basis_snapshot(actual_output));
}

struct ProvenanceObservation {
  bool mapping_checked = false;
  bool mapping_valid = false;
  int input_root_bone = -1;
  u64 producer_serial = 0;
  u64 input_root_hash = 0;
  u64 camera_hash = 0;
  u64 bind_pose_hash = 0;
  BasisSnapshot input_root_basis;
  BasisSnapshot camera_basis;
  BasisSnapshot output_basis;
  bool expected_output_valid = false;
  double output_expected_distance = 0.0;
  double input_translation_x = 0.0;
  double input_translation_y = 0.0;
  double input_translation_z = 0.0;
  TargetControlObservation target_control;

  bool valid() const {
    return mapping_checked && mapping_valid && input_root_basis.valid && camera_basis.valid &&
           output_basis.valid;
  }
};

enum TargetControlIssue : u8 {
  TARGET_CONTROL_FACING_DIVERGENCE = 1 << 0,
  TARGET_CONTROL_ATTACK_BOUNDARY = 1 << 1,
};

struct TargetControlEvent {
  u8 issue_mask = 0;
  u64 engine_frame_id = 0;
  u64 producer_serial = 0;
  u32 source_base = 0;
  u32 target_state_id = 0;
  u64 previous_target_attack_id = 0;
  u64 current_target_attack_id = 0;
  u32 button0_abs = 0;
  u32 button0_rel = 0;
  u8 left_x = 0;
  u8 left_y = 0;
  double stick_direction = 0.0;
  double stick_speed = 0.0;
  double pad_magnitude = 0.0;
  FacingSnapshot intent_forward;
  FacingSnapshot desired_forward;
  FacingSnapshot control_forward;
  FacingSnapshot render_forward;
  FacingSnapshot root_forward;
  double intent_control_dot = 0.0;
  double control_render_dot = 0.0;
  double render_root_dot = 0.0;

  bool valid() const { return issue_mask != 0; }
};

struct TargetControlTraceResult {
  bool capture_attempted = false;
  bool valid_observation = false;
  jak1_target_control_capture::Stage capture_stage =
      jak1_target_control_capture::Stage::NOT_ATTEMPTED;
  jak1_target_control_capture::Result capture_result =
      jak1_target_control_capture::Result::NOT_ATTEMPTED;
  TargetControlObservation observation;
  TargetControlEvent event;
};

class TargetControlTracker {
 public:
  static constexpr u32 kSquareButton = 1u << 15;
  static constexpr double kMinimumActiveMagnitude = 0.7;
  static constexpr double kFacingDivergenceDot = 0.0;

  TargetControlEvent observe(u64 engine_frame_id,
                             int bone_slot,
                             const TargetControlObservation& observation) {
    return observe_with_status(engine_frame_id, bone_slot, observation).event;
  }

  TargetControlTraceResult observe_with_status(u64 engine_frame_id,
                                               int bone_slot,
                                               const TargetControlObservation& observation) {
    TargetControlTraceResult result;
    if (!engine_frame_id || bone_slot < 0 || bone_slot >= static_cast<int>(m_histories.size())) {
      return result;
    }
    const auto slot = static_cast<std::size_t>(bone_slot);
    const bool attempted = observation.valid ||
                           observation.capture_result !=
                               jak1_target_control_capture::Result::NOT_ATTEMPTED;
    if (!attempted || m_last_capture_frames[slot] == engine_frame_id) {
      return result;
    }
    m_last_capture_frames[slot] = engine_frame_id;
    result.capture_attempted = true;
    result.capture_stage = observation.capture_stage;
    result.capture_result = observation.capture_result;
    if (!observation.valid) {
      return result;
    }
    result.valid_observation = true;
    result.observation = observation;
    auto& event = result.event;

    auto& previous = m_histories[slot];
    if (previous.valid && previous.engine_frame_id == engine_frame_id) {
      return result;
    }
    const double intent_control_dot =
        facing_dot(observation.intent_forward, observation.control_forward);
    if (observation.stick_speed >= kMinimumActiveMagnitude &&
        observation.pad_magnitude >= kMinimumActiveMagnitude && std::isfinite(intent_control_dot) &&
        intent_control_dot < kFacingDivergenceDot) {
      event.issue_mask |= TARGET_CONTROL_FACING_DIVERGENCE;
    }
    const bool consecutive = previous.valid && previous.engine_frame_id + 1 == engine_frame_id;
    if ((observation.button0_rel & kSquareButton) ||
        (consecutive && previous.target_attack_id != observation.target_attack_id)) {
      event.issue_mask |= TARGET_CONTROL_ATTACK_BOUNDARY;
    }

    if (event.valid()) {
      event.engine_frame_id = engine_frame_id;
      event.producer_serial = observation.producer_serial;
      event.source_base = observation.source_base;
      event.target_state_id = observation.target_state_id;
      event.previous_target_attack_id = previous.target_attack_id;
      event.current_target_attack_id = observation.target_attack_id;
      event.button0_abs = observation.button0_abs;
      event.button0_rel = observation.button0_rel;
      event.left_x = observation.left_x;
      event.left_y = observation.left_y;
      event.stick_direction = observation.stick_direction;
      event.stick_speed = observation.stick_speed;
      event.pad_magnitude = observation.pad_magnitude;
      event.intent_forward = observation.intent_forward;
      event.desired_forward = observation.desired_forward;
      event.control_forward = observation.control_forward;
      event.render_forward = observation.render_forward;
      event.root_forward = observation.root_forward;
      event.intent_control_dot = intent_control_dot;
      event.control_render_dot =
          facing_dot(observation.control_forward, observation.render_forward);
      event.render_root_dot = facing_dot(observation.render_forward, observation.root_forward);
    }

    previous.valid = true;
    previous.engine_frame_id = engine_frame_id;
    previous.target_attack_id = observation.target_attack_id;
    return result;
  }

 private:
  struct History {
    bool valid = false;
    u64 engine_frame_id = 0;
    u64 target_attack_id = 0;
  };

  std::array<History, kBoneSlotCount> m_histories = {};
  std::array<u64, kBoneSlotCount> m_last_capture_frames = {};
};

enum DiscontinuityIssue : u8 {
  SCALE_DISCONTINUITY = 1 << 0,
  ASPECT_DISCONTINUITY = 1 << 1,
  INPUT_ROOT_ALTERNATION = 1 << 2,
  OUTPUT_ONLY_ALTERNATION = 1 << 3,
  CAMERA_DRIVEN_ALTERNATION = 1 << 4,
  OUTPUT_STALE = 1 << 5,
  SOURCE_MAPPING_DISCONTINUITY = 1 << 6,
  OUTPUT_COMPOSITION_MISMATCH = 1 << 7,
};

struct Event {
  u8 issue_mask = 0;
  int bone_slot = -1;
  u64 model_name_hash = 0;
  u64 older_frame_id = 0;
  u64 previous_frame_id = 0;
  u64 current_frame_id = 0;
  u64 older_matrix_hash = 0;
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
  u64 older_source_base = 0;
  u64 previous_source_base = 0;
  u64 current_source_base = 0;
  int input_root_bone = -1;
  u64 older_producer_serial = 0;
  u64 previous_producer_serial = 0;
  u64 current_producer_serial = 0;
  u64 older_input_root_hash = 0;
  u64 previous_input_root_hash = 0;
  u64 current_input_root_hash = 0;
  u64 older_camera_hash = 0;
  u64 previous_camera_hash = 0;
  u64 current_camera_hash = 0;
  u64 previous_bind_pose_hash = 0;
  u64 current_bind_pose_hash = 0;
  double previous_output_expected_distance = 0.0;
  double current_output_expected_distance = 0.0;
  double input_older_to_previous_distance = 0.0;
  double input_previous_to_current_distance = 0.0;
  double input_older_to_current_distance = 0.0;
  double output_older_to_previous_distance = 0.0;
  double output_previous_to_current_distance = 0.0;
  double output_older_to_current_distance = 0.0;
  double camera_older_to_previous_distance = 0.0;
  double camera_previous_to_current_distance = 0.0;
  double camera_older_to_current_distance = 0.0;
  double previous_input_translation_x = 0.0;
  double previous_input_translation_y = 0.0;
  double previous_input_translation_z = 0.0;
  double current_input_translation_x = 0.0;
  double current_input_translation_y = 0.0;
  double current_input_translation_z = 0.0;

  bool valid() const { return issue_mask != 0; }
};

constexpr u8 kProvenanceIssueMask = INPUT_ROOT_ALTERNATION | OUTPUT_ONLY_ALTERNATION |
                                    CAMERA_DRIVEN_ALTERNATION | OUTPUT_STALE |
                                    SOURCE_MAPPING_DISCONTINUITY | OUTPUT_COMPOSITION_MISMATCH;

inline bool retain_provenance_event(const Event& event,
                                    int* count,
                                    Event* first,
                                    Event* last) {
  if (!(event.issue_mask & kProvenanceIssueMask)) {
    return false;
  }
  if (count) {
    (*count)++;
  }
  if (first && !first->valid()) {
    *first = event;
  }
  if (last) {
    *last = event;
  }
  return true;
}

// Jak's bone transforms may rotate every frame, but a rigid transform preserves the lengths and
// aspect ratio of its basis vectors. Scale/aspect reports remain adjacent-frame checks. Optional
// producer provenance retains exactly two older samples so rigid A/B/A facing changes can be
// attributed to the world root, camera, or post-producer output. Its bind-pose snapshot also checks
// the current output against camera * bone * bind without depending on a temporal pattern.
class Tracker {
 public:
  static constexpr double kDiscontinuityRatio = 2.0;
  static constexpr double kMinimumBasisDistance = 1e-4;
  static constexpr double kReturnDistanceRatio = 0.5;
  static constexpr double kOutputCompositionMismatchDistance = 1e-3;

  Event observe(u64 frame_id,
                int bone_slot,
                u64 model_name_hash,
                u64 matrix_hash,
                double axis_norm_x,
                double axis_norm_y,
                double axis_norm_z,
                u64 source_base) {
    return observe(frame_id, bone_slot, model_name_hash, matrix_hash, axis_norm_x, axis_norm_y,
                   axis_norm_z, source_base, {});
  }

  Event observe(u64 frame_id,
                int bone_slot,
                u64 model_name_hash,
                u64 matrix_hash,
                double axis_norm_x,
                double axis_norm_y,
                double axis_norm_z,
                u64 source_base,
                const ProvenanceObservation& provenance) {
    Event event;
    if (!frame_id || bone_slot < 0 || bone_slot >= static_cast<int>(m_histories.size())) {
      return event;
    }

    auto& history = m_histories[bone_slot];
    const Sample current = make_sample(frame_id, matrix_hash, axis_norm_x, axis_norm_y, axis_norm_z,
                                       source_base, provenance);
    if (!current.valid) {
      history = {};
      return event;
    }
    if (current.provenance.expected_output_valid &&
        current.provenance.output_expected_distance >= kOutputCompositionMismatchDistance) {
      event.issue_mask |= OUTPUT_COMPOSITION_MISMATCH;
    }
    if (!history.previous.valid || frame_id != history.previous.frame_id + 1) {
      if (frame_id != history.previous.frame_id) {
        if (event.valid()) {
          populate_event(&event, bone_slot, model_name_hash, {}, {}, current);
        }
        history.older = {};
        history.previous = current;
      }
      return event;
    }

    const double scale_ratio = ratio(history.previous.scale, current.scale);
    const double aspect_ratio =
        std::max({ratio(history.previous.axis_norm_x / history.previous.scale,
                        current.axis_norm_x / current.scale),
                  ratio(history.previous.axis_norm_y / history.previous.scale,
                        current.axis_norm_y / current.scale),
                  ratio(history.previous.axis_norm_z / history.previous.scale,
                        current.axis_norm_z / current.scale)});
    if (scale_ratio >= kDiscontinuityRatio) {
      event.issue_mask |= SCALE_DISCONTINUITY;
    }
    if (aspect_ratio >= kDiscontinuityRatio) {
      event.issue_mask |= ASPECT_DISCONTINUITY;
    }
    if (current.provenance.mapping_checked && !current.provenance.mapping_valid) {
      event.issue_mask |= SOURCE_MAPPING_DISCONTINUITY;
    }

    if (history.previous.provenance.valid() && current.provenance.valid()) {
      const double input_leg = basis_distance(history.previous.provenance.input_root_basis,
                                              current.provenance.input_root_basis);
      const double output_leg =
          basis_distance(history.previous.provenance.output_basis, current.provenance.output_basis);
      const double camera_leg =
          basis_distance(history.previous.provenance.camera_basis, current.provenance.camera_basis);
      if (input_leg >= kMinimumBasisDistance && output_leg < kMinimumBasisDistance &&
          camera_leg < kMinimumBasisDistance) {
        event.issue_mask |= OUTPUT_STALE;
      }
    }

    const bool has_three_frame_provenance =
        history.older.valid && history.older.frame_id + 1 == history.previous.frame_id &&
        history.older.provenance.valid() && history.previous.provenance.valid() &&
        current.provenance.valid();
    if (has_three_frame_provenance) {
      const bool input_alternation = strong_return(history.older.provenance.input_root_basis,
                                                   history.previous.provenance.input_root_basis,
                                                   current.provenance.input_root_basis);
      const bool output_alternation =
          strong_return(history.older.provenance.output_basis,
                        history.previous.provenance.output_basis, current.provenance.output_basis);
      const bool camera_alternation =
          strong_return(history.older.provenance.camera_basis,
                        history.previous.provenance.camera_basis, current.provenance.camera_basis);
      const bool input_stable = stable_window(history.older.provenance.input_root_basis,
                                              history.previous.provenance.input_root_basis,
                                              current.provenance.input_root_basis);
      const bool camera_stable =
          stable_window(history.older.provenance.camera_basis,
                        history.previous.provenance.camera_basis, current.provenance.camera_basis);
      if (input_alternation && output_alternation) {
        event.issue_mask |= INPUT_ROOT_ALTERNATION;
      } else if (output_alternation && input_stable && camera_alternation) {
        event.issue_mask |= CAMERA_DRIVEN_ALTERNATION;
      } else if (output_alternation && input_stable && camera_stable) {
        event.issue_mask |= OUTPUT_ONLY_ALTERNATION;
      }
    }

    if (event.valid()) {
      populate_event(&event, bone_slot, model_name_hash, history.older, history.previous, current);
    }
    history.older = history.previous;
    history.previous = current;
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
    ProvenanceObservation provenance;
  };

  struct History {
    Sample older;
    Sample previous;
  };

  static Sample make_sample(u64 frame_id,
                            u64 matrix_hash,
                            double axis_norm_x,
                            double axis_norm_y,
                            double axis_norm_z,
                            u64 source_base,
                            const ProvenanceObservation& provenance) {
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
    out.provenance = provenance;
    if (!std::isfinite(out.scale) || !std::isfinite(out.aspect) || out.scale <= 0.0) {
      return {};
    }
    return out;
  }

  static bool strong_return(const BasisSnapshot& older,
                            const BasisSnapshot& previous,
                            const BasisSnapshot& current) {
    const double older_to_previous = basis_distance(older, previous);
    const double previous_to_current = basis_distance(previous, current);
    const double older_to_current = basis_distance(older, current);
    const double minimum_leg = std::min(older_to_previous, previous_to_current);
    return minimum_leg >= kMinimumBasisDistance &&
           older_to_current <= minimum_leg * kReturnDistanceRatio;
  }

  static bool stable_window(const BasisSnapshot& older,
                            const BasisSnapshot& previous,
                            const BasisSnapshot& current) {
    return basis_distance(older, previous) < kMinimumBasisDistance &&
           basis_distance(previous, current) < kMinimumBasisDistance;
  }

  static void populate_event(Event* event,
                             int bone_slot,
                             u64 model_name_hash,
                             const Sample& older,
                             const Sample& previous,
                             const Sample& current) {
    event->bone_slot = bone_slot;
    event->model_name_hash = model_name_hash;
    event->older_frame_id = older.frame_id;
    event->previous_frame_id = previous.frame_id;
    event->current_frame_id = current.frame_id;
    event->older_matrix_hash = older.matrix_hash;
    event->previous_matrix_hash = previous.matrix_hash;
    event->current_matrix_hash = current.matrix_hash;
    event->previous_axis_norm_x = previous.axis_norm_x;
    event->previous_axis_norm_y = previous.axis_norm_y;
    event->previous_axis_norm_z = previous.axis_norm_z;
    event->current_axis_norm_x = current.axis_norm_x;
    event->current_axis_norm_y = current.axis_norm_y;
    event->current_axis_norm_z = current.axis_norm_z;
    event->previous_scale = previous.scale;
    event->current_scale = current.scale;
    event->previous_aspect = previous.aspect;
    event->current_aspect = current.aspect;
    event->older_source_base = older.source_base;
    event->previous_source_base = previous.source_base;
    event->current_source_base = current.source_base;
    event->input_root_bone = current.provenance.input_root_bone;
    event->older_producer_serial = older.provenance.producer_serial;
    event->previous_producer_serial = previous.provenance.producer_serial;
    event->current_producer_serial = current.provenance.producer_serial;
    event->older_input_root_hash = older.provenance.input_root_hash;
    event->previous_input_root_hash = previous.provenance.input_root_hash;
    event->current_input_root_hash = current.provenance.input_root_hash;
    event->older_camera_hash = older.provenance.camera_hash;
    event->previous_camera_hash = previous.provenance.camera_hash;
    event->current_camera_hash = current.provenance.camera_hash;
    event->previous_bind_pose_hash = previous.provenance.bind_pose_hash;
    event->current_bind_pose_hash = current.provenance.bind_pose_hash;
    event->previous_output_expected_distance = previous.provenance.output_expected_distance;
    event->current_output_expected_distance = current.provenance.output_expected_distance;
    event->previous_input_translation_x = previous.provenance.input_translation_x;
    event->previous_input_translation_y = previous.provenance.input_translation_y;
    event->previous_input_translation_z = previous.provenance.input_translation_z;
    event->current_input_translation_x = current.provenance.input_translation_x;
    event->current_input_translation_y = current.provenance.input_translation_y;
    event->current_input_translation_z = current.provenance.input_translation_z;

    if (!previous.provenance.valid() || !current.provenance.valid()) {
      return;
    }
    event->input_previous_to_current_distance =
        basis_distance(previous.provenance.input_root_basis, current.provenance.input_root_basis);
    event->output_previous_to_current_distance =
        basis_distance(previous.provenance.output_basis, current.provenance.output_basis);
    event->camera_previous_to_current_distance =
        basis_distance(previous.provenance.camera_basis, current.provenance.camera_basis);
    if (!older.provenance.valid()) {
      return;
    }
    event->input_older_to_previous_distance =
        basis_distance(older.provenance.input_root_basis, previous.provenance.input_root_basis);
    event->input_older_to_current_distance =
        basis_distance(older.provenance.input_root_basis, current.provenance.input_root_basis);
    event->output_older_to_previous_distance =
        basis_distance(older.provenance.output_basis, previous.provenance.output_basis);
    event->output_older_to_current_distance =
        basis_distance(older.provenance.output_basis, current.provenance.output_basis);
    event->camera_older_to_previous_distance =
        basis_distance(older.provenance.camera_basis, previous.provenance.camera_basis);
    event->camera_older_to_current_distance =
        basis_distance(older.provenance.camera_basis, current.provenance.camera_basis);
  }

  static double ratio(double a, double b) {
    const double minimum = std::min(a, b);
    return minimum > 0.0 ? std::max(a, b) / minimum : 0.0;
  }

  std::array<History, kBoneSlotCount> m_histories = {};
};

}  // namespace metal_merc_transform_trace
