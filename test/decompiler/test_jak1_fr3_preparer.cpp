#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "common/versions/jak1_iso_revisions.h"
#include "decompiler/extractor/jak1_fr3_preparer.h"

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
