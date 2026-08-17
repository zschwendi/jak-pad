#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "decompiler/extractor/jak2_import_composer.h"
#include "decompiler/extractor/jak2_iso_validation.h"
#include "decompiler/extractor/jak1_retail_object_catalog.h"
#include "goalc/make/Jak2OutputRecipeGenerator.h"

namespace jak2_import_composer::internal {

inline constexpr std::size_t kNtscV2RetailOccurrenceCount = 1724;
inline constexpr std::size_t kNtscV2RetailObjectCount = 1278;
inline constexpr std::size_t kNtscV2RetailArchiveCount = 149;

struct RetailObjectRequirement {
  std::string source_archive_relative_path;
  std::string internal_name;
  std::string unique_name;

  bool operator==(const RetailObjectRequirement&) const = default;
};

struct RetailRequirements {
  std::size_t occurrence_count = 0;
  std::vector<std::string> source_archive_relative_paths;
  std::vector<RetailObjectRequirement> objects;
};

Result<RetailRequirements> derive_retail_requirements(
    const jak2_output_recipe_generator::Graph& graph,
    const Options& options = {});

Result<std::vector<jak2_output_recipe_generator::RetailCatalogObject>>
select_exact_retail_catalog(
    const RetailRequirements& requirements,
    std::span<const jak1_retail_object_catalog::Entry> entries,
    std::uint64_t max_total_object_bytes,
    const Options& options = {});

std::optional<Error> write_file_atomically(const std::filesystem::path& destination,
                                           std::span<const std::uint8_t> bytes,
                                           const Options& options = {});

Error map_iso_extraction_failure(const jak2_iso::ValidationError& error);

struct WorkPaths {
  std::filesystem::path candidate_root;
  std::filesystem::path work_root;
  std::filesystem::path prepared_root;
};

struct FinalContract {
  std::vector<std::string> iso_basenames;
  std::vector<std::string> fr3_basenames;
};

struct StageAction {
  StageAction(Phase stage_phase,
              std::function<std::optional<Error>(const WorkPaths&)> stage_run)
      : phase(stage_phase), run(std::move(stage_run)) {}
  StageAction(
      Phase stage_phase,
      std::function<std::optional<Error>(const WorkPaths&, const Options&)> stage_run)
      : phase(stage_phase), run_with_options(std::move(stage_run)) {}

  Phase phase = Phase::extracting_iso;
  std::function<std::optional<Error>(const WorkPaths&)> run;
  std::function<std::optional<Error>(const WorkPaths&, const Options&)> run_with_options;
};

Result<Summary> compose_in_fresh_candidate(const std::filesystem::path& candidate_root,
                                           const Options& options,
                                           std::span<const StageAction> stages,
                                           const std::optional<Summary>* produced_summary,
                                           const std::optional<FinalContract>* produced_contract);

}  // namespace jak2_import_composer::internal
