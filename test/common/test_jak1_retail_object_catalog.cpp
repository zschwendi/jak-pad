#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "decompiler/extractor/jak1_retail_object_catalog.h"

#include "third-party/zstd/lib/common/xxhash.h"

namespace {

using jak1_retail_object_catalog::ArchiveSource;
using jak1_retail_object_catalog::ErrorCode;
using jak1_retail_object_catalog::ObjectVersion;

#define CHECK(condition)                                                                   \
  do {                                                                                     \
    if (!(condition)) {                                                                    \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " << #condition \
                << '\n';                                                                   \
      return false;                                                                        \
    }                                                                                      \
  } while (false)

void write_u32(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value) {
  (*bytes)[offset] = value & 0xff;
  (*bytes)[offset + 1] = (value >> 8) & 0xff;
  (*bytes)[offset + 2] = (value >> 16) & 0xff;
  (*bytes)[offset + 3] = value >> 24;
}

void append_u32(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
  const auto offset = bytes->size();
  bytes->resize(offset + 4);
  write_u32(bytes, offset, value);
}

void write_name(std::vector<std::uint8_t>* bytes,
                std::size_t offset,
                std::size_t capacity,
                const std::string& name) {
  std::fill(bytes->begin() + offset, bytes->begin() + offset + capacity, 0);
  std::copy(name.begin(), name.end(), bytes->begin() + offset);
}

void append_name(std::vector<std::uint8_t>* bytes, const std::string& name) {
  const auto offset = bytes->size();
  bytes->resize(offset + 60);
  write_name(bytes, offset, 60, name);
}

std::vector<std::uint8_t> make_v2(std::uint8_t seed = 1) {
  std::vector<std::uint8_t> bytes(144);
  write_u32(&bytes, 0, 0xffffffff);
  write_u32(&bytes, 4, 64);
  write_u32(&bytes, 8, 2);
  for (std::size_t index = 64; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return bytes;
}

std::vector<std::uint8_t> make_v4(std::uint8_t seed = 1) {
  std::vector<std::uint8_t> bytes(144);
  write_u32(&bytes, 0, 0xffffffff);
  write_u32(&bytes, 4, 64);
  write_u32(&bytes, 8, 4);
  write_u32(&bytes, 12, 64);
  for (std::size_t index = 16; index < 80; ++index) {
    bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  write_u32(&bytes, 80, 0xffffffff);
  write_u32(&bytes, 84, 64);
  write_u32(&bytes, 88, 2);
  return bytes;
}

std::vector<std::uint8_t> make_v3(const std::string& name) {
  std::vector<std::uint8_t> bytes(160);
  write_u32(&bytes, 0, 0);
  write_u32(&bytes, 4, 144);
  write_u32(&bytes, 8, 3);
  write_u32(&bytes, 12, 3);
  write_name(&bytes, 16, 64, name);

  write_u32(&bytes, 80, 128);
  write_u32(&bytes, 84, 0);
  write_u32(&bytes, 88, 16);
  write_u32(&bytes, 92, 0);

  write_u32(&bytes, 96, 130);
  write_u32(&bytes, 100, 16);
  write_u32(&bytes, 104, 0);
  write_u32(&bytes, 108, 0);

  write_u32(&bytes, 112, 132);
  write_u32(&bytes, 116, 16);
  write_u32(&bytes, 120, 0);
  write_u32(&bytes, 124, 0);
  std::fill(bytes.begin() + 144, bytes.end(), 0x5a);
  return bytes;
}

std::vector<std::uint8_t> make_unsupported() {
  auto bytes = make_v2();
  write_u32(&bytes, 8, 5);
  return bytes;
}

struct FixtureObject {
  std::string name;
  std::vector<std::uint8_t> bytes;
};

std::vector<std::uint8_t> make_dgo(const std::string& archive_name,
                                   const std::vector<FixtureObject>& objects) {
  std::vector<std::uint8_t> bytes;
  append_u32(&bytes, objects.size());
  append_name(&bytes, archive_name);
  for (const auto& object : objects) {
    append_u32(&bytes, object.bytes.size());
    append_name(&bytes, object.name);
    bytes.insert(bytes.end(), object.bytes.begin(), object.bytes.end());
    while (bytes.size() % 16) {
      bytes.push_back(0);
    }
  }
  return bytes;
}

bool indexes_exact_provenance_in_deterministic_order() {
  const auto shared = make_v2(7);
  const auto archive_z = make_dgo("Z.DGO", {{"shared", shared}, {"zeta", make_v2(9)}});
  const auto archive_a = make_dgo("A.DGO", {{"alpha", make_v4(3)}, {"shared", shared}});
  const std::vector<ArchiveSource> sources = {
      {"DGO/Z.DGO", archive_z},
      {"DGO/A.DGO", archive_a},
  };

  std::vector<jak1_retail_object_catalog::Progress> progress;
  jak1_retail_object_catalog::Options options;
  options.on_progress = [&](const auto& update) { progress.push_back(update); };
  const auto result = jak1_retail_object_catalog::build(sources, options);
  CHECK(result);
  CHECK(result.value().entries().size() == 4);
  CHECK(result.value().entries()[0].provenance.source_archive_relative_path == "DGO/A.DGO");
  CHECK(result.value().entries()[0].provenance.archive_object_index == 0);
  CHECK(result.value().entries()[0].provenance.internal_name == "alpha");
  CHECK(result.value().entries()[0].provenance.unique_name == "alpha");
  const auto expected_alpha = make_v4(3);
  CHECK(result.value().entries()[0].provenance.byte_size == expected_alpha.size());
  CHECK(result.value().entries()[0].provenance.xxh64 ==
        XXH64(expected_alpha.data(), expected_alpha.size(), 0));
  CHECK(result.value().entries()[0].provenance.object_version == ObjectVersion::v4);
  CHECK(result.value().entries()[1].provenance.internal_name == "shared");
  CHECK(result.value().entries()[1].provenance.unique_name == "shared");
  CHECK(result.value().entries()[2].provenance.source_archive_relative_path == "DGO/Z.DGO");
  CHECK(result.value().entries()[2].provenance.internal_name == "shared");
  CHECK(result.value().entries()[2].provenance.unique_name == "shared");
  CHECK(result.value().entries()[3].provenance.internal_name == "zeta");
  CHECK(result.value().entries()[3].provenance.unique_name == "zeta");
  CHECK(progress.front().stage == jak1_retail_object_catalog::ProgressStage::starting_archive);
  CHECK(progress.back().stage == jak1_retail_object_catalog::ProgressStage::complete);
  CHECK(progress.back().entries_indexed == 4);

  const std::vector<ArchiveSource> already_sorted = {
      {"DGO/A.DGO", archive_a},
      {"DGO/Z.DGO", archive_z},
  };
  const auto second_result = jak1_retail_object_catalog::build(already_sorted);
  CHECK(second_result);
  CHECK(second_result.value().entries().size() == result.value().entries().size());
  for (std::size_t index = 0; index < result.value().entries().size(); ++index) {
    CHECK(second_result.value().entries()[index].provenance ==
          result.value().entries()[index].provenance);
  }

  const auto expected = result.value().entries()[0].provenance;
  const auto lookup = result.value().lookup(expected);
  CHECK(lookup);
  CHECK(lookup.value()->provenance == expected);

  auto mismatched = expected;
  mismatched.xxh64 ^= 1;
  const auto mismatch_result = result.value().lookup(mismatched);
  CHECK(!mismatch_result);
  CHECK(mismatch_result.error().code == ErrorCode::provenance_mismatch);

  auto basename_only = expected;
  basename_only.source_archive_relative_path = "A.DGO";
  const auto basename_result = result.value().lookup(basename_only);
  CHECK(!basename_result);
  CHECK(basename_result.error().code == ErrorCode::object_not_found);
  return true;
}

bool divergent_names_require_exact_provenance() {
  const auto archive_a = make_dgo("A.DGO", {{"same", make_v2(1)}});
  const auto archive_b = make_dgo("B.DGO", {{"same", make_v2(2)}});
  const std::vector<ArchiveSource> divergent = {
      {"DGO/A.DGO", archive_a},
      {"DGO/B.DGO", archive_b},
  };
  auto result = jak1_retail_object_catalog::build(divergent);
  CHECK(result);
  CHECK(result.value().entries().size() == 2);
  CHECK(result.value().entries()[0].provenance.internal_name == "same");
  CHECK(result.value().entries()[1].provenance.internal_name == "same");
  CHECK(result.value().entries()[0].provenance.xxh64 !=
        result.value().entries()[1].provenance.xxh64);
  CHECK(result.value().lookup(result.value().entries()[0].provenance));
  CHECK(result.value().lookup(result.value().entries()[1].provenance));

  auto crossed_provenance = result.value().entries()[0].provenance;
  crossed_provenance.xxh64 = result.value().entries()[1].provenance.xxh64;
  const auto crossed_lookup = result.value().lookup(crossed_provenance);
  CHECK(!crossed_lookup);
  CHECK(crossed_lookup.error().code == ErrorCode::provenance_mismatch);

  auto wrong_unique_name = result.value().entries()[0].provenance;
  wrong_unique_name.unique_name = "same-144";
  const auto unique_name_lookup = result.value().lookup(wrong_unique_name);
  CHECK(!unique_name_lookup);
  CHECK(unique_name_lookup.error().code == ErrorCode::provenance_mismatch);

  const auto wrong_internal_name = make_dgo("OTHER.DGO", {{"one", make_v2()}});
  const std::vector<ArchiveSource> mismatched = {
      {"DGO/EXPECTED.DGO", wrong_internal_name},
  };
  result = jak1_retail_object_catalog::build(mismatched);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::checked_dgo_failed);
  CHECK(result.error().checked_dgo_error.has_value());
  CHECK(result.error().checked_dgo_error->code ==
        jak1_checked_dgo::ErrorCode::unexpected_archive_name);
  return true;
}

bool retains_checked_unique_names_and_rejects_ambiguity() {
  const auto duplicate_archive =
      make_dgo("DUP.DGO", {{"same", make_v2(1)}, {"same", make_v2(2)}});
  std::vector<ArchiveSource> sources = {{"DGO/DUP.DGO", duplicate_archive}};
  auto result = jak1_retail_object_catalog::build(sources);
  CHECK(result);
  CHECK(result.value().entries().size() == 2);
  CHECK(result.value().entries()[0].provenance.internal_name == "same");
  CHECK(result.value().entries()[0].provenance.unique_name == "same");
  CHECK(result.value().entries()[1].provenance.internal_name == "same");
  CHECK(result.value().entries()[1].provenance.unique_name == "same-144");

  const auto exact_lookup =
      result.value().lookup(result.value().entries()[1].provenance);
  CHECK(exact_lookup);
  CHECK(exact_lookup.value()->provenance.unique_name == "same-144");

  auto crossed_unique_name = result.value().entries()[0].provenance;
  crossed_unique_name.unique_name = result.value().entries()[1].provenance.unique_name;
  const auto crossed_lookup = result.value().lookup(crossed_unique_name);
  CHECK(!crossed_lookup);
  CHECK(crossed_lookup.error().code == ErrorCode::provenance_mismatch);

  auto art_group = make_v2(3);
  const std::string marker = "/src/next/data/art-group6/darkeco-ag.go";
  std::copy(marker.begin(), marker.end(), art_group.begin() + 80);
  art_group[80 + marker.size()] = 0;
  const auto art_archive = make_dgo("ART.DGO", {{"darkeco", art_group}});
  sources = {{"DGO/ART.DGO", art_archive}};
  result = jak1_retail_object_catalog::build(sources);
  CHECK(result);
  CHECK(result.value().entries().size() == 1);
  CHECK(result.value().entries()[0].provenance.internal_name == "darkeco");
  CHECK(result.value().entries()[0].provenance.unique_name == "darkeco-ag");

  const auto ambiguous_archive = make_dgo(
      "AMBIG.DGO", {{"same", make_v2(1)}, {"same", make_v2(2)}, {"same", make_v2(3)}});
  sources = {{"DGO/AMBIG.DGO", ambiguous_archive}};
  result = jak1_retail_object_catalog::build(sources);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::checked_dgo_failed);
  CHECK(result.error().checked_dgo_error.has_value());
  CHECK(result.error().checked_dgo_error->code ==
        jak1_checked_dgo::ErrorCode::duplicate_object_name);
  CHECK(result.error().checked_dgo_error->object_index == 2);
  return true;
}

bool skips_code_and_rejects_invalid_or_unsupported_headers() {
  auto archive = make_dgo("CODE.DGO", {{"code", make_v3("code")}});
  std::vector<ArchiveSource> sources = {{"DGO/CODE.DGO", archive}};
  auto result = jak1_retail_object_catalog::build(sources);
  CHECK(result);
  CHECK(result.value().entries().empty());
  CHECK(result.value().skipped_code_object_count() == 1);

  archive =
      make_dgo("MIXED.DGO",
               {{"code-a", make_v3("code-a")}, {"data", make_v4()}, {"code-b", make_v3("code-b")}});
  sources = {{"DGO/MIXED.DGO", archive}};
  jak1_retail_object_catalog::Options one_data_entry;
  one_data_entry.max_entries = 1;
  result = jak1_retail_object_catalog::build(sources, one_data_entry);
  CHECK(result);
  CHECK(result.value().entries().size() == 1);
  CHECK(result.value().entries()[0].provenance.archive_object_index == 1);
  CHECK(result.value().entries()[0].provenance.internal_name == "data");
  CHECK(result.value().skipped_code_object_count() == 2);

  auto invalid_v3 = make_v3("code");
  write_u32(&invalid_v3, 92, 1);
  archive = make_dgo("BADV3.DGO", {{"code", invalid_v3}});
  sources = {{"DGO/BADV3.DGO", archive}};
  result = jak1_retail_object_catalog::build(sources);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::invalid_object_header);
  CHECK(result.error().detected_object_version == 3);

  auto invalid_v4 = make_v4();
  write_u32(&invalid_v4, 84, 32);
  archive = make_dgo("BAD.DGO", {{"bad", invalid_v4}});
  sources = {{"DGO/BAD.DGO", archive}};
  result = jak1_retail_object_catalog::build(sources);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::invalid_object_header);

  archive = make_dgo("NEW.DGO", {{"new", make_unsupported()}});
  sources = {{"DGO/NEW.DGO", archive}};
  result = jak1_retail_object_catalog::build(sources);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::unsupported_object_version);
  CHECK(result.error().detected_object_version == 5);
  return true;
}

bool enforces_caps_paths_and_callbacks() {
  jak1_retail_object_catalog::Options defaults;
  CHECK(!defaults.compressed_trailing_alignment_bytes);
  const auto archive_a = make_dgo("A.DGO", {{"one", make_v2()}});
  const auto archive_b = make_dgo("B.DGO", {{"two", make_v4()}});
  const std::vector<ArchiveSource> sources = {
      {"DGO/A.DGO", archive_a},
      {"DGO/B.DGO", archive_b},
  };

  jak1_retail_object_catalog::Options options;
  options.max_archives = 1;
  auto result = jak1_retail_object_catalog::build(sources, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::archive_limit_exceeded);

  const auto two_objects = make_dgo("TWO.DGO", {{"one", make_v2()}, {"two", make_v4()}});
  const std::vector<ArchiveSource> two_source = {{"DGO/TWO.DGO", two_objects}};
  options = {};
  options.max_entries = 1;
  result = jak1_retail_object_catalog::build(two_source, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::entry_limit_exceeded);

  options = {};
  options.max_total_object_bytes = 200;
  result = jak1_retail_object_catalog::build(sources, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::total_byte_limit_exceeded);

  options = {};
  options.max_internal_name_bytes = 2;
  result = jak1_retail_object_catalog::build(sources, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::checked_dgo_failed);
  CHECK(result.error().checked_dgo_error.has_value());
  CHECK(result.error().checked_dgo_error->code == jak1_checked_dgo::ErrorCode::invalid_name);

  const std::vector<ArchiveSource> duplicate_path = {
      {"DGO/A.DGO", archive_a},
      {"DGO/A.DGO", archive_a},
  };
  result = jak1_retail_object_catalog::build(duplicate_path);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::duplicate_source_archive_path);

  const std::vector<ArchiveSource> traversal = {{"DGO/../A.DGO", archive_a}};
  result = jak1_retail_object_catalog::build(traversal);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::invalid_source_archive_path);

  options = {};
  options.compressed_trailing_alignment_bytes = 0;
  result = jak1_retail_object_catalog::build(sources, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::invalid_argument);

  options = {};
  options.should_cancel = []() { return true; };
  result = jak1_retail_object_catalog::build(sources, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::cancelled);

  options = {};
  options.on_progress = [](const auto&) { throw std::runtime_error("synthetic callback"); };
  result = jak1_retail_object_catalog::build(sources, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::callback_failed);
  return true;
}

bool error_names_are_stable() {
  CHECK(std::string(jak1_retail_object_catalog::error_code_name(
            ErrorCode::unsupported_object_version)) == "unsupported_object_version");
  CHECK(std::string(jak1_retail_object_catalog::error_code_name(ErrorCode::provenance_mismatch)) ==
        "provenance_mismatch");
  return true;
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, bool (*)()>> tests = {
      {"indexes_exact_provenance_in_deterministic_order",
       indexes_exact_provenance_in_deterministic_order},
      {"divergent_names_require_exact_provenance", divergent_names_require_exact_provenance},
      {"retains_checked_unique_names_and_rejects_ambiguity",
       retains_checked_unique_names_and_rejects_ambiguity},
      {"skips_code_and_rejects_invalid_or_unsupported_headers",
       skips_code_and_rejects_invalid_or_unsupported_headers},
      {"enforces_caps_paths_and_callbacks", enforces_caps_paths_and_callbacks},
      {"error_names_are_stable", error_names_are_stable},
  };

  std::size_t passed = 0;
  for (const auto& [name, test] : tests) {
    if (!test()) {
      std::cerr << "FAILED: " << name << '\n';
      return 1;
    }
    std::cout << "PASS: " << name << '\n';
    ++passed;
  }
  std::cout << "Passed " << passed << " deterministic synthetic retail-object catalog tests.\n";
  return 0;
}
