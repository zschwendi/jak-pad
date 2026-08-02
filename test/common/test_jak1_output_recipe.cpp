#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/custom_data/Jak1OutputRecipe.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace {

using namespace jak1_output_recipe;

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": " #condition << '\n'; \
      return false;                                                                              \
    }                                                                                            \
  } while (false)

constexpr SourceObjectPackIdentity kSourcePack = {2, 0x123456789abcdef0ULL};

RevisionProvenance provenance_of(const jak1_iso::Revision& revision) {
  return {
      std::string(revision.serial),
      revision.elf_hash,
      revision.contents_hash,
      revision.file_count,
      std::string(revision.decomp_config_version),
      revision.territory,
      revision.black_label,
  };
}

Options make_options() {
  Options options;
  options.expected_source_object_pack = kSourcePack;
  return options;
}

Recipe make_recipe() {
  Recipe recipe;
  recipe.revision = provenance_of(jak1_iso::default_revision());
  recipe.source_object_pack = kSourcePack;
  recipe.archives = {
      {
          "GAME.CGO",
          {
              {"shared", BundledSourceObject{"obj/shared.o", 3, 0x1111111111111111ULL}},
              {"shared", VerifiedRetailObject{"DGO/GAME.CGO", 12, 4, 5, 0x2222222222222222ULL}},
              {"game-cnt", GeneratedData{GeneratedDataKind::game_count}},
          },
      },
      {
          "VIL1.DGO",
          {
              {"shared", BundledSourceObject{"obj/shared.o", 3, 0x1111111111111111ULL}},
              {"village1", BundledSourceObject{"obj/village1.o", 7, 0x3333333333333333ULL}},
              {"tpage-12",
               VerifiedRetailObject{"DGO/VILLAGE1.DGO", 27, 2, 11, 0x4444444444444444ULL}},
          },
      },
  };
  recipe.flat_file_copies = {
      {"MUS/TWEAKVAL.MUS", "TWEAKVAL.MUS"},
      {"VAG/VAGDIR.AYB", "VAGDIR.AYB"},
  };
  for (uint32_t language = 0; language < 7; ++language) {
    recipe.generated_flat_files.push_back(
        {GeneratedFlatFileKind::game_text, std::to_string(language) + "COMMON.TXT"});
    recipe.generated_flat_files.push_back(
        {GeneratedFlatFileKind::game_subtitle, std::to_string(language) + "SUBTIT.TXT"});
  }
  recipe.expected_fr3_basenames = {"beach.fr3", "village1.fr3"};
  return recipe;
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

size_t find_wire_string(const std::vector<uint8_t>& bytes, std::string_view value) {
  std::vector<uint8_t> needle(4 + value.size());
  const auto size = static_cast<uint32_t>(value.size());
  for (int shift = 0; shift < 32; shift += 8) {
    needle[shift / 8] = static_cast<uint8_t>(size >> shift);
  }
  std::copy(value.begin(), value.end(), needle.begin() + 4);
  const auto position = std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end());
  return position == bytes.end() ? bytes.size() : static_cast<size_t>(position - bytes.begin());
}

bool deterministic_little_endian_round_trip() {
  const auto recipe = make_recipe();
  const auto options = make_options();
  const auto encoded = encode(recipe, options);
  const auto encoded_again = encode(recipe, options);
  CHECK(encoded);
  CHECK(encoded_again);
  CHECK(encoded.value() == encoded_again.value());
  const auto& bytes = encoded.value();
  CHECK(std::equal(kMagic.begin(), kMagic.end(), bytes.begin()));
  CHECK(bytes[8] == 2 && bytes[9] == 0 && bytes[10] == 0 && bytes[11] == 0);
  CHECK(read_le64(bytes.data() + 12) == bytes.size() - 28);
  CHECK(read_le64(bytes.data() + bytes.size() - 8) == XXH64(bytes.data(), bytes.size() - 8, 0));
  const std::array<uint8_t, 8> source_size_le = {3, 0, 0, 0, 0, 0, 0, 0};
  CHECK(std::search(bytes.begin(), bytes.end(), source_size_le.begin(), source_size_le.end()) !=
        bytes.end());

  const auto decoded = decode(bytes, options);
  CHECK(decoded);
  CHECK(decoded.value() == recipe);
  CHECK(decoded.value().archives[0].objects[0].internal_name == "shared");
  CHECK(decoded.value().archives[0].objects[1].internal_name == "shared");
  CHECK(std::holds_alternative<BundledSourceObject>(decoded.value().archives[0].objects[0].source));
  CHECK(
      std::holds_alternative<VerifiedRetailObject>(decoded.value().archives[0].objects[1].source));
  CHECK(decoded.value().generated_flat_files.size() == 14);
  CHECK(decoded.value().generated_flat_files.front().kind == GeneratedFlatFileKind::game_text);
  CHECK(decoded.value().generated_flat_files.back().kind == GeneratedFlatFileKind::game_subtitle);
  return true;
}

bool rejects_corruption_bad_framing_and_every_truncation() {
  const auto options = make_options();
  const auto encoded = encode(make_recipe(), options);
  CHECK(encoded);

  auto wrong_magic = encoded.value();
  wrong_magic[0] ^= 1;
  auto result = decode(wrong_magic, options);
  CHECK(!result && result.error().code == ErrorCode::wrong_magic);

  auto wrong_schema = encoded.value();
  wrong_schema[8] = 3;
  rewrite_hash(&wrong_schema);
  result = decode(wrong_schema, options);
  CHECK(!result && result.error().code == ErrorCode::unsupported_schema);

  auto corrupt = encoded.value();
  corrupt[30] ^= 1;
  result = decode(corrupt, options);
  CHECK(!result && result.error().code == ErrorCode::corrupt_hash);

  auto trailing = encoded.value();
  trailing.push_back(0);
  result = decode(trailing, options);
  CHECK(!result && result.error().code == ErrorCode::trailing_data);

  auto overflow = encoded.value();
  write_le64(overflow.data() + 12, std::numeric_limits<uint64_t>::max());
  result = decode(overflow, options);
  CHECK(!result && result.error().code == ErrorCode::integer_overflow);

  for (size_t length = 0; length < encoded.value().size(); ++length) {
    result = decode(std::span<const uint8_t>(encoded.value().data(), length), options);
    CHECK(!result);
  }
  return true;
}

bool supports_only_exact_known_revisions() {
  auto options = make_options();
  for (const auto& revision : jak1_iso::supported_revisions()) {
    auto recipe = make_recipe();
    recipe.revision = provenance_of(revision);
    const auto encoded = encode(recipe, options);
    CHECK(encoded);
    const auto decoded = decode(encoded.value(), options);
    CHECK(decoded);
    CHECK(decoded.value().revision == recipe.revision);
  }

  auto recipe = make_recipe();
  recipe.revision.contents_hash ^= 1;
  auto result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::unsupported_revision);

  recipe = make_recipe();
  recipe.revision.config_version = "unknown";
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::unsupported_revision);

  recipe = make_recipe();
  recipe.producer = "other";
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::wrong_provenance);
  return true;
}

bool rejects_wrong_source_pack_and_object_versions() {
  auto options = make_options();
  auto recipe = make_recipe();
  recipe.source_object_pack.aggregate_xxh64 ^= 1;
  auto result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::wrong_source_pack);

  options.expected_source_object_pack.aggregate_xxh64 ^= 1;
  result = encode(make_recipe(), options);
  CHECK(!result && result.error().code == ErrorCode::wrong_source_pack);

  const auto canonical_wire = encode(make_recipe(), make_options());
  CHECK(canonical_wire);
  const auto decoded_with_wrong_pack = decode(canonical_wire.value(), options);
  CHECK(!decoded_with_wrong_pack &&
        decoded_with_wrong_pack.error().code == ErrorCode::wrong_source_pack);

  options = make_options();
  recipe = make_recipe();
  recipe.source_object_pack.object_count = 3;
  options.expected_source_object_pack = recipe.source_object_pack;
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::wrong_source_pack);

  options = make_options();
  recipe = make_recipe();
  std::get<VerifiedRetailObject>(recipe.archives[0].objects[1].source).object_version = 3;
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::invalid_object_version);

  recipe = make_recipe();
  std::get<VerifiedRetailObject>(recipe.archives[0].objects[1].source).object_version = 5;
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::invalid_object_version);
  return true;
}

bool enforces_name_path_count_and_size_caps() {
  const auto options = make_options();
  auto recipe = make_recipe();
  std::get<BundledSourceObject>(recipe.archives[0].objects[0].source).bundle_relative_path =
      "../obj/shared.o";
  auto result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::unsafe_path);

  recipe = make_recipe();
  std::get<BundledSourceObject>(recipe.archives[0].objects[0].source).bundle_relative_path =
      "obj//shared.o";
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::unsafe_path);

  recipe = make_recipe();
  std::get<VerifiedRetailObject>(recipe.archives[0].objects[1].source)
      .source_archive_relative_path = "C:\\GAME.CGO";
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::unsafe_path);

  recipe = make_recipe();
  recipe.flat_file_copies[0].extracted_iso_relative_path = "/MUS/TWEAKVAL.MUS";
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::unsafe_path);

  recipe = make_recipe();
  recipe.archives[0].destination_basename = "GAME.BIN";
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::invalid_name);

  recipe = make_recipe();
  recipe.expected_fr3_basenames[0] = "../beach.fr3";
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::invalid_name);

  const auto canonical_wire = encode(make_recipe(), options);
  CHECK(canonical_wire);
  auto traversing_wire = canonical_wire.value();
  const auto bundle_path = find_wire_string(traversing_wire, "obj/shared.o");
  CHECK(bundle_path < traversing_wire.size());
  const std::string replacement = "../shared.oX";
  CHECK(replacement.size() == std::string("obj/shared.o").size());
  std::copy(replacement.begin(), replacement.end(), traversing_wire.begin() + bundle_path + 4);
  rewrite_hash(&traversing_wire);
  const auto traversing_decode = decode(traversing_wire, options);
  CHECK(!traversing_decode && traversing_decode.error().code == ErrorCode::unsafe_path);

  recipe = make_recipe();
  auto capped = make_options();
  capped.limits.max_archives = 1;
  result = encode(recipe, capped);
  CHECK(!result && result.error().code == ErrorCode::limit_exceeded);

  capped = make_options();
  capped.limits.max_objects_per_archive = 2;
  result = encode(recipe, capped);
  CHECK(!result && result.error().code == ErrorCode::limit_exceeded);

  capped = make_options();
  capped.limits.max_object_bytes = 4;
  result = encode(recipe, capped);
  CHECK(!result && result.error().code == ErrorCode::limit_exceeded);

  capped = make_options();
  capped.limits.max_generated_flat_files = 13;
  result = encode(recipe, capped);
  CHECK(!result && result.error().code == ErrorCode::limit_exceeded);

  const auto encoded = encode(recipe, options);
  CHECK(encoded);
  capped = make_options();
  capped.limits.max_wire_bytes = encoded.value().size() - 1;
  const auto decoded = decode(encoded.value(), capped);
  CHECK(!decoded && decoded.error().code == ErrorCode::limit_exceeded);
  return true;
}

bool rejects_ambiguity_duplicates_and_identity_conflicts() {
  const auto options = make_options();
  auto recipe = make_recipe();
  recipe.archives[0].objects.push_back({"shared", BundledSourceObject{"obj/other.o", 1, 5}});
  auto result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::ambiguous_object);

  recipe = make_recipe();
  recipe.archives[0].objects.push_back(recipe.archives[0].objects[0]);
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::ambiguous_object);

  recipe = make_recipe();
  std::get<BundledSourceObject>(recipe.archives[1].objects[0].source).size = 4;
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::ambiguous_object);

  recipe = make_recipe();
  recipe.archives[1].objects.push_back(
      {"other", VerifiedRetailObject{"DGO/GAME.CGO", 12, 4, 5, 0x2222222222222222ULL}});
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::ambiguous_object);

  recipe = make_recipe();
  recipe.archives.push_back(recipe.archives.back());
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::duplicate_destination);

  recipe = make_recipe();
  recipe.flat_file_copies.push_back({"OTHER/VAGDIR.AYB", "VAGDIR.AYB"});
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::duplicate_destination);

  recipe = make_recipe();
  recipe.flat_file_copies[1].extracted_iso_relative_path =
      recipe.flat_file_copies[0].extracted_iso_relative_path;
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::duplicate_value);

  recipe = make_recipe();
  recipe.flat_file_copies = {{"OTHER/GAME.CGO", "GAME.CGO"}};
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::duplicate_destination);

  recipe = make_recipe();
  recipe.flat_file_copies = {{"OTHER/game.cgo", "game.cgo"}};
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::duplicate_destination);

  recipe = make_recipe();
  recipe.generated_flat_files = {
      {GeneratedFlatFileKind::game_text, "TWEAKVAL.MUS"},
  };
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::duplicate_destination);

  recipe = make_recipe();
  recipe.generated_flat_files[0].kind = static_cast<GeneratedFlatFileKind>(99);
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::invalid_generated_flat_kind);

  recipe = make_recipe();
  std::get<BundledSourceObject>(recipe.archives[1].objects[0].source).bundle_relative_path =
      "OBJ/shared.o";
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::ambiguous_object);

  recipe = make_recipe();
  recipe.expected_fr3_basenames.push_back("village1.fr3");
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::duplicate_value);

  recipe = make_recipe();
  recipe.expected_fr3_basenames = {"VILLAGE1.fr3", "village1.fr3"};
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::duplicate_destination);
  return true;
}

bool enforces_deterministic_outer_order_and_preserves_object_order() {
  const auto options = make_options();
  auto recipe = make_recipe();
  std::swap(recipe.archives[0], recipe.archives[1]);
  auto result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::invalid_order);

  recipe = make_recipe();
  std::swap(recipe.flat_file_copies[0], recipe.flat_file_copies[1]);
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::invalid_order);

  recipe = make_recipe();
  std::swap(recipe.expected_fr3_basenames[0], recipe.expected_fr3_basenames[1]);
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::invalid_order);

  recipe = make_recipe();
  std::swap(recipe.generated_flat_files[0], recipe.generated_flat_files[1]);
  result = encode(recipe, options);
  CHECK(!result && result.error().code == ErrorCode::invalid_order);

  const auto original = make_recipe();
  auto reordered_objects = original;
  std::swap(reordered_objects.archives[0].objects[0], reordered_objects.archives[0].objects[1]);
  const auto original_wire = encode(original, options);
  const auto reordered_wire = encode(reordered_objects, options);
  CHECK(original_wire && reordered_wire);
  CHECK(original_wire.value() != reordered_wire.value());
  const auto decoded = decode(reordered_wire.value(), options);
  CHECK(decoded);
  CHECK(
      std::holds_alternative<VerifiedRetailObject>(decoded.value().archives[0].objects[0].source));
  CHECK(std::holds_alternative<BundledSourceObject>(decoded.value().archives[0].objects[1].source));
  return true;
}

bool rejects_unknown_wire_kinds() {
  const auto options = make_options();
  const auto encoded = encode(make_recipe(), options);
  CHECK(encoded);

  auto unknown_source = encoded.value();
  const auto shared = find_wire_string(unknown_source, "shared");
  CHECK(shared < unknown_source.size());
  unknown_source[shared + 4 + std::string("shared").size()] = 9;
  rewrite_hash(&unknown_source);
  auto result = decode(unknown_source, options);
  CHECK(!result && result.error().code == ErrorCode::invalid_source_kind);

  auto unknown_generated = encoded.value();
  const auto game_count = find_wire_string(unknown_generated, "game-cnt");
  CHECK(game_count < unknown_generated.size());
  const auto generated_kind_offset = game_count + 4 + std::string("game-cnt").size() + 1;
  unknown_generated[generated_kind_offset] = 99;
  rewrite_hash(&unknown_generated);
  result = decode(unknown_generated, options);
  CHECK(!result && result.error().code == ErrorCode::invalid_generated_kind);

  auto unknown_generated_flat = encoded.value();
  const auto generated_flat = find_wire_string(unknown_generated_flat, "0COMMON.TXT");
  CHECK(generated_flat > 0 && generated_flat < unknown_generated_flat.size());
  unknown_generated_flat[generated_flat - 1] = 99;
  rewrite_hash(&unknown_generated_flat);
  result = decode(unknown_generated_flat, options);
  CHECK(!result && result.error().code == ErrorCode::invalid_generated_flat_kind);
  return true;
}

bool cancellation_and_callback_failures_are_typed() {
  auto options = make_options();
  int calls = 0;
  options.should_cancel = [&]() { return ++calls == 6; };
  auto result = encode(make_recipe(), options);
  CHECK(!result && result.error().code == ErrorCode::cancelled);

  options = make_options();
  const auto encoded = encode(make_recipe(), options);
  CHECK(encoded);
  calls = 0;
  options.limits.hash_chunk_bytes = 1;
  options.should_cancel = [&]() { return ++calls == 4; };
  const auto decoded = decode(encoded.value(), options);
  CHECK(!decoded && decoded.error().code == ErrorCode::cancelled);

  options = make_options();
  options.should_cancel = []() -> bool { throw 1; };
  result = encode(make_recipe(), options);
  CHECK(!result && result.error().code == ErrorCode::callback_failed);
  return true;
}

bool error_names_are_stable() {
  CHECK(std::string(error_code_name(ErrorCode::wrong_source_pack)) == "wrong_source_pack");
  CHECK(std::string(error_code_name(ErrorCode::invalid_object_version)) ==
        "invalid_object_version");
  CHECK(std::string(error_code_name(ErrorCode::invalid_generated_flat_kind)) ==
        "invalid_generated_flat_kind");
  CHECK(std::string(error_code_name(ErrorCode::corrupt_hash)) == "corrupt_hash");
  return true;
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, bool (*)()>> tests = {
      {"deterministic_little_endian_round_trip", deterministic_little_endian_round_trip},
      {"rejects_corruption_bad_framing_and_every_truncation",
       rejects_corruption_bad_framing_and_every_truncation},
      {"supports_only_exact_known_revisions", supports_only_exact_known_revisions},
      {"rejects_wrong_source_pack_and_object_versions",
       rejects_wrong_source_pack_and_object_versions},
      {"enforces_name_path_count_and_size_caps", enforces_name_path_count_and_size_caps},
      {"rejects_ambiguity_duplicates_and_identity_conflicts",
       rejects_ambiguity_duplicates_and_identity_conflicts},
      {"enforces_deterministic_outer_order_and_preserves_object_order",
       enforces_deterministic_outer_order_and_preserves_object_order},
      {"rejects_unknown_wire_kinds", rejects_unknown_wire_kinds},
      {"cancellation_and_callback_failures_are_typed",
       cancellation_and_callback_failures_are_typed},
      {"error_names_are_stable", error_names_are_stable},
  };

  for (const auto& [name, test] : tests) {
    if (!test()) {
      std::cerr << "FAILED: " << name << '\n';
      return 1;
    }
    std::cout << "PASS: " << name << '\n';
  }
  return 0;
}
