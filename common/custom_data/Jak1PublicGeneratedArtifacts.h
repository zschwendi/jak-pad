#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/custom_data/Jak1OutputGraph.h"

namespace jak1_public_generated_artifacts {

struct DirectoryTpages {
  std::vector<std::uint32_t> lengths;
};

struct GameCountEntry {
  std::uint32_t money = 0;
  std::uint32_t buzzer = 0;
};

struct GameCount {
  std::vector<GameCountEntry> entries;
  std::uint32_t unknown_1 = 0;
  std::uint32_t unknown_2 = 0;
};

struct GameTextLine {
  std::uint32_t id = 0;
  std::string encoded_text;
};

struct GameTextBank {
  std::string destination_basename;
  std::uint32_t language_id = 0;
  std::string group_name;
  std::vector<GameTextLine> lines;
};

struct SubtitleLine {
  std::uint32_t frame_start = 0;
  std::string encoded_text;
  std::string encoded_speaker;
  bool offscreen = false;
};

struct SubtitleScene {
  std::string name;
  bool is_cutscene = true;
  std::uint32_t hint_id = 0;
  std::vector<SubtitleLine> lines;
};

struct SubtitleBank {
  std::string destination_basename;
  std::uint32_t language_id = 0;
  std::vector<SubtitleScene> scenes;
};

struct Inputs {
  DirectoryTpages directory_tpages;
  GameCount game_count;
  std::vector<GameTextBank> game_text;
  std::vector<SubtitleBank> subtitles;
};

enum class ArtifactStorage : std::uint8_t {
  object = 1,
  flat_file = 2,
};

struct Artifact {
  ArtifactStorage storage = ArtifactStorage::object;
  std::optional<jak1_output_recipe::GeneratedDataKind> object_kind;
  std::optional<jak1_output_recipe::GeneratedFlatFileKind> flat_file_kind;
  std::string internal_name;
  std::string destination_basename;
  std::string relative_path;
  std::vector<std::uint8_t> bytes;
  std::uint64_t xxh64 = 0;

  bool operator==(const Artifact&) const = default;
};

struct Build {
  std::vector<Artifact> artifacts;
  std::uint64_t total_bytes = 0;

  bool operator==(const Build&) const = default;
};

using CancelCallback = std::function<bool()>;

struct Limits {
  std::uint32_t max_tpage_entries = 4096;
  std::uint32_t max_game_count_entries = 256;
  std::uint32_t max_banks = 64;
  std::uint32_t max_text_lines_per_bank = 65536;
  std::uint32_t max_subtitle_scenes_per_bank = 65535;
  std::uint32_t max_subtitle_lines_per_scene = 65535;
  std::uint32_t max_name_bytes = 128;
  std::uint32_t max_string_bytes = 1024 * 1024;
  std::uint64_t max_artifact_bytes = 64ull * 1024 * 1024;
  std::uint64_t max_total_bytes = 256ull * 1024 * 1024;
};

struct Options {
  Limits limits;
  CancelCallback should_cancel;
};

enum class ErrorCode {
  invalid_argument,
  cancelled,
  callback_failed,
  allocation_failed,
  invalid_public_graph,
  unsupported_graph_output,
  missing_input,
  duplicate_input,
  unexpected_input,
  invalid_input,
  limit_exceeded,
  generation_failed,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  std::string message;
  std::optional<std::uint32_t> bank_index;
  std::optional<std::uint32_t> item_index;
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

Result<Build> build(const jak1_output_graph::Graph& public_graph,
                    const Inputs& inputs,
                    const Options& options = {});

const char* error_code_name(ErrorCode code);

}  // namespace jak1_public_generated_artifacts
