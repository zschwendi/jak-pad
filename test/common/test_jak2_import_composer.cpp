#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/custom_data/Jak2PublicOutputGraph.h"

#include "decompiler/extractor/jak2_fr3_preparer.h"
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

void write_text(const fs::path& path, const std::string& text = "synthetic") {
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

composer::internal::FinalContract launch_contract() {
  return {{"KERNEL.CGO", "GAME.CGO", "TITLE.DGO", "CWI.DGO", "CTA.DGO", "PRI.DGO",
           "FEA.DGO", "INTROCST.DGO", "LDJAKBRN.DGO", "DEMO1.SBK", "CTYWIDE1.SBK",
           "FOREXIT1.SBK", "FOREXIT2.SBK"},
          {"GAME.fr3", "title.fr3", "ctywide.fr3", "ctysluma.fr3", "prison.fr3",
           "forexita.fr3", "introcst.fr3", "ldjakbrn.fr3"}};
}

void write_prepared_tree(const composer::internal::WorkPaths& paths,
                         const composer::internal::FinalContract& contract) {
  fs::create_directories(paths.prepared_root / "iso");
  fs::create_directory(paths.prepared_root / "fr3");
  for (const auto& name : contract.iso_basenames) {
    write_text(paths.prepared_root / "iso" / name);
  }
  for (const auto& name : contract.fr3_basenames) {
    write_text(paths.prepared_root / "fr3" / name);
  }
}

bool exact_prepared_tree_is_promoted() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "complete.candidate";
  const auto contract = launch_contract();
  std::optional<composer::Summary> summary = composer::Summary{2, 4, 11, 8, 1234};
  std::optional<composer::internal::FinalContract> produced_contract = contract;
  std::vector<composer::Phase> phases;
  composer::Options options;
  options.on_progress = [&](const composer::Progress& progress) {
    phases.push_back(progress.phase);
  };
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::materializing_output,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_prepared_tree(paths, contract);
         return {};
       }},
  }};

  const auto result = composer::internal::compose_in_fresh_candidate(
      candidate, options, stages, &summary, &produced_contract);
  CHECK(result);
  CHECK(result.value().output_bytes == 1234);
  CHECK(direct_entries(candidate) == std::set<std::string>({"fr3", "iso"}));
  CHECK(direct_entries(candidate / "iso") ==
        std::set<std::string>(contract.iso_basenames.begin(), contract.iso_basenames.end()));
  CHECK(direct_entries(candidate / "fr3") ==
        std::set<std::string>(contract.fr3_basenames.begin(), contract.fr3_basenames.end()));
  CHECK(!fs::exists(candidate / ".opengoal-import"));
  CHECK(!phases.empty());
  CHECK(phases.back() == composer::Phase::finalizing_candidate);
  return true;
}

bool incomplete_or_extra_output_never_succeeds() {
  TemporaryRoot temporary;
  const auto contract = launch_contract();
  std::optional<composer::Summary> summary = composer::Summary{2, 4, 11, 8, 1234};
  std::optional<composer::internal::FinalContract> produced_contract = contract;

  const auto missing_candidate = temporary.root / "missing.candidate";
  const std::array<composer::internal::StageAction, 1> missing_stages = {{
      {composer::Phase::materializing_output,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_prepared_tree(paths, contract);
         fs::remove(paths.prepared_root / "iso/KERNEL.CGO");
         return {};
       }},
  }};
  auto result = composer::internal::compose_in_fresh_candidate(
      missing_candidate, {}, missing_stages, &summary, &produced_contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::candidate_finalize_failed);
  CHECK(result.error().preserved_candidate_root == missing_candidate);
  CHECK(!fs::exists(missing_candidate / "iso"));

  const auto extra_candidate = temporary.root / "extra.candidate";
  const std::array<composer::internal::StageAction, 1> extra_stages = {{
      {composer::Phase::materializing_output,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_prepared_tree(paths, contract);
         write_text(paths.prepared_root / "iso/unexpected.bin");
         return {};
       }},
  }};
  result = composer::internal::compose_in_fresh_candidate(
      extra_candidate, {}, extra_stages, &summary, &produced_contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::candidate_finalize_failed);
  CHECK(result.error().preserved_candidate_root == extra_candidate);
  return true;
}

bool linked_output_is_rejected() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "linked.candidate";
  const auto contract = launch_contract();
  std::optional<composer::Summary> summary = composer::Summary{2, 4, 11, 8, 1234};
  std::optional<composer::internal::FinalContract> produced_contract = contract;
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::materializing_output,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_prepared_tree(paths, contract);
         fs::remove(paths.prepared_root / "fr3/title.fr3");
         fs::create_symlink("GAME.fr3", paths.prepared_root / "fr3/title.fr3");
         return {};
       }},
  }};
  const auto result = composer::internal::compose_in_fresh_candidate(
      candidate, {}, stages, &summary, &produced_contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::candidate_finalize_failed);
  CHECK(result.error().preserved_candidate_root == candidate);
  return true;
}

bool failure_preserves_work_and_removes_partial_files() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "failed.candidate";
  const auto active = temporary.root / "active";
  fs::create_directory(active);
  write_text(active / "sentinel", "active stays untouched");
  std::optional<composer::Summary> summary;
  std::optional<composer::internal::FinalContract> contract;
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::extracting_iso,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_text(paths.work_root / "recoverable.marker", "keep me");
         write_text(paths.work_root / "artifact.partial", "remove me");
         return composer::Error{composer::ErrorCode::iso_validation_failed, "synthetic failure",
                                std::nullopt, std::nullopt};
       }},
  }};
  const auto result = composer::internal::compose_in_fresh_candidate(
      candidate, {}, stages, &summary, &contract);
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
  std::optional<composer::Summary> summary;
  std::optional<composer::internal::FinalContract> contract;
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::extracting_iso,
       [](const composer::internal::WorkPaths&) -> std::optional<composer::Error> { return {}; }},
  }};
  composer::Options cancelled;
  cancelled.should_cancel = [] { return true; };
  const auto cancelled_candidate = temporary.root / "cancelled.candidate";
  auto result = composer::internal::compose_in_fresh_candidate(
      cancelled_candidate, cancelled, stages, &summary, &contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::cancelled);
  CHECK(result.error().preserved_candidate_root == cancelled_candidate);

  composer::Options callback;
  callback.on_progress = [](const composer::Progress&) {
    throw std::runtime_error("synthetic callback failure");
  };
  const auto callback_candidate = temporary.root / "callback.candidate";
  result = composer::internal::compose_in_fresh_candidate(
      callback_candidate, callback, stages, &summary, &contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::callback_failed);
  CHECK(result.error().preserved_candidate_root == callback_candidate);
  return true;
}

bool existing_candidate_and_input_containment_are_rejected() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "existing.candidate";
  fs::create_directory(candidate);
  write_text(candidate / "sentinel", "original");
  std::optional<composer::Summary> summary;
  std::optional<composer::internal::FinalContract> contract;
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::extracting_iso,
       [](const composer::internal::WorkPaths&) -> std::optional<composer::Error> { return {}; }},
  }};
  auto result = composer::internal::compose_in_fresh_candidate(
      candidate, {}, stages, &summary, &contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::invalid_argument);
  CHECK(!result.error().preserved_candidate_root);
  CHECK(read_text(candidate / "sentinel") == "original");

  const auto iso = temporary.root / "disc.iso";
  const auto source = temporary.root / "source-pack";
  const auto resources = temporary.root / "resources";
  fs::create_directory(source);
  fs::create_directory(resources);
  write_text(iso);
  const auto contained_candidate = source / "candidate";
  result = composer::compose({iso, source, resources, contained_candidate});
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::invalid_argument);
  CHECK(!fs::exists(contained_candidate));
  return true;
}

bool invalid_source_pack_fails_before_candidate_creation() {
  TemporaryRoot temporary;
  const auto iso = temporary.root / "disc.iso";
  const auto source = temporary.root / "source-pack";
  const auto resources = temporary.root / "resources";
  const auto candidate = temporary.root / "candidate";
  write_text(iso);
  fs::create_directory(source);
  fs::create_directory(resources);
  const auto result = composer::compose({iso, source, resources, candidate});
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::source_pack_failed);
  CHECK(!result.error().preserved_candidate_root);
  CHECK(!fs::exists(candidate));
  return true;
}

bool checked_graph_contains_the_iso_launch_contract() {
  const auto graph = jak2_public_output_graph::decode_base_retail();
  CHECK(graph);
  std::set<std::string> destinations;
  for (const auto& archive : graph.value().archives) {
    destinations.emplace(archive.destination_basename);
  }
  for (const auto& copy : graph.value().flat_file_copies) {
    destinations.emplace(copy.destination_basename);
  }
  for (const auto& generated : graph.value().generated_flat_files) {
    destinations.emplace(generated.destination_basename);
  }
  const auto contract = launch_contract();
  for (const auto& required : contract.iso_basenames) {
    CHECK(destinations.contains(required));
  }
  return true;
}

bool optional_real_import_oracle() {
  constexpr std::array variables = {
      "OPENGOAL_JAK2_IMPORT_TEST_ISO",
      "OPENGOAL_JAK2_IMPORT_TEST_SOURCE_PACK",
      "OPENGOAL_JAK2_IMPORT_TEST_PROJECT_RESOURCES",
      "OPENGOAL_JAK2_IMPORT_TEST_CANDIDATE",
  };
  std::array<const char*, variables.size()> values{};
  std::size_t configured = 0;
  for (std::size_t index = 0; index < variables.size(); ++index) {
    values[index] = std::getenv(variables[index]);
    configured += values[index] && values[index][0] != '\0';
  }
  if (configured == 0) {
    std::cout << "Jak II real-import oracle skipped (environment unset)\n";
    return true;
  }
  CHECK(configured == variables.size());

  std::vector<composer::Phase> phases;
  composer::Options options;
  options.on_progress = [&](const composer::Progress& progress) {
    phases.push_back(progress.phase);
  };
  const composer::Request request{fs::path(values[0]), fs::path(values[1]), fs::path(values[2]),
                                  fs::path(values[3])};
  const auto result = composer::compose(request, options);
  if (!result) {
    std::cerr << "real-import oracle failed: " << composer::error_code_name(result.error().code)
              << ": " << result.error().message << '\n';
    return false;
  }
  CHECK(result.value().archives_written == 150);
  CHECK(result.value().flat_files_written == 417);
  CHECK(result.value().objects_written > 0);
  CHECK(result.value().fr3_files_written == jak2_fr3::kNtscV2ExpectedFr3Files);
  CHECK(result.value().output_bytes > 0);
  CHECK(!fs::exists(request.candidate_root / ".opengoal-import"));
  CHECK(direct_entries(request.candidate_root) == std::set<std::string>({"fr3", "iso"}));
  CHECK(direct_entries(request.candidate_root / "iso").size() == 567);
  CHECK(direct_entries(request.candidate_root / "fr3").size() ==
        result.value().fr3_files_written);
  const auto contract = launch_contract();
  for (const auto& name : contract.iso_basenames) {
    CHECK(fs::symlink_status(request.candidate_root / "iso" / name).type() ==
          fs::file_type::regular);
  }
  for (const auto& name : contract.fr3_basenames) {
    CHECK(fs::symlink_status(request.candidate_root / "fr3" / name).type() ==
          fs::file_type::regular);
  }
  CHECK(!phases.empty());
  CHECK(phases.back() == composer::Phase::finalizing_candidate);
  std::cout << "Jak II base-retail real-import oracle passed: "
            << result.value().archives_written
            << " archives, " << result.value().objects_written << " objects, "
            << result.value().flat_files_written << " flat files, "
            << result.value().fr3_files_written << " FR3 files\n";
  return true;
}

}  // namespace

int main() {
  const std::array tests = {
      exact_prepared_tree_is_promoted,
      incomplete_or_extra_output_never_succeeds,
      linked_output_is_rejected,
      failure_preserves_work_and_removes_partial_files,
      cancellation_and_callback_failures_are_typed,
      existing_candidate_and_input_containment_are_rejected,
      invalid_source_pack_fails_before_candidate_creation,
      checked_graph_contains_the_iso_launch_contract,
      optional_real_import_oracle,
  };
  for (const auto test : tests) {
    if (!test()) {
      return 1;
    }
  }
  std::cout << "Jak II import composer tests passed\n";
  return 0;
}
