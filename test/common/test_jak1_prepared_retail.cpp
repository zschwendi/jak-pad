#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "common/custom_data/Jak1PreparedRetail.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace {

using namespace jak1_prepared_retail;

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": " #condition << '\n'; \
      return false;                                                                              \
    }                                                                                            \
  } while (false)

Catalog make_catalog() {
  Catalog catalog;
  catalog.provenance.revision = {
      "SCUS-97124",
      7280758013604870207ULL,
      11363853835861842434ULL,
      337,
      "ntsc_v1",
      Territory::scea,
      true,
      0x123456789abcdef0ULL,
  };
  catalog.village1_remaps = {{19, 4}, {7, 12}};
  catalog.raw_texture_ids = {3, 8, 13};
  catalog.raw_texture_page_count = 3;
  catalog.raw_adgifs = {{
      0x0102030405060708ULL,
      0x1112131415161718ULL,
      0x2122232425262728ULL,
      0x3132333435363738ULL,
      0x4142434445464748ULL,
      0x5152535455565758ULL,
      0x6162636465666768ULL,
      0x7172737475767778ULL,
      0x8182838485868788ULL,
      0x9192939495969798ULL,
  }};
  catalog.village1_vis_alpha_combo_ids = {0x10002, 0x10004, 0x20001};
  catalog.enum_maps[0].entries = {{"confirm", 0x103}, {"zero", 0}};
  catalog.enum_maps[1].entries = {{"ALLOW_Z_ROT", 1 << 3}, {"BUTT_CAM", 1}};
  catalog.enum_maps[2].entries = {{"complete", 1}, {"none", 0}, {"village1-yakow", 10}};
  catalog.enum_maps[3].entries = {{"eco-blue", 3}, {"money", 5}, {"none", 0}};
  return catalog;
}

uint64_t read_le64(const uint8_t* bytes) {
  uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= uint64_t(*bytes++) << shift;
  }
  return value;
}

void write_le64(uint8_t* bytes, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    *bytes++ = static_cast<uint8_t>(value >> shift);
  }
}

void rewrite_hash(std::vector<uint8_t>* bytes) {
  const auto hash = XXH64(bytes->data(), bytes->size() - sizeof(uint64_t), 0);
  write_le64(bytes->data() + bytes->size() - sizeof(uint64_t), hash);
}

bool same_adgif(const jak1_prepared_retail::AdGifRecord& left,
                const jak1_prepared_retail::AdGifRecord& right) {
  return left.tex0_data == right.tex0_data && left.tex0_addr == right.tex0_addr &&
         left.tex1_data == right.tex1_data && left.tex1_addr == right.tex1_addr &&
         left.mip_data == right.mip_data && left.mip_addr == right.mip_addr &&
         left.clamp_data == right.clamp_data && left.clamp_addr == right.clamp_addr &&
         left.alpha_data == right.alpha_data && left.alpha_addr == right.alpha_addr;
}

bool catalogs_match(const Catalog& left, const Catalog& right) {
  const auto& lp = left.provenance;
  const auto& rp = right.provenance;
  const auto& lr = lp.revision;
  const auto& rr = rp.revision;
  return lp.producer == rp.producer && lp.game == rp.game && lr.serial == rr.serial &&
         lr.executable_hash == rr.executable_hash && lr.contents_hash == rr.contents_hash &&
         lr.file_count == rr.file_count && lr.config_version == rr.config_version &&
         lr.territory == rr.territory && lr.black_label == rr.black_label &&
         lr.source_manifest_hash == rr.source_manifest_hash &&
         left.sources.level_name == right.sources.level_name &&
         left.sources.level_file_name == right.sources.level_file_name &&
         left.sources.texture_page_name == right.sources.texture_page_name &&
         left.village1_remaps == right.village1_remaps &&
         left.raw_texture_ids == right.raw_texture_ids &&
         left.raw_texture_page_count == right.raw_texture_page_count &&
         left.raw_adgifs.size() == right.raw_adgifs.size() &&
         std::equal(left.raw_adgifs.begin(), left.raw_adgifs.end(), right.raw_adgifs.begin(),
                    same_adgif) &&
         left.village1_vis_alpha_combo_ids == right.village1_vis_alpha_combo_ids &&
         left.enum_maps == right.enum_maps;
}

bool deterministic_little_endian_round_trip() {
  const auto catalog = make_catalog();
  const auto encoded = encode(catalog);
  const auto encoded_again = encode(catalog);
  CHECK(encoded);
  CHECK(encoded_again);
  CHECK(encoded.value() == encoded_again.value());
  const auto& bytes = encoded.value();
  CHECK(std::equal(kMagic.begin(), kMagic.end(), bytes.begin()));
  CHECK(bytes[8] == 1 && bytes[9] == 0 && bytes[10] == 0 && bytes[11] == 0);
  CHECK(read_le64(bytes.data() + 12) == bytes.size() - 28);
  CHECK(read_le64(bytes.data() + bytes.size() - 8) == XXH64(bytes.data(), bytes.size() - 8, 0));
  const std::array<uint8_t, 8> first_adgif_word = {8, 7, 6, 5, 4, 3, 2, 1};
  CHECK(std::search(bytes.begin(), bytes.end(), first_adgif_word.begin(), first_adgif_word.end()) !=
        bytes.end());

  const auto decoded = decode(bytes);
  CHECK(decoded);
  CHECK(catalogs_match(catalog, decoded.value()));
  return true;
}

bool rejects_bad_framing_and_hash() {
  const auto encoded = encode(make_catalog());
  CHECK(encoded);

  auto wrong_magic = encoded.value();
  wrong_magic[0] ^= 1;
  auto result = decode(wrong_magic);
  CHECK(!result && result.error().code == ErrorCode::wrong_magic);

  auto wrong_schema = encoded.value();
  wrong_schema[8] = 2;
  rewrite_hash(&wrong_schema);
  result = decode(wrong_schema);
  CHECK(!result && result.error().code == ErrorCode::unsupported_schema);

  auto corrupt = encoded.value();
  corrupt[24] ^= 1;
  result = decode(corrupt);
  CHECK(!result && result.error().code == ErrorCode::corrupt_hash);

  auto truncated = encoded.value();
  truncated.pop_back();
  result = decode(truncated);
  CHECK(!result && result.error().code == ErrorCode::truncated);

  auto trailing = encoded.value();
  trailing.push_back(0);
  result = decode(trailing);
  CHECK(!result && result.error().code == ErrorCode::trailing_data);

  auto overflow = encoded.value();
  write_le64(overflow.data() + 12, std::numeric_limits<uint64_t>::max());
  result = decode(overflow);
  CHECK(!result && result.error().code == ErrorCode::integer_overflow);
  return true;
}

bool every_truncated_prefix_fails_cleanly() {
  const auto encoded = encode(make_catalog());
  CHECK(encoded);
  for (size_t length = 0; length < encoded.value().size(); ++length) {
    const auto result = decode(std::span<const uint8_t>(encoded.value().data(), length));
    CHECK(!result);
  }
  return true;
}

bool rejects_wrong_revision_and_source_provenance() {
  auto catalog = make_catalog();
  catalog.provenance.producer = "not-opengoal";
  auto result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::wrong_provenance);

  catalog = make_catalog();
  catalog.provenance.revision.config_version = "unknown";
  result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::wrong_provenance);

  catalog = make_catalog();
  catalog.provenance.revision.territory = Territory::scee;
  result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::wrong_provenance);

  catalog = make_catalog();
  catalog.provenance.revision.black_label = false;
  result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::wrong_provenance);

  catalog = make_catalog();
  catalog.provenance.revision.source_manifest_hash = 0;
  result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::wrong_provenance);

  catalog = make_catalog();
  catalog.sources.level_file_name = "other.fr3";
  result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::wrong_provenance);

  const auto encoded = encode(make_catalog());
  CHECK(encoded);
  auto tampered = encoded.value();
  tampered[24] = 'x';
  rewrite_hash(&tampered);
  const auto decoded = decode(tampered);
  CHECK(!decoded && decoded.error().code == ErrorCode::wrong_provenance);
  return true;
}

bool supports_all_verified_revision_profiles() {
  const std::array<RevisionProvenance, 5> revisions = {{
      {"SCUS-97124", 7280758013604870207ULL, 11363853835861842434ULL, 337, "ntsc_v1",
       Territory::scea, true, 1},
      {"SCUS-97124", 744661860962747854ULL, 8538304367812415885ULL, 338, "ntsc_v2", Territory::scea,
       false, 2},
      {"SCES-50361", 12150718117852276522ULL, 16850370297611763875ULL, 338, "pal", Territory::scee,
       false, 3},
      {"SCPS-15021", 16909372048085114219ULL, 1262350561338887717ULL, 338, "jp", Territory::scei,
       false, 4},
      {"SCPS-56003", 7280758013604870207ULL, 13924540661438229398ULL, 338, "ntsc_v1",
       Territory::scea, false, 5},
  }};
  for (const auto& revision : revisions) {
    auto catalog = make_catalog();
    catalog.provenance.revision = revision;
    const auto encoded = encode(catalog);
    CHECK(encoded);
    const auto decoded = decode(encoded.value());
    CHECK(decoded);
    CHECK(catalogs_match(catalog, decoded.value()));
  }
  return true;
}

bool rejects_duplicate_and_out_of_order_texture_metadata() {
  auto catalog = make_catalog();
  catalog.village1_remaps.push_back({19, 99});
  auto result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::duplicate_value);

  catalog = make_catalog();
  catalog.raw_texture_page_count = 2;
  result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::invalid_count);

  catalog = make_catalog();
  catalog.village1_vis_alpha_combo_ids = {1, 1};
  result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::duplicate_value);

  catalog = make_catalog();
  catalog.village1_vis_alpha_combo_ids = {2, 1};
  result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::invalid_order);
  return true;
}

bool enforces_wire_string_and_collection_caps() {
  auto catalog = make_catalog();
  Limits limits;
  limits.max_texture_ids = 2;
  auto result = encode(catalog, limits);
  CHECK(!result && result.error().code == ErrorCode::limit_exceeded);

  const auto encoded = encode(make_catalog());
  CHECK(encoded);
  limits = {};
  limits.max_string_bytes = 3;
  const auto cannot_encode = encode(make_catalog(), limits);
  CHECK(!cannot_encode && cannot_encode.error().code == ErrorCode::limit_exceeded);
  const auto decoded = decode(encoded.value(), limits);
  CHECK(!decoded && decoded.error().code == ErrorCode::limit_exceeded);

  limits = {};
  limits.max_wire_bytes = encoded.value().size() - 1;
  const auto too_large = decode(encoded.value(), limits);
  CHECK(!too_large && too_large.error().code == ErrorCode::limit_exceeded);

  limits = {};
  limits.max_wire_bytes = 1;
  const auto invalid = encode(make_catalog(), limits);
  CHECK(!invalid && invalid.error().code == ErrorCode::invalid_argument);
  return true;
}

bool validates_exact_bounded_enum_maps() {
  auto catalog = make_catalog();
  catalog.enum_maps[0].name = "other";
  auto result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::wrong_provenance);

  catalog = make_catalog();
  catalog.enum_maps[0].entries = {{"zero", 0}, {"zero", 1}};
  result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::duplicate_value);

  catalog = make_catalog();
  catalog.enum_maps[0].entries = {{"zero", 0}, {"confirm", 0x103}};
  result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::invalid_order);

  catalog = make_catalog();
  catalog.enum_maps[0].entries = {{"bad\nname", 0}};
  result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::invalid_name);

  catalog = make_catalog();
  catalog.enum_maps[0].entries = {{"too-large", int64_t(UINT32_MAX) + 1}};
  result = encode(catalog);
  CHECK(!result && result.error().code == ErrorCode::limit_exceeded);

  catalog = make_catalog();
  Limits limits;
  limits.max_enum_entries_per_map = 1;
  result = encode(catalog, limits);
  CHECK(!result && result.error().code == ErrorCode::limit_exceeded);
  return true;
}

bool error_names_are_stable() {
  CHECK(std::string(error_code_name(ErrorCode::truncated)) == "truncated");
  CHECK(std::string(error_code_name(ErrorCode::corrupt_hash)) == "corrupt_hash");
  return true;
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, bool (*)()>> tests = {
      {"deterministic_little_endian_round_trip", deterministic_little_endian_round_trip},
      {"rejects_bad_framing_and_hash", rejects_bad_framing_and_hash},
      {"every_truncated_prefix_fails_cleanly", every_truncated_prefix_fails_cleanly},
      {"rejects_wrong_revision_and_source_provenance",
       rejects_wrong_revision_and_source_provenance},
      {"supports_all_verified_revision_profiles", supports_all_verified_revision_profiles},
      {"rejects_duplicate_and_out_of_order_texture_metadata",
       rejects_duplicate_and_out_of_order_texture_metadata},
      {"enforces_wire_string_and_collection_caps", enforces_wire_string_and_collection_caps},
      {"validates_exact_bounded_enum_maps", validates_exact_bounded_enum_maps},
      {"error_names_are_stable", error_names_are_stable},
  };

  size_t passed = 0;
  for (const auto& [name, test] : tests) {
    if (!test()) {
      std::cerr << "FAILED: " << name << '\n';
      return 1;
    }
    std::cout << "PASS: " << name << '\n';
    passed++;
  }
  std::cout << "Passed " << passed << " deterministic prepared-retail wire tests.\n";
  return 0;
}
