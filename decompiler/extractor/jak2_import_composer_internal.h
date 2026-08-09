#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>

#include "decompiler/extractor/jak2_import_composer.h"

namespace jak2_import_composer::internal {

struct WorkPaths {
  std::filesystem::path candidate_root;
  std::filesystem::path work_root;
};

struct StageAction {
  Phase phase = Phase::extracting_iso;
  std::function<std::optional<Error>(const WorkPaths&)> run;
};

/// Run verified-input stages in a fresh recoverable candidate. Until final prepared-output stages
/// exist, completing every supplied stage still returns `prepared_output_unavailable`.
Result<Summary> compose_in_fresh_candidate(const std::filesystem::path& candidate_root,
                                           const Options& options,
                                           std::span<const StageAction> stages);

}  // namespace jak2_import_composer::internal
