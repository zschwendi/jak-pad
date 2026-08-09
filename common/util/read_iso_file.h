#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/util/FileUtil.h"

struct IsoFile {
  struct Entry {
    bool is_dir = false;
    std::string name;

    // if file
    size_t offset_in_file = 0;
    size_t size = 0;

    // if dir
    std::vector<Entry> children;
    void print(std::string* result, const std::string& prefix) const;
  };

  std::string print() const;

  Entry root;

  int files_extracted = 0;
  bool shouldHash = false;
  // There is no reason to map to the files, as we don't retain mappings of each file's expected
  // hash
  std::vector<uint64_t> hashes = {};

  IsoFile();
};

namespace iso_file {

enum class ErrorCode {
  invalid_argument,
  cancelled,
  seek_failed,
  read_failed,
  invalid_descriptor,
  unsupported_format,
  invalid_directory_record,
  unsafe_path,
  extent_out_of_bounds,
  depth_limit_exceeded,
  entry_limit_exceeded,
  file_size_limit_exceeded,
  total_size_limit_exceeded,
  output_create_failed,
  output_write_failed,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  uint64_t image_offset = 0;
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

struct Progress {
  uint64_t bytes_completed = 0;
  uint64_t bytes_total = 0;
  uint32_t files_completed = 0;
  uint32_t files_total = 0;
  std::string current_path;
};

using CancelCallback = std::function<bool()>;
using ProgressCallback = std::function<void(const Progress&)>;

struct Options {
  uint64_t max_image_bytes = 8ull * 1024 * 1024 * 1024;
  uint64_t max_directory_bytes = 64ull * 1024 * 1024;
  uint64_t max_file_bytes = 4ull * 1024 * 1024 * 1024;
  uint64_t max_total_output_bytes = 8ull * 1024 * 1024 * 1024;
  uint32_t max_entries = 100000;
  uint32_t max_depth = 32;
  uint32_t max_name_bytes = 128;
  uint32_t max_path_bytes = 1024;
  size_t read_chunk_bytes = 256 * 1024;
  bool hash_files = false;
  CancelCallback should_cancel;
  ProgressCallback on_progress;
};

#ifndef _WIN32
/// Retains the exact POSIX directory created by extract_to_owned_staging across caller callbacks.
/// Destruction removes only entries whose identities were recorded when the reader created them.
class OwnedStagingDirectory {
 public:
  OwnedStagingDirectory();
  ~OwnedStagingDirectory();
  OwnedStagingDirectory(const OwnedStagingDirectory&) = delete;
  OwnedStagingDirectory& operator=(const OwnedStagingDirectory&) = delete;
  OwnedStagingDirectory(OwnedStagingDirectory&&) noexcept;
  OwnedStagingDirectory& operator=(OwnedStagingDirectory&&) noexcept;

  /// Remove the exact reader-created entries and directory. Unexpected or replaced entries are
  /// preserved and reported as an error.
  std::optional<std::string> cleanup();

  /// Leave the successfully validated staging directory in place and relinquish its descriptors.
  /// Returns false and retains ownership when its caller-visible path or recorded tree changed.
  bool keep();

  /// Confirm that the retained directory is still linked and contains exactly the recorded tree.
  bool is_linked() const;

  /// Descriptor-relative helpers for importer metadata created after ISO extraction.
  int directory_descriptor() const;
  bool track_created_file(std::string_view name, int descriptor);
  bool rename_tracked_file(std::string_view old_name, std::string_view new_name);

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

  friend Result<IsoFile> extract_to_owned_staging(FILE*,
                                                  const std::filesystem::path&,
                                                  OwnedStagingDirectory*,
                                                  const Options&);
};
#endif

/// Return the exact safe basename used when an inspected ISO entry is extracted.
std::string extracted_output_name(std::string_view entry_name);

/// Parse and validate an ISO9660 image without writing any output.
Result<IsoFile> inspect(FILE* file, const Options& options = {});

/// Extract an already inspected layout. Partial output can remain on failure; callers that need
/// cleanup should use extract_to_staging.
Result<IsoFile> extract_layout(FILE* file,
                               IsoFile layout,
                               const std::filesystem::path& destination,
                               const Options& options = {});

/// Inspect and extract into a new staging directory. The staging directory is removed whenever
/// parsing, cancellation, reading, or writing fails, and is left in place only on success.
Result<IsoFile> extract_to_staging(FILE* file,
                                   const std::filesystem::path& staging_directory,
                                   const Options& options = {});

#ifndef _WIN32
/// Inspect and extract while returning a creation-owned POSIX staging handle. The caller must
/// retain the handle until validation finishes, then call keep() on success or cleanup() on
/// failure.
Result<IsoFile> extract_to_owned_staging(FILE* file,
                                         const std::filesystem::path& staging_directory,
                                         OwnedStagingDirectory* owned_staging,
                                         const Options& options = {});
#endif

const char* error_code_name(ErrorCode code);

class Exception final : public std::runtime_error {
 public:
  explicit Exception(Error error);
  const Error& error() const { return m_error; }

 private:
  Error m_error;
};

}  // namespace iso_file

// Desktop compatibility API. Input failures now throw iso_file::Exception instead of terminating
// the process through ASSERT. New callers should use the typed Result API above.
IsoFile find_files_in_iso(FILE* fp);
void unpack_iso_files(FILE* fp, IsoFile& layout, const fs::path& dest);
void unpack_iso_files(FILE* fp, IsoFile& layout, const fs::path& dest, bool print_progress);
IsoFile unpack_iso_files(FILE* fp,
                         const fs::path& dest,
                         bool print_progress,
                         bool hashFiles = false);
