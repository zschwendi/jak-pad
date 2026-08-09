#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "decompiler/extractor/jak2_fr3_preparer.h"

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
    root = fs::temp_directory_path() / ("jak2-fr3-preparer-test-" + std::to_string(nonce));
    fs::create_directory(root);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }

  fs::path root;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: test_jak2_fr3_preparer PROJECT_ROOT\n";
    return 2;
  }
  const fs::path project_root = argv[1];

  jak2_fr3::Options defaults;
  CHECK(defaults.max_archives == 256);
  CHECK(defaults.max_levels == 256);
  CHECK(!defaults.require_game_count);
  constexpr std::uintmax_t kRecordedTotalExpandedArchiveBytes = 596'611'024;
  CHECK(jak2_fr3::kNtscV2TotalExpandedArchiveBytes ==
        kRecordedTotalExpandedArchiveBytes);
  CHECK(defaults.max_total_expanded_archive_bytes ==
        kRecordedTotalExpandedArchiveBytes);
  CHECK(kRecordedTotalExpandedArchiveBytes <= defaults.max_total_expanded_archive_bytes);
  CHECK(kRecordedTotalExpandedArchiveBytes + 1 >
        defaults.max_total_expanded_archive_bytes);
  constexpr std::uintmax_t kMeasuredFr3WorkBytes = 1'186'140'404;
  constexpr std::uintmax_t kMaxFr3WorkBytes = 1'342'177'280;
  CHECK(jak2_fr3::kNtscV2MeasuredFr3WorkBytes == kMeasuredFr3WorkBytes);
  CHECK(jak2_fr3::kNtscV2MaxFr3WorkBytes == kMaxFr3WorkBytes);
  CHECK(defaults.max_output_bytes == kMaxFr3WorkBytes);
  CHECK(kMeasuredFr3WorkBytes <= defaults.max_output_bytes);
  CHECK(defaults.max_output_bytes - kMeasuredFr3WorkBytes == 156'036'876);
  CHECK(kMaxFr3WorkBytes - 1 <= defaults.max_output_bytes);
  CHECK(kMaxFr3WorkBytes <= defaults.max_output_bytes);
  CHECK(kMaxFr3WorkBytes + 1 > defaults.max_output_bytes);
  CHECK(jak2_fr3::kNtscV2TrackedLevelCount == 147);
  CHECK(jak2_fr3::kNtscV2ExpectedFr3Files == 148);
  CHECK(jak2_fr3::kNtscV2ExpectedFr3Files == jak2_fr3::kNtscV2TrackedLevelCount + 1);
  CHECK(defaults.compressed_trailing_alignment_bytes ==
        jak2_fr3::kNtscV2CompressedArchiveAlignmentBytes);
  constexpr std::size_t kLwidebPayloadEnd = 0x14b6d0;
  constexpr std::size_t kLwidebTrailingPadding = 0x34930;
  CHECK(kLwidebPayloadEnd + kLwidebTrailingPadding == 0x180000);
  CHECK((kLwidebPayloadEnd + kLwidebTrailingPadding) %
            jak2_fr3::kNtscV2CompressedArchiveAlignmentBytes ==
        0);
  CHECK(kLwidebTrailingPadding < jak2_fr3::kNtscV2CompressedArchiveAlignmentBytes);

  auto invalid = jak2_fr3::prepare({}, {}, {});
  CHECK(!invalid);
  CHECK(invalid.error().code == jak2_fr3::ErrorCode::invalid_argument);

  jak2_fr3::Options cancelled_options;
  cancelled_options.should_cancel = [] { return true; };
  auto cancelled = jak2_fr3::prepare("project", "iso", "work", cancelled_options);
  CHECK(!cancelled);
  CHECK(cancelled.error().code == jak2_fr3::ErrorCode::cancelled);

  TemporaryDirectory temporary;
  const auto iso = temporary.root / "iso";
  fs::create_directory(iso);

  jak2_fr3::Options broken_progress_options;
  broken_progress_options.report_progress = [](const jak2_fr3::Progress&) {
    throw std::runtime_error("callback");
  };
  auto broken_progress = jak2_fr3::prepare(
      project_root, iso, temporary.root / "broken-progress", broken_progress_options);
  CHECK(!broken_progress);
  CHECK(broken_progress.error().code == jak2_fr3::ErrorCode::callback_failed);
  CHECK(!fs::exists(temporary.root / "broken-progress"));

  std::vector<jak2_fr3::Progress> progress;
  jak2_fr3::Options narrow_options;
  narrow_options.max_archives = 64;
  narrow_options.report_progress = [&](const jak2_fr3::Progress& update) {
    progress.push_back(update);
  };
  const auto narrow_work = temporary.root / "narrow-work";
  auto narrow = jak2_fr3::prepare(project_root, iso, narrow_work, narrow_options);
  CHECK(!narrow);
  CHECK(narrow.error().code == jak2_fr3::ErrorCode::archive_limit_exceeded);
  CHECK(narrow.error().message.find("Jak II archive list") != std::string::npos);
  CHECK(progress.size() == 2);
  CHECK(progress.front().phase == jak2_fr3::Phase::loading_configuration);
  CHECK(progress.front().completed == 0);
  CHECK(progress.back().phase == jak2_fr3::Phase::loading_configuration);
  CHECK(progress.back().completed == 1);
  CHECK(!fs::exists(narrow_work));

  jak2_fr3::Options narrow_levels_options;
  narrow_levels_options.max_levels = jak2_fr3::kNtscV2TrackedLevelCount - 1;
  const auto narrow_levels_work = temporary.root / "narrow-levels-work";
  auto narrow_levels =
      jak2_fr3::prepare(project_root, iso, narrow_levels_work, narrow_levels_options);
  CHECK(!narrow_levels);
  CHECK(narrow_levels.error().code == jak2_fr3::ErrorCode::archive_limit_exceeded);
  CHECK(narrow_levels.error().message.find("Jak II level list") != std::string::npos);
  CHECK(!fs::exists(narrow_levels_work));

  jak2_fr3::Options exact_levels_options;
  exact_levels_options.max_levels = jak2_fr3::kNtscV2TrackedLevelCount;
  const auto missing_work = temporary.root / "missing-work";
  auto missing = jak2_fr3::prepare(project_root, iso, missing_work, exact_levels_options);
  CHECK(!missing);
  CHECK(missing.error().code == jak2_fr3::ErrorCode::input_missing);
  CHECK(missing.error().message.find("Jak II text object") != std::string::npos);
  CHECK(!fs::exists(missing_work));

  CHECK(std::string(jak2_fr3::error_code_name(jak2_fr3::ErrorCode::unsafe_output)) ==
        "unsafe_output");
  return 0;
}
