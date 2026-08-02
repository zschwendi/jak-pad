#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/custom_data/Jak1PublicGeneratedArtifacts.h"
#include "common/versions/jak1_iso_revisions.h"

#include "decompiler/extractor/jak1_checked_dgo.h"

namespace jak1_extracted_generated_inputs {

namespace artifacts = jak1_public_generated_artifacts;

struct ValidatedTree {
  /// Private staging root and exact revision returned by the ISO extraction/validation phase.
  std::filesystem::path root;
  jak1_iso::Revision revision;
};

/// Public, redistributable data that is not present on the retail disc. Strings must already use
/// Jak 1's game encoding, not UTF-8. Game-text entries are merged over the matching retail bank by
/// ID, matching the desktop text compiler. Subtitles must contain banks 0 through 6; they are
/// copied and canonicalized because Jak 1 has no retail SUBTIT.TXT inputs.
struct PublicAdditions {
  std::vector<artifacts::GameTextBank> game_text;
  std::vector<artifacts::SubtitleBank> subtitles;
};

enum class ProgressStage {
  opening_game_archive,
  reading_game_archive,
  parsing_directory_tpages,
  parsing_game_count,
  reading_game_text,
  merging_public_data,
  complete,
};

struct Progress {
  ProgressStage stage = ProgressStage::opening_game_archive;
  std::size_t units_completed = 0;
  std::size_t units_total = 0;
  std::string source_relative_path;
  std::optional<std::uint32_t> language_id;
};

using CancelCallback = std::function<bool()>;
using ProgressCallback = std::function<void(const Progress&)>;

struct Limits {
  std::size_t max_game_archive_input_bytes = 512ull * 1024 * 1024;
  std::size_t max_game_archive_compressed_bytes = 512ull * 1024 * 1024;
  std::size_t max_game_archive_expanded_bytes = 1024ull * 1024 * 1024;
  std::size_t max_archive_object_bytes = 256ull * 1024 * 1024;
  std::size_t max_archive_total_object_bytes = 1024ull * 1024 * 1024;
  std::size_t max_direct_object_bytes = 64ull * 1024 * 1024;
  std::size_t file_read_chunk_bytes = 256 * 1024;
  std::uint32_t max_archive_objects = 4096;
  std::uint32_t max_archive_expansion_ratio = 256;
  std::size_t max_pointer_links = 131072;
  std::size_t max_named_links = 131072;
  std::uint32_t max_tpage_entries = 4096;
  std::uint32_t max_game_count_entries = 256;
  std::uint32_t max_text_lines_per_bank = 65536;
  std::uint32_t max_subtitle_scenes_per_bank = 65535;
  std::uint32_t max_subtitle_lines_per_scene = 65535;
  std::uint32_t max_name_bytes = 128;
  std::uint32_t max_string_bytes = 1024 * 1024;
  std::size_t max_generated_bank_string_bytes = 64ull * 1024 * 1024;
};

struct Options {
  Limits limits;
  artifacts::SubtitleMode subtitle_mode = artifacts::SubtitleMode::public_content;
  CancelCallback should_cancel;
  ProgressCallback on_progress;
};

enum class ErrorCode {
  invalid_argument,
  unsupported_revision,
  invalid_extracted_tree,
  missing_input,
  input_open_failed,
  input_read_failed,
  input_too_large,
  checked_dgo_failed,
  missing_retail_object,
  duplicate_retail_object,
  invalid_data_object,
  unsupported_data_object_version,
  invalid_link_data,
  invalid_pointer,
  unexpected_type,
  invalid_value,
  duplicate_text_id,
  invalid_public_data,
  limit_exceeded,
  cancelled,
  callback_failed,
  allocation_failed,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  std::string message;
  std::string source_relative_path;
  std::optional<std::uint32_t> archive_object_index;
  std::optional<std::uint32_t> language_id;
  std::optional<std::size_t> byte_offset;
  std::optional<jak1_checked_dgo::Error> checked_dgo_error;
};

template <typename T>
class Result {
 public:
  static Result success(T value) {
    Result result;
    result.m_value.emplace(std::move(value));
    return result;
  }

  static Result failure(Error error) {
    Result result;
    result.m_error.emplace(std::move(error));
    return result;
  }

  explicit operator bool() const { return m_value.has_value(); }
  const T& value() const { return *m_value; }
  T take_value() { return std::move(*m_value); }
  const Error& error() const { return *m_error; }

 private:
  std::optional<T> m_value;
  std::optional<Error> m_error;
};

Result<artifacts::Inputs> build(const ValidatedTree& tree,
                                const PublicAdditions& public_additions,
                                const Options& options = {});

const char* error_code_name(ErrorCode code);

}  // namespace jak1_extracted_generated_inputs
