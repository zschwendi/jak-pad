#pragma once

#include <filesystem>
#include <span>
#include <string>

#include "common/custom_data/Jak1SourceObjectPack.h"

namespace jak2_source_object_pack {

inline constexpr const char* kManifestName = jak1_source_object_pack::kManifestName;
inline constexpr std::uint32_t kExpectedObjectCount = 840;
inline constexpr std::uint64_t kRecordedAggregateXXH64 = 0x5a2079981b8e7eb6ULL;

using Phase = jak1_source_object_pack::Phase;
using Progress = jak1_source_object_pack::Progress;
using CancelCallback = jak1_source_object_pack::CancelCallback;
using ProgressCallback = jak1_source_object_pack::ProgressCallback;
using Limits = jak1_source_object_pack::Limits;
using Options = jak1_source_object_pack::Options;
using ErrorCode = jak1_source_object_pack::ErrorCode;
using Error = jak1_source_object_pack::Error;
template <typename T>
using Result = jak1_source_object_pack::Result<T>;
using Summary = jak1_source_object_pack::Summary;

Result<Summary> validate(const std::filesystem::path& root,
                         std::span<const std::string> expected_source_files,
                         const Options& options = {});

/// Validate the exact checked Jak II bundle identity used by the on-device importer. The recorded
/// manifest aggregate covers the ordered source graph, object names, sizes, and hashes, so the
/// expected graph can be recovered from that already pinned manifest without shipping MakeSystem.
Result<Summary> validate_recorded(const std::filesystem::path& root, const Options& options = {});

inline const char* error_code_name(ErrorCode code) {
  return jak1_source_object_pack::error_code_name(code);
}

}  // namespace jak2_source_object_pack
