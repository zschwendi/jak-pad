#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

#include "common/common_types.h"

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
};

class Registry {
 public:
  bool record(u64 output_base,
              u64 joints_base,
              u64 bones_base,
              u64 bone_count,
              u64 camera_base,
              const u8* ee_memory,
              std::size_t ee_memory_size) {
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

 private:
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
