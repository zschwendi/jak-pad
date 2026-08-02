#include "Jak1OutputRecipe.h"

#include <algorithm>
#include <limits>
#include <new>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace jak1_output_recipe {
namespace {

constexpr size_t kHeaderBytes = kMagic.size() + sizeof(uint32_t) + sizeof(uint64_t);
constexpr size_t kHashBytes = sizeof(uint64_t);

enum class SourceKind : uint8_t {
  bundled_source_v3 = 1,
  verified_retail_v2_v4 = 2,
  generated_data = 3,
};

Error make_error(ErrorCode code,
                 size_t offset,
                 std::string message,
                 std::optional<uint32_t> archive_index = {},
                 std::optional<uint32_t> object_index = {}) {
  return {code, offset, archive_index, object_index, std::move(message)};
}

std::optional<Error> check_cancelled(const Options& options,
                                     std::optional<uint32_t> archive_index = {},
                                     std::optional<uint32_t> object_index = {}) {
  if (!options.should_cancel) {
    return {};
  }
  try {
    if (options.should_cancel()) {
      return make_error(ErrorCode::cancelled, 0, "Output-recipe processing was cancelled.",
                        archive_index, object_index);
    }
  } catch (...) {
    return make_error(ErrorCode::callback_failed, 0,
                      "The output-recipe cancellation callback failed.", archive_index,
                      object_index);
  }
  return {};
}

bool known_revision(const RevisionProvenance& revision) {
  const auto revisions = jak1_iso::supported_revisions();
  return std::any_of(revisions.begin(), revisions.end(), [&](const auto& known) {
    return revision.serial == known.serial && revision.executable_hash == known.elf_hash &&
           revision.contents_hash == known.contents_hash &&
           revision.file_count == known.file_count &&
           revision.config_version == known.decomp_config_version &&
           revision.territory == known.territory && revision.black_label == known.black_label;
  });
}

bool valid_options(const Options& options) {
  const auto& limits = options.limits;
  return limits.max_wire_bytes >= kHeaderBytes + kHashBytes && limits.max_name_bytes > 0 &&
         limits.max_path_bytes > 0 && limits.max_archives > 0 &&
         limits.max_objects_per_archive > 0 && limits.max_total_objects > 0 &&
         limits.max_source_pack_objects > 0 && limits.max_flat_file_copies > 0 &&
         limits.max_generated_flat_files > 0 && limits.max_expected_fr3_files > 0 &&
         limits.max_object_bytes > 0 && limits.max_total_object_bytes > 0 &&
         limits.hash_chunk_bytes > 0 && known_revision(options.expected_revision) &&
         options.expected_source_object_pack.object_count > 0 &&
         options.expected_source_object_pack.object_count <= limits.max_source_pack_objects &&
         options.expected_source_object_pack.aggregate_xxh64 != 0;
}

bool valid_name(std::string_view value, uint32_t cap) {
  if (value.empty() || value.size() > cap || value == "." || value == ".." || value.back() == '.') {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
    return byte >= 0x21 && byte <= 0x7e && byte != '/' && byte != '\\' && byte != ':';
  });
}

bool safe_relative_path(std::string_view value, uint32_t cap) {
  if (value.empty() || value.size() > cap || value.front() == '/') {
    return false;
  }
  size_t component_start = 0;
  for (size_t index = 0; index <= value.size(); ++index) {
    if (index != value.size()) {
      const auto byte = static_cast<unsigned char>(value[index]);
      if (byte < 0x21 || byte > 0x7e || value[index] == '\\' || value[index] == ':') {
        return false;
      }
      if (value[index] != '/') {
        continue;
      }
    }
    const auto component = value.substr(component_start, index - component_start);
    if (component.empty() || component == "." || component == ".." || component.back() == '.') {
      return false;
    }
    component_start = index + 1;
  }
  return true;
}

bool has_suffix(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool archive_basename(std::string_view value, uint32_t cap) {
  return valid_name(value, cap) && (has_suffix(value, ".DGO") || has_suffix(value, ".CGO"));
}

bool fr3_basename(std::string_view value, uint32_t cap) {
  return valid_name(value, cap) && has_suffix(value, ".fr3");
}

std::string collision_key(std::string_view value) {
  std::string key(value);
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char byte) {
    return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A'))
                                      : static_cast<char>(byte);
  });
  return key;
}

bool reserved_destination(std::string_view value) {
  return collision_key(value) == "savegame.ico";
}

SourceKind source_kind(const ObjectSource& source) {
  if (std::holds_alternative<BundledSourceObject>(source)) {
    return SourceKind::bundled_source_v3;
  }
  if (std::holds_alternative<VerifiedRetailObject>(source)) {
    return SourceKind::verified_retail_v2_v4;
  }
  return SourceKind::generated_data;
}

bool valid_generated_kind(GeneratedDataKind kind) {
  switch (kind) {
    case GeneratedDataKind::directory_tpages:
    case GeneratedDataKind::game_count:
    case GeneratedDataKind::custom_actor:
    case GeneratedDataKind::custom_level:
      return true;
  }
  return false;
}

bool valid_generated_flat_kind(GeneratedFlatFileKind kind) {
  switch (kind) {
    case GeneratedFlatFileKind::game_text:
    case GeneratedFlatFileKind::game_subtitle:
      return true;
  }
  return false;
}

bool valid_output_profile(OutputProfile profile) {
  switch (profile) {
    case OutputProfile::full_public:
    case OutputProfile::jak1_base_retail:
      return true;
  }
  return false;
}

std::string retail_key(const VerifiedRetailObject& retail) {
  std::string key = collision_key(retail.source_archive_relative_path);
  key.push_back('\0');
  for (int shift = 0; shift < 32; shift += 8) {
    key.push_back(static_cast<char>(retail.archive_object_index >> shift));
  }
  return key;
}

struct BundledIdentity {
  std::string relative_path;
  std::string internal_name;
  uint64_t size = 0;
  uint64_t xxh64 = 0;

  bool operator==(const BundledIdentity&) const = default;
};

struct RetailIdentity {
  std::string relative_path;
  std::string internal_name;
  uint32_t version = 0;
  uint64_t size = 0;
  uint64_t xxh64 = 0;

  bool operator==(const RetailIdentity&) const = default;
};

std::optional<Error> validate_recipe(const Recipe& recipe, const Options& options) {
  if (!valid_options(options)) {
    return make_error(ErrorCode::invalid_argument, 0, "The output-recipe options are invalid.");
  }
  if (const auto error = check_cancelled(options)) {
    return error;
  }
  const auto& limits = options.limits;
  if (recipe.producer != kProvenanceId || recipe.game != kGameId ||
      !valid_name(recipe.producer, limits.max_path_bytes) ||
      !valid_name(recipe.game, limits.max_name_bytes)) {
    return make_error(ErrorCode::wrong_provenance, 0,
                      "The recipe does not identify the Jak 1 output-recipe schema.");
  }
  if (!valid_output_profile(recipe.profile)) {
    return make_error(ErrorCode::wrong_provenance, 0,
                      "The recipe identifies an unsupported Jak 1 output profile.");
  }
  if (!known_revision(recipe.revision)) {
    return make_error(ErrorCode::unsupported_revision, 0,
                      "The recipe does not identify an exact supported Jak 1 revision.");
  }
  if (recipe.revision != options.expected_revision) {
    return make_error(ErrorCode::unsupported_revision, 0,
                      "The recipe revision does not match the exact expected Jak 1 revision.");
  }
  if (recipe.source_object_pack != options.expected_source_object_pack) {
    return make_error(ErrorCode::wrong_source_pack, 0,
                      "The recipe source-object pack does not match the expected signed pack.");
  }
  if ((recipe.profile == OutputProfile::full_public && !recipe.projected_source_objects.empty()) ||
      (recipe.profile == OutputProfile::jak1_base_retail &&
       recipe.projected_source_objects.size() != 1)) {
    return make_error(ErrorCode::wrong_source_pack, 0,
                      "The recipe source projection does not match its output profile.");
  }

  std::unordered_map<std::string, BundledSourceObject> projected_source_objects;
  for (const auto& source : recipe.projected_source_objects) {
    if (!safe_relative_path(source.bundle_relative_path, limits.max_path_bytes) ||
        !has_suffix(source.bundle_relative_path, ".o")) {
      return make_error(ErrorCode::unsafe_path, 0,
                        "A projected source object path is not a safe relative .o path.");
    }
    if (source.size == 0 || source.size > limits.max_object_bytes || source.xxh64 == 0 ||
        !projected_source_objects.emplace(collision_key(source.bundle_relative_path), source)
             .second) {
      return make_error(ErrorCode::wrong_source_pack, 0,
                        "A projected source object identity is invalid or repeated.");
    }
  }
  if (recipe.profile == OutputProfile::jak1_base_retail &&
      recipe.projected_source_objects.front().bundle_relative_path !=
          kBaseRetailProjectedBundlePath) {
    return make_error(ErrorCode::wrong_source_pack, 0,
                      "The base-retail recipe projects an unexpected source object.");
  }
  if (recipe.archives.empty() || recipe.archives.size() > limits.max_archives ||
      recipe.flat_file_copies.size() > limits.max_flat_file_copies ||
      recipe.generated_flat_files.size() > limits.max_generated_flat_files ||
      recipe.expected_fr3_basenames.size() > limits.max_expected_fr3_files) {
    return make_error(ErrorCode::limit_exceeded, 0,
                      "An output-recipe collection is empty or exceeds its configured cap.");
  }

  const std::array<std::string_view, 4> provenance_strings = {
      recipe.producer,
      recipe.game,
      recipe.revision.serial,
      recipe.revision.config_version,
  };
  if (std::any_of(provenance_strings.begin(), provenance_strings.end(),
                  [&](auto value) { return !valid_name(value, limits.max_path_bytes); })) {
    return make_error(ErrorCode::invalid_name, 0,
                      "An output-recipe provenance identifier is invalid.");
  }

  std::unordered_set<std::string> destination_basenames;
  std::unordered_map<std::string, BundledIdentity> bundled_identities;
  std::unordered_map<std::string, RetailIdentity> retail_identities;
  uint64_t total_object_bytes = 0;
  uint32_t total_objects = 0;
  std::string previous_archive;
  bool contains_base_retail_excluded_output = false;
  for (uint32_t archive_index = 0; archive_index < recipe.archives.size(); ++archive_index) {
    if (const auto error = check_cancelled(options, archive_index)) {
      return error;
    }
    const auto& archive = recipe.archives[archive_index];
    if (collision_key(archive.destination_basename) == "tsz.dgo") {
      contains_base_retail_excluded_output = true;
    }
    if (reserved_destination(archive.destination_basename)) {
      return make_error(ErrorCode::invalid_name, 0,
                        "SAVEGAME.ICO is reserved and cannot be an output archive destination.",
                        archive_index);
    }
    if (!archive_basename(archive.destination_basename, limits.max_name_bytes)) {
      return make_error(ErrorCode::invalid_name, 0,
                        "An output archive has an invalid DGO/CGO basename.", archive_index);
    }
    if (!previous_archive.empty() && archive.destination_basename <= previous_archive) {
      return make_error(
          archive.destination_basename == previous_archive ? ErrorCode::duplicate_destination
                                                           : ErrorCode::invalid_order,
          0, "Output archives are not strictly ordered by destination basename.", archive_index);
    }
    previous_archive = archive.destination_basename;
    if (!destination_basenames.emplace(collision_key(archive.destination_basename)).second) {
      return make_error(ErrorCode::duplicate_destination, 0,
                        "The recipe repeats an output destination basename.", archive_index);
    }
    if (archive.objects.empty() || archive.objects.size() > limits.max_objects_per_archive ||
        archive.objects.size() > limits.max_total_objects - total_objects) {
      return make_error(ErrorCode::limit_exceeded, 0,
                        "An archive object list is empty or exceeds its configured cap.",
                        archive_index);
    }
    total_objects += static_cast<uint32_t>(archive.objects.size());

    std::unordered_set<std::string> local_object_keys;
    for (uint32_t object_index = 0; object_index < archive.objects.size(); ++object_index) {
      if (const auto error = check_cancelled(options, archive_index, object_index)) {
        return error;
      }
      const auto& object = archive.objects[object_index];
      if (!valid_name(object.internal_name, limits.max_name_bytes)) {
        return make_error(ErrorCode::invalid_name, 0,
                          "An archive object has an invalid internal name.", archive_index,
                          object_index);
      }
      const auto kind = source_kind(object.source);
      auto local_key = object.internal_name;
      local_key.push_back('\0');
      local_key.push_back(static_cast<char>(kind));
      if (!local_object_keys.emplace(std::move(local_key)).second) {
        return make_error(ErrorCode::ambiguous_object, 0,
                          "An archive repeats an internal name with the same source kind.",
                          archive_index, object_index);
      }

      if (kind == SourceKind::bundled_source_v3) {
        const auto& bundled = std::get<BundledSourceObject>(object.source);
        if (!safe_relative_path(bundled.bundle_relative_path, limits.max_path_bytes) ||
            !has_suffix(bundled.bundle_relative_path, ".o")) {
          return make_error(ErrorCode::unsafe_path, 0,
                            "A bundled source object path is not a safe relative .o path.",
                            archive_index, object_index);
        }
        if (bundled.size == 0 || bundled.size > limits.max_object_bytes ||
            bundled.size > limits.max_total_object_bytes - total_object_bytes) {
          return make_error(ErrorCode::limit_exceeded, 0,
                            "A bundled source object exceeds its configured size cap.",
                            archive_index, object_index);
        }
        total_object_bytes += bundled.size;
        const BundledIdentity identity{bundled.bundle_relative_path, object.internal_name,
                                       bundled.size, bundled.xxh64};
        if (projected_source_objects.contains(collision_key(bundled.bundle_relative_path))) {
          return make_error(ErrorCode::wrong_source_pack, 0,
                            "A source object is both projected and referenced by the recipe.",
                            archive_index, object_index);
        }
        const auto [entry, inserted] =
            bundled_identities.emplace(collision_key(bundled.bundle_relative_path), identity);
        if (!inserted && entry->second != identity) {
          return make_error(ErrorCode::ambiguous_object, 0,
                            "A bundled source path has conflicting object identities.",
                            archive_index, object_index);
        }
      } else if (kind == SourceKind::verified_retail_v2_v4) {
        const auto& retail = std::get<VerifiedRetailObject>(object.source);
        if (!safe_relative_path(retail.source_archive_relative_path, limits.max_path_bytes) ||
            !(has_suffix(retail.source_archive_relative_path, ".DGO") ||
              has_suffix(retail.source_archive_relative_path, ".CGO"))) {
          return make_error(ErrorCode::unsafe_path, 0,
                            "A retail source archive path is not a safe relative DGO/CGO path.",
                            archive_index, object_index);
        }
        if (retail.archive_object_index > limits.max_archive_object_index) {
          return make_error(ErrorCode::limit_exceeded, 0,
                            "A retail archive object index exceeds its configured cap.",
                            archive_index, object_index);
        }
        if (retail.object_version != 2 && retail.object_version != 4) {
          return make_error(ErrorCode::invalid_object_version, 0,
                            "A verified retail object is not a v2/v4 data object.", archive_index,
                            object_index);
        }
        if (retail.size == 0 || retail.size > limits.max_object_bytes ||
            retail.size > limits.max_total_object_bytes - total_object_bytes) {
          return make_error(ErrorCode::limit_exceeded, 0,
                            "A verified retail object exceeds its configured size cap.",
                            archive_index, object_index);
        }
        total_object_bytes += retail.size;
        const auto key = retail_key(retail);
        const RetailIdentity identity{retail.source_archive_relative_path, object.internal_name,
                                      retail.object_version, retail.size, retail.xxh64};
        const auto [entry, inserted] = retail_identities.emplace(key, identity);
        if (!inserted && entry->second != identity) {
          return make_error(ErrorCode::ambiguous_object, 0,
                            "A retail archive path and index have conflicting identities.",
                            archive_index, object_index);
        }
      } else {
        const auto generated = std::get<GeneratedData>(object.source);
        if (!valid_generated_kind(generated.kind)) {
          return make_error(ErrorCode::invalid_generated_kind, 0,
                            "An archive object has an unsupported generated-data kind.",
                            archive_index, object_index);
        }
        if (generated.kind == GeneratedDataKind::custom_actor ||
            generated.kind == GeneratedDataKind::custom_level) {
          contains_base_retail_excluded_output = true;
        }
      }
    }
  }

  const auto referenced_source_objects = bundled_identities.size();
  if (recipe.projected_source_objects.size() >= recipe.source_object_pack.object_count) {
    return make_error(ErrorCode::wrong_source_pack, 0,
                      "The recipe projects too many objects from its exact source pack.");
  }
  const auto expected_referenced_source_objects =
      static_cast<size_t>(recipe.source_object_pack.object_count) -
      recipe.projected_source_objects.size();
  if (total_objects > limits.max_total_objects ||
      referenced_source_objects != expected_referenced_source_objects) {
    return make_error(ErrorCode::wrong_source_pack, 0,
                      recipe.profile == OutputProfile::jak1_base_retail
                          ? "The base-retail recipe does not project exactly one object out of "
                            "its exact source pack."
                          : "The recipe does not reference every object in its exact source pack.");
  }
  if (recipe.profile == OutputProfile::jak1_base_retail && contains_base_retail_excluded_output) {
    return make_error(ErrorCode::wrong_provenance, 0,
                      "The base-retail recipe contains a TSZ or custom generated output.");
  }

  std::unordered_set<std::string> copy_sources;
  std::string previous_copy_destination;
  for (uint32_t index = 0; index < recipe.flat_file_copies.size(); ++index) {
    if (const auto error = check_cancelled(options)) {
      return error;
    }
    const auto& copy = recipe.flat_file_copies[index];
    if (reserved_destination(copy.destination_basename)) {
      return make_error(ErrorCode::invalid_name, 0,
                        "SAVEGAME.ICO is reserved and cannot be a flat-file destination.");
    }
    if (!safe_relative_path(copy.extracted_iso_relative_path, limits.max_path_bytes)) {
      return make_error(ErrorCode::unsafe_path, 0,
                        "A flat-file source is not a safe extracted-ISO relative path.");
    }
    if (!valid_name(copy.destination_basename, limits.max_name_bytes)) {
      return make_error(ErrorCode::invalid_name, 0,
                        "A flat-file destination is not a safe basename.");
    }
    if (!previous_copy_destination.empty() &&
        copy.destination_basename <= previous_copy_destination) {
      return make_error(copy.destination_basename == previous_copy_destination
                            ? ErrorCode::duplicate_destination
                            : ErrorCode::invalid_order,
                        0, "Flat-file copies are not strictly ordered by destination basename.");
    }
    previous_copy_destination = copy.destination_basename;
    if (!destination_basenames.emplace(collision_key(copy.destination_basename)).second) {
      return make_error(ErrorCode::duplicate_destination, 0,
                        "An archive and flat-file copy share a destination basename.");
    }
    if (!copy_sources.emplace(collision_key(copy.extracted_iso_relative_path)).second) {
      return make_error(ErrorCode::duplicate_value, 0,
                        "The recipe repeats an extracted-ISO flat-file source.");
    }
  }

  std::string previous_generated_destination;
  for (const auto& generated : recipe.generated_flat_files) {
    if (const auto error = check_cancelled(options)) {
      return error;
    }
    if (!valid_generated_flat_kind(generated.kind)) {
      return make_error(ErrorCode::invalid_generated_flat_kind, 0,
                        "A generated flat file has an unsupported kind.");
    }
    if (reserved_destination(generated.destination_basename)) {
      return make_error(ErrorCode::invalid_name, 0,
                        "SAVEGAME.ICO is reserved and cannot be a generated-file destination.");
    }
    if (!valid_name(generated.destination_basename, limits.max_name_bytes)) {
      return make_error(ErrorCode::invalid_name, 0,
                        "A generated flat-file destination is not a safe basename.");
    }
    if (!previous_generated_destination.empty() &&
        generated.destination_basename <= previous_generated_destination) {
      return make_error(generated.destination_basename == previous_generated_destination
                            ? ErrorCode::duplicate_destination
                            : ErrorCode::invalid_order,
                        0,
                        "Generated flat files are not strictly ordered by destination basename.");
    }
    previous_generated_destination = generated.destination_basename;
    if (!destination_basenames.emplace(collision_key(generated.destination_basename)).second) {
      return make_error(ErrorCode::duplicate_destination, 0,
                        "A generated flat file collides with another output destination.");
    }
  }

  std::string previous_fr3;
  std::unordered_set<std::string> fr3_destinations;
  for (const auto& basename : recipe.expected_fr3_basenames) {
    if (const auto error = check_cancelled(options)) {
      return error;
    }
    if (!fr3_basename(basename, limits.max_name_bytes)) {
      return make_error(ErrorCode::invalid_name, 0,
                        "An expected FR3 entry is not a safe .fr3 basename.");
    }
    if (!previous_fr3.empty() && basename <= previous_fr3) {
      return make_error(
          basename == previous_fr3 ? ErrorCode::duplicate_value : ErrorCode::invalid_order, 0,
          "Expected FR3 basenames are not strictly ordered.");
    }
    if (!fr3_destinations.emplace(collision_key(basename)).second) {
      return make_error(ErrorCode::duplicate_destination, 0,
                        "Expected FR3 basenames collide under portable path rules.");
    }
    previous_fr3 = basename;
  }
  return {};
}

class Writer {
 public:
  explicit Writer(size_t cap) : max_bytes(cap) {}

  bool u8(uint8_t value) { return append(std::span<const uint8_t>(&value, 1)); }

  bool u32(uint32_t value) {
    std::array<uint8_t, 4> encoded{};
    for (int shift = 0; shift < 32; shift += 8) {
      encoded[shift / 8] = static_cast<uint8_t>(value >> shift);
    }
    return append(encoded);
  }

  bool u64(uint64_t value) {
    std::array<uint8_t, 8> encoded{};
    for (int shift = 0; shift < 64; shift += 8) {
      encoded[shift / 8] = static_cast<uint8_t>(value >> shift);
    }
    return append(encoded);
  }

  bool string(std::string_view value) {
    return value.size() <= std::numeric_limits<uint32_t>::max() &&
           u32(static_cast<uint32_t>(value.size())) &&
           append(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value.data()),
                                           value.size()));
  }

  bool append(std::span<const uint8_t> input) {
    if (input.size() > max_bytes - bytes.size()) {
      error = make_error(ErrorCode::limit_exceeded, bytes.size(),
                         "The encoded output recipe exceeds its configured wire cap.");
      return false;
    }
    bytes.insert(bytes.end(), input.begin(), input.end());
    return true;
  }

  std::vector<uint8_t> bytes;
  std::optional<Error> error;

 private:
  size_t max_bytes;
};

class Reader {
 public:
  explicit Reader(std::span<const uint8_t> input) : bytes(input) {}

  bool u8(uint8_t* value) {
    if (!require(1)) {
      return false;
    }
    *value = bytes[position++];
    return true;
  }

  bool u32(uint32_t* value) {
    if (!require(4)) {
      return false;
    }
    *value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      *value |= uint32_t(bytes[position++]) << shift;
    }
    return true;
  }

  bool u64(uint64_t* value) {
    if (!require(8)) {
      return false;
    }
    *value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
      *value |= uint64_t(bytes[position++]) << shift;
    }
    return true;
  }

  bool string(std::string* value, uint32_t cap) {
    uint32_t size = 0;
    if (!u32(&size)) {
      return false;
    }
    if (size > cap) {
      error = make_error(ErrorCode::limit_exceeded, position - 4,
                         "A wire string exceeds its configured field cap.");
      return false;
    }
    if (!require(size)) {
      return false;
    }
    value->assign(reinterpret_cast<const char*>(bytes.data() + position), size);
    position += size;
    return true;
  }

  bool count(uint32_t cap, uint32_t* value) {
    if (!u32(value)) {
      return false;
    }
    if (*value > cap) {
      error = make_error(ErrorCode::limit_exceeded, position - 4,
                         "A wire collection exceeds its configured count cap.");
      return false;
    }
    return true;
  }

  bool require(size_t size) {
    if (position > bytes.size() || size > bytes.size() - position) {
      error =
          make_error(ErrorCode::truncated, position, "The output-recipe wire data is truncated.");
      return false;
    }
    return true;
  }

  std::span<const uint8_t> bytes;
  size_t position = 0;
  std::optional<Error> error;
};

template <typename T>
Result<T> reader_failure(const Reader& reader, size_t base_offset = 0) {
  auto error = *reader.error;
  error.offset += base_offset;
  return Result<T>::failure(std::move(error));
}

Result<uint64_t> wire_hash(std::span<const uint8_t> bytes, const Options& options) {
  XXH64_state_t state;
  if (XXH64_reset(&state, 0) != XXH_OK) {
    return Result<uint64_t>::failure(
        make_error(ErrorCode::invalid_argument, 0, "Could not initialize XXH64."));
  }
  size_t offset = 0;
  while (offset < bytes.size()) {
    if (const auto error = check_cancelled(options)) {
      auto adjusted = *error;
      adjusted.offset = offset;
      return Result<uint64_t>::failure(std::move(adjusted));
    }
    const auto chunk = std::min(options.limits.hash_chunk_bytes, bytes.size() - offset);
    if (XXH64_update(&state, bytes.data() + offset, chunk) != XXH_OK) {
      return Result<uint64_t>::failure(
          make_error(ErrorCode::invalid_argument, offset, "Could not update XXH64."));
    }
    offset += chunk;
  }
  return Result<uint64_t>::success(XXH64_digest(&state));
}

Result<std::vector<uint8_t>> encode_impl(const Recipe& recipe, const Options& options) {
  if (const auto error = validate_recipe(recipe, options)) {
    return Result<std::vector<uint8_t>>::failure(*error);
  }
  const auto payload_cap = options.limits.max_wire_bytes - kHeaderBytes - kHashBytes;
  Writer payload(payload_cap);
  const auto write_revision = [&]() {
    return payload.string(recipe.producer) && payload.string(recipe.game) &&
           payload.u8(static_cast<uint8_t>(recipe.profile)) &&
           payload.string(recipe.revision.serial) && payload.u64(recipe.revision.executable_hash) &&
           payload.u64(recipe.revision.contents_hash) && payload.u32(recipe.revision.file_count) &&
           payload.string(recipe.revision.config_version) &&
           payload.u8(static_cast<uint8_t>(recipe.revision.territory)) &&
           payload.u8(recipe.revision.black_label ? 1 : 0) &&
           payload.u32(recipe.source_object_pack.object_count) &&
           payload.u64(recipe.source_object_pack.aggregate_xxh64);
  };
  if (!write_revision() ||
      !payload.u32(static_cast<uint32_t>(recipe.projected_source_objects.size()))) {
    return Result<std::vector<uint8_t>>::failure(*payload.error);
  }
  for (const auto& source : recipe.projected_source_objects) {
    if (!payload.string(source.bundle_relative_path) || !payload.u64(source.size) ||
        !payload.u64(source.xxh64)) {
      return Result<std::vector<uint8_t>>::failure(*payload.error);
    }
  }
  if (!payload.u32(static_cast<uint32_t>(recipe.archives.size()))) {
    return Result<std::vector<uint8_t>>::failure(*payload.error);
  }
  for (uint32_t archive_index = 0; archive_index < recipe.archives.size(); ++archive_index) {
    if (const auto error = check_cancelled(options, archive_index)) {
      return Result<std::vector<uint8_t>>::failure(*error);
    }
    const auto& archive = recipe.archives[archive_index];
    if (!payload.string(archive.destination_basename) ||
        !payload.u32(static_cast<uint32_t>(archive.objects.size()))) {
      return Result<std::vector<uint8_t>>::failure(*payload.error);
    }
    for (uint32_t object_index = 0; object_index < archive.objects.size(); ++object_index) {
      if (const auto error = check_cancelled(options, archive_index, object_index)) {
        return Result<std::vector<uint8_t>>::failure(*error);
      }
      const auto& object = archive.objects[object_index];
      const auto kind = source_kind(object.source);
      if (!payload.string(object.internal_name) || !payload.u8(static_cast<uint8_t>(kind))) {
        return Result<std::vector<uint8_t>>::failure(*payload.error);
      }
      if (kind == SourceKind::bundled_source_v3) {
        const auto& source = std::get<BundledSourceObject>(object.source);
        if (!payload.string(source.bundle_relative_path) || !payload.u64(source.size) ||
            !payload.u64(source.xxh64)) {
          return Result<std::vector<uint8_t>>::failure(*payload.error);
        }
      } else if (kind == SourceKind::verified_retail_v2_v4) {
        const auto& source = std::get<VerifiedRetailObject>(object.source);
        if (!payload.string(source.source_archive_relative_path) ||
            !payload.u32(source.archive_object_index) || !payload.u32(source.object_version) ||
            !payload.u64(source.size) || !payload.u64(source.xxh64)) {
          return Result<std::vector<uint8_t>>::failure(*payload.error);
        }
      } else if (!payload.u8(static_cast<uint8_t>(std::get<GeneratedData>(object.source).kind))) {
        return Result<std::vector<uint8_t>>::failure(*payload.error);
      }
    }
  }

  if (!payload.u32(static_cast<uint32_t>(recipe.flat_file_copies.size()))) {
    return Result<std::vector<uint8_t>>::failure(*payload.error);
  }
  for (const auto& copy : recipe.flat_file_copies) {
    if (const auto error = check_cancelled(options)) {
      return Result<std::vector<uint8_t>>::failure(*error);
    }
    if (!payload.string(copy.extracted_iso_relative_path) ||
        !payload.string(copy.destination_basename)) {
      return Result<std::vector<uint8_t>>::failure(*payload.error);
    }
  }
  if (!payload.u32(static_cast<uint32_t>(recipe.generated_flat_files.size()))) {
    return Result<std::vector<uint8_t>>::failure(*payload.error);
  }
  for (const auto& generated : recipe.generated_flat_files) {
    if (const auto error = check_cancelled(options)) {
      return Result<std::vector<uint8_t>>::failure(*error);
    }
    if (!payload.u8(static_cast<uint8_t>(generated.kind)) ||
        !payload.string(generated.destination_basename)) {
      return Result<std::vector<uint8_t>>::failure(*payload.error);
    }
  }
  if (!payload.u32(static_cast<uint32_t>(recipe.expected_fr3_basenames.size()))) {
    return Result<std::vector<uint8_t>>::failure(*payload.error);
  }
  for (const auto& basename : recipe.expected_fr3_basenames) {
    if (const auto error = check_cancelled(options)) {
      return Result<std::vector<uint8_t>>::failure(*error);
    }
    if (!payload.string(basename)) {
      return Result<std::vector<uint8_t>>::failure(*payload.error);
    }
  }

  Writer wire(options.limits.max_wire_bytes);
  if (!wire.append(kMagic) || !wire.u32(kSchemaVersion) || !wire.u64(payload.bytes.size()) ||
      !wire.append(payload.bytes)) {
    return Result<std::vector<uint8_t>>::failure(*wire.error);
  }
  auto hash = wire_hash(wire.bytes, options);
  if (!hash) {
    return Result<std::vector<uint8_t>>::failure(hash.error());
  }
  if (!wire.u64(hash.value())) {
    return Result<std::vector<uint8_t>>::failure(*wire.error);
  }
  return Result<std::vector<uint8_t>>::success(std::move(wire.bytes));
}

Result<Recipe> decode_impl(std::span<const uint8_t> bytes, const Options& options) {
  if (!valid_options(options)) {
    return Result<Recipe>::failure(
        make_error(ErrorCode::invalid_argument, 0, "The output-recipe options are invalid."));
  }
  if (bytes.size() > options.limits.max_wire_bytes) {
    return Result<Recipe>::failure(make_error(
        ErrorCode::limit_exceeded, 0, "The output-recipe wire data exceeds its configured cap."));
  }
  if (bytes.size() < kHeaderBytes + kHashBytes) {
    return Result<Recipe>::failure(
        make_error(ErrorCode::truncated, bytes.size(), "The output-recipe header is truncated."));
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
    return Result<Recipe>::failure(
        make_error(ErrorCode::wrong_magic, 0, "The output-recipe magic is invalid."));
  }

  Reader header(bytes);
  header.position = kMagic.size();
  uint32_t schema = 0;
  uint64_t payload_size_u64 = 0;
  if (!header.u32(&schema) || !header.u64(&payload_size_u64)) {
    return reader_failure<Recipe>(header);
  }
  if (schema != kSchemaVersion) {
    return Result<Recipe>::failure(make_error(ErrorCode::unsupported_schema, kMagic.size(),
                                              "The output-recipe schema is unsupported."));
  }
  if (payload_size_u64 > std::numeric_limits<size_t>::max() - kHeaderBytes - kHashBytes) {
    return Result<Recipe>::failure(
        make_error(ErrorCode::integer_overflow, kMagic.size() + sizeof(uint32_t),
                   "The output-recipe payload length cannot be represented."));
  }
  const auto expected_size = kHeaderBytes + static_cast<size_t>(payload_size_u64) + kHashBytes;
  if (expected_size > options.limits.max_wire_bytes) {
    return Result<Recipe>::failure(
        make_error(ErrorCode::limit_exceeded, kMagic.size() + sizeof(uint32_t),
                   "The declared output-recipe wire size exceeds its configured cap."));
  }
  if (bytes.size() < expected_size) {
    return Result<Recipe>::failure(make_error(ErrorCode::truncated, bytes.size(),
                                              "The output-recipe payload or hash is truncated."));
  }
  if (bytes.size() > expected_size) {
    return Result<Recipe>::failure(make_error(ErrorCode::trailing_data, expected_size,
                                              "The output recipe contains trailing wire data."));
  }

  Reader hash_reader(bytes.last(kHashBytes));
  uint64_t stored_hash = 0;
  if (!hash_reader.u64(&stored_hash)) {
    return reader_failure<Recipe>(hash_reader, expected_size - kHashBytes);
  }
  auto computed_hash = wire_hash(bytes.first(bytes.size() - kHashBytes), options);
  if (!computed_hash) {
    return Result<Recipe>::failure(computed_hash.error());
  }
  if (stored_hash != computed_hash.value()) {
    return Result<Recipe>::failure(make_error(ErrorCode::corrupt_hash, bytes.size() - kHashBytes,
                                              "The output-recipe XXH64 is invalid."));
  }

  const auto payload = bytes.subspan(kHeaderBytes, static_cast<size_t>(payload_size_u64));
  Reader reader(payload);
  Recipe recipe;
  uint8_t profile = 0;
  uint8_t territory = 0;
  uint8_t black_label = 0;
  if (!reader.string(&recipe.producer, options.limits.max_path_bytes) ||
      !reader.string(&recipe.game, options.limits.max_name_bytes) || !reader.u8(&profile) ||
      !reader.string(&recipe.revision.serial, options.limits.max_name_bytes) ||
      !reader.u64(&recipe.revision.executable_hash) ||
      !reader.u64(&recipe.revision.contents_hash) || !reader.u32(&recipe.revision.file_count) ||
      !reader.string(&recipe.revision.config_version, options.limits.max_name_bytes) ||
      !reader.u8(&territory) || !reader.u8(&black_label) ||
      !reader.u32(&recipe.source_object_pack.object_count) ||
      !reader.u64(&recipe.source_object_pack.aggregate_xxh64)) {
    return reader_failure<Recipe>(reader, kHeaderBytes);
  }
  recipe.profile = static_cast<OutputProfile>(profile);
  if (territory > static_cast<uint8_t>(Territory::scek) || black_label > 1) {
    return Result<Recipe>::failure(
        make_error(ErrorCode::wrong_provenance, kHeaderBytes + reader.position,
                   "The output-recipe revision has an invalid territory or label value."));
  }
  recipe.revision.territory = static_cast<Territory>(territory);
  recipe.revision.black_label = black_label != 0;

  uint32_t projected_source_count = 0;
  if (!reader.count(options.limits.max_source_pack_objects, &projected_source_count)) {
    return reader_failure<Recipe>(reader, kHeaderBytes);
  }
  recipe.projected_source_objects.resize(projected_source_count);
  for (auto& source : recipe.projected_source_objects) {
    if (!reader.string(&source.bundle_relative_path, options.limits.max_path_bytes) ||
        !reader.u64(&source.size) || !reader.u64(&source.xxh64)) {
      return reader_failure<Recipe>(reader, kHeaderBytes);
    }
  }

  uint32_t archive_count = 0;
  if (!reader.count(options.limits.max_archives, &archive_count)) {
    return reader_failure<Recipe>(reader, kHeaderBytes);
  }
  recipe.archives.resize(archive_count);
  uint32_t total_objects = 0;
  for (uint32_t archive_index = 0; archive_index < archive_count; ++archive_index) {
    if (const auto error = check_cancelled(options, archive_index)) {
      return Result<Recipe>::failure(*error);
    }
    auto& archive = recipe.archives[archive_index];
    uint32_t object_count = 0;
    if (!reader.string(&archive.destination_basename, options.limits.max_name_bytes) ||
        !reader.count(options.limits.max_objects_per_archive, &object_count)) {
      return reader_failure<Recipe>(reader, kHeaderBytes);
    }
    if (object_count > options.limits.max_total_objects - total_objects) {
      return Result<Recipe>::failure(
          make_error(ErrorCode::limit_exceeded, kHeaderBytes + reader.position,
                     "The decoded recipe exceeds its total object-count cap.", archive_index));
    }
    total_objects += object_count;
    archive.objects.resize(object_count);
    for (uint32_t object_index = 0; object_index < object_count; ++object_index) {
      if (const auto error = check_cancelled(options, archive_index, object_index)) {
        return Result<Recipe>::failure(*error);
      }
      auto& object = archive.objects[object_index];
      uint8_t kind = 0;
      if (!reader.string(&object.internal_name, options.limits.max_name_bytes) ||
          !reader.u8(&kind)) {
        return reader_failure<Recipe>(reader, kHeaderBytes);
      }
      if (kind == static_cast<uint8_t>(SourceKind::bundled_source_v3)) {
        BundledSourceObject source;
        if (!reader.string(&source.bundle_relative_path, options.limits.max_path_bytes) ||
            !reader.u64(&source.size) || !reader.u64(&source.xxh64)) {
          return reader_failure<Recipe>(reader, kHeaderBytes);
        }
        object.source = std::move(source);
      } else if (kind == static_cast<uint8_t>(SourceKind::verified_retail_v2_v4)) {
        VerifiedRetailObject source;
        if (!reader.string(&source.source_archive_relative_path, options.limits.max_path_bytes) ||
            !reader.u32(&source.archive_object_index) || !reader.u32(&source.object_version) ||
            !reader.u64(&source.size) || !reader.u64(&source.xxh64)) {
          return reader_failure<Recipe>(reader, kHeaderBytes);
        }
        object.source = std::move(source);
      } else if (kind == static_cast<uint8_t>(SourceKind::generated_data)) {
        uint8_t generated_kind = 0;
        if (!reader.u8(&generated_kind)) {
          return reader_failure<Recipe>(reader, kHeaderBytes);
        }
        if (generated_kind < static_cast<uint8_t>(GeneratedDataKind::directory_tpages) ||
            generated_kind > static_cast<uint8_t>(GeneratedDataKind::custom_level)) {
          return Result<Recipe>::failure(
              make_error(ErrorCode::invalid_generated_kind, kHeaderBytes + reader.position - 1,
                         "The output recipe has an unknown generated-data kind.", archive_index,
                         object_index));
        }
        object.source = GeneratedData{static_cast<GeneratedDataKind>(generated_kind)};
      } else {
        return Result<Recipe>::failure(make_error(
            ErrorCode::invalid_source_kind, kHeaderBytes + reader.position - 1,
            "The output recipe has an unknown object source kind.", archive_index, object_index));
      }
    }
  }

  uint32_t copy_count = 0;
  if (!reader.count(options.limits.max_flat_file_copies, &copy_count)) {
    return reader_failure<Recipe>(reader, kHeaderBytes);
  }
  recipe.flat_file_copies.resize(copy_count);
  for (auto& copy : recipe.flat_file_copies) {
    if (const auto error = check_cancelled(options)) {
      return Result<Recipe>::failure(*error);
    }
    if (!reader.string(&copy.extracted_iso_relative_path, options.limits.max_path_bytes) ||
        !reader.string(&copy.destination_basename, options.limits.max_name_bytes)) {
      return reader_failure<Recipe>(reader, kHeaderBytes);
    }
  }

  uint32_t generated_count = 0;
  if (!reader.count(options.limits.max_generated_flat_files, &generated_count)) {
    return reader_failure<Recipe>(reader, kHeaderBytes);
  }
  recipe.generated_flat_files.resize(generated_count);
  for (auto& generated : recipe.generated_flat_files) {
    if (const auto error = check_cancelled(options)) {
      return Result<Recipe>::failure(*error);
    }
    uint8_t kind = 0;
    if (!reader.u8(&kind) ||
        !reader.string(&generated.destination_basename, options.limits.max_name_bytes)) {
      return reader_failure<Recipe>(reader, kHeaderBytes);
    }
    if (kind < static_cast<uint8_t>(GeneratedFlatFileKind::game_text) ||
        kind > static_cast<uint8_t>(GeneratedFlatFileKind::game_subtitle)) {
      return Result<Recipe>::failure(
          make_error(ErrorCode::invalid_generated_flat_kind, kHeaderBytes + reader.position,
                     "The output recipe has an unknown generated flat-file kind."));
    }
    generated.kind = static_cast<GeneratedFlatFileKind>(kind);
  }

  uint32_t fr3_count = 0;
  if (!reader.count(options.limits.max_expected_fr3_files, &fr3_count)) {
    return reader_failure<Recipe>(reader, kHeaderBytes);
  }
  recipe.expected_fr3_basenames.resize(fr3_count);
  for (auto& basename : recipe.expected_fr3_basenames) {
    if (const auto error = check_cancelled(options)) {
      return Result<Recipe>::failure(*error);
    }
    if (!reader.string(&basename, options.limits.max_name_bytes)) {
      return reader_failure<Recipe>(reader, kHeaderBytes);
    }
  }
  if (reader.position != reader.bytes.size()) {
    return Result<Recipe>::failure(make_error(ErrorCode::trailing_data,
                                              kHeaderBytes + reader.position,
                                              "The output-recipe payload has unparsed fields."));
  }
  if (const auto error = validate_recipe(recipe, options)) {
    auto adjusted = *error;
    adjusted.offset += kHeaderBytes;
    return Result<Recipe>::failure(std::move(adjusted));
  }
  return Result<Recipe>::success(std::move(recipe));
}

}  // namespace

Result<std::vector<uint8_t>> encode(const Recipe& recipe, const Options& options) {
  try {
    return encode_impl(recipe, options);
  } catch (const std::bad_alloc&) {
    return Result<std::vector<uint8_t>>::failure(
        make_error(ErrorCode::allocation_failed, 0, "Could not allocate output-recipe wire data."));
  }
}

Result<Recipe> decode(std::span<const uint8_t> bytes, const Options& options) {
  try {
    return decode_impl(bytes, options);
  } catch (const std::bad_alloc&) {
    return Result<Recipe>::failure(
        make_error(ErrorCode::allocation_failed, 0, "Could not allocate decoded recipe data."));
  }
}

const char* error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::invalid_argument:
      return "invalid_argument";
    case ErrorCode::cancelled:
      return "cancelled";
    case ErrorCode::callback_failed:
      return "callback_failed";
    case ErrorCode::truncated:
      return "truncated";
    case ErrorCode::integer_overflow:
      return "integer_overflow";
    case ErrorCode::limit_exceeded:
      return "limit_exceeded";
    case ErrorCode::allocation_failed:
      return "allocation_failed";
    case ErrorCode::wrong_magic:
      return "wrong_magic";
    case ErrorCode::unsupported_schema:
      return "unsupported_schema";
    case ErrorCode::wrong_provenance:
      return "wrong_provenance";
    case ErrorCode::unsupported_revision:
      return "unsupported_revision";
    case ErrorCode::wrong_source_pack:
      return "wrong_source_pack";
    case ErrorCode::invalid_name:
      return "invalid_name";
    case ErrorCode::unsafe_path:
      return "unsafe_path";
    case ErrorCode::invalid_source_kind:
      return "invalid_source_kind";
    case ErrorCode::invalid_generated_kind:
      return "invalid_generated_kind";
    case ErrorCode::invalid_generated_flat_kind:
      return "invalid_generated_flat_kind";
    case ErrorCode::invalid_object_version:
      return "invalid_object_version";
    case ErrorCode::duplicate_value:
      return "duplicate_value";
    case ErrorCode::duplicate_destination:
      return "duplicate_destination";
    case ErrorCode::ambiguous_object:
      return "ambiguous_object";
    case ErrorCode::invalid_order:
      return "invalid_order";
    case ErrorCode::trailing_data:
      return "trailing_data";
    case ErrorCode::corrupt_hash:
      return "corrupt_hash";
  }
  return "unknown";
}

}  // namespace jak1_output_recipe
