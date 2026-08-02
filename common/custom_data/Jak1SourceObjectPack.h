#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "common/custom_data/Jak1OutputRecipe.h"

namespace jak1_source_object_pack {

inline constexpr const char* kManifestName = "object_pack_manifest.tsv";
inline constexpr std::uint32_t kExpectedObjectCount = 518;

enum class Phase {
  reading_manifest,
  validating_objects,
};

struct Progress {
  Phase phase = Phase::reading_manifest;
  std::uint32_t completed = 0;
  std::uint32_t total = 0;
  std::uint64_t bytes_hashed = 0;
  std::string current_file;
};

using CancelCallback = std::function<bool()>;
using ProgressCallback = std::function<void(const Progress&)>;

struct Limits {
  std::uint64_t max_manifest_bytes = 4ull * 1024 * 1024;
  std::uint64_t max_object_bytes = 256ull * 1024 * 1024;
  std::uint64_t max_total_object_bytes = 512ull * 1024 * 1024;
  std::size_t io_chunk_bytes = 256 * 1024;
};

struct Options {
  Limits limits;
  std::optional<jak1_output_recipe::SourceObjectPackIdentity> expected_identity;
  CancelCallback should_cancel;
  ProgressCallback on_progress;
};

enum class ErrorCode {
  invalid_argument,
  cancelled,
  callback_failed,
  allocation_failed,
  root_missing,
  unsafe_entry,
  manifest_missing,
  manifest_too_large,
  manifest_read_failed,
  manifest_invalid,
  wrong_identity,
  source_graph_mismatch,
  contents_mismatch,
  object_too_large,
  object_read_failed,
  object_hash_mismatch,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  std::string message;
  std::optional<std::uint32_t> entry_index;
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
  jak1_output_recipe::SourceObjectPackIdentity identity;
  std::uint64_t total_object_bytes = 0;
};

Result<Summary> validate(const std::filesystem::path& root, const Options& options = {});

const char* error_code_name(ErrorCode code);

}  // namespace jak1_source_object_pack
