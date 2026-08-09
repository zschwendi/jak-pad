#pragma once

#include <cstdint>
#include <filesystem>

#include "decompiler/extractor/jak1_fr3_preparer.h"

namespace jak2_fr3 {

// Canonical NTSC-U v2 compressed archive entries are zero-padded to this boundary. The checked
// reader accepts less than one alignment unit after the BLZO payload and still requires it all zero.
inline constexpr std::size_t kNtscV2CompressedArchiveAlignmentBytes = 0x40000;

// Sum of the BLZO expanded-size headers for the 149 archives selected by the tracked NTSC-U v2
// import profile. Using the exact profile total keeps aggregate expansion fail-closed on drift.
inline constexpr std::uintmax_t kNtscV2TotalExpandedArchiveBytes = 596'611'024;

// The preparer budgets its complete work tree, including raw objects, intermediate metadata,
// entities, and FR3 files. The canonical tracked profile measures 1,186,140,404 bytes; this
// 1,280 MiB ceiling retains bounded headroom while keeping output and storage checks fail-closed.
inline constexpr std::uintmax_t kNtscV2MeasuredFr3WorkBytes = 1'186'140'404;
inline constexpr std::uintmax_t kNtscV2MaxFr3WorkBytes = 1'280ull * 1024 * 1024;
inline constexpr std::uint32_t kNtscV2TrackedLevelCount = 147;
inline constexpr std::uint32_t kNtscV2ExpectedFr3Files = kNtscV2TrackedLevelCount + 1;

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
    // Jak II's build/output graph has no game-cnt; desktop extraction treats its absence as normal.
    require_game_count = false;
    max_total_expanded_archive_bytes = kNtscV2TotalExpandedArchiveBytes;
    max_output_bytes = kNtscV2MaxFr3WorkBytes;
    compressed_trailing_alignment_bytes = kNtscV2CompressedArchiveAlignmentBytes;
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
