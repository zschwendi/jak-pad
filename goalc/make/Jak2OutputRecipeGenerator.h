#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "common/custom_data/Jak2OutputRecipe.h"
#include "common/custom_data/Jak2PublicOutputGraph.h"
#include "goalc/make/Jak1OutputRecipeGenerator.h"

namespace jak2_output_recipe_generator {

using Graph = jak1_output_recipe_generator::Graph;
using RetailCatalogObject = jak1_output_recipe_generator::RetailCatalogObject;
using Limits = jak1_output_recipe_generator::Limits;
using ErrorCode = jak1_output_recipe_generator::ErrorCode;
using Error = jak1_output_recipe_generator::Error;

template <typename T>
using Result = jak1_output_recipe_generator::Result<T>;

inline constexpr std::size_t kBaseRetailArchiveCount = 150;
inline constexpr std::size_t kBaseRetailFlatCopyCount = 408;
inline constexpr std::size_t kBaseRetailGeneratedFlatCount = 9;

struct VerifiedInputs {
  std::string extracted_iso_root;
  std::vector<std::string> verified_extracted_iso_relative_paths;
  std::vector<RetailCatalogObject> retail_catalog;
  std::vector<std::string> expected_fr3_basenames;
};

struct Options {
  Limits limits;
  jak2_output_recipe::Limits recipe_limits;
  jak2_output_recipe::CancelCallback should_cancel;

  Options();
};

/// Generate the checked base-retail recipe from `decode_base_retail()` and the manifest already
/// accepted by `jak2_source_object_pack::validate_recorded`.
Result<jak2_output_recipe::Recipe> generate_from_graph(
    const Graph& graph,
    std::string_view source_object_pack_manifest,
    const jak2_iso::Revision& revision,
    const VerifiedInputs& verified_inputs,
    const Options& options = {});

inline const char* error_code_name(ErrorCode code) {
  return jak1_output_recipe_generator::error_code_name(code);
}

}  // namespace jak2_output_recipe_generator
