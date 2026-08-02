#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>

#include "common/log/log.h"

#include "decompiler/extractor/jak1_fr3_preparer.h"

namespace {

std::atomic_bool g_cancelled = false;

void cancel(int) {
  g_cancelled.store(true, std::memory_order_relaxed);
}

const char* phase_name(jak1_fr3::Phase phase) {
  switch (phase) {
    case jak1_fr3::Phase::loading_configuration:
      return "configuration";
    case jak1_fr3::Phase::reading_archives:
      return "archives";
    case jak1_fr3::Phase::linking_objects:
      return "linking";
    case jak1_fr3::Phase::extracting_intermediates:
      return "intermediates";
    case jak1_fr3::Phase::extracting_levels:
      return "levels";
  }
  return "unknown";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: jak1-fr3-prepare PROJECT_ROOT EXTRACTED_ISO_ROOT WORK_ROOT\n";
    return 2;
  }

  std::signal(SIGINT, cancel);
  std::signal(SIGTERM, cancel);
  lg::initialize();

  jak1_fr3::Options options;
  options.should_cancel = [] { return g_cancelled.load(std::memory_order_relaxed); };
  options.report_progress = [](const jak1_fr3::Progress& progress) {
    std::cout << phase_name(progress.phase) << ' ' << progress.completed << '/'
              << progress.total;
    if (!progress.current_item.empty()) {
      std::cout << ' ' << progress.current_item;
    }
    std::cout << '\n';
  };

  const auto result = jak1_fr3::prepare(std::filesystem::path(argv[1]),
                                        std::filesystem::path(argv[2]),
                                        std::filesystem::path(argv[3]),
                                        jak1_iso::default_revision(), options);
  if (!result) {
    std::cerr << jak1_fr3::error_code_name(result.error().code) << ": "
              << result.error().message << '\n';
    return 1;
  }

  const auto& summary = result.value();
  std::cout << "prepared " << summary.levels_written << " FR3 files and "
            << summary.raw_objects_written << " raw objects from " << summary.archives_read
            << " archives (" << summary.output_bytes << " bytes)\n";
  return 0;
}
