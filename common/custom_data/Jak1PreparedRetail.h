#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/versions/jak1_iso_revisions.h"

namespace jak1_prepared_retail {

inline constexpr std::array<uint8_t, 8> kMagic = {'J', '1', 'P', 'R', 'E', 'T', 'L', 0};
inline constexpr uint32_t kSchemaVersion = 1;
inline constexpr const char* kProvenanceId = "opengoal-jak1-retail";
inline constexpr const char* kGameId = "jak1";
inline constexpr const char* kLevelName = "village1";
inline constexpr const char* kLevelFileName = "village1.fr3";
inline constexpr const char* kTexturePageName = "village1-vis-alpha";
inline constexpr std::array<const char*, 4> kEnumMapNames = {
    "text-id",
    "cam-slave-options",
    "game-task",
    "pickup-type",
};

using Territory = jak1_iso::Territory;

struct RevisionProvenance {
  std::string serial;
  uint64_t executable_hash = 0;
  uint64_t contents_hash = 0;
  uint32_t file_count = 0;
  std::string config_version;
  Territory territory = Territory::scea;
  bool black_label = false;
  uint64_t source_manifest_hash = 0;
};

struct Provenance {
  std::string producer = kProvenanceId;
  std::string game = kGameId;
  RevisionProvenance revision;
};

struct SourceIdentifiers {
  std::string level_name = kLevelName;
  std::string level_file_name = kLevelFileName;
  std::string texture_page_name = kTexturePageName;
};

struct TextureRemap {
  uint32_t original_texid = 0;
  uint32_t new_texid = 0;

  bool operator==(const TextureRemap&) const = default;
};

struct AdGifRecord {
  uint64_t tex0_data = 0;
  uint64_t tex0_addr = 0;
  uint64_t tex1_data = 0;
  uint64_t tex1_addr = 0;
  uint64_t mip_data = 0;
  uint64_t mip_addr = 0;
  uint64_t clamp_data = 0;
  uint64_t clamp_addr = 0;
  uint64_t alpha_data = 0;
  uint64_t alpha_addr = 0;

  bool operator==(const AdGifRecord&) const = default;
};

struct EnumEntry {
  std::string name;
  int64_t value = 0;

  bool operator==(const EnumEntry&) const = default;
};

struct EnumMap {
  std::string name;
  std::vector<EnumEntry> entries;

  bool operator==(const EnumMap&) const = default;
};

struct Catalog {
  Provenance provenance;
  SourceIdentifiers sources;
  std::vector<TextureRemap> village1_remaps;
  std::vector<uint32_t> raw_texture_ids;
  uint32_t raw_texture_page_count = 0;
  std::vector<AdGifRecord> raw_adgifs;
  std::vector<uint32_t> village1_vis_alpha_combo_ids;
  std::array<EnumMap, 4> enum_maps = {{
      {kEnumMapNames[0], {}},
      {kEnumMapNames[1], {}},
      {kEnumMapNames[2], {}},
      {kEnumMapNames[3], {}},
  }};
};

struct Limits {
  size_t max_wire_bytes = 2 * 1024 * 1024;
  uint32_t max_string_bytes = 128;
  uint32_t max_texture_remaps = 4096;
  uint32_t max_texture_ids = 4096;
  uint32_t max_adgif_records = 4096;
  uint32_t max_combo_ids = 4096;
  uint32_t max_enum_entries_per_map = 65536;
  int64_t min_enum_value = INT32_MIN;
  int64_t max_enum_value = UINT32_MAX;
};

enum class ErrorCode {
  invalid_argument,
  truncated,
  integer_overflow,
  limit_exceeded,
  wrong_magic,
  unsupported_schema,
  wrong_provenance,
  invalid_name,
  duplicate_value,
  invalid_order,
  invalid_count,
  trailing_data,
  corrupt_hash,
};

struct Error {
  ErrorCode code = ErrorCode::invalid_argument;
  size_t offset = 0;
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

Result<std::vector<uint8_t>> encode(const Catalog& catalog, const Limits& limits = {});
Result<Catalog> decode(std::span<const uint8_t> bytes, const Limits& limits = {});
const char* error_code_name(ErrorCode code);

}  // namespace jak1_prepared_retail
