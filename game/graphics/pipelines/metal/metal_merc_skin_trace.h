#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

#include "common/common_types.h"
#include "common/custom_data/Tfrag3Data.h"

namespace metal_merc_skin_trace {

constexpr std::size_t kMercBoneSlotCount = 128;
constexpr std::size_t kShaderMatrixStrideFloats = 32;
constexpr std::size_t kMaximumTupleGroupsPerDraw = 64;
constexpr double kCollapsedAxisRatio = 8.0;
constexpr double kCollapsedNormalizedDeterminant = 0.125;

enum class VertexStream : u8 {
  STATIC = 0,
  MODIFIED = 1,
};

enum class Coverage : u8 {
  NONE = 0,
  LOCALIZED = 1,
  MIXED = 2,
  WIDESPREAD = 3,
};

struct InfluenceTuple {
  std::array<float, 3> weights = {};
  std::array<u8, 3> matrices = {};
};

inline InfluenceTuple make_influence_tuple(const tfrag3::MercVertex& vertex) {
  InfluenceTuple out;
  memcpy(out.weights.data(), vertex.weights, sizeof(vertex.weights));
  memcpy(out.matrices.data(), vertex.mats, sizeof(vertex.mats));
  return out;
}

inline InfluenceTuple make_influence_tuple(const void* vertex_bytes) {
  InfluenceTuple out;
  const auto* bytes = static_cast<const u8*>(vertex_bytes);
  memcpy(out.weights.data(), bytes + offsetof(tfrag3::MercVertex, weights),
         sizeof(out.weights));
  memcpy(out.matrices.data(), bytes + offsetof(tfrag3::MercVertex, mats),
         sizeof(out.matrices));
  return out;
}

inline bool same_tuple(const InfluenceTuple& a, const InfluenceTuple& b) {
  return memcmp(a.weights.data(), b.weights.data(), sizeof(a.weights)) == 0 &&
         memcmp(a.matrices.data(), b.matrices.data(), sizeof(a.matrices)) == 0;
}

inline bool has_distinct_positive_bones(const InfluenceTuple& tuple) {
  std::array<u8, 3> slots = {};
  std::size_t slot_count = 0;
  for (std::size_t influence = 0; influence < tuple.weights.size(); influence++) {
    if (!(tuple.weights[influence] > 0.f)) {
      continue;
    }
    const u8 slot = tuple.matrices[influence];
    if (std::find(slots.begin(), slots.begin() + slot_count, slot) ==
        slots.begin() + slot_count) {
      slots[slot_count++] = slot;
    }
  }
  return slot_count >= 2;
}

struct TupleGroup {
  InfluenceTuple tuple;
  u32 representative_vertex = 0;
  u32 vertex_multiplicity = 0;
};

struct DrawProfile {
  bool valid = false;
  VertexStream stream = VertexStream::STATIC;
  u32 first_index = 0;
  u32 index_count = 0;
  u32 unique_vertex_count = 0;
  u32 multi_bone_vertex_count = 0;
  u64 identity = 0;
  std::array<u64, 2> used_bone_slots = {};
  u32 capacity_dropped_vertices = 0;
  std::vector<TupleGroup> tuple_groups;
};

inline void retain_used_bone_slots(const InfluenceTuple& tuple,
                                   std::array<u64, 2>* used_bone_slots) {
  for (std::size_t influence = 0; influence < tuple.weights.size(); influence++) {
    if (influence > 0 && !(tuple.weights[influence] > 0.f)) {
      continue;
    }
    const u8 slot = tuple.matrices[influence];
    if (slot >= kMercBoneSlotCount) {
      continue;
    }
    (*used_bone_slots)[slot / 64] |= 1ull << (slot % 64);
  }
}

inline void merge_used_bone_slots(const DrawProfile& profile,
                                  std::array<u64, 2>* used_bone_slots) {
  (*used_bone_slots)[0] |= profile.used_bone_slots[0];
  (*used_bone_slots)[1] |= profile.used_bone_slots[1];
}

inline void merge_active_draw_slots(bool enabled,
                                    bool use_modified_path,
                                    const std::vector<DrawProfile>& fixed_draws,
                                    const std::vector<DrawProfile>& modified_draws,
                                    const std::vector<DrawProfile>& all_draws,
                                    std::array<u64, 2>* used_bone_slots) {
  if (!enabled) {
    return;
  }
  const auto merge_profiles = [&](const std::vector<DrawProfile>& profiles) {
    for (const auto& profile : profiles) {
      merge_used_bone_slots(profile, used_bone_slots);
    }
  };
  if (use_modified_path) {
    merge_profiles(fixed_draws);
    merge_profiles(modified_draws);
  } else {
    merge_profiles(all_draws);
  }
}

inline u64 hash_palette(const float* palette,
                        std::size_t palette_count,
                        const std::array<u64, 2>& used_bone_slots,
                        std::size_t matrix_stride_floats = kShaderMatrixStrideFloats) {
  if (!palette || matrix_stride_floats < 28) {
    return 0;
  }

  constexpr std::array<std::size_t, 25> kSemanticLanes = {
      0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
      13, 14, 15, 16, 17, 18, 20, 21, 22, 24, 25, 26};
  u64 hash = 0xcbf29ce484222325;
  for (std::size_t slot = 0; slot < palette_count && slot < kMercBoneSlotCount; slot++) {
    if (!(used_bone_slots[slot / 64] & (1ull << (slot % 64)))) {
      continue;
    }
    hash = 1099511628211ull * (static_cast<u64>(slot) ^ hash);
    const float* matrix = palette + slot * matrix_stride_floats;
    for (const std::size_t lane : kSemanticLanes) {
      const auto* bytes = reinterpret_cast<const u8*>(matrix + lane);
      for (std::size_t byte = 0; byte < sizeof(float); byte++) {
        hash = 1099511628211ull * (static_cast<u64>(bytes[byte]) ^ hash);
      }
    }
  }
  return hash;
}

inline u64 hash_draw_palette(const DrawProfile& profile,
                             const float* palette,
                             std::size_t palette_count,
                             std::size_t matrix_stride_floats = kShaderMatrixStrideFloats) {
  return hash_palette(palette, palette_count, profile.used_bone_slots, matrix_stride_floats);
}

inline DrawProfile build_draw_profile(const std::vector<tfrag3::MercVertex>& vertices,
                                      const std::vector<u32>& indices,
                                      const tfrag3::MercDraw& draw,
                                      VertexStream stream,
                                      u64 identity) {
  DrawProfile out;
  out.stream = stream;
  out.first_index = draw.first_index;
  out.index_count = draw.index_count;
  out.identity = identity;
  if (static_cast<u64>(draw.first_index) + draw.index_count > indices.size()) {
    return out;
  }

  std::vector<u32> unique_vertices;
  unique_vertices.reserve(draw.index_count);
  for (u32 index_offset = 0; index_offset < draw.index_count; index_offset++) {
    const u32 vertex_index = indices[draw.first_index + index_offset];
    if (vertex_index != UINT32_MAX && vertex_index < vertices.size()) {
      unique_vertices.push_back(vertex_index);
    }
  }
  std::sort(unique_vertices.begin(), unique_vertices.end());
  unique_vertices.erase(std::unique(unique_vertices.begin(), unique_vertices.end()),
                        unique_vertices.end());
  out.unique_vertex_count = static_cast<u32>(unique_vertices.size());

  for (u32 vertex_index : unique_vertices) {
    const InfluenceTuple tuple = make_influence_tuple(vertices[vertex_index]);
    retain_used_bone_slots(tuple, &out.used_bone_slots);
    if (!has_distinct_positive_bones(tuple)) {
      continue;
    }
    out.multi_bone_vertex_count++;
    auto group = std::find_if(out.tuple_groups.begin(), out.tuple_groups.end(),
                              [&](const TupleGroup& candidate) {
                                return same_tuple(candidate.tuple, tuple);
                              });
    if (group == out.tuple_groups.end()) {
      if (out.tuple_groups.size() == kMaximumTupleGroupsPerDraw) {
        out.capacity_dropped_vertices++;
      } else {
        out.tuple_groups.push_back({tuple, vertex_index, 1});
      }
    } else {
      group->vertex_multiplicity++;
    }
  }
  out.valid = true;
  return out;
}

struct BasisObservation {
  bool valid = false;
  bool collapse_candidate = false;
  double axis_norm_x = 0.0;
  double axis_norm_y = 0.0;
  double axis_norm_z = 0.0;
  double axis_ratio = 0.0;
  double normalized_abs_determinant = 0.0;
};

// Mirrors the position half of merc_skin. Influence zero is always evaluated;
// influences one and two are evaluated only when their weights are positive.
inline BasisObservation analyze_weighted_basis(
    const InfluenceTuple& tuple,
    const float* palette,
    std::size_t palette_count,
    std::size_t matrix_stride_floats = kShaderMatrixStrideFloats) {
  BasisObservation out;
  if (!palette || matrix_stride_floats < 16 || !has_distinct_positive_bones(tuple)) {
    return out;
  }

  std::array<double, 9> basis = {};
  for (std::size_t influence = 0; influence < tuple.weights.size(); influence++) {
    const float weight = tuple.weights[influence];
    if (influence > 0 && !(weight > 0.f)) {
      continue;
    }
    const u8 slot = tuple.matrices[influence];
    if (slot >= palette_count || !std::isfinite(weight)) {
      return out;
    }
    const float* matrix = palette + static_cast<std::size_t>(slot) * matrix_stride_floats;
    for (std::size_t column = 0; column < 3; column++) {
      for (std::size_t row = 0; row < 3; row++) {
        const float lane = matrix[column * 4 + row];
        if (!std::isfinite(lane)) {
          return out;
        }
        basis[column * 3 + row] -= static_cast<double>(weight) * lane;
      }
    }
  }

  const auto norm = [&](std::size_t column) {
    const double* axis = basis.data() + column * 3;
    return std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  };
  out.axis_norm_x = norm(0);
  out.axis_norm_y = norm(1);
  out.axis_norm_z = norm(2);
  const double minimum = std::min({out.axis_norm_x, out.axis_norm_y, out.axis_norm_z});
  const double maximum = std::max({out.axis_norm_x, out.axis_norm_y, out.axis_norm_z});
  const double determinant =
      basis[0] * (basis[4] * basis[8] - basis[5] * basis[7]) -
      basis[3] * (basis[1] * basis[8] - basis[2] * basis[7]) +
      basis[6] * (basis[1] * basis[5] - basis[2] * basis[4]);
  const double norm_product = out.axis_norm_x * out.axis_norm_y * out.axis_norm_z;
  if (!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(determinant) ||
      minimum <= 0.0 || norm_product <= 0.0) {
    return out;
  }
  out.axis_ratio = maximum / minimum;
  out.normalized_abs_determinant = std::abs(determinant) / norm_product;
  out.valid = std::isfinite(out.axis_ratio) &&
              std::isfinite(out.normalized_abs_determinant);
  out.collapse_candidate =
      out.valid && (out.axis_ratio >= kCollapsedAxisRatio ||
                    out.normalized_abs_determinant <= kCollapsedNormalizedDeterminant);
  return out;
}

inline Coverage classify_coverage(u64 total, u64 affected) {
  if (!total || !affected) {
    return Coverage::NONE;
  }
  if (affected * 4 < total) {
    return Coverage::LOCALIZED;
  }
  if (affected * 4 <= total * 3) {
    return Coverage::MIXED;
  }
  return Coverage::WIDESPREAD;
}

struct SkinStats {
  // Vertices are unique within one actual base draw, then summed across draws.
  u64 profiled_base_draws = 0;
  u64 profiled_vertices = 0;
  u64 multi_bone_vertices = 0;
  u64 affected_vertices = 0;
  u64 static_multi_bone_vertices = 0;
  u64 static_affected_vertices = 0;
  u64 modified_multi_bone_vertices = 0;
  u64 modified_affected_vertices = 0;
  u64 tuple_groups = 0;
  u64 affected_tuple_groups = 0;
  u64 valid_basis_groups = 0;
  u64 invalid_basis_groups = 0;
  u64 affected_draws = 0;
  u64 affected_effects = 0;
  u64 profile_tuple_mismatches = 0;
  u64 profile_capacity_drops = 0;
  double maximum_axis_ratio = 0.0;
  double minimum_normalized_abs_determinant = 0.0;
  Coverage coverage = Coverage::NONE;

  void add(const SkinStats& other) {
    const u64 previous_valid_basis_groups = valid_basis_groups;
    profiled_base_draws += other.profiled_base_draws;
    profiled_vertices += other.profiled_vertices;
    multi_bone_vertices += other.multi_bone_vertices;
    affected_vertices += other.affected_vertices;
    static_multi_bone_vertices += other.static_multi_bone_vertices;
    static_affected_vertices += other.static_affected_vertices;
    modified_multi_bone_vertices += other.modified_multi_bone_vertices;
    modified_affected_vertices += other.modified_affected_vertices;
    tuple_groups += other.tuple_groups;
    affected_tuple_groups += other.affected_tuple_groups;
    valid_basis_groups += other.valid_basis_groups;
    invalid_basis_groups += other.invalid_basis_groups;
    affected_draws += other.affected_draws;
    affected_effects += other.affected_effects;
    profile_tuple_mismatches += other.profile_tuple_mismatches;
    profile_capacity_drops += other.profile_capacity_drops;
    maximum_axis_ratio = std::max(maximum_axis_ratio, other.maximum_axis_ratio);
    if (other.valid_basis_groups && !previous_valid_basis_groups) {
      minimum_normalized_abs_determinant = other.minimum_normalized_abs_determinant;
    } else if (other.valid_basis_groups) {
      minimum_normalized_abs_determinant =
          std::min(minimum_normalized_abs_determinant,
                   other.minimum_normalized_abs_determinant);
    }
    coverage = classify_coverage(multi_bone_vertices, affected_vertices);
  }
};

inline SkinStats analyze_draw(const DrawProfile& profile,
                              const void* bound_vertex_bytes,
                              std::size_t bound_vertex_count,
                              const float* bound_palette,
                              std::size_t palette_count,
                              std::size_t matrix_stride_floats = kShaderMatrixStrideFloats,
                              std::size_t vertex_stride_bytes = sizeof(tfrag3::MercVertex)) {
  SkinStats out;
  if (!profile.valid || !bound_vertex_bytes || !bound_palette ||
      vertex_stride_bytes < sizeof(tfrag3::MercVertex)) {
    return out;
  }
  out.profiled_base_draws = 1;
  out.profiled_vertices = profile.unique_vertex_count;
  out.multi_bone_vertices = profile.multi_bone_vertex_count;
  out.tuple_groups = profile.tuple_groups.size();
  out.profile_capacity_drops = profile.capacity_dropped_vertices;
  if (profile.stream == VertexStream::MODIFIED) {
    out.modified_multi_bone_vertices = profile.multi_bone_vertex_count;
  } else {
    out.static_multi_bone_vertices = profile.multi_bone_vertex_count;
  }

  bool have_minimum_determinant = false;
  for (const auto& group : profile.tuple_groups) {
    if (group.representative_vertex >= bound_vertex_count) {
      out.profile_tuple_mismatches++;
      out.invalid_basis_groups++;
      continue;
    }
    const auto* vertex = static_cast<const u8*>(bound_vertex_bytes) +
                         group.representative_vertex * vertex_stride_bytes;
    const InfluenceTuple bound_tuple = make_influence_tuple(vertex);
    if (!same_tuple(group.tuple, bound_tuple)) {
      out.profile_tuple_mismatches++;
      out.invalid_basis_groups++;
      continue;
    }
    const BasisObservation observation =
        analyze_weighted_basis(bound_tuple, bound_palette, palette_count, matrix_stride_floats);
    if (!observation.valid) {
      out.invalid_basis_groups++;
      continue;
    }
    out.valid_basis_groups++;
    out.maximum_axis_ratio = std::max(out.maximum_axis_ratio, observation.axis_ratio);
    if (!have_minimum_determinant) {
      out.minimum_normalized_abs_determinant = observation.normalized_abs_determinant;
      have_minimum_determinant = true;
    } else {
      out.minimum_normalized_abs_determinant =
          std::min(out.minimum_normalized_abs_determinant,
                   observation.normalized_abs_determinant);
    }
    if (observation.collapse_candidate) {
      out.affected_tuple_groups++;
      out.affected_vertices += group.vertex_multiplicity;
    }
  }
  if (profile.stream == VertexStream::MODIFIED) {
    out.modified_affected_vertices = out.affected_vertices;
  } else {
    out.static_affected_vertices = out.affected_vertices;
  }
  out.affected_draws = out.affected_vertices ? 1 : 0;
  out.coverage = classify_coverage(out.multi_bone_vertices, out.affected_vertices);
  return out;
}

struct PacketResult {
  bool accepted = false;
  bool unique_source = false;
  bool repeated = false;
  bool conflicting_palette = false;
  bool capacity_dropped = false;
  u32 packet_sequence = 0;
};

struct DrawResult {
  bool accepted = false;
  bool unique = false;
  bool repeated = false;
  bool conflicting_palette = false;
  bool capacity_dropped = false;
};

struct EffectResult {
  bool unique = false;
  bool capacity_dropped = false;
};

struct DuplicationStats {
  u64 packets = 0;
  u64 unique_source_bases = 0;
  u64 repeated_packets = 0;
  u64 repeated_packets_same_palette = 0;
  u64 repeated_packets_conflicting_palette = 0;
  u64 actual_base_draws = 0;
  u64 unique_base_draws = 0;
  u64 repeated_base_draws = 0;
  u64 repeated_base_draws_same_palette = 0;
  u64 repeated_base_draws_conflicting_palette = 0;
  u64 capacity_drops = 0;
  u64 bound_palette_hash_mismatches = 0;

  void add(const DuplicationStats& other) {
    packets += other.packets;
    unique_source_bases += other.unique_source_bases;
    repeated_packets += other.repeated_packets;
    repeated_packets_same_palette += other.repeated_packets_same_palette;
    repeated_packets_conflicting_palette += other.repeated_packets_conflicting_palette;
    actual_base_draws += other.actual_base_draws;
    unique_base_draws += other.unique_base_draws;
    repeated_base_draws += other.repeated_base_draws;
    repeated_base_draws_same_palette += other.repeated_base_draws_same_palette;
    repeated_base_draws_conflicting_palette += other.repeated_base_draws_conflicting_palette;
    capacity_drops += other.capacity_drops;
    bound_palette_hash_mismatches += other.bound_palette_hash_mismatches;
  }
};

class FrameTracker {
 public:
  static constexpr std::size_t kMaximumSources = 64;
  static constexpr std::size_t kMaximumBaseDraws = 512;
  static constexpr std::size_t kMaximumAffectedEffects = 128;

  PacketResult observe_packet(u64 frame_id,
                              bool source_base_valid,
                              u64 source_base,
                              u64 palette_hash) {
    PacketResult out;
    if (!frame_id) {
      return out;
    }
    reset_for_frame(frame_id);
    out.accepted = true;
    out.packet_sequence = ++m_packet_sequence;
    if (!source_base_valid) {
      return out;
    }
    auto prior = std::find_if(m_sources.begin(), m_sources.begin() + m_source_count,
                              [&](const SourceEntry& entry) {
                                return entry.source_base == source_base;
                              });
    if (prior != m_sources.begin() + m_source_count) {
      out.repeated = true;
      out.conflicting_palette = prior->palette_hash != palette_hash;
      return out;
    }
    if (m_source_count == m_sources.size()) {
      out.capacity_dropped = true;
      return out;
    }
    m_sources[m_source_count++] = {source_base, palette_hash};
    out.unique_source = true;
    return out;
  }

  DrawResult observe_base_draw(u64 frame_id,
                               bool source_base_valid,
                               u64 source_base,
                               u32 packet_sequence,
                               u64 draw_identity,
                               u64 palette_hash) {
    DrawResult out;
    if (!frame_id || !packet_sequence) {
      return out;
    }
    reset_for_frame(frame_id);
    out.accepted = true;
    const DrawKey key = {source_base_valid, source_base, packet_sequence, draw_identity};
    auto prior = std::find_if(m_draws.begin(), m_draws.begin() + m_draw_count,
                              [&](const DrawEntry& entry) { return entry.key == key; });
    if (prior != m_draws.begin() + m_draw_count) {
      out.repeated = true;
      out.conflicting_palette = prior->palette_hash != palette_hash;
      return out;
    }
    if (m_draw_count == m_draws.size()) {
      out.capacity_dropped = true;
      return out;
    }
    m_draws[m_draw_count++] = {key, palette_hash};
    out.unique = true;
    return out;
  }

  EffectResult observe_affected_effect(u64 frame_id,
                                       bool source_base_valid,
                                       u64 source_base,
                                       u32 packet_sequence,
                                       u16 effect_index) {
    EffectResult out;
    if (!frame_id || !packet_sequence) {
      return out;
    }
    reset_for_frame(frame_id);
    const EffectKey key = {source_base_valid, source_base, packet_sequence, effect_index};
    if (std::find(m_affected_effects.begin(),
                  m_affected_effects.begin() + m_affected_effect_count,
                  key) != m_affected_effects.begin() + m_affected_effect_count) {
      return out;
    }
    if (m_affected_effect_count == m_affected_effects.size()) {
      out.capacity_dropped = true;
      return out;
    }
    m_affected_effects[m_affected_effect_count++] = key;
    out.unique = true;
    return out;
  }

 private:
  struct SourceEntry {
    u64 source_base = 0;
    u64 palette_hash = 0;
  };

  struct DrawKey {
    bool source_base_valid = false;
    u64 source_base = 0;
    u32 packet_sequence = 0;
    u64 draw_identity = 0;

    bool operator==(const DrawKey& other) const {
      if (draw_identity != other.draw_identity || source_base_valid != other.source_base_valid) {
        return false;
      }
      return source_base_valid ? source_base == other.source_base
                               : packet_sequence == other.packet_sequence;
    }
  };

  struct DrawEntry {
    DrawKey key;
    u64 palette_hash = 0;
  };

  struct EffectKey {
    bool source_base_valid = false;
    u64 source_base = 0;
    u32 packet_sequence = 0;
    u16 effect_index = 0;

    bool operator==(const EffectKey& other) const {
      if (effect_index != other.effect_index || source_base_valid != other.source_base_valid) {
        return false;
      }
      return source_base_valid ? source_base == other.source_base
                               : packet_sequence == other.packet_sequence;
    }
  };

  void reset_for_frame(u64 frame_id) {
    if (m_frame_id == frame_id) {
      return;
    }
    m_frame_id = frame_id;
    m_packet_sequence = 0;
    m_source_count = 0;
    m_draw_count = 0;
    m_affected_effect_count = 0;
  }

  u64 m_frame_id = 0;
  u32 m_packet_sequence = 0;
  std::array<SourceEntry, kMaximumSources> m_sources = {};
  std::size_t m_source_count = 0;
  std::array<DrawEntry, kMaximumBaseDraws> m_draws = {};
  std::size_t m_draw_count = 0;
  std::array<EffectKey, kMaximumAffectedEffects> m_affected_effects = {};
  std::size_t m_affected_effect_count = 0;
};

inline void retain_packet_result(const PacketResult& result, DuplicationStats* stats) {
  stats->packets++;
  stats->unique_source_bases += result.unique_source;
  stats->repeated_packets += result.repeated;
  stats->repeated_packets_conflicting_palette += result.repeated && result.conflicting_palette;
  stats->repeated_packets_same_palette += result.repeated && !result.conflicting_palette;
  stats->capacity_drops += result.capacity_dropped;
}

inline void retain_draw_result(const DrawResult& result, DuplicationStats* stats) {
  stats->actual_base_draws++;
  stats->unique_base_draws += result.unique;
  stats->repeated_base_draws += result.repeated;
  stats->repeated_base_draws_conflicting_palette += result.repeated && result.conflicting_palette;
  stats->repeated_base_draws_same_palette += result.repeated && !result.conflicting_palette;
  stats->capacity_drops += result.capacity_dropped;
}

}  // namespace metal_merc_skin_trace
