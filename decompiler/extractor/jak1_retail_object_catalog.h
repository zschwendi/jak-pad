#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "decompiler/extractor/jak1_checked_dgo.h"

namespace jak1_retail_object_catalog {

enum class ObjectVersion : std::uint16_t {
  v2 = 2,
  v4 = 4,
};

struct Provenance {
  std::string source_archive_relative_path;
  std::uint32_t archive_object_index = 0;
  std::string internal_name;
  std::string unique_name;
  std::size_t byte_size = 0;
  std::uint64_t xxh64 = 0;
  ObjectVersion object_version = ObjectVersion::v2;

  bool operator==(const Provenance&) const = default;
};

struct Entry {
  Provenance provenance;
};

struct ArchiveSource {
  std::string source_archive_relative_path;
  std::span<const std::uint8_t> bytes;
};

enum class ProgressStage {
  starting_archive,
  indexed_object,
  skipped_code_object,
  complete,
};

struct Progress {
  ProgressStage stage = ProgressStage::starting_archive;
  std::size_t archives_total = 0;
  std::size_t archives_processed = 0;
  std::size_t entries_indexed = 0;
  std::size_t bytes_indexed = 0;
  std::size_t code_objects_skipped = 0;
  std::string source_archive_relative_path;
  std::optional<std::uint32_t> archive_object_index;
};

using CancelCallback = std::function<bool()>;
using ProgressCallback = std::function<void(const Progress&)>;

struct Options {
  std::size_t max_archives = 256;
  std::size_t max_entries = 16384;
  std::size_t max_total_object_bytes = 1024ull * 1024 * 1024;
  std::size_t max_archive_input_bytes = 512ull * 1024 * 1024;
  std::size_t max_archive_compressed_bytes = 512ull * 1024 * 1024;
  std::size_t max_archive_expanded_bytes = 1024ull * 1024 * 1024;
  std::size_t max_object_bytes = 256ull * 1024 * 1024;
  std::size_t max_source_path_bytes = 255;
  std::size_t max_internal_name_bytes = 59;
  std::size_t hash_chunk_bytes = 256 * 1024;
  std::uint32_t max_archive_expansion_ratio = 256;
  CancelCallback should_cancel;
  ProgressCallback on_progress;
};

enum class ErrorCode {
  invalid_argument,
  cancelled,
  callback_failed,
  allocation_failed,
  invalid_source_archive_path,
  duplicate_source_archive_path,
  archive_limit_exceeded,
  entry_limit_exceeded,
  total_byte_limit_exceeded,
  checked_dgo_failed,
  invalid_object_header,
  unsupported_object_version,
  hash_failed,
  object_not_found,
  provenance_mismatch,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  std::string source_archive_relative_path;
  std::optional<std::uint32_t> archive_object_index;
  std::optional<std::uint16_t> detected_object_version;
  std::optional<jak1_checked_dgo::Error> checked_dgo_error;
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

class Catalog {
 public:
  const std::vector<Entry>& entries() const { return m_entries; }
  std::size_t skipped_code_object_count() const { return m_skipped_code_objects; }
  Result<const Entry*> lookup(const Provenance& expected) const;

 private:
  friend Result<Catalog> build(std::span<const ArchiveSource>, const Options&);
  std::vector<Entry> m_entries;
  std::size_t m_skipped_code_objects = 0;
};

Result<Catalog> build(std::span<const ArchiveSource> sources, const Options& options = {});

const char* error_code_name(ErrorCode code);

}  // namespace jak1_retail_object_catalog
