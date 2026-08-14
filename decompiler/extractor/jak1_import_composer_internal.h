#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>

#include "decompiler/extractor/jak1_extracted_generated_inputs.h"
#include "decompiler/extractor/jak1_import_composer.h"

namespace jak1_import_composer::internal {

struct WorkPaths {
  std::filesystem::path candidate_root;
  std::filesystem::path work_root;
  std::filesystem::path prepared_root;
};

struct StageAction {
  Phase phase = Phase::extracting_iso;
  std::function<std::optional<Error>(const WorkPaths&)> run;
};

jak1_extracted_generated_inputs::PublicAdditions required_public_additions();

Result<Summary> compose_in_fresh_candidate(const std::filesystem::path& candidate_root,
                                           const Options& options,
                                           std::span<const StageAction> stages,
                                           const std::optional<Summary>* produced_summary);

}  // namespace jak1_import_composer::internal
