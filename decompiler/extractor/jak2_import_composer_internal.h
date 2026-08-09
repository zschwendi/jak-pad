#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "decompiler/extractor/jak2_import_composer.h"

namespace jak2_import_composer::internal {

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
  Phase phase = Phase::extracting_iso;
  std::function<std::optional<Error>(const WorkPaths&)> run;
};

Result<Summary> compose_in_fresh_candidate(const std::filesystem::path& candidate_root,
                                           const Options& options,
                                           std::span<const StageAction> stages,
                                           const std::optional<Summary>* produced_summary,
                                           const std::optional<FinalContract>* produced_contract);

}  // namespace jak2_import_composer::internal
