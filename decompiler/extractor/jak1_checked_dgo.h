#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/versions/versions.h"

namespace jak1_checked_dgo {

enum class ErrorCode {
  invalid_argument,
  cancelled,
  input_open_failed,
  input_read_failed,
  input_too_large,
  compressed_input_too_large,
  expanded_input_too_large,
  expansion_ratio_exceeded,
  allocation_failed,
  truncated_header,
  invalid_name,
  unexpected_archive_name,
  object_count_limit_exceeded,
  object_size_limit_exceeded,
  total_object_size_limit_exceeded,
  truncated_object,
  invalid_alignment,
  trailing_data,
  compressed_padding_limit_exceeded,
  compressed_chunk_limit_exceeded,
  invalid_compressed_chunk,
  decompression_failed,
  invalid_art_group_marker,
  duplicate_object_name,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  std::size_t offset = 0;
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

struct Object {
  std::string internal_name;
  std::string unique_name;
  std::vector<std::uint8_t> data;
};

struct Archive {
  std::string internal_name;
  std::vector<Object> objects;
  bool was_compressed = false;
  std::size_t input_size = 0;
  std::size_t expanded_size = 0;
};

using CancelCallback = std::function<bool()>;

struct Options {
  std::size_t max_input_bytes = 512ull * 1024 * 1024;
  std::size_t max_compressed_bytes = 512ull * 1024 * 1024;
  std::size_t max_expanded_bytes = 1024ull * 1024 * 1024;
  std::size_t max_object_bytes = 256ull * 1024 * 1024;
  std::size_t max_total_object_bytes = 1024ull * 1024 * 1024;
  std::uint32_t max_objects = 4096;
  std::size_t max_name_bytes = 59;
  std::size_t max_compressed_chunk_bytes = 0x7fff;
  std::size_t max_compressed_padding_bytes = 0x10000;
  // If set, the compressed input must end on this boundary with less than one unit of zero tail.
  std::optional<std::size_t> compressed_trailing_alignment_bytes;
  std::uint32_t max_compressed_chunks = 65536;
  std::uint32_t max_expansion_ratio = 256;
  std::size_t file_read_chunk_bytes = 256 * 1024;
  GameVersion game_version = GameVersion::Jak1;
  CancelCallback should_cancel;
};

Result<Archive> read(std::span<const std::uint8_t> input,
                     const std::optional<std::string>& expected_archive_name = {},
                     const Options& options = {});

Result<Archive> read_file(const std::filesystem::path& input_path,
                          const std::optional<std::string>& expected_archive_name = {},
                          const Options& options = {});

const char* error_code_name(ErrorCode code);

}  // namespace jak1_checked_dgo
