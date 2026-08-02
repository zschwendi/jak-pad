#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "decompiler/extractor/jak1_iso_validation.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace {

constexpr size_t kSectorSize = 2048;

void write_le16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value & 0xff);
  output[1] = static_cast<uint8_t>(value >> 8);
}

void write_be16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value >> 8);
  output[1] = static_cast<uint8_t>(value & 0xff);
}

void write_both16(uint8_t* output, uint16_t value) {
  write_le16(output, value);
  write_be16(output + 2, value);
}

void write_le32(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value & 0xff);
  output[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  output[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  output[3] = static_cast<uint8_t>(value >> 24);
}

void write_be32(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24);
  output[1] = static_cast<uint8_t>((value >> 16) & 0xff);
  output[2] = static_cast<uint8_t>((value >> 8) & 0xff);
  output[3] = static_cast<uint8_t>(value & 0xff);
}

void write_both32(uint8_t* output, uint32_t value) {
  write_le32(output, value);
  write_be32(output + 4, value);
}

size_t write_record(std::vector<uint8_t>* image,
                    size_t offset,
                    uint32_t extent_sector,
                    uint32_t data_size,
                    bool directory,
                    const std::vector<uint8_t>& identifier) {
  const size_t record_size = 33 + identifier.size() + (identifier.size() % 2 == 0 ? 1 : 0);
  auto* record = image->data() + offset;
  record[0] = static_cast<uint8_t>(record_size);
  write_both32(record + 2, extent_sector);
  write_both32(record + 10, data_size);
  record[25] = directory ? 0x02 : 0;
  write_both16(record + 28, 1);
  record[32] = static_cast<uint8_t>(identifier.size());
  std::copy(identifier.begin(), identifier.end(), record + 33);
  return record_size;
}

std::vector<uint8_t> identifier(const std::string& value) {
  return {value.begin(), value.end()};
}

struct FixtureFile {
  std::string iso_name;
  std::vector<uint8_t> contents;
};

std::vector<uint8_t> make_iso(const std::vector<FixtureFile>& files, bool include_dgo = true) {
  constexpr uint32_t sectors = 64;
  constexpr uint32_t root_sector = 20;
  constexpr uint32_t dgo_sector = 21;
  constexpr uint32_t first_file_sector = 22;
  std::vector<uint8_t> image(size_t(sectors) * kSectorSize);

  auto* primary = image.data() + 16 * kSectorSize;
  primary[0] = 1;
  std::memcpy(primary + 1, "CD001", 5);
  primary[6] = 1;
  write_both32(primary + 80, sectors);
  write_both16(primary + 128, kSectorSize);
  write_record(&image, 16 * kSectorSize + 156, root_sector, kSectorSize, true, {0});

  auto* terminator = image.data() + 17 * kSectorSize;
  terminator[0] = 255;
  std::memcpy(terminator + 1, "CD001", 5);
  terminator[6] = 1;

  size_t root_offset = root_sector * kSectorSize;
  root_offset += write_record(&image, root_offset, root_sector, kSectorSize, true, {0});
  root_offset += write_record(&image, root_offset, root_sector, kSectorSize, true, {1});
  for (size_t index = 0; index < files.size(); ++index) {
    const auto file_sector = first_file_sector + static_cast<uint32_t>(index);
    root_offset += write_record(&image, root_offset, file_sector,
                                static_cast<uint32_t>(files[index].contents.size()), false,
                                identifier(files[index].iso_name));
    std::copy(files[index].contents.begin(), files[index].contents.end(),
              image.begin() + size_t(file_sector) * kSectorSize);
  }
  if (include_dgo) {
    write_record(&image, root_offset, dgo_sector, kSectorSize, true, identifier("DGO"));
    size_t dgo_offset = dgo_sector * kSectorSize;
    dgo_offset += write_record(&image, dgo_offset, dgo_sector, kSectorSize, true, {0});
    write_record(&image, dgo_offset, root_sector, kSectorSize, true, {1});
  }
  return image;
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("opengoal-jak1-validation-test-" + std::to_string(stamp));
    std::filesystem::create_directory(path);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }

  std::filesystem::path path;
};

class OpenFile {
 public:
  explicit OpenFile(const std::filesystem::path& path) : file(fopen(path.string().c_str(), "rb")) {}
  ~OpenFile() {
    if (file) {
      fclose(file);
    }
  }

  FILE* file = nullptr;
};

bool write_bytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return bool(output);
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

uint64_t legacy_aggregate(std::span<const uint64_t> hashes) {
  uint64_t combined = 0;
  for (const auto hash : hashes) {
    combined ^= hash;
  }
  return XXH64(&combined, sizeof(combined), 0);
}

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": " #condition << '\n'; \
      return false;                                                                              \
    }                                                                                            \
  } while (false)

bool catalog_preserves_desktop_values() {
  struct Expected {
    std::string_view serial;
    uint64_t elf_hash;
    std::string_view name;
    jak1_iso::Territory territory;
    uint32_t files;
    uint64_t contents_hash;
    std::string_view config;
    bool black_label;
  };
  constexpr std::array expected = {
      Expected{"SCUS-97124", 7280758013604870207U,
               "Jak & Daxter™: The Precursor Legacy (Black Label)", jak1_iso::Territory::scea,
               337, 11363853835861842434U, "ntsc_v1", true},
      Expected{"SCUS-97124", 744661860962747854U, "Jak & Daxter™: The Precursor Legacy",
               jak1_iso::Territory::scea, 338, 8538304367812415885U, "ntsc_v2", false},
      Expected{"SCES-50361", 12150718117852276522U, "Jak & Daxter™: The Precursor Legacy",
               jak1_iso::Territory::scee, 338, 16850370297611763875U, "pal", false},
      Expected{"SCPS-15021", 16909372048085114219U,
               "ジャックＸダクスター　～　旧世界の遺産", jak1_iso::Territory::scei, 338,
               1262350561338887717U, "jp", false},
      Expected{"SCPS-56003", 7280758013604870207U, "Jak & Daxter: 구세계의 유산",
               jak1_iso::Territory::scea, 338, 13924540661438229398U, "ntsc_v1", false},
  };
  const auto revisions = jak1_iso::supported_revisions();
  CHECK(revisions.size() == expected.size());
  for (size_t index = 0; index < revisions.size(); ++index) {
    CHECK(revisions[index].serial == expected[index].serial);
    CHECK(revisions[index].elf_hash == expected[index].elf_hash);
    CHECK(revisions[index].canonical_name == expected[index].name);
    CHECK(revisions[index].territory == expected[index].territory);
    CHECK(revisions[index].file_count == expected[index].files);
    CHECK(revisions[index].contents_hash == expected[index].contents_hash);
    CHECK(revisions[index].decomp_config_version == expected[index].config);
    CHECK(revisions[index].black_label == expected[index].black_label);
  }
  CHECK(&jak1_iso::default_revision() == &revisions.front());
  return true;
}

bool fingerprint_matching_fails_closed() {
  for (const auto& revision : jak1_iso::supported_revisions()) {
    const jak1_iso::Fingerprint exact{std::string(revision.serial), revision.elf_hash,
                                      revision.contents_hash, revision.file_count};
    const auto matched = jak1_iso::match_supported_revision(exact);
    CHECK(matched);
    CHECK(matched.value().revision.serial == revision.serial);
    CHECK(matched.value().revision.elf_hash == revision.elf_hash);
  }

  const auto& revision = jak1_iso::default_revision();
  jak1_iso::Fingerprint fingerprint{std::string(revision.serial), revision.elf_hash,
                                    revision.contents_hash, revision.file_count};
  fingerprint.serial = "ABCD-12345";
  auto result = jak1_iso::match_supported_revision(fingerprint);
  CHECK(!result);
  CHECK(result.error().code == jak1_iso::ValidationErrorCode::unsupported_serial);

  fingerprint.serial = std::string(revision.serial);
  fingerprint.elf_hash ^= 1;
  result = jak1_iso::match_supported_revision(fingerprint);
  CHECK(!result);
  CHECK(result.error().code == jak1_iso::ValidationErrorCode::unsupported_executable);

  fingerprint.elf_hash = revision.elf_hash;
  ++fingerprint.file_count;
  result = jak1_iso::match_supported_revision(fingerprint);
  CHECK(!result);
  CHECK(result.error().code == jak1_iso::ValidationErrorCode::unexpected_file_count);

  fingerprint.file_count = revision.file_count;
  fingerprint.contents_hash ^= 1;
  result = jak1_iso::match_supported_revision(fingerprint);
  CHECK(!result);
  CHECK(result.error().code == jak1_iso::ValidationErrorCode::contents_hash_mismatch);
  return true;
}

bool aggregate_matches_desktop_algorithm() {
  const std::array<uint64_t, 4> hashes = {862502553176757176U, 2794345569481354659U,
                                          0x123456789abcdef0U, 0xfedcba9876543210U};
  CHECK(jak1_iso::aggregate_contents_hash(hashes) == legacy_aggregate(hashes));
  auto reordered = hashes;
  std::reverse(reordered.begin(), reordered.end());
  CHECK(jak1_iso::aggregate_contents_hash(hashes) ==
        jak1_iso::aggregate_contents_hash(reordered));
  CHECK(jak1_iso::aggregate_contents_hash({}) == legacy_aggregate({}));
  return true;
}

IsoFile base_layout() {
  IsoFile layout;
  IsoFile::Entry dgo;
  dgo.is_dir = true;
  dgo.name = "DGO";
  layout.root.children.push_back(std::move(dgo));
  IsoFile::Entry executable;
  executable.name = "SCUS_971.24";
  executable.size = 1;
  layout.root.children.push_back(std::move(executable));
  layout.shouldHash = true;
  layout.files_extracted = 1;
  layout.hashes = {jak1_iso::default_revision().elf_hash};
  return layout;
}

bool layout_validation_uses_reader_dfs_hash_order() {
  auto layout = base_layout();
  IsoFile::Entry nested;
  nested.name = "FIRST.BIN";
  nested.size = 1;
  layout.root.children.front().children.push_back(std::move(nested));
  layout.files_extracted = 2;
  layout.hashes = {123U, jak1_iso::default_revision().elf_hash};

  const auto result = jak1_iso::validate_extracted_layout(layout);
  CHECK(!result);
  CHECK(result.error().code == jak1_iso::ValidationErrorCode::unexpected_file_count);
  return true;
}

bool layout_structure_and_hash_failures_are_typed() {
  auto missing_dgo = base_layout();
  missing_dgo.root.children.erase(missing_dgo.root.children.begin());
  auto result = jak1_iso::validate_extracted_layout(missing_dgo);
  CHECK(!result);
  CHECK(result.error().code == jak1_iso::ValidationErrorCode::missing_dgo_directory);

  auto missing_executable = base_layout();
  missing_executable.root.children.pop_back();
  missing_executable.files_extracted = 0;
  missing_executable.hashes.clear();
  result = jak1_iso::validate_extracted_layout(missing_executable);
  CHECK(!result);
  CHECK(result.error().code == jak1_iso::ValidationErrorCode::missing_executable);

  auto ambiguous = base_layout();
  auto second = ambiguous.root.children.back();
  second.name = "SCES_503.61";
  ambiguous.root.children.push_back(std::move(second));
  ambiguous.files_extracted = 2;
  ambiguous.hashes.push_back(12150718117852276522U);
  result = jak1_iso::validate_extracted_layout(ambiguous);
  CHECK(!result);
  CHECK(result.error().code == jak1_iso::ValidationErrorCode::ambiguous_executable);

  auto missing_hash = base_layout();
  missing_hash.hashes.clear();
  result = jak1_iso::validate_extracted_layout(missing_hash);
  CHECK(!result);
  CHECK(result.error().code == jak1_iso::ValidationErrorCode::invalid_extraction_result);
  return true;
}

bool unsupported_synthetic_disc_is_removed() {
  TemporaryDirectory temp;
  const auto image_path = temp.path / "unsupported.iso";
  const auto staging = temp.path / "staging";
  CHECK(write_bytes(image_path, make_iso({{"ABCD_123.45;1", {'t', 'e', 's', 't'}}})));
  OpenFile image(image_path);
  CHECK(image.file);

  const auto result = jak1_iso::extract_and_validate(image.file, staging);
  CHECK(!result);
  CHECK(result.error().code == jak1_iso::ValidationErrorCode::unsupported_serial);
  CHECK(!result.error().reader_error.has_value());
  CHECK(!std::filesystem::exists(staging));
  return true;
}

bool cancellation_and_reader_failures_preserve_causes() {
  TemporaryDirectory temp;
  const auto valid_path = temp.path / "valid.iso";
  CHECK(write_bytes(valid_path, make_iso({{"ABCD_123.45;1", {'x'}}})));

  OpenFile cancelled_image(valid_path);
  CHECK(cancelled_image.file);
  iso_file::Options options;
  options.should_cancel = [] { return true; };
  const auto cancelled =
      jak1_iso::extract_and_validate(cancelled_image.file, temp.path / "cancelled", options);
  CHECK(!cancelled);
  CHECK(cancelled.error().code == jak1_iso::ValidationErrorCode::cancelled);
  CHECK(cancelled.error().reader_error.has_value());
  CHECK(cancelled.error().reader_error->code == iso_file::ErrorCode::cancelled);
  CHECK(!std::filesystem::exists(temp.path / "cancelled"));

  const auto malformed_path = temp.path / "malformed.iso";
  CHECK(write_bytes(malformed_path, std::vector<uint8_t>(kSectorSize, 0)));
  OpenFile malformed_image(malformed_path);
  CHECK(malformed_image.file);
  const auto malformed =
      jak1_iso::extract_and_validate(malformed_image.file, temp.path / "malformed");
  CHECK(!malformed);
  CHECK(malformed.error().code == jak1_iso::ValidationErrorCode::iso_reader_failed);
  CHECK(malformed.error().reader_error.has_value());
  CHECK(!std::filesystem::exists(temp.path / "malformed"));
  return true;
}

bool preexisting_staging_directory_is_never_deleted() {
  TemporaryDirectory temp;
  const auto image_path = temp.path / "valid.iso";
  const auto staging = temp.path / "existing";
  const auto marker = staging / "owned-by-caller.txt";
  CHECK(write_bytes(image_path, make_iso({{"ABCD_123.45;1", {'x'}}})));
  CHECK(std::filesystem::create_directory(staging));
  CHECK(write_bytes(marker, {'k', 'e', 'e', 'p'}));
  OpenFile image(image_path);
  CHECK(image.file);

  const auto result = jak1_iso::extract_and_validate(image.file, staging);
  CHECK(!result);
  CHECK(result.error().code == jak1_iso::ValidationErrorCode::iso_reader_failed);
  CHECK(result.error().reader_error.has_value());
  CHECK(result.error().reader_error->code == iso_file::ErrorCode::output_create_failed);
  CHECK(read_text(marker) == "keep");
  return true;
}

bool buildinfo_checkpoint_is_atomic_and_desktop_compatible() {
  TemporaryDirectory temp;
  const auto staging = temp.path / "staging";
  CHECK(std::filesystem::create_directory(staging));
  const auto& revision = jak1_iso::supported_revisions()[1];
  const jak1_iso::Fingerprint fingerprint{std::string(revision.serial), revision.elf_hash,
                                          revision.contents_hash, revision.file_count};
  const auto match = jak1_iso::match_supported_revision(fingerprint);
  CHECK(match);

  const auto written = jak1_iso::write_buildinfo_checkpoint(match.value(), staging);
  CHECK(written);
  const std::string expected =
      "[\n  {\n    \"elf_hash\": 744661860962747854,\n    \"serial\": \"SCUS-97124\"\n  }\n]";
  CHECK(read_text(written.value()) == expected);
  CHECK(!std::filesystem::exists(staging / ".buildinfo.json.tmp"));

  const auto duplicate = jak1_iso::write_buildinfo_checkpoint(match.value(), staging);
  CHECK(!duplicate);
  CHECK(duplicate.error().code == jak1_iso::ValidationErrorCode::checkpoint_write_failed);
  CHECK(read_text(written.value()) == expected);

  const auto missing_directory =
      jak1_iso::write_buildinfo_checkpoint(match.value(), temp.path / "missing");
  CHECK(!missing_directory);
  CHECK(missing_directory.error().code ==
        jak1_iso::ValidationErrorCode::checkpoint_write_failed);
  return true;
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, bool (*)()>> tests = {
      {"catalog_preserves_desktop_values", catalog_preserves_desktop_values},
      {"fingerprint_matching_fails_closed", fingerprint_matching_fails_closed},
      {"aggregate_matches_desktop_algorithm", aggregate_matches_desktop_algorithm},
      {"layout_validation_uses_reader_dfs_hash_order",
       layout_validation_uses_reader_dfs_hash_order},
      {"layout_structure_and_hash_failures_are_typed",
       layout_structure_and_hash_failures_are_typed},
      {"unsupported_synthetic_disc_is_removed", unsupported_synthetic_disc_is_removed},
      {"cancellation_and_reader_failures_preserve_causes",
       cancellation_and_reader_failures_preserve_causes},
      {"preexisting_staging_directory_is_never_deleted",
       preexisting_staging_directory_is_never_deleted},
      {"buildinfo_checkpoint_is_atomic_and_desktop_compatible",
       buildinfo_checkpoint_is_atomic_and_desktop_compatible},
  };

  for (const auto& [name, test] : tests) {
    if (!test()) {
      std::cerr << "FAIL: " << name << '\n';
      return 1;
    }
    std::cout << "PASS: " << name << '\n';
  }
  return 0;
}
