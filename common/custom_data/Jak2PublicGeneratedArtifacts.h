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

#include "decompiler/extractor/jak2_extracted_generated_inputs.h"

namespace jak2_public_generated_artifacts {

enum class ArtifactKind : std::uint8_t {
  directory_tpages = 1,
  game_text = 2,
  subtitle_v2 = 3,
};

struct Artifact {
  ArtifactKind kind = ArtifactKind::directory_tpages;
  std::string destination_basename;
  std::string output_relative_path;
  std::vector<std::uint8_t> bytes;
  std::uint64_t xxh64 = 0;

  bool operator==(const Artifact&) const = default;
};

struct Build {
  std::vector<Artifact> artifacts;
  std::uint64_t total_bytes = 0;

  bool operator==(const Build&) const = default;
};

enum class ProgressStage {
  validating_inputs,
  reading_public_source,
  parsing_retail_object,
  merging_public_text,
  compiling_subtitle_v2,
  generating_artifact,
  complete,
};

struct Progress {
  ProgressStage stage = ProgressStage::validating_inputs;
  std::size_t units_completed = 0;
  std::size_t units_total = 0;
  std::string relative_path;
  std::optional<std::uint32_t> language_id;
};

using CancelCallback = std::function<bool()>;
using ProgressCallback = std::function<void(const Progress&)>;

struct Limits {
  std::size_t max_public_source_bytes = 2 * 1024 * 1024;
  std::size_t max_total_public_source_bytes = 4 * 1024 * 1024;
  std::size_t max_retail_input_bytes = 64 * 1024 * 1024;
  std::size_t max_total_retail_input_bytes = 256 * 1024 * 1024;
  std::size_t max_artifact_bytes = 64 * 1024 * 1024;
  std::size_t max_total_artifact_bytes = 256 * 1024 * 1024;
  std::size_t file_read_chunk_bytes = 256 * 1024;
  std::uint32_t max_tpage_entries = 4096;
  std::uint32_t max_text_lines_per_bank = 65536;
  std::uint32_t max_name_bytes = 128;
  std::uint32_t max_string_bytes = 1024 * 1024;
  std::size_t max_bank_string_bytes = 64 * 1024 * 1024;
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
  invalid_retail_inputs,
  invalid_public_requirement,
  invalid_resource_root,
  missing_public_source,
  unsafe_public_source,
  public_source_open_failed,
  public_source_read_failed,
  public_source_drift,
  invalid_public_source,
  invalid_retail_object,
  limit_exceeded,
  cancelled,
  callback_failed,
  allocation_failed,
  generation_failed,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  std::string message;
  std::string relative_path;
  std::optional<std::uint32_t> language_id;
  std::optional<std::size_t> byte_offset;
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

Result<Build> build(const jak2_extracted_generated_inputs::Inputs& inputs,
                    const std::filesystem::path& project_resource_root,
                    const Options& options = {});

Result<Build> build(const jak2_extracted_generated_inputs::Inputs& inputs,
                    const jak1_output_graph::Graph& public_graph,
                    const std::filesystem::path& project_resource_root,
                    const Options& options = {});

const char* error_code_name(ErrorCode code);

}  // namespace jak2_public_generated_artifacts
