#include "Jak1PreparedRetail.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <unordered_set>

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace jak1_prepared_retail {
namespace {

constexpr size_t kHeaderBytes = kMagic.size() + sizeof(uint32_t) + sizeof(uint64_t);
constexpr size_t kHashBytes = sizeof(uint64_t);
constexpr size_t kAdGifWords = sizeof(AdGifData) / sizeof(uint64_t);
static_assert(kAdGifWords == 10);

Error make_error(ErrorCode code, size_t offset, std::string message) {
  return {code, offset, std::move(message)};
}

bool valid_limits(const Limits& limits) {
  return limits.max_wire_bytes >= kHeaderBytes + kHashBytes && limits.max_string_bytes > 0 &&
         limits.max_texture_remaps > 0 && limits.max_texture_ids > 0 &&
         limits.max_adgif_records > 0 && limits.max_combo_ids > 0 &&
         limits.max_enum_entries_per_map > 0 && limits.min_enum_value <= limits.max_enum_value;
}

struct KnownRevision {
  std::string_view serial;
  uint64_t executable_hash;
  uint64_t contents_hash;
  uint32_t file_count;
  std::string_view config;
  Territory territory;
  bool black_label;
};

constexpr std::array<KnownRevision, 5> kKnownRevisions = {{
    {"SCUS-97124", 7280758013604870207ULL, 11363853835861842434ULL, 337, "ntsc_v1", Territory::scea,
     true},
    {"SCUS-97124", 744661860962747854ULL, 8538304367812415885ULL, 338, "ntsc_v2", Territory::scea,
     false},
    {"SCES-50361", 12150718117852276522ULL, 16850370297611763875ULL, 338, "pal", Territory::scee,
     false},
    {"SCPS-15021", 16909372048085114219ULL, 1262350561338887717ULL, 338, "jp", Territory::scei,
     false},
    {"SCPS-56003", 7280758013604870207ULL, 13924540661438229398ULL, 338, "ntsc_v1", Territory::scea,
     false},
}};

bool known_revision(const RevisionProvenance& revision) {
  return std::any_of(kKnownRevisions.begin(), kKnownRevisions.end(), [&](const auto& known) {
    return revision.serial == known.serial && revision.executable_hash == known.executable_hash &&
           revision.contents_hash == known.contents_hash &&
           revision.file_count == known.file_count && revision.config_version == known.config &&
           revision.territory == known.territory && revision.black_label == known.black_label;
  });
}

bool valid_name(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  return std::all_of(name.begin(), name.end(),
                     [](unsigned char byte) { return byte >= 0x21 && byte <= 0x7e; });
}

std::optional<Error> validate_catalog(const Catalog& catalog, const Limits& limits) {
  if (!valid_limits(limits)) {
    return make_error(ErrorCode::invalid_argument, 0, "The prepared-retail limits are invalid.");
  }
  if (catalog.provenance.producer != kProvenanceId || catalog.provenance.game != kGameId ||
      !known_revision(catalog.provenance.revision) ||
      catalog.provenance.revision.source_manifest_hash == 0) {
    return make_error(ErrorCode::wrong_provenance, 0,
                      "The catalog does not identify a supported verified Jak 1 source.");
  }
  if (catalog.sources.level_name != kLevelName ||
      catalog.sources.level_file_name != kLevelFileName ||
      catalog.sources.texture_page_name != kTexturePageName) {
    return make_error(
        ErrorCode::wrong_provenance, 0,
        "The catalog source identifiers do not match the village1 preparation input.");
  }
  const std::array<std::string_view, 11> fixed_strings = {
      catalog.provenance.producer,        catalog.provenance.game,
      catalog.provenance.revision.serial, catalog.provenance.revision.config_version,
      catalog.sources.level_name,         catalog.sources.level_file_name,
      catalog.sources.texture_page_name,  catalog.enum_maps[0].name,
      catalog.enum_maps[1].name,          catalog.enum_maps[2].name,
      catalog.enum_maps[3].name,
  };
  if (std::any_of(fixed_strings.begin(), fixed_strings.end(),
                  [&](std::string_view value) { return value.size() > limits.max_string_bytes; })) {
    return make_error(ErrorCode::limit_exceeded, 0,
                      "A prepared-retail identifier exceeds its configured string cap.");
  }
  if (catalog.village1_remaps.size() > limits.max_texture_remaps ||
      catalog.raw_texture_ids.size() > limits.max_texture_ids ||
      catalog.raw_adgifs.size() > limits.max_adgif_records ||
      catalog.village1_vis_alpha_combo_ids.size() > limits.max_combo_ids) {
    return make_error(ErrorCode::limit_exceeded, 0,
                      "A prepared-retail collection exceeds its configured cap.");
  }
  if (catalog.raw_texture_page_count != catalog.raw_texture_ids.size()) {
    return make_error(ErrorCode::invalid_count, 0,
                      "The raw texture page count does not match the texture ID list.");
  }

  std::unordered_set<uint32_t> remap_sources;
  for (const auto& remap : catalog.village1_remaps) {
    if (!remap_sources.emplace(remap.original_texid).second) {
      return make_error(ErrorCode::duplicate_value, 0,
                        "The village1 remap table repeats an original texture ID.");
    }
  }

  for (size_t index = 1; index < catalog.village1_vis_alpha_combo_ids.size(); ++index) {
    const auto previous = catalog.village1_vis_alpha_combo_ids[index - 1];
    const auto current = catalog.village1_vis_alpha_combo_ids[index];
    if (current == previous) {
      return make_error(ErrorCode::duplicate_value, 0,
                        "The village1-vis-alpha combo ID list contains a duplicate.");
    }
    if (current < previous) {
      return make_error(ErrorCode::invalid_order, 0,
                        "The village1-vis-alpha combo IDs are not strictly sorted.");
    }
  }

  for (size_t map_index = 0; map_index < catalog.enum_maps.size(); ++map_index) {
    const auto& map = catalog.enum_maps[map_index];
    if (map.name != kEnumMapNames[map_index]) {
      return make_error(
          ErrorCode::wrong_provenance, 0,
          "The catalog does not contain the four expected enum maps in schema order.");
    }
    if (map.entries.size() > limits.max_enum_entries_per_map) {
      return make_error(ErrorCode::limit_exceeded, 0,
                        "A prepared-retail enum map exceeds its configured entry cap.");
    }
    for (size_t entry_index = 0; entry_index < map.entries.size(); ++entry_index) {
      const auto& entry = map.entries[entry_index];
      if (entry.name.size() > limits.max_string_bytes || !valid_name(entry.name)) {
        return make_error(ErrorCode::invalid_name, 0,
                          "A prepared-retail enum entry has an invalid name.");
      }
      if (entry.value < limits.min_enum_value || entry.value > limits.max_enum_value) {
        return make_error(ErrorCode::limit_exceeded, 0,
                          "A prepared-retail enum value exceeds its configured range.");
      }
      if (entry_index) {
        const auto& previous = map.entries[entry_index - 1].name;
        if (entry.name == previous) {
          return make_error(ErrorCode::duplicate_value, 0,
                            "A prepared-retail enum map contains a duplicate name.");
        }
        if (entry.name < previous) {
          return make_error(ErrorCode::invalid_order, 0,
                            "A prepared-retail enum map is not sorted by name.");
        }
      }
    }
  }
  return std::nullopt;
}

class Writer {
 public:
  void u8(uint8_t value) { bytes.push_back(value); }

  void u32(uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      u8(static_cast<uint8_t>(value >> shift));
    }
  }

  void u64(uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      u8(static_cast<uint8_t>(value >> shift));
    }
  }

  void string(std::string_view value) {
    u32(static_cast<uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
  }

  std::vector<uint8_t> bytes;
};

class Reader {
 public:
  Reader(std::span<const uint8_t> input, const Limits& input_limits)
      : bytes(input), limits(input_limits) {}

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

  bool i64(int64_t* value) {
    uint64_t bits = 0;
    if (!u64(&bits)) {
      return false;
    }
    *value =
        bits <= uint64_t(INT64_MAX) ? static_cast<int64_t>(bits) : -1 - static_cast<int64_t>(~bits);
    return true;
  }

  bool string(std::string* value) {
    uint32_t size = 0;
    if (!u32(&size)) {
      return false;
    }
    if (size > limits.max_string_bytes) {
      error = make_error(ErrorCode::limit_exceeded, position - 4,
                         "A prepared-retail string exceeds its configured cap.");
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
                         "A prepared-retail collection exceeds its configured cap.");
      return false;
    }
    return true;
  }

  bool require(size_t size) {
    if (position > bytes.size() || size > bytes.size() - position) {
      error =
          make_error(ErrorCode::truncated, position, "The prepared-retail payload is truncated.");
      return false;
    }
    return true;
  }

  std::span<const uint8_t> bytes;
  const Limits& limits;
  size_t position = 0;
  std::optional<Error> error;
};

template <typename T>
Result<T> reader_failure(const Reader& reader) {
  return Result<T>::failure(*reader.error);
}

uint64_t wire_hash(std::span<const uint8_t> bytes) {
  return XXH64(bytes.data(), bytes.size(), 0);
}

void write_adgif(Writer* writer, const AdGifData& adgif) {
  writer->u64(adgif.tex0_data);
  writer->u64(adgif.tex0_addr);
  writer->u64(adgif.tex1_data);
  writer->u64(adgif.tex1_addr);
  writer->u64(adgif.mip_data);
  writer->u64(adgif.mip_addr);
  writer->u64(adgif.clamp_data);
  writer->u64(adgif.clamp_addr);
  writer->u64(adgif.alpha_data);
  writer->u64(adgif.alpha_addr);
}

bool read_adgif(Reader* reader, AdGifData* adgif) {
  return reader->u64(&adgif->tex0_data) && reader->u64(&adgif->tex0_addr) &&
         reader->u64(&adgif->tex1_data) && reader->u64(&adgif->tex1_addr) &&
         reader->u64(&adgif->mip_data) && reader->u64(&adgif->mip_addr) &&
         reader->u64(&adgif->clamp_data) && reader->u64(&adgif->clamp_addr) &&
         reader->u64(&adgif->alpha_data) && reader->u64(&adgif->alpha_addr);
}

}  // namespace

Result<std::vector<uint8_t>> encode(const Catalog& catalog, const Limits& limits) {
  if (auto error = validate_catalog(catalog, limits)) {
    return Result<std::vector<uint8_t>>::failure(std::move(*error));
  }

  Writer payload;
  payload.string(catalog.provenance.producer);
  payload.string(catalog.provenance.game);
  payload.string(catalog.provenance.revision.serial);
  payload.u64(catalog.provenance.revision.executable_hash);
  payload.u64(catalog.provenance.revision.contents_hash);
  payload.u32(catalog.provenance.revision.file_count);
  payload.string(catalog.provenance.revision.config_version);
  payload.u8(static_cast<uint8_t>(catalog.provenance.revision.territory));
  payload.u8(catalog.provenance.revision.black_label ? 1 : 0);
  payload.u64(catalog.provenance.revision.source_manifest_hash);
  payload.string(catalog.sources.level_name);
  payload.string(catalog.sources.level_file_name);
  payload.string(catalog.sources.texture_page_name);

  payload.u32(static_cast<uint32_t>(catalog.village1_remaps.size()));
  for (const auto& remap : catalog.village1_remaps) {
    payload.u32(remap.original_texid);
    payload.u32(remap.new_texid);
  }
  payload.u32(static_cast<uint32_t>(catalog.raw_texture_ids.size()));
  for (const auto id : catalog.raw_texture_ids) {
    payload.u32(id);
  }
  payload.u32(catalog.raw_texture_page_count);
  payload.u32(static_cast<uint32_t>(catalog.raw_adgifs.size()));
  for (const auto& adgif : catalog.raw_adgifs) {
    write_adgif(&payload, adgif);
  }
  payload.u32(static_cast<uint32_t>(catalog.village1_vis_alpha_combo_ids.size()));
  for (const auto id : catalog.village1_vis_alpha_combo_ids) {
    payload.u32(id);
  }
  for (const auto& map : catalog.enum_maps) {
    payload.string(map.name);
    payload.u32(static_cast<uint32_t>(map.entries.size()));
    for (const auto& entry : map.entries) {
      payload.string(entry.name);
      payload.u64(static_cast<uint64_t>(entry.value));
    }
  }

  if (payload.bytes.size() > std::numeric_limits<size_t>::max() - kHeaderBytes - kHashBytes) {
    return Result<std::vector<uint8_t>>::failure(
        make_error(ErrorCode::integer_overflow, 0, "The prepared-retail wire size overflowed."));
  }
  const auto wire_size = kHeaderBytes + payload.bytes.size() + kHashBytes;
  if (wire_size > limits.max_wire_bytes) {
    return Result<std::vector<uint8_t>>::failure(make_error(
        ErrorCode::limit_exceeded, 0, "The prepared-retail wire data exceeds its configured cap."));
  }

  Writer wire;
  wire.bytes.insert(wire.bytes.end(), kMagic.begin(), kMagic.end());
  wire.u32(kSchemaVersion);
  wire.u64(payload.bytes.size());
  wire.bytes.insert(wire.bytes.end(), payload.bytes.begin(), payload.bytes.end());
  wire.u64(wire_hash(wire.bytes));
  return Result<std::vector<uint8_t>>::success(std::move(wire.bytes));
}

Result<Catalog> decode(std::span<const uint8_t> bytes, const Limits& limits) {
  if (!valid_limits(limits)) {
    return Result<Catalog>::failure(
        make_error(ErrorCode::invalid_argument, 0, "The prepared-retail limits are invalid."));
  }
  if (bytes.size() < kHeaderBytes + kHashBytes) {
    return Result<Catalog>::failure(make_error(ErrorCode::truncated, bytes.size(),
                                               "The prepared-retail wire header is truncated."));
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
    return Result<Catalog>::failure(
        make_error(ErrorCode::wrong_magic, 0, "The prepared-retail magic is invalid."));
  }

  Reader header(bytes, limits);
  header.position = kMagic.size();
  uint32_t schema = 0;
  uint64_t payload_size_u64 = 0;
  if (!header.u32(&schema) || !header.u64(&payload_size_u64)) {
    return reader_failure<Catalog>(header);
  }
  if (schema != kSchemaVersion) {
    return Result<Catalog>::failure(make_error(ErrorCode::unsupported_schema, kMagic.size(),
                                               "The prepared-retail schema is unsupported."));
  }
  if (payload_size_u64 > std::numeric_limits<size_t>::max() - kHeaderBytes - kHashBytes) {
    return Result<Catalog>::failure(
        make_error(ErrorCode::integer_overflow, kMagic.size() + sizeof(uint32_t),
                   "The prepared-retail payload length cannot be represented on this platform."));
  }
  const auto expected_size = kHeaderBytes + static_cast<size_t>(payload_size_u64) + kHashBytes;
  if (expected_size > limits.max_wire_bytes) {
    return Result<Catalog>::failure(
        make_error(ErrorCode::limit_exceeded, kMagic.size() + sizeof(uint32_t),
                   "The prepared-retail wire data exceeds its configured cap."));
  }
  if (bytes.size() < expected_size) {
    return Result<Catalog>::failure(make_error(
        ErrorCode::truncated, bytes.size(), "The prepared-retail payload or hash is truncated."));
  }
  if (bytes.size() > expected_size) {
    return Result<Catalog>::failure(
        make_error(ErrorCode::trailing_data, expected_size,
                   "The prepared-retail wire data has trailing bytes."));
  }

  uint64_t stored_hash = 0;
  Reader hash_reader(bytes.last(kHashBytes), limits);
  if (!hash_reader.u64(&stored_hash)) {
    return reader_failure<Catalog>(hash_reader);
  }
  const auto hashed_bytes = bytes.first(bytes.size() - kHashBytes);
  if (stored_hash != wire_hash(hashed_bytes)) {
    return Result<Catalog>::failure(
        make_error(ErrorCode::corrupt_hash, bytes.size() - kHashBytes,
                   "The prepared-retail corruption-detection hash does not match."));
  }

  Reader reader(bytes.subspan(kHeaderBytes, static_cast<size_t>(payload_size_u64)), limits);
  Catalog catalog;
  auto& revision = catalog.provenance.revision;
  uint8_t territory = 0;
  uint8_t black_label = 0;
  if (!reader.string(&catalog.provenance.producer) || !reader.string(&catalog.provenance.game) ||
      !reader.string(&revision.serial) || !reader.u64(&revision.executable_hash) ||
      !reader.u64(&revision.contents_hash) || !reader.u32(&revision.file_count) ||
      !reader.string(&revision.config_version) || !reader.u8(&territory) ||
      !reader.u8(&black_label) || !reader.u64(&revision.source_manifest_hash) ||
      !reader.string(&catalog.sources.level_name) ||
      !reader.string(&catalog.sources.level_file_name) ||
      !reader.string(&catalog.sources.texture_page_name)) {
    return reader_failure<Catalog>(reader);
  }
  if (territory > static_cast<uint8_t>(Territory::scek) || black_label > 1) {
    return Result<Catalog>::failure(
        make_error(ErrorCode::wrong_provenance, reader.position,
                   "The prepared-retail provenance contains an invalid territory or label value."));
  }
  revision.territory = static_cast<Territory>(territory);
  revision.black_label = black_label != 0;

  uint32_t count = 0;
  if (!reader.count(limits.max_texture_remaps, &count)) {
    return reader_failure<Catalog>(reader);
  }
  catalog.village1_remaps.resize(count);
  for (auto& remap : catalog.village1_remaps) {
    if (!reader.u32(&remap.original_texid) || !reader.u32(&remap.new_texid)) {
      return reader_failure<Catalog>(reader);
    }
  }
  if (!reader.count(limits.max_texture_ids, &count)) {
    return reader_failure<Catalog>(reader);
  }
  catalog.raw_texture_ids.resize(count);
  for (auto& id : catalog.raw_texture_ids) {
    if (!reader.u32(&id)) {
      return reader_failure<Catalog>(reader);
    }
  }
  if (!reader.u32(&catalog.raw_texture_page_count) ||
      !reader.count(limits.max_adgif_records, &count)) {
    return reader_failure<Catalog>(reader);
  }
  catalog.raw_adgifs.resize(count);
  for (auto& adgif : catalog.raw_adgifs) {
    if (!read_adgif(&reader, &adgif)) {
      return reader_failure<Catalog>(reader);
    }
  }
  if (!reader.count(limits.max_combo_ids, &count)) {
    return reader_failure<Catalog>(reader);
  }
  catalog.village1_vis_alpha_combo_ids.resize(count);
  for (auto& id : catalog.village1_vis_alpha_combo_ids) {
    if (!reader.u32(&id)) {
      return reader_failure<Catalog>(reader);
    }
  }
  for (auto& map : catalog.enum_maps) {
    if (!reader.string(&map.name) || !reader.count(limits.max_enum_entries_per_map, &count)) {
      return reader_failure<Catalog>(reader);
    }
    map.entries.resize(count);
    for (auto& entry : map.entries) {
      if (!reader.string(&entry.name) || !reader.i64(&entry.value)) {
        return reader_failure<Catalog>(reader);
      }
    }
  }
  if (reader.position != reader.bytes.size()) {
    return Result<Catalog>::failure(
        make_error(ErrorCode::trailing_data, kHeaderBytes + reader.position,
                   "The prepared-retail payload has unparsed trailing fields."));
  }
  if (auto error = validate_catalog(catalog, limits)) {
    error->offset += kHeaderBytes;
    return Result<Catalog>::failure(std::move(*error));
  }
  return Result<Catalog>::success(std::move(catalog));
}

const char* error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::invalid_argument:
      return "invalid_argument";
    case ErrorCode::truncated:
      return "truncated";
    case ErrorCode::integer_overflow:
      return "integer_overflow";
    case ErrorCode::limit_exceeded:
      return "limit_exceeded";
    case ErrorCode::wrong_magic:
      return "wrong_magic";
    case ErrorCode::unsupported_schema:
      return "unsupported_schema";
    case ErrorCode::wrong_provenance:
      return "wrong_provenance";
    case ErrorCode::invalid_name:
      return "invalid_name";
    case ErrorCode::duplicate_value:
      return "duplicate_value";
    case ErrorCode::invalid_order:
      return "invalid_order";
    case ErrorCode::invalid_count:
      return "invalid_count";
    case ErrorCode::trailing_data:
      return "trailing_data";
    case ErrorCode::corrupt_hash:
      return "corrupt_hash";
  }
  return "unknown";
}

}  // namespace jak1_prepared_retail
