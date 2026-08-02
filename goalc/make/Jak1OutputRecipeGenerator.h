#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/custom_data/Jak1OutputGraph.h"
#include "common/custom_data/Jak1OutputRecipe.h"

namespace jak1_output_recipe_generator {

using ObjectProducerKind = jak1_output_graph::ObjectProducerKind;
using GraphObject = jak1_output_graph::GraphObject;
using GraphArchive = jak1_output_graph::GraphArchive;
using GraphFlatCopy = jak1_output_graph::GraphFlatCopy;
using GraphGeneratedFlatFile = jak1_output_graph::GraphGeneratedFlatFile;
using Graph = jak1_output_graph::Graph;

struct SourceObjectPackEntry {
  std::string source_file;
  std::string tag;
  std::string bundle_relative_path;
  uint64_t size = 0;
  uint64_t xxh64 = 0;
};

struct SourceObjectPackManifest {
  jak1_output_recipe::SourceObjectPackIdentity identity;
  std::vector<SourceObjectPackEntry> entries;
};

// Snapshot of one entry from the already verified retail-object catalog. `unique_name` is the
// decompiler's deterministic raw_obj name and is required because retail archives can contain
// divergent objects with the same internal DGO name.
struct RetailCatalogObject {
  std::string source_archive_relative_path;
  uint32_t archive_object_index = 0;
  std::string internal_name;
  std::string unique_name;
  uint32_t object_version = 0;
  uint64_t size = 0;
  uint64_t xxh64 = 0;
};

struct VerifiedInputs {
  jak1_output_recipe::RevisionProvenance revision;
  std::string extracted_iso_root;
  std::vector<std::string> verified_extracted_iso_relative_paths;
  std::vector<RetailCatalogObject> retail_catalog;
  std::vector<std::string> expected_fr3_basenames;
};

using CancelCallback = std::function<bool()>;

struct Limits {
  size_t max_manifest_bytes = 4 * 1024 * 1024;
  uint32_t max_manifest_entries = 4096;
  uint32_t max_catalog_entries = 16384;
  uint32_t max_graph_archives = 128;
  uint32_t max_graph_objects = 65536;
  uint32_t max_path_bytes = 1024;
  uint32_t max_name_bytes = 128;
};

struct Options {
  Limits limits;
  jak1_output_recipe::Limits recipe_limits;
  uint32_t expected_source_object_count = 518;
  std::string iso_target = "GROUP:iso";
  std::string source_target = "GROUP:all-code";
  CancelCallback should_cancel;
};

enum class ErrorCode {
  invalid_argument,
  cancelled,
  callback_failed,
  allocation_failed,
  unsupported_revision,
  invalid_manifest,
  manifest_hash_mismatch,
  manifest_graph_mismatch,
  invalid_graph,
  unsupported_step,
  missing_source_object,
  invalid_retail_catalog,
  missing_retail_object,
  ambiguous_retail_object,
  unverified_flat_source,
  recipe_validation_failed,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  std::string message;
  std::optional<uint32_t> archive_index;
  std::optional<uint32_t> object_index;
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

Result<SourceObjectPackManifest> parse_source_object_pack_manifest(std::string_view bytes,
                                                                   const Options& options = {});

Result<jak1_output_recipe::Recipe> generate_from_graph(const Graph& graph,
                                                       std::string_view source_object_pack_manifest,
                                                       const VerifiedInputs& verified_inputs,
                                                       const Options& options = {});

const char* error_code_name(ErrorCode code);

}  // namespace jak1_output_recipe_generator
