#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace jak2_import_composer {

struct Request {
  std::filesystem::path iso_path;
  std::filesystem::path source_object_pack_root;
  std::filesystem::path candidate_root;
};

enum class Phase {
  validating_source_pack,
  extracting_iso,
};

struct Progress {
  Phase phase = Phase::validating_source_pack;
  std::uint64_t completed = 0;
  std::uint64_t total = 0;
  std::uint64_t bytes_completed = 0;
  std::string current_item;
};

using CancelCallback = std::function<bool()>;
using ProgressCallback = std::function<void(const Progress&)>;

struct Options {
  CancelCallback should_cancel;
  ProgressCallback on_progress;
};

enum class ErrorCode {
  invalid_argument,
  cancelled,
  callback_failed,
  source_pack_failed,
  iso_open_failed,
  iso_validation_failed,
  candidate_create_failed,
  candidate_cleanup_failed,
  prepared_output_unavailable,
  allocation_failed,
  unexpected_failure,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  std::string message;
  std::optional<std::filesystem::path> preserved_candidate_root;
  std::optional<std::string> cleanup_error;
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

struct Summary {
  std::uint32_t archives_written = 0;
  std::uint32_t objects_written = 0;
  std::uint32_t flat_files_written = 0;
  std::uint32_t fr3_files_written = 0;
  std::uint64_t output_bytes = 0;
};

/// Stage and validate the currently implemented Jak II import inputs in a fresh private candidate.
/// This API cannot return success until generated data, FR3 preparation, checked materialization,
/// and final candidate validation are implemented. The current `prepared_output_unavailable`
/// result preserves the verified extraction under `.opengoal-import/` for recovery.
Result<Summary> compose(const Request& request, const Options& options = {});

const char* error_code_name(ErrorCode code);
const char* phase_name(Phase phase);

}  // namespace jak2_import_composer
