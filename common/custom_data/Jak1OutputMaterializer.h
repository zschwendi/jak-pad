#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/custom_data/Jak1OutputRecipe.h"

namespace jak1_output_materializer {

struct GeneratedObjectArtifact {
  jak1_output_recipe::GeneratedDataKind kind =
      jak1_output_recipe::GeneratedDataKind::directory_tpages;
  std::string internal_name;
  std::string relative_path;
  std::uint64_t size = 0;
  std::uint64_t xxh64 = 0;
};

struct GeneratedFlatArtifact {
  jak1_output_recipe::GeneratedFlatFileKind kind =
      jak1_output_recipe::GeneratedFlatFileKind::game_text;
  std::string destination_basename;
  std::string relative_path;
  std::uint64_t size = 0;
  std::uint64_t xxh64 = 0;
};

struct Inputs {
  std::filesystem::path recipe_file;
  std::filesystem::path source_object_pack_root;
  std::filesystem::path extracted_iso_root;
  std::filesystem::path generated_artifact_root;
  std::filesystem::path prepared_fr3_root;
  std::vector<GeneratedObjectArtifact> generated_objects;
  std::vector<GeneratedFlatArtifact> generated_flat_files;
};

enum class Phase {
  validating,
  writing_archives,
  copying_flat_files,
  copying_fr3,
  installing,
};

struct Progress {
  Phase phase = Phase::validating;
  std::uint32_t completed = 0;
  std::uint32_t total = 0;
  std::uint64_t bytes_written = 0;
  std::string current_item;
};

using CancelCallback = std::function<bool()>;
using ProgressCallback = std::function<void(const Progress&)>;

struct Limits {
  std::uint64_t max_recipe_bytes = 16ull * 1024 * 1024;
  std::uint64_t max_source_object_bytes = 256ull * 1024 * 1024;
  std::uint64_t max_generated_object_bytes = 256ull * 1024 * 1024;
  std::uint64_t max_retail_archive_bytes = 512ull * 1024 * 1024;
  std::uint64_t max_flat_file_bytes = 2ull * 1024 * 1024 * 1024;
  std::uint64_t max_fr3_file_bytes = 512ull * 1024 * 1024;
  std::uint64_t max_total_output_bytes = 8ull * 1024 * 1024 * 1024;
  std::uint32_t max_generated_objects = 64;
  std::uint32_t max_generated_flat_files = 64;
  std::uint32_t max_path_bytes = 1024;
  std::uint32_t max_name_bytes = 128;
  std::size_t io_chunk_bytes = 256 * 1024;
};

struct Options {
  Limits limits;
  jak1_output_recipe::Limits recipe_limits;
  jak1_output_recipe::RevisionProvenance expected_revision;
  jak1_output_recipe::SourceObjectPackIdentity expected_source_object_pack;
  jak1_output_recipe::WireGame wire_game = jak1_output_recipe::WireGame::jak1;
  std::optional<std::size_t> compressed_trailing_alignment_bytes;
  CancelCallback should_cancel;
  ProgressCallback on_progress;
};

enum class ErrorCode {
  invalid_argument,
  cancelled,
  callback_failed,
  allocation_failed,
  unsafe_path,
  input_missing,
  input_not_regular,
  input_too_large,
  input_read_failed,
  recipe_invalid,
  revision_mismatch,
  source_pack_mismatch,
  source_object_mismatch,
  retail_archive_failed,
  retail_catalog_failed,
  retail_object_mismatch,
  generated_catalog_invalid,
  generated_artifact_mismatch,
  fr3_set_mismatch,
  destination_inspection_failed,
  destination_exists,
  stage_create_failed,
  output_limit_exceeded,
  output_write_failed,
  dgo_write_failed,
  stage_install_failed,
  stage_cleanup_failed,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  std::string message;
  std::optional<std::uint32_t> archive_index;
  std::optional<std::uint32_t> object_index;
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

Result<Summary> materialize(const Inputs& inputs,
                            const std::filesystem::path& destination_root,
                            const Options& options);

const char* error_code_name(ErrorCode code);

}  // namespace jak1_output_materializer
