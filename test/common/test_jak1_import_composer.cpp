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

#include "common/util/font/font_utils.h"

#include "decompiler/extractor/jak1_import_composer_internal.h"

namespace {

namespace composer = jak1_import_composer;
namespace fs = std::filesystem;

int failures = 0;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n';      \
      ++failures;                                                                         \
    }                                                                                     \
  } while (false)

class TemporaryRoot {
 public:
  TemporaryRoot() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    root = fs::temp_directory_path() / ("jak1-import-composer-test-" + std::to_string(nonce));
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

void test_required_public_text_includes_subtitle_notices() {
  const std::array<std::array<std::string, 3>, 7> expected = {
      std::array<std::string, 3>{"PRESS <PAD_SQUARE> TO TOGGLE SUBTITLES", "SUBTITLES ENABLED",
                                 "SUBTITLES DISABLED"},
      std::array<std::string, 3>{"APPUYER SUR <PAD_SQUARE> POUR ACTIVER LES SOUS-TITRES",
                                 "SOUS-TITRES ACTIVÉS", "SOUS-TITRES DÉSACTIVÉS"},
      std::array<std::string, 3>{"DRÜCKE <PAD_SQUARE> ZUM EIN-/AUSSCHALTEN DER UNTERTITEL",
                                 "UNTERTITEL EINGESCHALTET", "UNTERTITEL AUSGESCHALTET"},
      std::array<std::string, 3>{"PULSA <PAD_SQUARE> PARA ACTIVAR/DESACTIVAR LOS SUBTÍTULOS",
                                 "SUBTÍTULOS ACTIVADOS", "SUBTÍTULOS DESACTIVADOS"},
      std::array<std::string, 3>{"PREMI <PAD_SQUARE> PER ATTIVARE O DISATTIVARE I SOTTOTITOLI",
                                 "SOTTOTITOLI ATTIVATI", "SOTTOTITOLI DISATTIVATI"},
      std::array<std::string, 3>{"<PAD_SQUARE> をおすとじまくをトグルする", "じまくあり",
                                 "じまくなし"},
      std::array<std::string, 3>{"PRESS <PAD_SQUARE> TO TOGGLE SUBTITLES", "SUBTITLES ENABLED",
                                 "SUBTITLES DISABLED"},
  };
  const auto additions = composer::internal::required_public_additions();
  const auto* font = get_font_bank(GameTextVersion::JAK1_V2);

  CHECK(additions.game_text.size() == expected.size());
  CHECK(additions.subtitles.empty());
  for (std::size_t language = 0; language < expected.size(); ++language) {
    const auto& bank = additions.game_text[language];
    CHECK(bank.destination_basename == std::to_string(language) + "COMMON.TXT");
    CHECK(bank.language_id == language);
    CHECK(bank.group_name == "common");
    CHECK(bank.lines.size() == expected[language].size());
    if (bank.lines.size() == expected[language].size()) {
      for (std::size_t line = 0; line < expected[language].size(); ++line) {
        CHECK(bank.lines[line].id == 0x103f + static_cast<std::uint32_t>(line));
        CHECK(font->convert_game_to_utf8(bank.lines[line].encoded_text.c_str()) ==
              expected[language][line]);
      }
    }
  }
}

void test_happy_path_composes_and_cleans() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "happy.candidate";
  std::vector<composer::Phase> phases;
  composer::Options options;
  options.on_progress = [&](const composer::Progress& progress) { phases.push_back(progress.phase); };
  std::optional<composer::Summary> summary;
  const std::array<composer::internal::StageAction, 3> stages = {{
      {composer::Phase::extracting_iso,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_text(paths.work_root / "extracted.marker", "synthetic extraction");
         return {};
       }},
      {composer::Phase::generating_recipe,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_text(paths.work_root / "recipe.marker", "synthetic recipe");
         return {};
       }},
      {composer::Phase::materializing_output,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         fs::create_directories(paths.prepared_root / "iso");
         fs::create_directories(paths.prepared_root / "fr3");
         write_text(paths.prepared_root / "iso/SYNTH.DGO", "archive");
         write_text(paths.prepared_root / "fr3/synth.fr3", "level");
         summary = composer::Summary{1, 2, 0, 1, 12};
         return {};
       }},
  }};

  const auto result =
      composer::internal::compose_in_fresh_candidate(candidate, options, stages, &summary);
  CHECK(result);
  if (result) {
    CHECK(result.value().archives_written == 1);
    CHECK(result.value().fr3_files_written == 1);
    CHECK(result.value().output_bytes == 12);
  }
  CHECK(direct_entries(candidate) == std::set<std::string>({"fr3", "iso"}));
  CHECK(read_text(candidate / "iso/SYNTH.DGO") == "archive");
  CHECK(read_text(candidate / "fr3/synth.fr3") == "level");
  CHECK(!has_partial(candidate));
  CHECK(!phases.empty());
  CHECK(phases.back() == composer::Phase::finalizing_candidate);
}

void test_failure_preserves_candidate_and_active_output() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "failed.candidate";
  const auto active = temporary.root / "active";
  fs::create_directory(active);
  write_text(active / "sentinel", "active stays untouched");
  std::optional<composer::Summary> summary;
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::generating_data,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_text(paths.work_root / "recoverable.marker", "keep me");
         write_text(paths.work_root / "artifact.partial", "remove me");
         return composer::Error{composer::ErrorCode::generated_data_failed,
                                "synthetic generated-data failure", std::nullopt, std::nullopt};
       }},
  }};

  const auto result =
      composer::internal::compose_in_fresh_candidate(candidate, {}, stages, &summary);
  CHECK(!result);
  if (!result) {
    CHECK(result.error().code == composer::ErrorCode::generated_data_failed);
    CHECK(result.error().preserved_candidate_root == candidate);
    CHECK(!result.error().cleanup_error);
  }
  CHECK(fs::is_regular_file(candidate / ".opengoal-import/recoverable.marker"));
  CHECK(!has_partial(candidate));
  CHECK(read_text(active / "sentinel") == "active stays untouched");
}

void test_cancellation_preserves_completed_stages() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "cancelled.candidate";
  bool cancel = false;
  composer::Options options;
  options.should_cancel = [&] { return cancel; };
  std::optional<composer::Summary> summary;
  const std::array<composer::internal::StageAction, 2> stages = {{
      {composer::Phase::extracting_iso,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_text(paths.work_root / "extracted.marker", "recoverable extraction");
         cancel = true;
         return {};
       }},
      {composer::Phase::cataloging_retail,
       [&](const composer::internal::WorkPaths&) -> std::optional<composer::Error> {
         return {};
       }},
  }};

  const auto result =
      composer::internal::compose_in_fresh_candidate(candidate, options, stages, &summary);
  CHECK(!result);
  if (!result) {
    CHECK(result.error().code == composer::ErrorCode::cancelled);
    CHECK(result.error().preserved_candidate_root == candidate);
  }
  CHECK(fs::is_regular_file(candidate / ".opengoal-import/extracted.marker"));
  CHECK(!has_partial(candidate));
}

void test_existing_candidate_is_never_overwritten() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "existing.candidate";
  fs::create_directory(candidate);
  write_text(candidate / "sentinel", "original");
  std::optional<composer::Summary> summary;
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::extracting_iso,
       [&](const composer::internal::WorkPaths&) -> std::optional<composer::Error> { return {}; }},
  }};

  const auto result =
      composer::internal::compose_in_fresh_candidate(candidate, {}, stages, &summary);
  CHECK(!result);
  if (!result) {
    CHECK(result.error().code == composer::ErrorCode::invalid_argument);
    CHECK(!result.error().preserved_candidate_root);
  }
  CHECK(read_text(candidate / "sentinel") == "original");
}

void test_progress_callback_failure_is_typed_and_preserved() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "callback-failed.candidate";
  composer::Options options;
  options.on_progress = [](const composer::Progress&) { throw std::runtime_error("synthetic"); };
  std::optional<composer::Summary> summary;
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::extracting_iso,
       [&](const composer::internal::WorkPaths&) -> std::optional<composer::Error> { return {}; }},
  }};

  const auto result =
      composer::internal::compose_in_fresh_candidate(candidate, options, stages, &summary);
  CHECK(!result);
  if (!result) {
    CHECK(result.error().code == composer::ErrorCode::callback_failed);
    CHECK(result.error().preserved_candidate_root == candidate);
  }
  CHECK(direct_entries(candidate) == std::set<std::string>({".opengoal-import"}));
  CHECK(!has_partial(candidate));
}

void test_public_compose_rejects_invalid_source_pack_before_candidate_creation() {
  TemporaryRoot temporary;
  const auto iso = temporary.root / "synthetic.iso";
  const auto source_pack = temporary.root / "invalid-source-pack";
  const auto resources = temporary.root / "resources";
  const auto candidate = temporary.root / "invalid-pack.candidate";
  write_text(iso, "not reached");
  fs::create_directory(source_pack);
  fs::create_directory(resources);

  const auto result = composer::compose({iso, source_pack, resources, candidate});
  CHECK(!result);
  if (!result) {
    CHECK(result.error().code == composer::ErrorCode::source_pack_failed);
    CHECK(!result.error().preserved_candidate_root);
  }
  CHECK(!fs::exists(candidate));
}

void test_public_compose_rejects_candidate_inside_source_pack() {
  TemporaryRoot temporary;
  const auto iso = temporary.root / "synthetic.iso";
  const auto source_pack = temporary.root / "source-pack";
  const auto resources = temporary.root / "resources";
  const auto candidate = source_pack / "nested.candidate";
  write_text(iso, "not reached");
  fs::create_directory(source_pack);
  fs::create_directory(resources);

  const auto result = composer::compose({iso, source_pack, resources, candidate});
  CHECK(!result);
  if (!result) {
    CHECK(result.error().code == composer::ErrorCode::invalid_argument);
    CHECK(!result.error().preserved_candidate_root);
  }
  CHECK(!fs::exists(candidate));
}

}  // namespace

int main() {
  test_required_public_text_includes_subtitle_notices();
  test_happy_path_composes_and_cleans();
  test_failure_preserves_candidate_and_active_output();
  test_cancellation_preserves_completed_stages();
  test_existing_candidate_is_never_overwritten();
  test_progress_callback_failure_is_typed_and_preserved();
  test_public_compose_rejects_invalid_source_pack_before_candidate_creation();
  test_public_compose_rejects_candidate_inside_source_pack();
  if (failures != 0) {
    std::cerr << failures << " composer test(s) failed.\n";
    return 1;
  }
  std::cout << "All Jak 1 import composer ownership tests passed.\n";
  return 0;
}
