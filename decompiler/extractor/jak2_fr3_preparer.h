#pragma once

#include <filesystem>

#include "decompiler/extractor/jak1_fr3_preparer.h"

namespace jak2_fr3 {

using Phase = jak1_fr3::Phase;
using Progress = jak1_fr3::Progress;
using CancelCallback = jak1_fr3::CancelCallback;
using ProgressCallback = jak1_fr3::ProgressCallback;
using ErrorCode = jak1_fr3::ErrorCode;
using Error = jak1_fr3::Error;
using Summary = jak1_fr3::Summary;

template <typename T>
using Result = jak1_fr3::Result<T>;

struct Options : jak1_fr3::Options {
  Options() {
    max_archives = 256;
    max_levels = 256;
  }
};

/// Prepare the FR3 files described by the tracked Jak II NTSC-U v1 decompiler configuration.
/// The caller remains responsible for validating that `extracted_iso_root` is that revision.
Result<Summary> prepare(const std::filesystem::path& project_root,
                        const std::filesystem::path& extracted_iso_root,
                        const std::filesystem::path& work_root,
                        const Options& options = {});

inline const char* error_code_name(ErrorCode code) {
  return jak1_fr3::error_code_name(code);
}

}  // namespace jak2_fr3
