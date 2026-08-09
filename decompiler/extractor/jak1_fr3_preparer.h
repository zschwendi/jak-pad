#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "common/versions/jak1_iso_revisions.h"

namespace jak1_fr3 {

enum class Phase {
  loading_configuration,
  reading_archives,
  linking_objects,
  extracting_intermediates,
  extracting_levels,
};

struct Progress {
  Phase phase = Phase::loading_configuration;
  std::uint32_t completed = 0;
  std::uint32_t total = 0;
  std::string current_item;
};

using CancelCallback = std::function<bool()>;
using ProgressCallback = std::function<void(const Progress&)>;

struct Options {
  std::uintmax_t max_archive_bytes = 512ull * 1024 * 1024;
  std::uintmax_t max_expanded_archive_bytes = 1024ull * 1024 * 1024;
  std::uintmax_t max_total_archive_bytes = 512ull * 1024 * 1024;
  std::uintmax_t max_total_expanded_archive_bytes = 512ull * 1024 * 1024;
  std::uintmax_t max_output_bytes = 768ull * 1024 * 1024;
  std::uint32_t max_archives = 64;
  std::uint32_t max_levels = 32;
  std::optional<std::size_t> compressed_trailing_alignment_bytes;
  CancelCallback should_cancel;
  ProgressCallback report_progress;
};

enum class ErrorCode {
  invalid_argument,
  callback_failed,
  cancelled,
  unsupported_revision,
  output_exists,
  output_creation_failed,
  project_setup_failed,
  configuration_failed,
  input_missing,
  archive_failed,
  archive_limit_exceeded,
  extraction_failed,
  output_limit_exceeded,
  output_incomplete,
  unsafe_output,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  std::string message;
};

struct Summary {
  std::uint32_t archives_read = 0;
  std::uint32_t levels_written = 0;
  std::uint32_t raw_objects_written = 0;
  std::uintmax_t output_bytes = 0;
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

Result<Summary> prepare(const std::filesystem::path& project_root,
                        const std::filesystem::path& extracted_iso_root,
                        const std::filesystem::path& work_root,
                        const jak1_iso::Revision& revision,
                        const Options& options = {});

const char* error_code_name(ErrorCode code);

}  // namespace jak1_fr3
