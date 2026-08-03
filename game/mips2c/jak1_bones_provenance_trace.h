#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

#include "common/common_types.h"
#include "game/mips2c/jak1_target_control_capture.h"

namespace jak1_bones_provenance_trace {

constexpr std::size_t kTransformBytes = 4 * 16;
constexpr std::size_t kJointStride = 0x50;
constexpr std::size_t kJointBindPoseOffset = 0x10;
constexpr std::size_t kGoalBasicPointerBias = 4;
constexpr std::size_t kBoneStride = 96;
constexpr std::size_t kOutputStride = 128;
constexpr std::size_t kRootAnchorCount = 3;
constexpr std::size_t kCalculationCount = 64;
constexpr u32 kMaximumBoneCount = 128;

// Asserted Jak 1 layouts from decompiler/config/jak1/all-types.gc. The observer verifies the
// runtime type tag and allocated size before using any of these diagnostic-only offsets.
constexpr std::size_t kGoalTypeTagBytes = 4;
constexpr std::size_t kTypeAllocatedSizeOffset = 8;
constexpr std::size_t kTargetStateOffset = 56;
constexpr std::size_t kTargetRootOffset = 112;
constexpr u16 kTargetMinimumSize = 0x250;
constexpr std::size_t kControlQuatOffset = 32;
constexpr std::size_t kControlDirTargOffset = 112;
constexpr std::size_t kControlQuatForControlOffset = 496;
constexpr std::size_t kControlCpadOffset = 668;
constexpr std::size_t kControlTurnToTargetOffset = 720;
constexpr std::size_t kControlPadMagnitudeOffset = 912;
constexpr std::size_t kControlTargetAttackIdOffset = 2392;
constexpr u16 kControlMinimumSize = 0x4a3c;
constexpr std::size_t kCpadButtonAbsOffset = 44;
constexpr std::size_t kCpadButtonRelOffset = 60;
constexpr std::size_t kCpadStickDirectionOffset = 72;
constexpr std::size_t kCpadStickSpeedOffset = 76;
constexpr std::size_t kCpadLeftXOffset = 10;
constexpr std::size_t kCpadLeftYOffset = 11;
constexpr u16 kCpadMinimumSize = 0x88;

struct FacingSnapshot {
  bool valid = false;
  double x = 0.0;
  double z = 0.0;
};

struct TargetControlSnapshot {
  bool valid = false;
  jak1_target_control_capture::Stage capture_stage =
      jak1_target_control_capture::Stage::NOT_ATTEMPTED;
  jak1_target_control_capture::Result capture_result =
      jak1_target_control_capture::Result::NOT_ATTEMPTED;
  u32 target_address = 0;
  u32 control_address = 0;
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
};

struct TargetCaptureContext {
  u32 target_address = 0;
  u32 target_type = 0;
  u32 control_info_type = 0;
  u32 cpad_info_type = 0;
};

struct TransformSnapshot {
  bool valid = false;
  std::array<u8, kTransformBytes> bytes = {};
};

struct Calculation {
  bool valid = false;
  u64 serial = 0;
  u32 output_base = 0;
  u32 joints_base = 0;
  u32 bones_base = 0;
  u32 camera_base = 0;
  u32 bone_count = 0;
  TransformSnapshot camera;
  // Bone nodes 1, 2 and 3 are align, prejoint and main respectively.
  std::array<TransformSnapshot, kRootAnchorCount> root_anchors = {};
  std::array<TransformSnapshot, kRootAnchorCount> root_bind_poses = {};
  TargetControlSnapshot target_control;
};

class Registry {
 public:
  bool record(u64 output_base,
              u64 joints_base,
              u64 bones_base,
              u64 bone_count,
              u64 camera_base,
              const u8* ee_memory,
              std::size_t ee_memory_size,
              const TargetCaptureContext& target_context = {}) {
    if (!ee_memory || !bone_count || bone_count > kMaximumBoneCount || output_base > UINT32_MAX ||
        joints_base > UINT32_MAX || bones_base > UINT32_MAX || camera_base > UINT32_MAX ||
        !span_fits(output_base, bone_count * kOutputStride, ee_memory_size) ||
        !span_fits(bones_base, bone_count * kBoneStride, ee_memory_size) ||
        !span_fits(camera_base, kTransformBytes, ee_memory_size)) {
      return false;
    }

    Calculation calculation;
    calculation.valid = true;
    calculation.serial = ++m_serial;
    calculation.output_base = static_cast<u32>(output_base);
    calculation.joints_base = static_cast<u32>(joints_base);
    calculation.bones_base = static_cast<u32>(bones_base);
    calculation.camera_base = static_cast<u32>(camera_base);
    calculation.bone_count = static_cast<u32>(bone_count);
    copy_snapshot(ee_memory + camera_base, &calculation.camera);
    calculation.target_control = capture_target_control(target_context, ee_memory, ee_memory_size);

    for (std::size_t anchor = 0; anchor < calculation.root_anchors.size(); anchor++) {
      const std::size_t bone_index = anchor + 1;
      if (bone_index >= bone_count) {
        continue;
      }
      copy_snapshot(ee_memory + bones_base + bone_index * kBoneStride,
                    &calculation.root_anchors[anchor]);
      const u64 normalized_joints_base = joints_base & 0x7fffffff;
      if (normalized_joints_base < kGoalBasicPointerBias) {
        continue;
      }
      const u64 bind_pose_address = normalized_joints_base - kGoalBasicPointerBias +
                                    kJointBindPoseOffset + anchor * kJointStride;
      if (span_fits(bind_pose_address, kTransformBytes, ee_memory_size)) {
        copy_snapshot(ee_memory + bind_pose_address, &calculation.root_bind_poses[anchor]);
      }
    }

    m_calculations[m_next] = calculation;
    m_next = (m_next + 1) % m_calculations.size();
    return true;
  }

  std::optional<Calculation> find_output_base(u32 output_base) const {
    std::optional<Calculation> latest;
    for (const auto& calculation : m_calculations) {
      if (!calculation.valid || calculation.output_base != output_base ||
          (latest && latest->serial >= calculation.serial)) {
        continue;
      }
      latest = calculation;
    }
    return latest;
  }

  std::optional<Calculation> find_source_address(u32 source_address) const {
    std::optional<Calculation> latest;
    for (const auto& calculation : m_calculations) {
      if (!calculation.valid || source_address < calculation.output_base) {
        continue;
      }
      const u64 offset = static_cast<u64>(source_address) - calculation.output_base;
      if (offset % kOutputStride || offset >= calculation.bone_count * kOutputStride ||
          (latest && latest->serial >= calculation.serial)) {
        continue;
      }
      latest = calculation;
    }
    return latest;
  }

  void reset() {
    m_calculations = {};
    m_next = 0;
    m_serial = 0;
  }

  static TargetControlSnapshot capture_target_control(const TargetCaptureContext& context,
                                                      const u8* ee_memory,
                                                      std::size_t ee_memory_size) {
    TargetControlSnapshot out;
    out.capture_stage = jak1_target_control_capture::Stage::CONTEXT;
    if (!ee_memory) {
      out.capture_result = jak1_target_control_capture::Result::MISSING_MEMORY;
      return out;
    }
    if (!context.target_address) {
      out.capture_result = jak1_target_control_capture::Result::MISSING_TARGET_ADDRESS;
      return out;
    }
    if (!context.target_type || !context.control_info_type || !context.cpad_info_type) {
      out.capture_result = jak1_target_control_capture::Result::MISSING_TYPE_ADDRESS;
      return out;
    }

    const u32 target = normalize_goal_pointer(context.target_address);
    const u32 target_type = normalize_goal_pointer(context.target_type);
    const u32 control_type = normalize_goal_pointer(context.control_info_type);
    const u32 cpad_type = normalize_goal_pointer(context.cpad_info_type);
    if (!target) {
      out.capture_result = jak1_target_control_capture::Result::MISSING_TARGET_ADDRESS;
      return out;
    }
    if (!target_type || !control_type || !cpad_type) {
      out.capture_result = jak1_target_control_capture::Result::MISSING_TYPE_ADDRESS;
      return out;
    }
    out.capture_stage = jak1_target_control_capture::Stage::TARGET;
    out.capture_result = validate_exact_type(target, target_type, kTargetMinimumSize, ee_memory,
                                             ee_memory_size);
    if (out.capture_result != jak1_target_control_capture::Result::SUCCESS) {
      return out;
    }

    out.capture_stage = jak1_target_control_capture::Stage::CONTROL_POINTER;
    u32 raw_control = 0;
    if (!read_value(ee_memory, ee_memory_size, target + kTargetRootOffset, &raw_control)) {
      out.capture_result = jak1_target_control_capture::Result::POINTER_READ_FAILED;
      return out;
    }
    const u32 control = normalize_goal_pointer(raw_control);
    if (!control) {
      out.capture_result = jak1_target_control_capture::Result::NULL_OBJECT_POINTER;
      return out;
    }
    out.capture_stage = jak1_target_control_capture::Stage::CONTROL;
    out.capture_result = validate_exact_type(control, control_type, kControlMinimumSize, ee_memory,
                                             ee_memory_size);
    if (out.capture_result != jak1_target_control_capture::Result::SUCCESS) {
      return out;
    }

    out.capture_stage = jak1_target_control_capture::Stage::CPAD_POINTER;
    u32 raw_cpad = 0;
    if (!read_value(ee_memory, ee_memory_size, control + kControlCpadOffset, &raw_cpad)) {
      out.capture_result = jak1_target_control_capture::Result::POINTER_READ_FAILED;
      return out;
    }
    const u32 cpad = normalize_goal_pointer(raw_cpad);
    if (!cpad) {
      out.capture_result = jak1_target_control_capture::Result::NULL_OBJECT_POINTER;
      return out;
    }
    out.capture_stage = jak1_target_control_capture::Stage::CPAD;
    out.capture_result =
        validate_exact_type(cpad, cpad_type, kCpadMinimumSize, ee_memory, ee_memory_size);
    if (out.capture_result != jak1_target_control_capture::Result::SUCCESS) {
      return out;
    }

    std::array<float, 4> dir_targ = {};
    std::array<float, 4> quat_for_control = {};
    std::array<float, 4> render_quat = {};
    std::array<float, 4> turn_to_target = {};
    float stick_direction = 0.0f;
    float stick_speed = 0.0f;
    float pad_magnitude = 0.0f;
    out.capture_stage = jak1_target_control_capture::Stage::FIELDS;
    if (!read_value(ee_memory, ee_memory_size, control + kControlDirTargOffset, &dir_targ) ||
        !read_value(ee_memory, ee_memory_size, control + kControlQuatForControlOffset,
                    &quat_for_control) ||
        !read_value(ee_memory, ee_memory_size, control + kControlQuatOffset, &render_quat) ||
        !read_value(ee_memory, ee_memory_size, control + kControlTurnToTargetOffset,
                    &turn_to_target) ||
        !read_value(ee_memory, ee_memory_size, cpad + kCpadStickDirectionOffset,
                    &stick_direction) ||
        !read_value(ee_memory, ee_memory_size, cpad + kCpadStickSpeedOffset, &stick_speed) ||
        !read_value(ee_memory, ee_memory_size, control + kControlPadMagnitudeOffset,
                    &pad_magnitude)) {
      out.capture_result = jak1_target_control_capture::Result::FIELD_READ_FAILED;
      return out;
    }
    if (!all_finite(dir_targ) || !all_finite(quat_for_control) || !all_finite(render_quat) ||
        !all_finite(turn_to_target) || !std::isfinite(stick_direction) ||
        !std::isfinite(stick_speed) || !std::isfinite(pad_magnitude)) {
      out.capture_result = jak1_target_control_capture::Result::NONFINITE_FIELD;
      return out;
    }

    u32 raw_state = 0;
    read_value(ee_memory, ee_memory_size, target + kTargetStateOffset, &raw_state);
    const u32 state = normalize_goal_pointer(raw_state);
    out.target_state_id = span_fits(state, kGoalTypeTagBytes, ee_memory_size) ? state : 0;
    out.target_address = target;
    out.control_address = control;
    read_value(ee_memory, ee_memory_size, control + kControlTargetAttackIdOffset,
               &out.target_attack_id);
    read_value(ee_memory, ee_memory_size, cpad + kCpadButtonAbsOffset, &out.button0_abs);
    read_value(ee_memory, ee_memory_size, cpad + kCpadButtonRelOffset, &out.button0_rel);
    read_value(ee_memory, ee_memory_size, cpad + kCpadLeftXOffset, &out.left_x);
    read_value(ee_memory, ee_memory_size, cpad + kCpadLeftYOffset, &out.left_y);
    out.stick_direction = stick_direction;
    out.stick_speed = stick_speed;
    out.pad_magnitude = pad_magnitude;
    out.raw_dir_targ = dir_targ;
    out.raw_quat_for_control = quat_for_control;
    out.raw_render_quat = render_quat;
    out.raw_turn_to_target = turn_to_target;
    out.intent_forward = normalize_xz(turn_to_target[0], turn_to_target[2]);
    out.desired_forward = quaternion_forward_xz(dir_targ);
    out.control_forward = quaternion_forward_xz(quat_for_control);
    out.render_forward = quaternion_forward_xz(render_quat);
    out.valid = out.intent_forward.valid && out.desired_forward.valid &&
                out.control_forward.valid && out.render_forward.valid;
    if (!out.valid) {
      out.capture_stage = jak1_target_control_capture::Stage::FACING;
      out.capture_result = jak1_target_control_capture::Result::INVALID_FACING;
      return out;
    }
    out.capture_stage = jak1_target_control_capture::Stage::COMPLETE;
    out.capture_result = jak1_target_control_capture::Result::SUCCESS;
    return out;
  }

 private:
  static u32 normalize_goal_pointer(u32 address) { return address & 0x7fffffff; }

  template <typename T>
  static bool read_value(const u8* memory, std::size_t memory_size, u64 address, T* out) {
    if (!out || !span_fits(address, sizeof(T), memory_size)) {
      return false;
    }
    std::memcpy(out, memory + address, sizeof(T));
    return true;
  }

  static jak1_target_control_capture::Result validate_exact_type(u32 address,
                                                                 u32 expected_type,
                                                                 u16 minimum_size,
                                                                 const u8* memory,
                                                                 std::size_t memory_size) {
    if (address < kGoalTypeTagBytes || !span_fits(address, minimum_size, memory_size)) {
      return jak1_target_control_capture::Result::OBJECT_SPAN_INVALID;
    }
    u32 actual_type = 0;
    u16 allocated_size = 0;
    if (!read_value(memory, memory_size, address - kGoalTypeTagBytes, &actual_type)) {
      return jak1_target_control_capture::Result::FIELD_READ_FAILED;
    }
    if (normalize_goal_pointer(actual_type) != expected_type) {
      return jak1_target_control_capture::Result::TYPE_TAG_MISMATCH;
    }
    if (!read_value(memory, memory_size, expected_type + kTypeAllocatedSizeOffset,
                    &allocated_size)) {
      return jak1_target_control_capture::Result::TYPE_DESCRIPTOR_SPAN_INVALID;
    }
    if (allocated_size < minimum_size) {
      return jak1_target_control_capture::Result::TYPE_ALLOCATED_SIZE_TOO_SMALL;
    }
    return jak1_target_control_capture::Result::SUCCESS;
  }

  static bool all_finite(const std::array<float, 4>& values) {
    return std::all_of(values.begin(), values.end(),
                       [](float value) { return std::isfinite(value); });
  }

  static FacingSnapshot normalize_xz(double x, double z) {
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

  static FacingSnapshot quaternion_forward_xz(const std::array<float, 4>& quaternion) {
    const double x = quaternion[0];
    const double y = quaternion[1];
    const double z = quaternion[2];
    const double w = quaternion[3];
    const double norm = std::sqrt(x * x + y * y + z * z + w * w);
    if (!std::isfinite(norm) || norm <= 1e-12) {
      return {};
    }
    const double nx = x / norm;
    const double ny = y / norm;
    const double nz = z / norm;
    const double nw = w / norm;
    return normalize_xz(2.0 * (nx * nz + nw * ny), 1.0 - 2.0 * (nx * nx + ny * ny));
  }

  static bool span_fits(u64 start, u64 size, std::size_t memory_size) {
    return start <= memory_size && size <= memory_size - start;
  }

  static void copy_snapshot(const u8* source, TransformSnapshot* destination) {
    destination->valid = true;
    std::memcpy(destination->bytes.data(), source, destination->bytes.size());
  }

  std::array<Calculation, kCalculationCount> m_calculations = {};
  std::size_t m_next = 0;
  u64 m_serial = 0;
};

inline Registry& registry() {
  static Registry instance;
  return instance;
}

}  // namespace jak1_bones_provenance_trace
