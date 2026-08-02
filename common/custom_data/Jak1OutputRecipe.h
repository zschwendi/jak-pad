#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "common/versions/jak1_iso_revisions.h"

namespace jak1_output_recipe {

inline constexpr std::array<uint8_t, 8> kMagic = {'J', '1', 'O', 'U', 'T', 'P', 'U', 'T'};
inline constexpr uint32_t kSchemaVersion = 1;
inline constexpr const char* kProvenanceId = "opengoal-jak1-output-recipe";
inline constexpr const char* kGameId = "jak1";

using Territory = jak1_iso::Territory;

struct RevisionProvenance {
  std::string serial;
  uint64_t executable_hash = 0;
  uint64_t contents_hash = 0;
  uint32_t file_count = 0;
  std::string config_version;
  Territory territory = Territory::scea;
  bool black_label = false;

  bool operator==(const RevisionProvenance&) const = default;
};

struct SourceObjectPackIdentity {
  uint32_t object_count = 0;
  uint64_t aggregate_xxh64 = 0;

  bool operator==(const SourceObjectPackIdentity&) const = default;
};

struct BundledSourceObject {
  std::string bundle_relative_path;
  uint64_t size = 0;
  uint64_t xxh64 = 0;

  bool operator==(const BundledSourceObject&) const = default;
};

struct VerifiedRetailObject {
  std::string source_archive_relative_path;
  uint32_t archive_object_index = 0;
  uint32_t object_version = 0;
  uint64_t size = 0;
  uint64_t xxh64 = 0;

  bool operator==(const VerifiedRetailObject&) const = default;
};

enum class GeneratedDataKind : uint8_t {
  directory_tpages = 1,
  game_count = 2,
  custom_actor = 3,
  custom_level = 4,
};

struct GeneratedData {
  GeneratedDataKind kind = GeneratedDataKind::directory_tpages;

  bool operator==(const GeneratedData&) const = default;
};

using ObjectSource = std::variant<BundledSourceObject, VerifiedRetailObject, GeneratedData>;

struct ObjectEntry {
  std::string internal_name;
  ObjectSource source;

  bool operator==(const ObjectEntry&) const = default;
};

struct ArchiveRecord {
  std::string destination_basename;
  std::vector<ObjectEntry> objects;

  bool operator==(const ArchiveRecord&) const = default;
};

struct FlatFileCopy {
  std::string extracted_iso_relative_path;
  std::string destination_basename;

  bool operator==(const FlatFileCopy&) const = default;
};

struct Recipe {
  std::string producer = kProvenanceId;
  std::string game = kGameId;
  RevisionProvenance revision;
  SourceObjectPackIdentity source_object_pack;
  std::vector<ArchiveRecord> archives;
  std::vector<FlatFileCopy> flat_file_copies;
  std::vector<std::string> expected_fr3_basenames;

  bool operator==(const Recipe&) const = default;
};

struct Limits {
  size_t max_wire_bytes = 16 * 1024 * 1024;
  uint32_t max_name_bytes = 128;
  uint32_t max_path_bytes = 1024;
  uint32_t max_archives = 128;
  uint32_t max_objects_per_archive = 4096;
  uint32_t max_total_objects = 65536;
  uint32_t max_source_pack_objects = 4096;
  uint32_t max_archive_object_index = 4095;
  uint32_t max_flat_file_copies = 4096;
  uint32_t max_expected_fr3_files = 4096;
  uint64_t max_object_bytes = 1024ull * 1024 * 1024;
  uint64_t max_total_object_bytes = 64ull * 1024 * 1024 * 1024;
  size_t hash_chunk_bytes = 256 * 1024;
};

using CancelCallback = std::function<bool()>;

struct Options {
  Limits limits;
  SourceObjectPackIdentity expected_source_object_pack;
  CancelCallback should_cancel;
};

enum class ErrorCode {
  invalid_argument,
  cancelled,
  callback_failed,
  truncated,
  integer_overflow,
  limit_exceeded,
  allocation_failed,
  wrong_magic,
  unsupported_schema,
  wrong_provenance,
  unsupported_revision,
  wrong_source_pack,
  invalid_name,
  unsafe_path,
  invalid_source_kind,
  invalid_generated_kind,
  invalid_object_version,
  duplicate_value,
  duplicate_destination,
  ambiguous_object,
  invalid_order,
  trailing_data,
  corrupt_hash,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  size_t offset = 0;
  std::optional<uint32_t> archive_index;
  std::optional<uint32_t> object_index;
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

Result<std::vector<uint8_t>> encode(const Recipe& recipe, const Options& options);
Result<Recipe> decode(std::span<const uint8_t> bytes, const Options& options);
const char* error_code_name(ErrorCode code);

}  // namespace jak1_output_recipe
