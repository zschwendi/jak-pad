#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/custom_data/Jak1OutputRecipe.h"

namespace jak1_output_graph {

inline constexpr std::array<std::uint8_t, 8> kJak1Magic = {'J', '1', 'G', 'R', 'A', 'P', 'H', 0};
inline constexpr std::array<std::uint8_t, 8> kJak2Magic = {'J', '2', 'G', 'R', 'A', 'P', 'H', 0};
inline constexpr auto kMagic = kJak1Magic;
inline constexpr std::uint32_t kSchemaVersion = 1;

enum class WireGame : std::uint8_t {
  jak1 = 1,
  jak2 = 2,
};

enum class ObjectProducerKind : std::uint8_t {
  bundled_source = 1,
  verified_retail = 2,
  directory_tpages = 3,
  game_count = 4,
  custom_actor = 5,
  custom_level = 6,
};

struct GraphObject {
  std::string prepared_basename;
  std::string internal_name;
  ObjectProducerKind producer = ObjectProducerKind::bundled_source;
  std::string retail_source_archive;

  bool operator==(const GraphObject&) const = default;
};

struct GraphArchive {
  std::string destination_basename;
  std::vector<GraphObject> objects;

  bool operator==(const GraphArchive&) const = default;
};

struct GraphFlatCopy {
  std::string source_path;
  std::string destination_basename;

  bool operator==(const GraphFlatCopy&) const = default;
};

struct GraphGeneratedFlatFile {
  jak1_output_recipe::GeneratedFlatFileKind kind =
      jak1_output_recipe::GeneratedFlatFileKind::game_text;
  std::string destination_basename;

  bool operator==(const GraphGeneratedFlatFile&) const = default;
};

// Public, data-only projection of GROUP:all-code and GROUP:iso. Archive object order and source
// order are significant. FR3 expectations are verified preparation output and are not graph data.
struct Graph {
  std::vector<std::string> ordered_source_files;
  std::vector<GraphArchive> archives;
  std::vector<GraphFlatCopy> flat_file_copies;
  std::vector<GraphGeneratedFlatFile> generated_flat_files;

  bool operator==(const Graph&) const = default;
};

using CancelCallback = std::function<bool()>;

struct Limits {
  std::size_t max_wire_bytes = 4 * 1024 * 1024;
  std::uint32_t max_source_files = 4096;
  std::uint32_t max_archives = 128;
  std::uint32_t max_objects_per_archive = 4096;
  std::uint32_t max_total_objects = 65536;
  std::uint32_t max_flat_file_copies = 4096;
  std::uint32_t max_generated_flat_files = 64;
  std::uint32_t max_name_bytes = 128;
  std::uint32_t max_path_bytes = 1024;
};

struct Options {
  Limits limits;
  WireGame wire_game = WireGame::jak1;
  CancelCallback should_cancel;
};

enum class ErrorCode {
  invalid_argument,
  cancelled,
  callback_failed,
  allocation_failed,
  limit_exceeded,
  invalid_graph,
  wrong_magic,
  unsupported_schema,
  truncated,
  corrupt_hash,
  trailing_data,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  std::size_t offset = 0;
  std::optional<std::uint32_t> archive_index;
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

Result<std::vector<std::uint8_t>> encode(const Graph& graph, const Options& options = {});
Result<Graph> decode(std::span<const std::uint8_t> bytes, const Options& options = {});
const char* error_code_name(ErrorCode code);

}  // namespace jak1_output_graph
