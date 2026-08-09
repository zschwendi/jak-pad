#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

#include "common/versions/jak1_iso_revisions.h"
#include "decompiler/extractor/jak1_fr3_preparer.h"
#include "decompiler/level_extractor/level_output_policy.h"

namespace {

namespace fs = std::filesystem;

#define CHECK(condition)                                                                      \
  do {                                                                                        \
    if (!(condition)) {                                                                       \
      std::cerr << "check failed at line " << __LINE__ << ": " #condition << '\n';          \
      return 1;                                                                               \
    }                                                                                         \
  } while (false)

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    root = fs::temp_directory_path() / ("jak1-fr3-preparer-test-" + std::to_string(nonce));
    fs::create_directory(root);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }

  fs::path root;
};

}  // namespace

int main() {
  const auto& revision = jak1_iso::default_revision();

  jak1_fr3::Options defaults;
  CHECK(defaults.max_total_expanded_archive_bytes == 512ull * 1024 * 1024);
  CHECK(defaults.max_output_bytes == 768ull * 1024 * 1024);
  CHECK(defaults.require_game_count);
  CHECK(!defaults.expected_distinct_fr3_files);
  CHECK(!defaults.require_validated_file_identities);

  std::set<std::string> expected_outputs = {"GAME.fr3"};
  std::set<std::string> level_outputs;
  CHECK(jak1_fr3::internal::update_expected_fr3_outputs(
            &expected_outputs, &level_outputs, "alpha.fr3", 1, 3) ==
        jak1_fr3::internal::LevelOutputUpdate::added);
  const auto expected_after_add = expected_outputs;
  const auto levels_after_add = level_outputs;
  CHECK(jak1_fr3::internal::update_expected_fr3_outputs(
            &expected_outputs, &level_outputs, "alpha.fr3", 0, 3) ==
        jak1_fr3::internal::LevelOutputUpdate::invalid);
  CHECK(expected_outputs == expected_after_add);
  CHECK(level_outputs == levels_after_add);
  CHECK(jak1_fr3::internal::update_expected_fr3_outputs(
            &expected_outputs, &level_outputs, "GAME.fr3", 0, 2) ==
        jak1_fr3::internal::LevelOutputUpdate::invalid);
  CHECK(jak1_fr3::internal::update_expected_fr3_outputs(
            &expected_outputs, &level_outputs, "../unsafe.fr3", 0, 2) ==
        jak1_fr3::internal::LevelOutputUpdate::invalid);

  TemporaryDirectory unsafe_output_temporary;
  const auto inside_marker = unsafe_output_temporary.root / "inside-writer-mutation";
  const auto outside_marker = unsafe_output_temporary.root / "outside-writer-mutation";
  const auto rejected_output = decompiler::internal::validate_and_write_level_output(
      "../escape", jak1_fr3::internal::safe_fr3_output_basename,
      [&](std::string_view) {
        fs::create_directory(inside_marker);
        fs::create_directory(outside_marker);
      });
  CHECK(!rejected_output);
  CHECK(!fs::exists(inside_marker));
  CHECK(!fs::exists(outside_marker));
  const auto accepted_marker = unsafe_output_temporary.root / "accepted-writer-mutation";
  const auto accepted_output = decompiler::internal::validate_and_write_level_output(
      "safe-name", jak1_fr3::internal::safe_fr3_output_basename,
      [&](std::string_view) { fs::create_directory(accepted_marker); });
  CHECK(accepted_output == "safe-name.fr3");
  CHECK(fs::is_directory(accepted_marker));

  auto invalid = jak1_fr3::prepare({}, {}, {}, revision);
  CHECK(!invalid);
  CHECK(invalid.error().code == jak1_fr3::ErrorCode::invalid_argument);

  auto unsupported_revision = revision;
  unsupported_revision.contents_hash ^= 1;
  auto unsupported = jak1_fr3::prepare("project", "iso", "work", unsupported_revision);
  CHECK(!unsupported);
  CHECK(unsupported.error().code == jak1_fr3::ErrorCode::unsupported_revision);

  jak1_fr3::Options cancelled_options;
  cancelled_options.should_cancel = [] { return true; };
  auto cancelled = jak1_fr3::prepare("project", "iso", "work", revision, cancelled_options);
  CHECK(!cancelled);
  CHECK(cancelled.error().code == jak1_fr3::ErrorCode::cancelled);

  jak1_fr3::Options broken_cancel_options;
  broken_cancel_options.should_cancel = []() -> bool { throw std::runtime_error("callback"); };
  auto broken_cancel =
      jak1_fr3::prepare("project", "iso", "work", revision, broken_cancel_options);
  CHECK(!broken_cancel);
  CHECK(broken_cancel.error().code == jak1_fr3::ErrorCode::callback_failed);

  TemporaryDirectory temporary;
  const auto project = temporary.root / "project";
  const auto iso = temporary.root / "iso";
  const auto work = temporary.root / "work";
  fs::create_directory(project);
  fs::create_directory(iso);

  jak1_fr3::Options broken_progress_options;
  broken_progress_options.report_progress = [](const jak1_fr3::Progress&) {
    throw std::runtime_error("callback");
  };
  auto broken_progress =
      jak1_fr3::prepare(project, iso, work, revision, broken_progress_options);
  CHECK(!broken_progress);
  CHECK(broken_progress.error().code == jak1_fr3::ErrorCode::callback_failed);
  CHECK(!fs::exists(work));

  fs::create_directory(work);
  auto existing = jak1_fr3::prepare(project, iso, work, revision);
  CHECK(!existing);
  CHECK(existing.error().code == jak1_fr3::ErrorCode::output_exists);

  CHECK(std::string(jak1_fr3::error_code_name(jak1_fr3::ErrorCode::unsafe_output)) ==
        "unsafe_output");
  return 0;
}
