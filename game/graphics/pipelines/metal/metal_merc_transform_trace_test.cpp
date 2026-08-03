#include "game/graphics/pipelines/metal/metal_merc_transform_trace.h"
#include "game/graphics/pipelines/metal/metal_merc_skin_trace.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "game/mips2c/jak1_bones_provenance_trace.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  if (condition) {
    std::printf("ok   %s\n", what);
  } else {
    std::printf("FAIL %s\n", what);
    failures++;
  }
}

metal_merc_transform_trace::Event observe(metal_merc_transform_trace::Tracker& tracker,
                                          u64 frame_id,
                                          int slot,
                                          double x,
                                          double y,
                                          double z) {
  return tracker.observe(frame_id, slot, 0x1234, frame_id * 17, x, y, z, 0x1000);
}

metal_merc_transform_trace::MatrixSnapshot yaw_matrix(double degrees) {
  constexpr double kPi = 3.14159265358979323846;
  const double radians = degrees * kPi / 180.0;
  const float cosine = static_cast<float>(std::cos(radians));
  const float sine = static_cast<float>(std::sin(radians));
  return {cosine, 0.f, sine, 0.f, 0.f, 1.f, 0.f, 0.f, -sine, 0.f, cosine, 0.f, 0.f, 0.f, 0.f, 1.f};
}

metal_merc_transform_trace::MatrixSnapshot pitch_matrix(double degrees) {
  constexpr double kPi = 3.14159265358979323846;
  const double radians = degrees * kPi / 180.0;
  const float cosine = static_cast<float>(std::cos(radians));
  const float sine = static_cast<float>(std::sin(radians));
  return {1.f, 0.f, 0.f, 0.f, 0.f, cosine, -sine, 0.f,
          0.f, sine, cosine, 0.f, 0.f, 0.f, 0.f, 1.f};
}

metal_merc_transform_trace::MatrixSnapshot roll_matrix(double degrees) {
  constexpr double kPi = 3.14159265358979323846;
  const double radians = degrees * kPi / 180.0;
  const float cosine = static_cast<float>(std::cos(radians));
  const float sine = static_cast<float>(std::sin(radians));
  return {cosine, -sine, 0.f, 0.f, sine, cosine, 0.f, 0.f,
          0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
}

metal_merc_transform_trace::BasisSnapshot yaw_basis(double degrees) {
  const auto matrix = yaw_matrix(degrees);
  return metal_merc_transform_trace::make_basis_snapshot(matrix.data());
}

metal_merc_transform_trace::ProvenanceObservation provenance(double input_degrees,
                                                             double camera_degrees,
                                                             double output_degrees,
                                                             u64 identity,
                                                             bool mapping_valid = true);

metal_merc_transform_trace::ProvenanceObservation composed_provenance(double input_degrees,
                                                                      double camera_degrees,
                                                                      double bind_degrees,
                                                                      double output_error_degrees,
                                                                      u64 identity) {
  const auto input = yaw_matrix(input_degrees);
  const auto camera = yaw_matrix(camera_degrees);
  const auto bind = yaw_matrix(bind_degrees);
  const auto bone_bind = metal_merc_transform_trace::multiply_matrices(input.data(), bind.data());
  const auto expected =
      metal_merc_transform_trace::multiply_matrices(camera.data(), bone_bind.data());
  const auto error = yaw_matrix(output_error_degrees);
  const auto actual = metal_merc_transform_trace::multiply_matrices(error.data(), expected.data());

  auto out = provenance(input_degrees, camera_degrees, 0.0, identity);
  out.bind_pose_hash = identity * 7;
  out.output_basis = metal_merc_transform_trace::make_basis_snapshot(actual.data());
  out.output_expected_distance = metal_merc_transform_trace::output_composition_distance(
      camera.data(), input.data(), bind.data(), actual.data());
  out.expected_output_valid = std::isfinite(out.output_expected_distance);
  return out;
}

metal_merc_transform_trace::ProvenanceObservation provenance(double input_degrees,
                                                             double camera_degrees,
                                                             double output_degrees,
                                                             u64 identity,
                                                             bool mapping_valid) {
  metal_merc_transform_trace::ProvenanceObservation out;
  out.mapping_checked = true;
  out.mapping_valid = mapping_valid;
  out.input_root_bone = 3;
  out.producer_serial = identity;
  out.input_root_hash = identity * 3;
  out.camera_hash = identity * 5;
  out.input_root_basis = yaw_basis(input_degrees);
  out.camera_basis = yaw_basis(camera_degrees);
  out.output_basis = yaw_basis(output_degrees);
  return out;
}

metal_merc_transform_trace::Event observe_provenance(metal_merc_transform_trace::Tracker& tracker,
                                                     u64 frame_id,
                                                     double input_degrees,
                                                     double camera_degrees,
                                                     double output_degrees,
                                                     bool mapping_valid = true) {
  return tracker.observe(
      frame_id, 3, 0xe1c4a2, frame_id * 19, 1.0, 1.0, 1.0, 0x2000,
      provenance(input_degrees, camera_degrees, output_degrees, frame_id, mapping_valid));
}

void store_identity(std::vector<u8>& memory, std::size_t address, float translation) {
  const std::array<float, 16> matrix = {1.f, 0.f, 0.f, 0.f, 0.f,         1.f, 0.f, 0.f,
                                        0.f, 0.f, 1.f, 0.f, translation, 0.f, 0.f, 1.f};
  std::memcpy(memory.data() + address, matrix.data(), sizeof(matrix));
}

void store_matrix(std::vector<u8>& memory,
                  std::size_t address,
                  const metal_merc_transform_trace::MatrixSnapshot& matrix) {
  std::memcpy(memory.data() + address, matrix.data(), sizeof(matrix));
}

template <typename T>
void store_value(std::vector<u8>& memory, std::size_t address, const T& value) {
  std::memcpy(memory.data() + address, &value, sizeof(value));
}

metal_merc_transform_trace::TargetControlObservation target_control_observation(
    double intent_x,
    double intent_z,
    double control_x,
    double control_z,
    u64 attack_id,
    u32 button_rel = 0) {
  metal_merc_transform_trace::TargetControlObservation out;
  out.valid = true;
  out.capture_stage = jak1_target_control_capture::Stage::COMPLETE;
  out.capture_result = jak1_target_control_capture::Result::SUCCESS;
  out.producer_serial = attack_id + 100;
  out.source_base = 0x2000;
  out.target_state_id = 0x3000;
  out.target_attack_id = attack_id;
  out.button0_rel = button_rel;
  out.left_x = 255;
  out.left_y = 127;
  out.stick_direction = -1.5707963267948966;
  out.stick_speed = 1.0;
  out.pad_magnitude = 1.0;
  out.intent_forward = metal_merc_transform_trace::make_facing_snapshot(intent_x, intent_z);
  out.desired_forward = out.intent_forward;
  out.control_forward = metal_merc_transform_trace::make_facing_snapshot(control_x, control_z);
  out.render_forward = out.control_forward;
  out.root_forward = out.control_forward;
  const auto identity = yaw_matrix(0.0);
  out.camera_basis = metal_merc_transform_trace::make_basis_snapshot(identity.data());
  out.input_root_deformation =
      metal_merc_transform_trace::make_deformation_snapshot(identity.data());
  out.output_deformation = metal_merc_transform_trace::make_deformation_snapshot(identity.data());
  return out;
}

void write_yaw(std::array<float, 64>& palette, std::size_t slot, double degrees) {
  constexpr double kPi = 3.14159265358979323846;
  const double radians = degrees * kPi / 180.0;
  const float cosine = static_cast<float>(std::cos(radians));
  const float sine = static_cast<float>(std::sin(radians));
  float* matrix = palette.data() +
                  slot * metal_merc_skin_trace::kShaderMatrixStrideFloats;
  matrix[0] = cosine;
  matrix[2] = sine;
  matrix[5] = 1.f;
  matrix[8] = -sine;
  matrix[10] = cosine;
  matrix[15] = 1.f;
}

void test_weighted_skin_trace() {
  using namespace metal_merc_skin_trace;

  std::vector<tfrag3::MercVertex> vertices(4);
  for (int vertex = 0; vertex < 2; vertex++) {
    vertices[vertex].weights[0] = 0.5f;
    vertices[vertex].weights[1] = 0.5f;
    vertices[vertex].mats[0] = 0;
    vertices[vertex].mats[1] = 1;
  }
  vertices[2].weights[0] = 0.5f;
  vertices[2].weights[1] = 0.5f;
  vertices[2].mats[0] = 0;
  vertices[2].mats[1] = 0;
  vertices[3].weights[0] = 1.f;
  vertices[3].mats[0] = 0;
  const std::vector<u32> indices = {0, 1, 1, 2, 3};
  tfrag3::MercDraw draw;
  draw.first_index = 0;
  draw.index_count = static_cast<u32>(indices.size());
  const DrawProfile profile =
      build_draw_profile(vertices, indices, draw, VertexStream::STATIC, 0x101);
  check(profile.valid && profile.unique_vertex_count == 4 &&
            profile.multi_bone_vertex_count == 2 && profile.tuple_groups.size() == 1 &&
            profile.tuple_groups[0].vertex_multiplicity == 2,
        "weighted skin profile deduplicates draw indices and groups bit-exact multi-bone tuples");
  const DrawProfile modified_profile =
      build_draw_profile(vertices, indices, draw, VertexStream::MODIFIED, 0x102);
  check(modified_profile.valid && modified_profile.stream == VertexStream::MODIFIED &&
            modified_profile.identity != profile.identity,
        "modified draw profiles retain their local vertex-stream identity");

  std::array<float, 96> hash_palette_source = {};
  hash_palette_source[0] = 1.f;
  hash_palette_source[kShaderMatrixStrideFloats] = 2.f;
  const u64 relevant_hash = hash_draw_palette(profile, hash_palette_source.data(), 3);
  auto unused_slot_changed = hash_palette_source;
  unused_slot_changed[2 * kShaderMatrixStrideFloats] = 99.f;
  auto nmat_padding_changed = hash_palette_source;
  nmat_padding_changed[19] = 99.f;
  nmat_padding_changed[23] = 98.f;
  nmat_padding_changed[27] = 97.f;
  auto semantic_lane_changed = hash_palette_source;
  semantic_lane_changed[18] = 99.f;
  check(relevant_hash == hash_draw_palette(profile, unused_slot_changed.data(), 3) &&
            relevant_hash == hash_draw_palette(profile, nmat_padding_changed.data(), 3) &&
            relevant_hash != hash_draw_palette(profile, semantic_lane_changed.data(), 3),
        "palette identity ignores unused slots and mat3 padding but retains shader-visible lanes");
  InfluenceTuple malformed_slot;
  malformed_slot.weights[0] = 1.f;
  malformed_slot.matrices[0] = 200;
  std::array<u64, 2> malformed_slots = {};
  retain_used_bone_slots(malformed_slot, &malformed_slots);
  check(malformed_slots[0] == 0 && malformed_slots[1] == 0,
        "palette identity rejects malformed bone slots without indexing beyond its fixed mask");
  DrawProfile fixed_slots;
  fixed_slots.used_bone_slots[0] = 1ull << 3;
  DrawProfile modified_slots;
  modified_slots.used_bone_slots[0] = 1ull << 4;
  DrawProfile all_slots;
  all_slots.used_bone_slots[0] = 1ull << 5;
  const std::vector<DrawProfile> fixed_profiles = {fixed_slots};
  const std::vector<DrawProfile> modified_profiles = {modified_slots};
  const std::vector<DrawProfile> all_profiles = {all_slots};
  std::array<u64, 2> disabled_path_slots = {};
  merge_active_draw_slots(false, true, fixed_profiles, modified_profiles, all_profiles,
                          &disabled_path_slots);
  std::array<u64, 2> modified_path_slots = {};
  merge_active_draw_slots(true, true, fixed_profiles, modified_profiles, all_profiles,
                          &modified_path_slots);
  std::array<u64, 2> fallback_path_slots = {};
  merge_active_draw_slots(true, false, fixed_profiles, modified_profiles, all_profiles,
                          &fallback_path_slots);
  check(disabled_path_slots[0] == 0 &&
            modified_path_slots[0] == ((1ull << 3) | (1ull << 4)) &&
            fallback_path_slots[0] == (1ull << 5),
        "packet palette scope follows enabled effects and the selected fixed/mod or fallback path");

  std::vector<tfrag3::MercVertex> capacity_vertices(kMaximumTupleGroupsPerDraw + 1);
  std::vector<u32> capacity_indices(capacity_vertices.size());
  for (std::size_t vertex = 0; vertex < capacity_vertices.size(); vertex++) {
    capacity_vertices[vertex].weights[0] = 0.25f + static_cast<float>(vertex) / 1000.f;
    capacity_vertices[vertex].weights[1] = 0.75f - static_cast<float>(vertex) / 1000.f;
    capacity_vertices[vertex].mats[0] = 0;
    capacity_vertices[vertex].mats[1] = 1;
    capacity_indices[vertex] = static_cast<u32>(vertex);
  }
  tfrag3::MercDraw capacity_draw;
  capacity_draw.first_index = 0;
  capacity_draw.index_count = static_cast<u32>(capacity_indices.size());
  const DrawProfile bounded_profile = build_draw_profile(
      capacity_vertices, capacity_indices, capacity_draw, VertexStream::STATIC, 0x103);
  check(bounded_profile.tuple_groups.size() == kMaximumTupleGroupsPerDraw &&
            bounded_profile.capacity_dropped_vertices == 1,
        "profile construction caps tuple grouping and records unprofiled vertices");

  std::array<float, 64> coherent_palette = {};
  write_yaw(coherent_palette, 0, 89.0);
  write_yaw(coherent_palette, 1, 89.0);
  const BasisObservation coherent =
      analyze_weighted_basis(profile.tuple_groups[0].tuple, coherent_palette.data(), 2);
  check(coherent.valid && std::abs(coherent.axis_ratio - 1.0) < 1e-4 &&
            !coherent.collapse_candidate,
        "common +89/+89 weighted yaw keeps a well-conditioned basis");

  std::array<float, 64> opposing_palette = {};
  write_yaw(opposing_palette, 0, 89.0);
  write_yaw(opposing_palette, 1, -89.0);
  const BasisObservation opposing =
      analyze_weighted_basis(profile.tuple_groups[0].tuple, opposing_palette.data(), 2);
  check(opposing.valid && opposing.axis_ratio > 50.0 && opposing.collapse_candidate,
        "opposing +89/-89 weighted yaw exposes the flat-silhouette axis collapse");

  std::array<float, 64> low_determinant_palette = {};
  for (std::size_t slot = 0; slot < 2; slot++) {
    float* matrix = low_determinant_palette.data() + slot * kShaderMatrixStrideFloats;
    matrix[0] = 1.f;
    matrix[5] = 1.f;
    matrix[8] = 0.998f;
    matrix[10] = 0.05f;
    matrix[15] = 1.f;
  }
  const BasisObservation low_determinant = analyze_weighted_basis(
      profile.tuple_groups[0].tuple, low_determinant_palette.data(), 2);
  check(low_determinant.valid && low_determinant.axis_ratio < 2.0 &&
            low_determinant.normalized_abs_determinant < 0.06 &&
            low_determinant.collapse_candidate,
        "weighted basis flags low determinant even when its axis lengths remain similar");

  const InfluenceTuple repeated_slot = make_influence_tuple(vertices[2]);
  check(!has_distinct_positive_bones(repeated_slot) &&
            !analyze_weighted_basis(repeated_slot, coherent_palette.data(), 2).valid,
        "two positive weights aimed at one slot are not classified as multi-bone skinning");

  const SkinStats coherent_draw =
      analyze_draw(profile, vertices.data(), vertices.size(), coherent_palette.data(), 2);
  const SkinStats opposing_draw =
      analyze_draw(profile, vertices.data(), vertices.size(), opposing_palette.data(), 2);
  check(coherent_draw.profiled_vertices == 4 && coherent_draw.multi_bone_vertices == 2 &&
            coherent_draw.affected_vertices == 0 && opposing_draw.affected_vertices == 2 &&
            opposing_draw.affected_draws == 1 &&
            opposing_draw.coverage == Coverage::WIDESPREAD,
        "draw analysis preserves raw vertex multiplicity and derives neutral coverage");
  std::vector<u8> unaligned_vertices(1 + vertices.size() * sizeof(tfrag3::MercVertex));
  memcpy(unaligned_vertices.data() + 1, vertices.data(),
         vertices.size() * sizeof(tfrag3::MercVertex));
  const SkinStats unaligned_draw =
      analyze_draw(profile, unaligned_vertices.data() + 1, vertices.size(),
                   opposing_palette.data(), 2);
  check(unaligned_draw.affected_vertices == opposing_draw.affected_vertices &&
            unaligned_draw.profile_tuple_mismatches == 0,
        "draw analysis reads a 16-byte-or-less aligned Metal vertex stream without typed access");
  const SkinStats bounded_skin =
      analyze_draw(bounded_profile, capacity_vertices.data(), capacity_vertices.size(),
                   coherent_palette.data(), 2);
  check(bounded_skin.tuple_groups == kMaximumTupleGroupsPerDraw &&
            bounded_skin.profile_capacity_drops == 1,
        "per-frame analysis remains bounded and surfaces profile capacity drops");
  SkinStats combined_skin;
  combined_skin.add(coherent_draw);
  combined_skin.add(opposing_draw);
  check(combined_skin.profiled_base_draws == 2 && combined_skin.multi_bone_vertices == 4 &&
            combined_skin.affected_vertices == 2 && combined_skin.coverage == Coverage::MIXED &&
            combined_skin.maximum_axis_ratio > 50.0,
        "weighted skin stats aggregate raw counts and extrema across Merc buckets");

  std::vector<tfrag3::MercVertex> changed_vertices = vertices;
  changed_vertices[0].weights[0] = 0.25f;
  const SkinStats mismatch = analyze_draw(profile, changed_vertices.data(),
                                          changed_vertices.size(), opposing_palette.data(), 2);
  check(mismatch.profile_tuple_mismatches == 1 && mismatch.invalid_basis_groups == 1 &&
            mismatch.affected_vertices == 0,
        "a changed bound tuple is reported instead of being measured through stale metadata");

  check(classify_coverage(10, 0) == Coverage::NONE &&
            classify_coverage(10, 2) == Coverage::LOCALIZED &&
            classify_coverage(10, 5) == Coverage::MIXED &&
            classify_coverage(10, 9) == Coverage::WIDESPREAD,
        "coverage labels are deterministic while raw counts remain authoritative");

  FrameTracker tracker;
  const PacketResult packet_a = tracker.observe_packet(10, true, 0x1000, 0xaa);
  const PacketResult packet_a_repeat = tracker.observe_packet(10, true, 0x1000, 0xaa);
  const PacketResult packet_b_conflict = tracker.observe_packet(10, true, 0x1000, 0xbb);
  check(packet_a.unique_source && packet_a.packet_sequence == 1 && packet_a_repeat.repeated &&
            !packet_a_repeat.conflicting_palette && packet_b_conflict.repeated &&
            packet_b_conflict.conflicting_palette,
        "packet tracker separates same-palette repeats from same-source palette conflicts");

  const DrawResult draw_a =
      tracker.observe_base_draw(10, true, 0x1000, packet_a.packet_sequence, 0x101, 0xaa);
  const DrawResult draw_a_repeat = tracker.observe_base_draw(
      10, true, 0x1000, packet_a_repeat.packet_sequence, 0x101, 0xaa);
  const DrawResult draw_b_conflict = tracker.observe_base_draw(
      10, true, 0x1000, packet_b_conflict.packet_sequence, 0x101, 0xbb);
  check(draw_a.unique && draw_a_repeat.repeated && !draw_a_repeat.conflicting_palette &&
            draw_b_conflict.repeated && draw_b_conflict.conflicting_palette,
        "actual base-draw tracker excludes palette from its key and retains palette conflicts");
  DuplicationStats duplication;
  retain_packet_result(packet_a, &duplication);
  retain_packet_result(packet_a_repeat, &duplication);
  retain_packet_result(packet_b_conflict, &duplication);
  retain_draw_result(draw_a, &duplication);
  retain_draw_result(draw_a_repeat, &duplication);
  retain_draw_result(draw_b_conflict, &duplication);
  check(duplication.packets == 3 && duplication.unique_source_bases == 1 &&
            duplication.repeated_packets == 2 &&
            duplication.repeated_packets_conflicting_palette == 1 &&
            duplication.actual_base_draws == 3 && duplication.unique_base_draws == 1 &&
            duplication.repeated_base_draws_conflicting_palette == 1,
        "duplicate stats retain packet, source, palette and actual-draw counts separately");
  check(tracker.observe_affected_effect(10, true, 0x1000, packet_a.packet_sequence, 3).unique &&
            !tracker
                 .observe_affected_effect(10, true, 0x1000,
                                          packet_b_conflict.packet_sequence, 3)
                 .unique &&
            tracker
                .observe_affected_effect(10, true, 0x1000,
                                         packet_b_conflict.packet_sequence, 4)
                .unique,
        "affected effects are unique per source and effect within an engine frame");

  const PacketResult next_frame = tracker.observe_packet(11, true, 0x1000, 0xaa);
  check(next_frame.unique_source &&
            tracker.observe_base_draw(11, true, 0x1000, next_frame.packet_sequence, 0x101, 0xaa)
                .unique,
        "packet and actual-draw duplicate state resets on the engine-frame boundary");
}

}  // namespace

int main() {
  test_weighted_skin_trace();
  std::array<float, metal_merc_transform_trace::kMatrixLaneCount> lanes = {};
  lanes[3] = std::numeric_limits<float>::infinity();
  lanes[16] = std::numeric_limits<float>::quiet_NaN();
  lanes[27] = -std::numeric_limits<float>::infinity();
  check(metal_merc_transform_trace::nonfinite_lane_mask(lanes.data()) ==
            ((1u << 3) | (1u << 16) | (1u << 27)),
        "the non-finite mask names exact tmat and nmat lanes");

  metal_merc_transform_trace::Tracker healthy;
  observe(healthy, 10, 0, 1.0, 1.0, 1.0);
  check(!observe(healthy, 11, 0, 1.1, 1.0, 0.9).valid(),
        "ordinary consecutive-frame scale variation is quiet");

  metal_merc_transform_trace::Tracker scale;
  observe(scale, 20, 1, 1.0, 1.0, 1.0);
  const auto scaled = observe(scale, 21, 1, 2.0, 2.0, 2.0);
  check(scaled.issue_mask == metal_merc_transform_trace::SCALE_DISCONTINUITY &&
            scaled.previous_frame_id == 20 && scaled.current_frame_id == 21 &&
            scaled.previous_scale == 1.0 && scaled.current_scale == 2.0 &&
            scaled.previous_matrix_hash != scaled.current_matrix_hash,
        "an exact 2x uniform jump reports scale with consecutive-frame evidence");

  metal_merc_transform_trace::Tracker aspect;
  observe(aspect, 30, 2, 1.0, 1.0, 1.0);
  const auto flattened = observe(aspect, 31, 2, 0.25, 2.0, 2.0);
  check(flattened.issue_mask == metal_merc_transform_trace::ASPECT_DISCONTINUITY &&
            flattened.previous_aspect == 1.0 && flattened.current_aspect == 8.0 &&
            flattened.previous_scale == flattened.current_scale,
        "a volume-preserving flattening reports aspect without scale");

  const auto reshaped = observe(aspect, 32, 2, 2.0, 0.25, 2.0);
  check(reshaped.issue_mask == metal_merc_transform_trace::ASPECT_DISCONTINUITY &&
            reshaped.previous_aspect == reshaped.current_aspect,
        "a flattened axis swap reports aspect even when the max/min ratio is unchanged");

  metal_merc_transform_trace::Tracker uniform_aba;
  const auto uniform_b = uniform_aba.observe(60, 6, 0x6000, 0xa0, 1.0, 1.0, 1.0, 0x1600);
  check(!uniform_b.valid(), "the first uniform A sample only establishes a baseline");
  const auto uniform_ab = uniform_aba.observe(61, 6, 0x6000, 0xb0, 3.0, 3.0, 3.0, 0x2600);
  const auto uniform_ba = uniform_aba.observe(62, 6, 0x6000, 0xa0, 1.0, 1.0, 1.0, 0x1600);
  check(uniform_ab.issue_mask == metal_merc_transform_trace::SCALE_DISCONTINUITY &&
            uniform_ba.issue_mask == metal_merc_transform_trace::SCALE_DISCONTINUITY &&
            uniform_ab.previous_scale == uniform_ba.current_scale &&
            uniform_ab.current_scale == uniform_ba.previous_scale &&
            uniform_ab.previous_aspect == uniform_ab.current_aspect &&
            uniform_ba.previous_aspect == uniform_ba.current_aspect,
        "uniform A/B/A retains reversible scale-only evidence");
  check(uniform_ab.previous_matrix_hash == uniform_ba.current_matrix_hash &&
            uniform_ab.current_matrix_hash == uniform_ba.previous_matrix_hash &&
            uniform_ab.previous_source_base == uniform_ba.current_source_base &&
            uniform_ab.current_source_base == uniform_ba.previous_source_base,
        "uniform A/B/A retains reversible matrix and source identities");

  metal_merc_transform_trace::Tracker flattened_aba;
  flattened_aba.observe(70, 7, 0x7000, 0xa1, 1.0, 1.0, 1.0, 0x1700);
  const auto flattened_ab = flattened_aba.observe(71, 7, 0x7000, 0xb1, 0.25, 2.0, 2.0, 0x2700);
  const auto flattened_ba = flattened_aba.observe(72, 7, 0x7000, 0xa1, 1.0, 1.0, 1.0, 0x1700);
  check(flattened_ab.issue_mask == metal_merc_transform_trace::ASPECT_DISCONTINUITY &&
            flattened_ba.issue_mask == metal_merc_transform_trace::ASPECT_DISCONTINUITY &&
            flattened_ab.previous_scale == flattened_ab.current_scale &&
            flattened_ba.previous_scale == flattened_ba.current_scale &&
            flattened_ab.previous_axis_norm_x == flattened_ba.current_axis_norm_x &&
            flattened_ab.current_axis_norm_x == flattened_ba.previous_axis_norm_x &&
            flattened_ab.previous_aspect == flattened_ba.current_aspect &&
            flattened_ab.current_aspect == flattened_ba.previous_aspect,
        "flattened A/B/A retains reversible aspect and per-axis evidence without scale change");

  metal_merc_transform_trace::Tracker gaps;
  observe(gaps, 40, 3, 1.0, 1.0, 1.0);
  check(!observe(gaps, 42, 3, 4.0, 4.0, 4.0).valid(),
        "a frame gap resets comparison instead of inventing a jump");
  check(!observe(gaps, 42, 3, 8.0, 8.0, 8.0).valid(),
        "a duplicate frame never generates a second observation");
  check(!observe(gaps, 43, 4, 16.0, 16.0, 16.0).valid(), "bone slots keep independent histories");

  metal_merc_transform_trace::Tracker invalid;
  observe(invalid, 50, 5, 1.0, 1.0, 1.0);
  check(!observe(invalid, 51, 5, std::numeric_limits<double>::infinity(), 1.0, 1.0).valid(),
        "invalid axis data clears the affected slot without reporting a finite jump");
  check(!observe(invalid, 52, 5, 4.0, 4.0, 4.0).valid(),
        "tracking resumes from scratch after invalid axis data");
  check(!observe(invalid, 0, 5, 8.0, 8.0, 8.0).valid(), "unknown engine frame identity is ignored");

  metal_merc_transform_trace::Tracker input_root;
  observe_provenance(input_root, 100, 0.0, 0.0, 0.0);
  observe_provenance(input_root, 101, 90.0, 0.0, 90.0);
  const auto input_return = observe_provenance(input_root, 102, 0.0, 0.0, 0.0);
  check(input_return.issue_mask == metal_merc_transform_trace::INPUT_ROOT_ALTERNATION &&
            input_return.older_frame_id == 100 && input_return.previous_frame_id == 101 &&
            input_return.current_frame_id == 102 && input_return.input_root_bone == 3 &&
            input_return.input_older_to_current_distance == 0.0 &&
            input_return.output_older_to_current_distance == 0.0,
        "matching world-root and output A/B/A is attributed to the producer input");

  metal_merc_transform_trace::Tracker output_only;
  observe_provenance(output_only, 200, 0.0, 0.0, 0.0);
  observe_provenance(output_only, 201, 0.0, 0.0, 90.0);
  const auto output_return = observe_provenance(output_only, 202, 0.0, 0.0, 0.0);
  check(output_return.issue_mask == metal_merc_transform_trace::OUTPUT_ONLY_ALTERNATION &&
            output_return.input_older_to_previous_distance == 0.0 &&
            output_return.camera_older_to_previous_distance == 0.0 &&
            output_return.output_older_to_previous_distance > 0.1,
        "stable input and camera attribute output A/B/A after the producer boundary");

  metal_merc_transform_trace::Tracker camera_driven;
  observe_provenance(camera_driven, 300, 0.0, 0.0, 0.0);
  observe_provenance(camera_driven, 301, 0.0, 90.0, 90.0);
  const auto camera_return = observe_provenance(camera_driven, 302, 0.0, 0.0, 0.0);
  check(camera_return.issue_mask == metal_merc_transform_trace::CAMERA_DRIVEN_ALTERNATION &&
            camera_return.camera_older_to_previous_distance > 0.1,
        "camera and output A/B/A with a stable root is attributed to the camera input");

  metal_merc_transform_trace::Tracker mixed_provenance;
  mixed_provenance.observe(350, 3, 0xe1c4a2, 1, 1.0, 1.0, 1.0, 0x2000,
                           provenance(0.0, 0.0, 0.0, 350));
  mixed_provenance.observe(351, 3, 0xe1c4a2, 2, 3.0, 3.0, 3.0, 0x2000,
                           provenance(90.0, 0.0, 90.0, 351));
  const auto mixed_return =
      mixed_provenance.observe(352, 3, 0xe1c4a2, 3, 1.0, 1.0, 1.0, 0x2000,
                               provenance(0.0, 0.0, 0.0, 352));
  check(mixed_return.issue_mask == (metal_merc_transform_trace::SCALE_DISCONTINUITY |
                                    metal_merc_transform_trace::INPUT_ROOT_ALTERNATION),
        "a mixed scale and 0x04 provenance event retains both issue classes");

  int retained_provenance_count = 0;
  metal_merc_transform_trace::Event first_retained_provenance;
  metal_merc_transform_trace::Event last_retained_provenance;
  const bool retained_scale = metal_merc_transform_trace::retain_provenance_event(
      scaled, &retained_provenance_count, &first_retained_provenance,
      &last_retained_provenance);
  const bool retained_input = metal_merc_transform_trace::retain_provenance_event(
      input_return, &retained_provenance_count, &first_retained_provenance,
      &last_retained_provenance);
  const bool retained_mixed = metal_merc_transform_trace::retain_provenance_event(
      mixed_return, &retained_provenance_count, &first_retained_provenance,
      &last_retained_provenance);
  check(!retained_scale && retained_input && retained_mixed && retained_provenance_count == 2 &&
            first_retained_provenance.issue_mask ==
                metal_merc_transform_trace::INPUT_ROOT_ALTERNATION &&
            last_retained_provenance.issue_mask ==
                (metal_merc_transform_trace::SCALE_DISCONTINUITY |
                 metal_merc_transform_trace::INPUT_ROOT_ALTERNATION),
        "generic provenance retention excludes pure scale but keeps 0x04 and mixed events");

  metal_merc_transform_trace::Tracker stale_output;
  observe_provenance(stale_output, 400, 0.0, 0.0, 0.0);
  const auto stale = observe_provenance(stale_output, 401, 90.0, 0.0, 0.0);
  check(stale.issue_mask == metal_merc_transform_trace::OUTPUT_STALE &&
            stale.input_previous_to_current_distance > 0.1 &&
            stale.output_previous_to_current_distance == 0.0,
        "a changed world root with frozen camera/output is classified as stale output");

  metal_merc_transform_trace::Tracker mapping;
  observe_provenance(mapping, 500, 0.0, 0.0, 0.0);
  const auto missing_mapping = observe_provenance(mapping, 501, 0.0, 0.0, 0.0, false);
  check(missing_mapping.issue_mask == metal_merc_transform_trace::SOURCE_MAPPING_DISCONTINUITY,
        "a missing producer record is distinguished from a healthy palette source");

  metal_merc_transform_trace::Tracker unavailable_mapping;
  unavailable_mapping.observe(550, 3, 0xe1c4a2, 1, 1.0, 1.0, 1.0, 0x2000, {});
  check(!unavailable_mapping.observe(551, 3, 0xe1c4a2, 2, 1.0, 1.0, 1.0, 0x2000, {}).valid(),
        "an uninstrumented producer stays quiet instead of reporting a false source failure");

  metal_merc_transform_trace::Tracker provenance_gap;
  observe_provenance(provenance_gap, 560, 0.0, 0.0, 0.0);
  observe_provenance(provenance_gap, 562, 90.0, 0.0, 90.0);
  check(!observe_provenance(provenance_gap, 563, 0.0, 0.0, 0.0).valid(),
        "a provenance frame gap clears the three-frame return window");

  metal_merc_transform_trace::Tracker translation_only;
  auto translated_a = provenance(0.0, 0.0, 0.0, 600);
  auto translated_b = provenance(0.0, 0.0, 0.0, 601);
  translated_b.input_translation_x = 4096.0;
  translation_only.observe(600, 3, 0xe1c4a2, 1, 1.0, 1.0, 1.0, 0x2000, translated_a);
  check(!translation_only.observe(601, 3, 0xe1c4a2, 2, 1.0, 1.0, 1.0, 0x2000, translated_b).valid(),
        "ordinary translation is retained as evidence without becoming a facing or scale issue");

  metal_merc_transform_trace::Tracker composed_motion;
  const std::array<std::array<double, 2>, 4> arbitrary_motion = {
      std::array<double, 2>{0.0, 0.0}, {75.0, 12.0}, {165.0, -25.0}, {270.0, 40.0}};
  bool composed_motion_quiet = true;
  for (std::size_t frame = 0; frame < arbitrary_motion.size(); frame++) {
    const auto event = composed_motion.observe(
        700 + frame, 3, 0xe1c4a2, frame, 1.0, 1.0, 1.0, 0x2000,
        composed_provenance(arbitrary_motion[frame][0], arbitrary_motion[frame][1], 17.0, 0.0,
                            700 + frame));
    composed_motion_quiet &=
        !(event.issue_mask & metal_merc_transform_trace::OUTPUT_COMPOSITION_MISMATCH);
  }
  check(
      composed_motion_quiet,
      "arbitrary root spin and camera motion stay quiet when camera * bone * bind matches output");

  metal_merc_transform_trace::Tracker composition_mismatch;
  bool sustained_mismatch_reported = true;
  metal_merc_transform_trace::Event last_composition_mismatch;
  for (u64 frame = 710; frame < 713; frame++) {
    last_composition_mismatch = composition_mismatch.observe(
        frame, 3, 0xe1c4a2, frame, 1.0, 1.0, 1.0, 0x2000,
        composed_provenance((frame - 710) * 35.0, (frame - 710) * -8.0, 17.0, 20.0, frame));
    sustained_mismatch_reported =
        sustained_mismatch_reported && (last_composition_mismatch.issue_mask &
                                        metal_merc_transform_trace::OUTPUT_COMPOSITION_MISMATCH);
  }
  check(sustained_mismatch_reported &&
            last_composition_mismatch.current_output_expected_distance >=
                metal_merc_transform_trace::Tracker::kOutputCompositionMismatchDistance &&
            last_composition_mismatch.previous_output_expected_distance >=
                metal_merc_transform_trace::Tracker::kOutputCompositionMismatchDistance,
        "a sustained arbitrary output mismatch reports 0x80 with current and previous evidence");

  metal_merc_transform_trace::Tracker subthreshold_composition;
  const auto subthreshold = subthreshold_composition.observe(
      720, 3, 0xe1c4a2, 1, 1.0, 1.0, 1.0, 0x2000, composed_provenance(0.0, 0.0, 0.0, 0.05, 720));
  const auto over_threshold = subthreshold_composition.observe(
      721, 3, 0xe1c4a2, 2, 1.0, 1.0, 1.0, 0x2000, composed_provenance(0.0, 0.0, 0.0, 0.2, 721));
  check(!(subthreshold.issue_mask & metal_merc_transform_trace::OUTPUT_COMPOSITION_MISMATCH) &&
            (over_threshold.issue_mask & metal_merc_transform_trace::OUTPUT_COMPOSITION_MISMATCH),
        "the composition comparator is quiet below 1e-3 RMS and reports above it");

  metal_merc_transform_trace::TargetControlTracker target_control_tracker;
  const auto facing_ok =
      target_control_tracker.observe(800, 3, target_control_observation(0.0, 1.0, 0.0, 1.0, 7));
  check(!facing_ok.valid(), "matching active intent and control facing stay quiet");
  metal_merc_transform_trace::TargetControlTracker ordinary_sample_tracker;
  auto moderate_anisotropy =
      target_control_observation(0.0, 1.0, 0.0, 1.0, 7);
  auto moderately_narrow = yaw_matrix(0.0);
  moderately_narrow[0] = 0.8f;
  moderate_anisotropy.output_deformation =
      metal_merc_transform_trace::make_deformation_snapshot(moderately_narrow.data());
  const auto ordinary_sample =
      ordinary_sample_tracker.observe_with_status(805, 3, moderate_anisotropy);
  check(ordinary_sample.capture_attempted && ordinary_sample.valid_observation &&
            !ordinary_sample.event.valid() && ordinary_sample.observation.valid &&
            ordinary_sample.observation.engine_frame_id == 805 &&
            ordinary_sample.observation.probe_bone_slot == 3 &&
            std::abs(ordinary_sample.observation.output_deformation.aspect - 1.25) < 1e-6 &&
            ordinary_sample.observation.output_deformation.aspect <
                metal_merc_transform_trace::Tracker::kDiscontinuityRatio,
        "a quiet valid sample retains moderate anisotropy below the discontinuity threshold");
  metal_merc_transform_trace::TargetControlTracker unavailable_provenance_tracker;
  metal_merc_transform_trace::TargetControlObservation capture_without_provenance;
  capture_without_provenance.capture_stage = jak1_target_control_capture::Stage::COMPLETE;
  capture_without_provenance.capture_result = jak1_target_control_capture::Result::SUCCESS;
  const auto unavailable_provenance = unavailable_provenance_tracker.observe_with_status(
      806, 3, capture_without_provenance);
  check(unavailable_provenance.capture_attempted &&
            unavailable_provenance.capture_stage == jak1_target_control_capture::Stage::COMPLETE &&
            unavailable_provenance.capture_result ==
                jak1_target_control_capture::Result::SUCCESS &&
            !unavailable_provenance.valid_observation && !unavailable_provenance.event.valid(),
        "a successful capture still reports its outcome when mapping cannot form an observation");
  const auto facing_mismatch =
      target_control_tracker.observe(801, 3, target_control_observation(0.0, 1.0, 0.0, -1.0, 7));
  check(
      facing_mismatch.issue_mask == metal_merc_transform_trace::TARGET_CONTROL_FACING_DIVERGENCE &&
          facing_mismatch.intent_control_dot == -1.0 && facing_mismatch.control_render_dot == 1.0 &&
          facing_mismatch.render_root_dot == 1.0,
      "opposed active intent and control facing report bounded divergence evidence");
  const auto attack_transition =
      target_control_tracker.observe(802, 3, target_control_observation(0.0, 1.0, 0.0, 1.0, 8));
  check(
      attack_transition.issue_mask == metal_merc_transform_trace::TARGET_CONTROL_ATTACK_BOUNDARY &&
          attack_transition.previous_target_attack_id == 7 &&
          attack_transition.current_target_attack_id == 8,
      "a consecutive target attack id transition retains one attack-boundary event");
  metal_merc_transform_trace::TargetControlTracker square_edge_tracker;
  auto stationary_square = target_control_observation(
      0.0, 0.0, 1.0, 0.0, 12,
      metal_merc_transform_trace::TargetControlTracker::kSquareButton);
  stationary_square.stick_speed = 0.0;
  stationary_square.pad_magnitude = 0.0;
  const auto stationary_square_trace =
      square_edge_tracker.observe_with_status(900, 3, stationary_square);
  const auto& square_edge = stationary_square_trace.event;
  check(stationary_square_trace.valid_observation &&
            stationary_square_trace.observation.engine_frame_id == 900 &&
            stationary_square_trace.observation.probe_bone_slot == 3 &&
            !stationary_square_trace.observation.intent_forward.valid &&
            square_edge.issue_mask == metal_merc_transform_trace::TARGET_CONTROL_ATTACK_BOUNDARY &&
            square_edge.button0_rel ==
                metal_merc_transform_trace::TargetControlTracker::kSquareButton,
        "a stationary Square press retains its sample and attack boundary without an intent vector");
  metal_merc_transform_trace::TargetControlTracker duplicate_target_control;
  const auto first_target_control_trace = duplicate_target_control.observe_with_status(
      950, 3, target_control_observation(0.0, 1.0, 0.0, -1.0, 20));
  const auto duplicate_target_control_trace = duplicate_target_control.observe_with_status(
      950, 3,
      target_control_observation(0.0, 1.0, 0.0, 1.0, 99,
                                 metal_merc_transform_trace::TargetControlTracker::kSquareButton));
  const auto after_duplicate_trace = duplicate_target_control.observe_with_status(
      951, 3, target_control_observation(0.0, 1.0, 0.0, 1.0, 20));
  check(first_target_control_trace.event.issue_mask ==
                metal_merc_transform_trace::TARGET_CONTROL_FACING_DIVERGENCE &&
            first_target_control_trace.observation.engine_frame_id == 950 &&
            first_target_control_trace.observation.probe_bone_slot == 3 &&
            !duplicate_target_control_trace.capture_attempted &&
            !duplicate_target_control_trace.valid_observation &&
            after_duplicate_trace.valid_observation &&
            after_duplicate_trace.observation.engine_frame_id == 951 &&
            after_duplicate_trace.observation.probe_bone_slot == 3 &&
            !after_duplicate_trace.event.valid(),
        "a duplicate frame cannot overwrite the stamped observation or attack history");

  auto& registry = jak1_bones_provenance_trace::registry();
  registry.reset();
  std::vector<u8> memory(0x20000, 0);
  constexpr u32 kOutput = 0x1000;
  constexpr u32 kJointsObject = 0x2000;
  constexpr u32 kJoints = kJointsObject + jak1_bones_provenance_trace::kGoalBasicPointerBias;
  constexpr u32 kBones = 0x3000;
  constexpr u32 kCamera = 0x5000;
  constexpr u32 kCount = 4;
  const auto camera_matrix = pitch_matrix(23.0);
  std::array<metal_merc_transform_trace::MatrixSnapshot,
             jak1_bones_provenance_trace::kRootAnchorCount>
      root_matrices = {yaw_matrix(31.0), yaw_matrix(-47.0), yaw_matrix(68.0)};
  for (u32 component = 0; component < 3; component++) {
    root_matrices[0][component] *= 2.0f;
    root_matrices[1][4 + component] *= 3.0f;
    root_matrices[2][8 + component] *= 4.0f;
  }
  const std::array<std::array<float, 3>, jak1_bones_provenance_trace::kRootAnchorCount>
      root_scales = {{{1.25f, 2.25f, 3.25f},
                      {4.5f, 5.5f, 6.5f},
                      {7.75f, 8.75f, 9.75f}}};
  const std::array<u32, jak1_bones_provenance_trace::kRootAnchorCount> root_scale_w_bits = {
      0, 0x80000000u, 0x3f800000u};
  const std::array<metal_merc_transform_trace::MatrixSnapshot,
                   jak1_bones_provenance_trace::kRootAnchorCount>
      bind_pose_matrices = {roll_matrix(-19.0), roll_matrix(37.0), roll_matrix(59.0)};
  store_identity(memory, kBones, 0.f);
  for (u32 root = 0; root < jak1_bones_provenance_trace::kRootAnchorCount; root++) {
    store_matrix(memory,
                 kBones + (root + 1) * jak1_bones_provenance_trace::kBoneStride,
                 root_matrices[root]);
    const u32 scale_address =
        kBones + (root + 1) * jak1_bones_provenance_trace::kBoneStride +
        jak1_bones_provenance_trace::kBoneScaleOffset;
    store_value(memory, scale_address, root_scales[root][0]);
    store_value(memory, scale_address + sizeof(float), root_scales[root][1]);
    store_value(memory, scale_address + 2 * sizeof(float), root_scales[root][2]);
    store_value(memory, scale_address + 3 * sizeof(float), root_scale_w_bits[root]);
  }
  for (u32 joint = 0; joint < jak1_bones_provenance_trace::kRootAnchorCount; joint++) {
    store_matrix(memory,
                 kJointsObject + jak1_bones_provenance_trace::kJointBindPoseOffset +
                     joint * jak1_bones_provenance_trace::kJointStride,
                 bind_pose_matrices[joint]);
  }
  store_matrix(memory, kCamera, camera_matrix);
  check(registry.record(kOutput, kJoints, kBones, kCount, kCamera, memory.data(), memory.size()),
        "the producer registry records an in-bounds calculation");
  const auto recorded = registry.find_output_base(kOutput);
  check(recorded && recorded->serial == 1 && recorded->bone_count == kCount &&
            recorded->camera.valid && recorded->joints_base == kJoints &&
            recorded->root_anchors[0].valid && recorded->root_anchors[1].valid &&
            recorded->root_anchors[2].valid && recorded->root_bind_poses[0].valid &&
            recorded->root_bind_poses[1].valid && recorded->root_bind_poses[2].valid &&
            recorded->root_scales[0].valid && recorded->root_scales[1].valid &&
            recorded->root_scales[2].valid,
        "one record retains camera, bind poses, and align, prejoint and main transforms and scales");
  bool bind_pose_bytes_match = recorded.has_value();
  if (recorded) {
    for (u32 joint = 0; joint < jak1_bones_provenance_trace::kRootAnchorCount; joint++) {
      bind_pose_bytes_match &= std::memcmp(recorded->root_bind_poses[joint].bytes.data(),
                                           memory.data() + kJointsObject +
                                               jak1_bones_provenance_trace::kJointBindPoseOffset +
                                               joint * jak1_bones_provenance_trace::kJointStride,
                                           jak1_bones_provenance_trace::kTransformBytes) == 0;
    }
  }
  check(bind_pose_bytes_match,
        "the GOAL basic a1 pointer bias resolves bind pose +0x10 from the object base");
  bool root_mapping_matches = recorded.has_value();
  bool correct_composition_matches = recorded.has_value();
  bool reversed_order_is_rejected = recorded.has_value();
  bool crossed_bind_mapping_is_rejected = recorded.has_value();
  if (recorded) {
    constexpr double kCompositionTolerance = 1e-6;
    constexpr double kOrderMismatchFloor = 1e-2;
    for (u32 root = 0; root < jak1_bones_provenance_trace::kRootAnchorCount; root++) {
      root_mapping_matches &=
          std::memcmp(recorded->root_anchors[root].bytes.data(), root_matrices[root].data(),
                      jak1_bones_provenance_trace::kTransformBytes) == 0 &&
          std::memcmp(recorded->root_bind_poses[root].bytes.data(),
                      bind_pose_matrices[root].data(),
                      jak1_bones_provenance_trace::kTransformBytes) == 0;

      const auto bone_bind = metal_merc_transform_trace::multiply_matrices(
          root_matrices[root].data(), bind_pose_matrices[root].data());
      const auto expected = metal_merc_transform_trace::multiply_matrices(
          camera_matrix.data(), bone_bind.data());
      correct_composition_matches &=
          metal_merc_transform_trace::output_composition_distance(
              camera_matrix.data(), root_matrices[root].data(), bind_pose_matrices[root].data(),
              expected.data()) < kCompositionTolerance;

      const auto bind_bone = metal_merc_transform_trace::multiply_matrices(
          bind_pose_matrices[root].data(), root_matrices[root].data());
      const auto wrong_bone_bind_order = metal_merc_transform_trace::multiply_matrices(
          camera_matrix.data(), bind_bone.data());
      const auto camera_bind = metal_merc_transform_trace::multiply_matrices(
          camera_matrix.data(), bind_pose_matrices[root].data());
      const auto wrong_camera_bone_order = metal_merc_transform_trace::multiply_matrices(
          root_matrices[root].data(), camera_bind.data());
      reversed_order_is_rejected &=
          metal_merc_transform_trace::output_composition_distance(
              camera_matrix.data(), root_matrices[root].data(), bind_pose_matrices[root].data(),
              wrong_bone_bind_order.data()) > kOrderMismatchFloor &&
          metal_merc_transform_trace::output_composition_distance(
              camera_matrix.data(), root_matrices[root].data(), bind_pose_matrices[root].data(),
              wrong_camera_bone_order.data()) > kOrderMismatchFloor;

      const auto& crossed_bind = bind_pose_matrices[(root + 1) % bind_pose_matrices.size()];
      const auto crossed_bone_bind = metal_merc_transform_trace::multiply_matrices(
          root_matrices[root].data(), crossed_bind.data());
      const auto crossed_output = metal_merc_transform_trace::multiply_matrices(
          camera_matrix.data(), crossed_bone_bind.data());
      crossed_bind_mapping_is_rejected &=
          metal_merc_transform_trace::output_composition_distance(
              camera_matrix.data(), root_matrices[root].data(), bind_pose_matrices[root].data(),
              crossed_output.data()) > kOrderMismatchFloor;
    }
  }
  check(root_mapping_matches,
        "root slots 1, 2 and 3 retain their matching align, prejoint and main bind poses");
  bool root_scale_and_deformation_match = recorded.has_value();
  std::array<metal_merc_transform_trace::RootAnchorObservation,
             jak1_bones_provenance_trace::kRootAnchorCount>
      root_observations = {};
  if (recorded) {
    for (u32 root = 0; root < jak1_bones_provenance_trace::kRootAnchorCount; root++) {
      metal_merc_transform_trace::MatrixSnapshot matrix = {};
      std::memcpy(matrix.data(), recorded->root_anchors[root].bytes.data(),
                  recorded->root_anchors[root].bytes.size());
      const auto& root_scale = recorded->root_scales[root];
      root_observations[root] = metal_merc_transform_trace::make_root_anchor_observation(
          matrix.data(), root_scale.x, root_scale.y, root_scale.z, root_scale.w_bits);
      root_scale_and_deformation_match &=
          root_observations[root].valid && root_observations[root].deformation.valid &&
          std::abs(root_observations[root].deformation.aspect - static_cast<double>(root + 2)) <
              1e-6 &&
          root_observations[root].scale_x == root_scales[root][0] &&
          root_observations[root].scale_y == root_scales[root][1] &&
          root_observations[root].scale_z == root_scales[root][2] &&
          root_observations[root].scale_w_bits == root_scale_w_bits[root];
    }
  }
  const bool node3_parent_scale_cancellation_selected =
      root_observations[1].valid && root_observations[1].scale_w_bits != 0;
  check(root_scale_and_deformation_match && node3_parent_scale_cancellation_selected,
        "root nodes 1-3 retain distinct deformation and raw scale fields while node 3 derives "
        "parent-scale cancellation from node 2 raw scale.w bits");
  check(correct_composition_matches,
        "non-commuting X camera, Y bone and Z bind rotations match camera * bone * bind");
  check(reversed_order_is_rejected,
        "non-commuting root matrices reject reversed camera, bone and bind order");
  check(crossed_bind_mapping_is_rejected,
        "each root rejects the bind pose belonging to the next root slot");
  const auto source_match =
      registry.find_source_address(kOutput + 3 * jak1_bones_provenance_trace::kOutputStride);
  check(source_match && source_match->output_base == kOutput &&
            !registry.find_source_address(kOutput + 1),
        "source lookup accepts an aligned palette member and rejects a misaligned address");
  check(!registry.record(memory.size() - 64, kJoints, kBones, kCount, kCamera, memory.data(),
                         memory.size()),
        "the producer registry rejects an output span outside EE memory");

  registry.record_post_flag(0x7000, 0x80, 12);
  const auto visible_post_flag = registry.latest_post_flag();
  registry.record_post_flag(0x7000, 0x82, 13);
  const auto hidden_post_flag = registry.latest_post_flag();
  check(visible_post_flag && visible_post_flag->serial == 1 && !visible_post_flag->hidden() &&
            visible_post_flag->draw_status == 0x80 && visible_post_flag->target_attack_id == 12 &&
            hidden_post_flag && hidden_post_flag->serial == 2 && hidden_post_flag->hidden() &&
            hidden_post_flag->target_address == 0x7000 && hidden_post_flag->draw_status == 0x82 &&
            hidden_post_flag->target_attack_id == 13 &&
            registry.find_output_base(kOutput)->serial == 1,
        "post-flag samples atomically retain raw visibility and same-sample attack identity "
        "without requiring a new bones or Merc record");

  constexpr u32 kTargetType = 0x100;
  constexpr u32 kControlType = 0x200;
  constexpr u32 kCpadType = 0x300;
  constexpr u32 kTarget = 0x7000;
  constexpr u32 kControl = 0x8000;
  constexpr u32 kCpad = 0xd000;
  constexpr u32 kState = 0xe000;
  constexpr u32 kDecoyNodeListType = 0x400;
  constexpr u32 kDecoyNodeList = 0x10000;
  const u16 target_size = jak1_bones_provenance_trace::kTargetMinimumAssertedSize;
  const u16 control_size = jak1_bones_provenance_trace::kControlMinimumAssertedSize;
  const u16 cpad_size = jak1_bones_provenance_trace::kCpadMinimumAssertedSize;
  store_value(memory, kTargetType + jak1_bones_provenance_trace::kTypeAllocatedSizeOffset,
              target_size);
  store_value(memory, kControlType + jak1_bones_provenance_trace::kTypeAllocatedSizeOffset,
              control_size);
  store_value(memory, kCpadType + jak1_bones_provenance_trace::kTypeAllocatedSizeOffset, cpad_size);
  store_value(memory, kTarget - jak1_bones_provenance_trace::kGoalTypeTagBytes, kTargetType);
  store_value(memory, kControl - jak1_bones_provenance_trace::kGoalTypeTagBytes, kControlType);
  store_value(memory, kCpad - jak1_bones_provenance_trace::kGoalTypeTagBytes, kCpadType);
  store_value(memory, kTarget + jak1_bones_provenance_trace::kTargetRootOffset, kControl);
  store_value(memory,
              kTarget + jak1_bones_provenance_trace::kTargetRootOffset +
                  jak1_bones_provenance_trace::kGoalBasicPointerBias,
              kDecoyNodeList);
  store_value(memory,
              kDecoyNodeList - jak1_bones_provenance_trace::kGoalTypeTagBytes,
              kDecoyNodeListType);
  store_value(memory, kTarget + jak1_bones_provenance_trace::kTargetStateOffset, kState);
  store_value(memory, kControl + jak1_bones_provenance_trace::kControlCpadOffset, kCpad);
  const std::array<float, 4> identity_quaternion = {0.f, 0.f, 0.f, 1.f};
  const std::array<float, 4> forward_velocity = {0.f, 0.f, 16384.f, 0.f};
  store_value(memory, kControl + jak1_bones_provenance_trace::kControlDirTargOffset,
              identity_quaternion);
  store_value(memory, kControl + jak1_bones_provenance_trace::kControlQuatForControlOffset,
              identity_quaternion);
  store_value(memory, kControl + jak1_bones_provenance_trace::kControlQuatOffset,
              identity_quaternion);
  store_value(memory, kControl + jak1_bones_provenance_trace::kControlTurnToTargetOffset,
              forward_velocity);
  const float stick_direction = 0.f;
  const float stick_speed = 1.f;
  const float pad_magnitude = 1.f;
  const u64 attack_id = 41;
  const u32 square_abs = metal_merc_transform_trace::TargetControlTracker::kSquareButton;
  const u32 square_rel = metal_merc_transform_trace::TargetControlTracker::kSquareButton;
  const u8 left_x = 128;
  const u8 left_y = 0;
  store_value(memory, kCpad + jak1_bones_provenance_trace::kCpadStickDirectionOffset,
              stick_direction);
  store_value(memory, kCpad + jak1_bones_provenance_trace::kCpadStickSpeedOffset, stick_speed);
  store_value(memory, kControl + jak1_bones_provenance_trace::kControlPadMagnitudeOffset,
              pad_magnitude);
  store_value(memory, kControl + jak1_bones_provenance_trace::kControlTargetAttackIdOffset,
              attack_id);
  store_value(memory, kCpad + jak1_bones_provenance_trace::kCpadButtonAbsOffset, square_abs);
  store_value(memory, kCpad + jak1_bones_provenance_trace::kCpadButtonRelOffset, square_rel);
  store_value(memory, kCpad + jak1_bones_provenance_trace::kCpadLeftXOffset, left_x);
  store_value(memory, kCpad + jak1_bones_provenance_trace::kCpadLeftYOffset, left_y);

  const jak1_bones_provenance_trace::TargetCaptureContext target_context = {
      kTarget, kTargetType, kControlType, kCpadType};
  jak1_bones_provenance_trace::Registry target_registry;
  check(target_registry.record(kOutput, kJoints, kBones, kCount, kCamera, memory.data(),
                               memory.size(), target_context),
        "target-control capture stays attached to a valid producer calculation");
  const auto target_record = target_registry.find_output_base(kOutput);
  check(target_record && target_record->target_control.valid &&
            target_record->target_control.capture_stage ==
                jak1_target_control_capture::Stage::COMPLETE &&
            target_record->target_control.capture_result ==
                jak1_target_control_capture::Result::SUCCESS &&
            target_record->target_control.target_address == kTarget &&
            target_record->target_control.control_address == kControl &&
            target_record->target_control.target_state_id == kState &&
            target_record->target_control.target_attack_id == attack_id &&
            target_record->target_control.button0_abs == square_abs &&
            target_record->target_control.button0_rel == square_rel &&
            target_record->target_control.left_x == left_x &&
            target_record->target_control.left_y == left_y &&
            target_record->target_control.intent_forward.z == 1.0 &&
            target_record->target_control.control_forward.z == 1.0,
        "live BASIC_OFFSET-adjusted fields ignore the old node-list slot and capture pad, facing, "
        "state and attack evidence");

  const auto tight_cpad_end = kCpad + jak1_bones_provenance_trace::kCpadMinimumLiveSize;
  std::vector<u8> tight_memory(memory.begin(), memory.begin() + tight_cpad_end);
  const auto tight_live_span = jak1_bones_provenance_trace::Registry::capture_target_control(
      target_context, tight_memory.data(), tight_memory.size());
  check(tight_live_span.valid &&
            tight_live_span.capture_stage == jak1_target_control_capture::Stage::COMPLETE &&
            tight_live_span.capture_result == jak1_target_control_capture::Result::SUCCESS,
        "an exact live cpad span excludes its preceding type tag while its descriptor retains the "
        "asserted allocated size");

  const auto missing_target = jak1_bones_provenance_trace::Registry::capture_target_control(
      {}, memory.data(), memory.size());
  check(!missing_target.valid &&
            missing_target.capture_stage == jak1_target_control_capture::Stage::CONTEXT &&
            missing_target.capture_result ==
                jak1_target_control_capture::Result::MISSING_TARGET_ADDRESS,
        "a null target context reports its missing address");
  auto invalid_target = target_context;
  invalid_target.target_address = static_cast<u32>(memory.size() - 2);
  const auto invalid_target_span = jak1_bones_provenance_trace::Registry::capture_target_control(
      invalid_target, memory.data(), memory.size());
  check(!invalid_target_span.valid &&
            invalid_target_span.capture_stage == jak1_target_control_capture::Stage::TARGET &&
            invalid_target_span.capture_result ==
                jak1_target_control_capture::Result::OBJECT_SPAN_INVALID,
        "an out-of-range target pointer reports a target object span failure");
  auto wrong_target_type = target_context;
  const u32 wrong_type = 0x400;
  store_value(memory, kTarget - jak1_bones_provenance_trace::kGoalTypeTagBytes, wrong_type);
  const auto mismatched_target_type =
      jak1_bones_provenance_trace::Registry::capture_target_control(
          wrong_target_type, memory.data(), memory.size());
  check(!mismatched_target_type.valid &&
            mismatched_target_type.capture_stage == jak1_target_control_capture::Stage::TARGET &&
            mismatched_target_type.capture_result ==
                jak1_target_control_capture::Result::TYPE_TAG_MISMATCH,
        "a mismatched target runtime type reports the exact-type rejection");
  store_value(memory, kTarget - jak1_bones_provenance_trace::kGoalTypeTagBytes, kTargetType);

  auto unreadable_target_type = target_context;
  unreadable_target_type.target_type = static_cast<u32>(memory.size() - 4);
  store_value(memory, kTarget - jak1_bones_provenance_trace::kGoalTypeTagBytes,
              unreadable_target_type.target_type);
  const auto unreadable_type_descriptor =
      jak1_bones_provenance_trace::Registry::capture_target_control(
          unreadable_target_type, memory.data(), memory.size());
  check(!unreadable_type_descriptor.valid &&
            unreadable_type_descriptor.capture_stage ==
                jak1_target_control_capture::Stage::TARGET &&
            unreadable_type_descriptor.capture_result ==
                jak1_target_control_capture::Result::TYPE_DESCRIPTOR_SPAN_INVALID,
        "an unreadable target type descriptor reports a bounded metadata read failure");
  store_value(memory, kTarget - jak1_bones_provenance_trace::kGoalTypeTagBytes, kTargetType);

  const u16 undersized_target = jak1_bones_provenance_trace::kTargetMinimumAssertedSize - 1;
  store_value(memory, kTargetType + jak1_bones_provenance_trace::kTypeAllocatedSizeOffset,
              undersized_target);
  const auto target_type_too_small =
      jak1_bones_provenance_trace::Registry::capture_target_control(
          target_context, memory.data(), memory.size());
  check(!target_type_too_small.valid &&
            target_type_too_small.capture_stage == jak1_target_control_capture::Stage::TARGET &&
            target_type_too_small.capture_result ==
                jak1_target_control_capture::Result::TYPE_ALLOCATED_SIZE_TOO_SMALL,
        "an undersized exact target type reports its asserted layout failure");
  store_value(memory, kTargetType + jak1_bones_provenance_trace::kTypeAllocatedSizeOffset,
              target_size);

  const u32 unsafe_cpad = static_cast<u32>(memory.size() - 16);
  store_value(memory, kControl + jak1_bones_provenance_trace::kControlCpadOffset, unsafe_cpad);
  const auto invalid_cpad_span = jak1_bones_provenance_trace::Registry::capture_target_control(
      target_context, memory.data(), memory.size());
  check(!invalid_cpad_span.valid &&
            invalid_cpad_span.capture_stage == jak1_target_control_capture::Stage::CPAD &&
            invalid_cpad_span.capture_result ==
                jak1_target_control_capture::Result::OBJECT_SPAN_INVALID,
        "an out-of-range cpad pointer reports its object span failure");
  store_value(memory, kControl + jak1_bones_provenance_trace::kControlCpadOffset, kCpad);

  const float nonfinite_stick_speed = std::numeric_limits<float>::quiet_NaN();
  store_value(memory, kCpad + jak1_bones_provenance_trace::kCpadStickSpeedOffset,
              nonfinite_stick_speed);
  const auto nonfinite_target_field =
      jak1_bones_provenance_trace::Registry::capture_target_control(
          target_context, memory.data(), memory.size());
  check(!nonfinite_target_field.valid &&
            nonfinite_target_field.capture_stage == jak1_target_control_capture::Stage::FIELDS &&
            nonfinite_target_field.capture_result ==
                jak1_target_control_capture::Result::NONFINITE_FIELD,
        "a nonfinite control field reports its capture stage without producing a sample");
  store_value(memory, kCpad + jak1_bones_provenance_trace::kCpadStickSpeedOffset, stick_speed);

  const std::array<float, 4> zero_intent = {};
  store_value(memory, kControl + jak1_bones_provenance_trace::kControlTurnToTargetOffset,
              zero_intent);
  const auto stationary_target = jak1_bones_provenance_trace::Registry::capture_target_control(
      target_context, memory.data(), memory.size());
  check(stationary_target.valid && !stationary_target.intent_forward.valid &&
            stationary_target.capture_stage == jak1_target_control_capture::Stage::COMPLETE &&
            stationary_target.capture_result == jak1_target_control_capture::Result::SUCCESS &&
            stationary_target.button0_rel == square_rel,
        "a neutral intent still retains finite control and Square evidence");
  store_value(memory, kControl + jak1_bones_provenance_trace::kControlTurnToTargetOffset,
              forward_velocity);

  store_identity(memory, kCamera, 9.f);
  check(registry.record(kOutput, kJoints, kBones, kCount, kCamera, memory.data(), memory.size()) &&
            registry.find_output_base(kOutput)->serial == 2,
        "reused output storage resolves to the newest producer calculation");
  for (u32 index = 0; index < jak1_bones_provenance_trace::kCalculationCount; index++) {
    const u32 output = 0x6000 + index * 512;
    registry.record(output, kJoints, kBones, kCount, kCamera, memory.data(), memory.size());
  }
  check(!registry.find_output_base(kOutput) && registry.find_output_base(0x6000 + 63 * 512),
        "the fixed producer ring evicts its oldest record without growing");
  registry.reset();
  check(!registry.find_output_base(0x6000 + 63 * 512) && !registry.latest_post_flag(),
        "reset removes producer and post-flag provenance from an earlier runtime");

  return failures ? 1 : 0;
}
