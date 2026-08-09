#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "decompiler/extractor/jak2_import_composer_internal.h"

namespace {

namespace composer = jak2_import_composer;
namespace fs = std::filesystem;

#define CHECK(condition)                                                           \
  do {                                                                             \
    if (!(condition)) {                                                            \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n'; \
      return false;                                                                \
    }                                                                              \
  } while (false)

class TemporaryRoot {
 public:
  TemporaryRoot() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    root = fs::temp_directory_path() / ("jak2-import-composer-test-" + std::to_string(nonce));
    fs::create_directory(root);
  }

  ~TemporaryRoot() {
    std::error_code error;
    fs::remove_all(root, error);
  }

  fs::path root;
};

void write_text(const fs::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}

std::string read_text(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::set<std::string> direct_entries(const fs::path& root) {
  std::set<std::string> entries;
  for (const auto& entry : fs::directory_iterator(root)) {
    entries.emplace(entry.path().filename().string());
  }
  return entries;
}

bool has_partial(const fs::path& root) {
  if (!fs::exists(root)) {
    return false;
  }
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (entry.path().filename().string().ends_with(".partial")) {
      return true;
    }
  }
  return false;
}

bool completed_foundation_is_preserved_but_never_reports_success() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "foundation.candidate";
  std::vector<composer::Phase> phases;
  composer::Options options;
  options.on_progress = [&](const composer::Progress& progress) {
    phases.push_back(progress.phase);
  };
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::extracting_iso,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         fs::create_directory(paths.work_root / "extracted-iso");
         write_text(paths.work_root / "extracted-iso/buildinfo.json", "verified");
         return {};
       }},
  }};

  const auto result = composer::internal::compose_in_fresh_candidate(candidate, options, stages);
  CHECK(!result);
  if (result.error().code != composer::ErrorCode::prepared_output_unavailable) {
    std::cerr << "unexpected foundation error: " << composer::error_code_name(result.error().code)
              << ": " << result.error().message << '\n';
  }
  CHECK(result.error().code == composer::ErrorCode::prepared_output_unavailable);
  CHECK(result.error().preserved_candidate_root == candidate);
  CHECK(direct_entries(candidate) == std::set<std::string>({".opengoal-import"}));
  CHECK(read_text(candidate / ".opengoal-import/extracted-iso/buildinfo.json") == "verified");
  CHECK(!fs::exists(candidate / "iso"));
  CHECK(!fs::exists(candidate / "fr3"));
  CHECK(!phases.empty());
  CHECK(phases.back() == composer::Phase::extracting_iso);
  return true;
}

bool failure_preserves_candidate_and_active_output() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "failed.candidate";
  const auto active = temporary.root / "active";
  fs::create_directory(active);
  write_text(active / "sentinel", "active stays untouched");
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::extracting_iso,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_text(paths.work_root / "recoverable.marker", "keep me");
         write_text(paths.work_root / "artifact.partial", "remove me");
         return composer::Error{composer::ErrorCode::iso_validation_failed, "synthetic ISO failure",
                                std::nullopt, std::nullopt};
       }},
  }};

  const auto result = composer::internal::compose_in_fresh_candidate(candidate, {}, stages);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::iso_validation_failed);
  CHECK(result.error().preserved_candidate_root == candidate);
  CHECK(fs::is_regular_file(candidate / ".opengoal-import/recoverable.marker"));
  CHECK(!has_partial(candidate));
  CHECK(read_text(active / "sentinel") == "active stays untouched");
  return true;
}

bool cancellation_and_callback_failures_are_typed() {
  TemporaryRoot temporary;
  const auto cancelled_candidate = temporary.root / "cancelled.candidate";
  composer::Options cancelled_options;
  cancelled_options.should_cancel = [] { return true; };
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::extracting_iso,
       [](const composer::internal::WorkPaths&) -> std::optional<composer::Error> { return {}; }},
  }};
  auto result = composer::internal::compose_in_fresh_candidate(cancelled_candidate,
                                                               cancelled_options, stages);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::cancelled);
  CHECK(result.error().preserved_candidate_root == cancelled_candidate);

  const auto callback_candidate = temporary.root / "callback.candidate";
  composer::Options callback_options;
  callback_options.on_progress = [](const composer::Progress&) {
    throw std::runtime_error("synthetic callback failure");
  };
  result =
      composer::internal::compose_in_fresh_candidate(callback_candidate, callback_options, stages);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::callback_failed);
  CHECK(result.error().preserved_candidate_root == callback_candidate);
  return true;
}

bool existing_candidate_is_never_overwritten() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "existing.candidate";
  fs::create_directory(candidate);
  write_text(candidate / "sentinel", "original");
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::extracting_iso,
       [](const composer::internal::WorkPaths&) -> std::optional<composer::Error> { return {}; }},
  }};

  const auto result = composer::internal::compose_in_fresh_candidate(candidate, {}, stages);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::invalid_argument);
  CHECK(!result.error().preserved_candidate_root);
  CHECK(read_text(candidate / "sentinel") == "original");
  return true;
}

}  // namespace

int main() {
  const std::array tests = {
      completed_foundation_is_preserved_but_never_reports_success,
      failure_preserves_candidate_and_active_output,
      cancellation_and_callback_failures_are_typed,
      existing_candidate_is_never_overwritten,
  };
  for (const auto test : tests) {
    if (!test()) {
      return 1;
    }
  }
  std::cout << "Jak II import composer foundation tests passed\n";
  return 0;
}
