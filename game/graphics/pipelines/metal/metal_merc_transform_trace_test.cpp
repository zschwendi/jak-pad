#include "game/graphics/pipelines/metal/metal_merc_transform_trace.h"

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

metal_merc_transform_trace::BasisSnapshot yaw_basis(double degrees) {
  constexpr double kPi = 3.14159265358979323846;
  const double radians = degrees * kPi / 180.0;
  const float cosine = static_cast<float>(std::cos(radians));
  const float sine = static_cast<float>(std::sin(radians));
  const std::array<float, 12> matrix = {cosine, 0.f, sine,  0.f, 0.f,    1.f,
                                        0.f,    0.f, -sine, 0.f, cosine, 0.f};
  return metal_merc_transform_trace::make_basis_snapshot(matrix.data());
}

metal_merc_transform_trace::ProvenanceObservation provenance(double input_degrees,
                                                             double camera_degrees,
                                                             double output_degrees,
                                                             u64 identity,
                                                             bool mapping_valid = true) {
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

}  // namespace

int main() {
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

  auto& registry = jak1_bones_provenance_trace::registry();
  registry.reset();
  std::vector<u8> memory(0x10000, 0);
  constexpr u32 kOutput = 0x1000;
  constexpr u32 kBones = 0x3000;
  constexpr u32 kCamera = 0x5000;
  constexpr u32 kCount = 4;
  for (u32 bone = 0; bone < kCount; bone++) {
    store_identity(memory, kBones + bone * jak1_bones_provenance_trace::kBoneStride,
                   static_cast<float>(bone));
  }
  store_identity(memory, kCamera, 0.f);
  check(registry.record(kOutput, kBones, kCount, kCamera, memory.data(), memory.size()),
        "the producer registry records an in-bounds calculation");
  const auto recorded = registry.find_output_base(kOutput);
  check(recorded && recorded->serial == 1 && recorded->bone_count == kCount &&
            recorded->camera.valid && recorded->root_anchors[0].valid &&
            recorded->root_anchors[1].valid && recorded->root_anchors[2].valid,
        "one record retains camera plus align, prejoint and main transforms");
  const auto source_match =
      registry.find_source_address(kOutput + 3 * jak1_bones_provenance_trace::kOutputStride);
  check(source_match && source_match->output_base == kOutput &&
            !registry.find_source_address(kOutput + 1),
        "source lookup accepts an aligned palette member and rejects a misaligned address");
  check(!registry.record(memory.size() - 64, kBones, kCount, kCamera, memory.data(), memory.size()),
        "the producer registry rejects an output span outside EE memory");

  store_identity(memory, kCamera, 9.f);
  check(registry.record(kOutput, kBones, kCount, kCamera, memory.data(), memory.size()) &&
            registry.find_output_base(kOutput)->serial == 2,
        "reused output storage resolves to the newest producer calculation");
  for (u32 index = 0; index < jak1_bones_provenance_trace::kCalculationCount; index++) {
    const u32 output = 0x6000 + index * 512;
    registry.record(output, kBones, kCount, kCamera, memory.data(), memory.size());
  }
  check(!registry.find_output_base(kOutput) && registry.find_output_base(0x6000 + 63 * 512),
        "the fixed producer ring evicts its oldest record without growing");
  registry.reset();
  check(!registry.find_output_base(0x6000 + 63 * 512),
        "reset removes producer provenance from an earlier runtime");

  return failures ? 1 : 0;
}
