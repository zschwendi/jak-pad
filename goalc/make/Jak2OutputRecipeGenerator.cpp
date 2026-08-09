#include "Jak2OutputRecipeGenerator.h"

namespace jak2_output_recipe_generator {
namespace {

Error make_error(ErrorCode code, std::string message) {
  return {code, std::move(message), {}, {}};
}

}  // namespace

Options::Options() {
  limits.max_graph_archives = jak2_public_output_graph::kArchiveLimit;
  recipe_limits.max_archives = jak2_public_output_graph::kArchiveLimit;
}

Result<jak2_output_recipe::Recipe> generate_from_graph(
    const Graph& graph,
    std::string_view source_object_pack_manifest,
    const jak2_iso::Revision& revision,
    const VerifiedInputs& verified_inputs,
    const Options& options) {
  if (graph.ordered_source_files.size() != jak2_source_object_pack::kExpectedObjectCount ||
      graph.archives.size() != kBaseRetailArchiveCount ||
      graph.flat_file_copies.size() != kBaseRetailFlatCopyCount ||
      graph.generated_flat_files.size() != kBaseRetailGeneratedFlatCount) {
    return Result<jak2_output_recipe::Recipe>::failure(make_error(
        ErrorCode::invalid_graph,
        "The Jak II base-retail graph does not match its checked public shape."));
  }

  jak1_output_recipe_generator::VerifiedInputs core_inputs;
  core_inputs.revision = jak2_output_recipe::revision_provenance(revision);
  core_inputs.extracted_iso_root = verified_inputs.extracted_iso_root;
  core_inputs.verified_extracted_iso_relative_paths =
      verified_inputs.verified_extracted_iso_relative_paths;
  core_inputs.retail_catalog = verified_inputs.retail_catalog;
  core_inputs.expected_fr3_basenames = verified_inputs.expected_fr3_basenames;

  jak1_output_recipe_generator::Options core_options;
  core_options.limits = options.limits;
  core_options.recipe_limits = options.recipe_limits;
  core_options.output_profile = jak2_output_recipe::OutputProfile::jak2_base_retail;
  core_options.wire_game = jak1_output_recipe::WireGame::jak2;
  core_options.expected_source_object_count = jak2_source_object_pack::kExpectedObjectCount;
  core_options.should_cancel = options.should_cancel;
  auto generated = jak1_output_recipe_generator::generate_from_graph(
      graph, source_object_pack_manifest, core_inputs, core_options);
  if (!generated) {
    return generated;
  }
  if (generated.value().source_object_pack != jak2_output_recipe::kRecordedSourceObjectPack) {
    return Result<jak2_output_recipe::Recipe>::failure(make_error(
        ErrorCode::manifest_graph_mismatch,
        "The source-object-pack manifest does not match the recorded Jak II bundle."));
  }

  jak2_output_recipe::Options schema_options;
  schema_options.limits = options.recipe_limits;
  schema_options.should_cancel = options.should_cancel;
  const auto encoded = jak2_output_recipe::encode(generated.value(), revision, schema_options);
  if (!encoded) {
    return Result<jak2_output_recipe::Recipe>::failure(make_error(
        encoded.error().code == jak2_output_recipe::ErrorCode::cancelled
            ? ErrorCode::cancelled
            : ErrorCode::recipe_validation_failed,
        "The generated Jak II output recipe failed schema validation: " +
            encoded.error().message));
  }
  return generated;
}

}  // namespace jak2_output_recipe_generator
