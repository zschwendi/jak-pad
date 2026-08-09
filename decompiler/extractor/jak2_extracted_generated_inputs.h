#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/custom_data/Jak1OutputGraph.h"
#include "common/versions/jak2_iso_revisions.h"

#include "decompiler/extractor/jak1_checked_dgo.h"

namespace jak2_extracted_generated_inputs {

struct ValidatedTree {
  /// Private extracted-disc root and exact revision returned by the validation phase.
  std::filesystem::path root;
  jak2_iso::Revision revision;
};

enum class RetailSourceKind : std::uint8_t {
  archive_object = 1,
  direct_file = 2,
};

/// A private retail source needed to construct one public-graph generated output. These bytes are
/// inputs, not finished prepared artifacts: the later public-data compiler may merge additions and
/// canonicalize the data-object encoding.
struct RetailInput {
  RetailSourceKind source_kind = RetailSourceKind::archive_object;
  std::optional<jak1_output_recipe::GeneratedDataKind> object_kind;
  std::optional<jak1_output_recipe::GeneratedFlatFileKind> flat_file_kind;
  std::string source_relative_path;
  std::optional<std::uint32_t> archive_object_index;
  std::string internal_name;
  std::string destination_basename;
  std::string output_relative_path;
  std::vector<std::uint8_t> bytes;

  bool operator==(const RetailInput&) const = default;
};

/// The graph's subtitle-v2 output has no retail extracted-tree source. A later public-resource lane
/// must compile this tracked project input with the named desktop tool before materialization.
struct PublicInputRequirement {
  jak1_output_recipe::GeneratedFlatFileKind output_kind =
      jak1_output_recipe::GeneratedFlatFileKind::game_subtitle;
  std::string destination_basename;
  std::string output_relative_path;
  std::string project_relative_path;
  std::string compiler_tool;

  bool operator==(const PublicInputRequirement&) const = default;
};

struct Inputs {
  std::vector<RetailInput> retail;
  PublicInputRequirement public_subtitle_v2;
  std::uint64_t retail_bytes = 0;

  bool operator==(const Inputs&) const = default;
};

enum class ProgressStage {
  validating_public_graph,
  opening_game_archive,
  reading_game_archive,
  reading_game_text,
  complete,
};

struct Progress {
  ProgressStage stage = ProgressStage::validating_public_graph;
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
  std::size_t max_direct_input_bytes = 64ull * 1024 * 1024;
  std::size_t max_total_retail_bytes = 256ull * 1024 * 1024;
  std::size_t file_read_chunk_bytes = 256 * 1024;
  std::uint32_t max_archive_objects = 4096;
  std::uint32_t max_archive_expansion_ratio = 256;
};

struct Options {
  Limits limits;
  CancelCallback should_cancel;
  ProgressCallback on_progress;
};

enum class ErrorCode {
  invalid_argument,
  unsupported_revision,
  invalid_public_graph,
  unsupported_public_graph,
  invalid_extracted_tree,
  missing_input,
  input_open_failed,
  input_read_failed,
  input_too_large,
  checked_dgo_failed,
  missing_retail_object,
  duplicate_retail_object,
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

/// Build against the embedded base-retail Jak II public graph.
Result<Inputs> build(const ValidatedTree& tree, const Options& options = {});

/// Explicit graph overload for checked graph-to-input integration and deterministic tests.
Result<Inputs> build(const ValidatedTree& tree,
                     const jak1_output_graph::Graph& public_graph,
                     const Options& options = {});

const char* error_code_name(ErrorCode code);

}  // namespace jak2_extracted_generated_inputs
