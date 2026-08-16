#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jak1_checked_dgo_writer {

enum class ErrorCode {
  invalid_argument,
  cancelled,
  callback_failed,
  invalid_name,
  empty_archive,
  object_count_limit_exceeded,
  object_size_limit_exceeded,
  total_object_size_limit_exceeded,
  output_size_limit_exceeded,
  duplicate_object_name,
  allocation_failed,
  destination_inspection_failed,
  destination_exists,
  stage_create_failed,
  stage_write_failed,
  stage_sync_failed,
  stage_close_failed,
  atomic_install_failed,
  stage_cleanup_failed,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  std::optional<std::uint32_t> object_index;
  std::string message;
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

struct ObjectRecord {
  std::string_view internal_name;
  std::span<const std::uint8_t> data;
};

enum class DuplicateNamePolicy {
  reject,
  allow,
};

enum class ProgressPhase {
  validating,
  writing,
  installing,
};

struct Progress {
  ProgressPhase phase = ProgressPhase::validating;
  std::uint32_t objects_completed = 0;
  std::uint32_t object_count = 0;
  std::size_t bytes_completed = 0;
  std::size_t total_bytes = 0;
};

using CancelCallback = std::function<bool()>;
using ProgressCallback = std::function<void(const Progress&)>;

struct Options {
  std::size_t max_name_bytes = 59;
  std::uint32_t max_objects = 4096;
  std::size_t max_object_bytes = 256ull * 1024 * 1024;
  std::size_t max_total_object_bytes = 1024ull * 1024 * 1024;
  std::size_t max_output_bytes = 2ull * 1024 * 1024 * 1024;
  std::size_t write_chunk_bytes = 256 * 1024;
  DuplicateNamePolicy duplicate_name_policy = DuplicateNamePolicy::reject;
  CancelCallback should_cancel;
  ProgressCallback on_progress;
};

struct WriteSummary {
  std::uint32_t object_count = 0;
  std::size_t object_bytes = 0;
  std::size_t output_bytes = 0;
  std::uint64_t output_xxh64 = 0;
};

Result<std::vector<std::uint8_t>> build(std::string_view archive_name,
                                        std::span<const ObjectRecord> objects,
                                        const Options& options = {});

Result<WriteSummary> write_file(const std::filesystem::path& destination,
                                std::string_view archive_name,
                                std::span<const ObjectRecord> objects,
                                const Options& options = {});

Result<WriteSummary> write_file_at(int directory_fd,
                                   std::string_view destination_basename,
                                   std::string_view archive_name,
                                   std::span<const ObjectRecord> objects,
                                   const Options& options = {});

const char* error_code_name(ErrorCode code);

}  // namespace jak1_checked_dgo_writer
