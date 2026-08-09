#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "decompiler/extractor/jak2_iso_validation.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace {

namespace fs = std::filesystem;

constexpr size_t kSectorSize = 2048;

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": " #condition << '\n'; \
      return false;                                                                              \
    }                                                                                            \
  } while (false)

void write_le16(uint8_t* output, uint16_t value) {
  output[0] = value & 0xff;
  output[1] = value >> 8;
}

void write_be16(uint8_t* output, uint16_t value) {
  output[0] = value >> 8;
  output[1] = value & 0xff;
}

void write_both16(uint8_t* output, uint16_t value) {
  write_le16(output, value);
  write_be16(output + 2, value);
}

void write_le32(uint8_t* output, uint32_t value) {
  output[0] = value & 0xff;
  output[1] = (value >> 8) & 0xff;
  output[2] = (value >> 16) & 0xff;
  output[3] = value >> 24;
}

void write_be32(uint8_t* output, uint32_t value) {
  output[0] = value >> 24;
  output[1] = (value >> 16) & 0xff;
  output[2] = (value >> 8) & 0xff;
  output[3] = value & 0xff;
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
                    std::span<const uint8_t> identifier) {
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

std::vector<uint8_t> make_progress_iso() {
  constexpr uint32_t kSectors = 24;
  constexpr uint32_t kRootSector = 20;
  constexpr uint32_t kFileSector = 21;
  constexpr uint32_t kFileSize = 256;
  std::vector<uint8_t> image(size_t(kSectors) * kSectorSize);

  auto* primary = image.data() + 16 * kSectorSize;
  primary[0] = 1;
  std::memcpy(primary + 1, "CD001", 5);
  primary[6] = 1;
  write_both32(primary + 80, kSectors);
  write_both16(primary + 128, kSectorSize);
  const std::array<uint8_t, 1> dot = {0};
  const std::array<uint8_t, 1> dot_dot = {1};
  write_record(&image, 16 * kSectorSize + 156, kRootSector, kSectorSize, true, dot);

  auto* terminator = image.data() + 17 * kSectorSize;
  terminator[0] = 255;
  std::memcpy(terminator + 1, "CD001", 5);
  terminator[6] = 1;

  size_t root_offset = kRootSector * kSectorSize;
  root_offset += write_record(&image, root_offset, kRootSector, kSectorSize, true, dot);
  root_offset += write_record(&image, root_offset, kRootSector, kSectorSize, true, dot_dot);
  const std::string filename = "PAYLOAD.BIN;1";
  const std::vector<uint8_t> identifier(filename.begin(), filename.end());
  write_record(&image, root_offset, kFileSector, kFileSize, false, identifier);
  for (uint32_t index = 0; index < kFileSize; ++index) {
    image[kFileSector * kSectorSize + index] = static_cast<uint8_t>(index);
  }
  return image;
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() / ("opengoal-jak2-validation-test-" + std::to_string(stamp));
    fs::create_directory(path);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path, error);
  }

  fs::path path;
};

class OpenFile {
 public:
  explicit OpenFile(const fs::path& path) : file(std::fopen(path.string().c_str(), "rb")) {}
  ~OpenFile() {
    if (file) {
      std::fclose(file);
    }
  }

  FILE* file = nullptr;
};

void write_bytes(const fs::path& path, std::string_view bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_bytes(const fs::path& path, std::span<const uint8_t> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

std::string read_text(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

uint64_t desktop_aggregate(std::span<const uint64_t> hashes) {
  uint64_t combined = 0;
  for (const auto hash : hashes) {
    combined ^= hash;
  }
  return XXH64(&combined, sizeof(uint64_t), 0);
}

std::optional<uint64_t> hash_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  XXH64_state_t state;
  if (XXH64_reset(&state, 0) == XXH_ERROR) {
    return std::nullopt;
  }
  std::array<char, 256 * 1024> bytes{};
  while (input) {
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto count = input.gcount();
    if (count > 0 &&
        XXH64_update(&state, bytes.data(), static_cast<std::size_t>(count)) == XXH_ERROR) {
      return std::nullopt;
    }
  }
  if (!input.eof()) {
    return std::nullopt;
  }
  return XXH64_digest(&state);
}

std::optional<std::string> serial_from_executable(const std::string& name) {
  if (name.size() != 11 || name[4] != '_' || name[8] != '.') {
    return std::nullopt;
  }
  return name.substr(0, 4) + "-" + name.substr(5, 3) + name.substr(9, 2);
}

bool catalog_preserves_desktop_values() {
  struct Expected {
    std::string_view serial;
    uint64_t elf_hash;
    std::string_view name;
    jak2_iso::Territory territory;
    uint32_t files;
    uint64_t contents_hash;
    std::string_view config;
  };
  constexpr std::array expected = {
      Expected{"SCUS-97265", 18445016742498932084U, "Jak II (NTSC-U v1)", jak2_iso::Territory::scea,
               593, 3212700152698192932U, "ntsc_v1"},
      Expected{"SCUS-97265", 18445016742498932084U, "Jak II (NTSC-U v2)", jak2_iso::Territory::scea,
               593, 18208811100399420450U, "ntsc_v1"},
      Expected{"SCES-51608", 18188891052467821088U, "Jak II: Renegade", jak2_iso::Territory::scee,
               593, 15637648662558474533U, "pal"},
      Expected{"SCPS-15057", 7409991384254810731U, "ジャックＸダクスター2",
               jak2_iso::Territory::scei, 593, 709902535083998969U, "jp"},
      Expected{"SCKA-20010", 8398029689314218575U, "Jak II", jak2_iso::Territory::scei, 593,
               7504500484091955379U, "ko"},
  };
  const auto revisions = jak2_iso::supported_revisions();
  CHECK(revisions.size() == expected.size());
  for (size_t index = 0; index < revisions.size(); ++index) {
    CHECK(revisions[index].serial == expected[index].serial);
    CHECK(revisions[index].elf_hash == expected[index].elf_hash);
    CHECK(revisions[index].canonical_name == expected[index].name);
    CHECK(revisions[index].territory == expected[index].territory);
    CHECK(revisions[index].file_count == expected[index].files);
    CHECK(revisions[index].contents_hash == expected[index].contents_hash);
    CHECK(revisions[index].decomp_config_version == expected[index].config);
  }
  CHECK(&jak2_iso::default_revision() == &revisions.front());
  CHECK(&jak2_iso::import_revision() == &revisions[1]);
  CHECK(jak2_iso::import_revision().serial == "SCUS-97265");
  CHECK(jak2_iso::import_revision().elf_hash == 18445016742498932084U);
  CHECK(jak2_iso::import_revision().contents_hash == 18208811100399420450U);
  CHECK(jak2_iso::import_revision().file_count == 593);
  CHECK(jak2_iso::import_revision().decomp_config_version == "ntsc_v1");
  return true;
}

bool fingerprint_matching_fails_closed() {
  for (const auto& revision : jak2_iso::supported_revisions()) {
    const jak2_iso::Fingerprint exact{std::string(revision.serial), revision.elf_hash,
                                      revision.contents_hash, revision.file_count};
    const auto matched = jak2_iso::match_supported_revision(exact);
    CHECK(matched);
    CHECK(matched.value().revision.contents_hash == revision.contents_hash);
  }

  const auto& revision = jak2_iso::default_revision();
  jak2_iso::Fingerprint fingerprint{std::string(revision.serial), revision.elf_hash,
                                    revision.contents_hash, revision.file_count};
  fingerprint.serial = "ABCD-12345";
  auto result = jak2_iso::match_supported_revision(fingerprint);
  CHECK(!result);
  CHECK(result.error().code == jak2_iso::ValidationErrorCode::unsupported_serial);

  fingerprint.serial = std::string(revision.serial);
  fingerprint.elf_hash ^= 1;
  result = jak2_iso::match_supported_revision(fingerprint);
  CHECK(!result);
  CHECK(result.error().code == jak2_iso::ValidationErrorCode::unsupported_executable);

  fingerprint.elf_hash = revision.elf_hash;
  ++fingerprint.file_count;
  result = jak2_iso::match_supported_revision(fingerprint);
  CHECK(!result);
  CHECK(result.error().code == jak2_iso::ValidationErrorCode::unexpected_file_count);

  fingerprint.file_count = revision.file_count;
  fingerprint.contents_hash ^= 1;
  result = jak2_iso::match_supported_revision(fingerprint);
  CHECK(!result);
  CHECK(result.error().code == jak2_iso::ValidationErrorCode::contents_hash_mismatch);
  return true;
}

bool aggregate_matches_desktop_algorithm() {
  const std::array<uint64_t, 4> hashes = {862502553176757176U, 2794345569481354659U,
                                          0x123456789abcdef0U, 0xfedcba9876543210U};
  CHECK(jak2_iso::aggregate_contents_hash(hashes) == desktop_aggregate(hashes));
  auto reordered = hashes;
  std::reverse(reordered.begin(), reordered.end());
  CHECK(jak2_iso::aggregate_contents_hash(hashes) == jak2_iso::aggregate_contents_hash(reordered));
  return true;
}

IsoFile base_layout() {
  IsoFile layout;
  IsoFile::Entry dgo;
  dgo.is_dir = true;
  dgo.name = "DGO";
  layout.root.children.push_back(std::move(dgo));
  IsoFile::Entry executable;
  executable.name = "SCUS_972.65";
  executable.size = 1;
  layout.root.children.push_back(std::move(executable));
  layout.shouldHash = true;
  layout.files_extracted = 1;
  layout.hashes = {jak2_iso::default_revision().elf_hash};
  return layout;
}

bool layout_structure_and_hash_failures_are_typed() {
  auto missing_dgo = base_layout();
  missing_dgo.root.children.erase(missing_dgo.root.children.begin());
  auto result = jak2_iso::validate_extracted_layout(missing_dgo);
  CHECK(!result);
  CHECK(result.error().code == jak2_iso::ValidationErrorCode::missing_dgo_directory);

  auto missing_executable = base_layout();
  missing_executable.root.children.pop_back();
  missing_executable.files_extracted = 0;
  missing_executable.hashes.clear();
  result = jak2_iso::validate_extracted_layout(missing_executable);
  CHECK(!result);
  CHECK(result.error().code == jak2_iso::ValidationErrorCode::missing_executable);

  auto ambiguous = base_layout();
  auto second = ambiguous.root.children.back();
  second.name = "SCES_516.08";
  ambiguous.root.children.push_back(std::move(second));
  ambiguous.files_extracted = 2;
  ambiguous.hashes.push_back(18188891052467821088U);
  result = jak2_iso::validate_extracted_layout(ambiguous);
  CHECK(!result);
  CHECK(result.error().code == jak2_iso::ValidationErrorCode::ambiguous_executable);

  auto missing_hash = base_layout();
  missing_hash.hashes.clear();
  result = jak2_iso::validate_extracted_layout(missing_hash);
  CHECK(!result);
  CHECK(result.error().code == jak2_iso::ValidationErrorCode::invalid_extraction_result);
  return true;
}

bool file_identity_mapping_matches_exact_extraction_order() {
  IsoFile layout;
  IsoFile::Entry dgo;
  dgo.is_dir = true;
  dgo.name = "DGO";
  dgo.children.push_back({false, "A.DGO", 0, 5, {}});
  IsoFile::Entry cgo;
  cgo.is_dir = true;
  cgo.name = "CGO";
  cgo.children.push_back({false, "WATER_AN.CGO", 0, 7, {}});
  layout.root.children = {dgo, cgo, {false, "ROOT.BIN", 0, 9, {}}};
  layout.shouldHash = true;
  layout.files_extracted = 3;
  layout.hashes = {11, 22, 33};

  auto result = jak2_iso::validated_file_identities(layout);
  CHECK(result);
  const std::vector<checked_file_identity::Identity> expected = {
      {"DGO/A.DGO", 5, 11}, {"CGO/WATER-AN.CGO", 7, 22}, {"ROOT.BIN", 9, 33}};
  CHECK(result.value() == expected);

  auto missing_hash = layout;
  missing_hash.hashes.pop_back();
  result = jak2_iso::validated_file_identities(missing_hash);
  CHECK(!result);
  CHECK(result.error().code == jak2_iso::ValidationErrorCode::invalid_extraction_result);

  auto duplicate = layout;
  duplicate.root.children.push_back({false, "root.bin", 0, 4, {}});
  duplicate.files_extracted = 4;
  duplicate.hashes.push_back(44);
  result = jak2_iso::validated_file_identities(duplicate);
  CHECK(!result);
  CHECK(result.error().code == jak2_iso::ValidationErrorCode::invalid_extraction_result);
  return true;
}

bool buildinfo_checkpoint_is_atomic_and_desktop_compatible() {
  TemporaryDirectory temp;
  const auto staging = temp.path / "staging";
  CHECK(fs::create_directory(staging));
  const auto& revision = jak2_iso::supported_revisions()[1];
  const jak2_iso::Fingerprint fingerprint{std::string(revision.serial), revision.elf_hash,
                                          revision.contents_hash, revision.file_count};
  const auto match = jak2_iso::match_supported_revision(fingerprint);
  CHECK(match);

  const auto written = jak2_iso::write_buildinfo_checkpoint(match.value(), staging);
  CHECK(written);
  const std::string expected =
      "[\n  {\n    \"elf_hash\": 18445016742498932084,\n    \"serial\": "
      "\"SCUS-97265\"\n  }\n]";
  CHECK(read_text(written.value()) == expected);
  CHECK(!fs::exists(staging / ".buildinfo.json.tmp"));

  const auto duplicate = jak2_iso::write_buildinfo_checkpoint(match.value(), staging);
  CHECK(!duplicate);
  CHECK(duplicate.error().code == jak2_iso::ValidationErrorCode::checkpoint_write_failed);
  CHECK(read_text(written.value()) == expected);
  return true;
}

bool reader_failures_and_cancellation_leave_no_staging() {
  TemporaryDirectory temp;
  const auto image_path = temp.path / "malformed.iso";
  write_bytes(image_path, std::string(64 * 2048, '\0'));

  OpenFile malformed_image(image_path);
  CHECK(malformed_image.file);
  const auto malformed =
      jak2_iso::extract_and_validate(malformed_image.file, temp.path / "malformed-staging");
  CHECK(!malformed);
  CHECK(malformed.error().code == jak2_iso::ValidationErrorCode::iso_reader_failed);
  CHECK(malformed.error().reader_error);
  CHECK(!fs::exists(temp.path / "malformed-staging"));

  const auto progress_image_path = temp.path / "progress.iso";
  const auto progress_image = make_progress_iso();
  write_bytes(progress_image_path, progress_image);

  OpenFile cancelled_image(progress_image_path);
  CHECK(cancelled_image.file);
  iso_file::Options options;
  options.read_chunk_bytes = 64;
  bool cancel = false;
  options.should_cancel = [&] { return cancel; };
  options.on_progress = [&](const iso_file::Progress& progress) {
    cancel = progress.bytes_completed > 0;
  };
  const auto cancelled = jak2_iso::extract_and_validate(cancelled_image.file,
                                                        temp.path / "cancelled-staging", options);
  CHECK(!cancelled);
  CHECK(cancelled.error().code == jak2_iso::ValidationErrorCode::cancelled);
  CHECK(cancelled.error().reader_error);
  CHECK(cancelled.error().reader_error->code == iso_file::ErrorCode::cancelled);
  CHECK(!fs::exists(temp.path / "cancelled-staging"));

  OpenFile callback_image(progress_image_path);
  CHECK(callback_image.file);
  options.should_cancel = {};
  options.on_progress = [](const iso_file::Progress& progress) {
    if (progress.bytes_completed > 0) {
      throw std::runtime_error("synthetic callback");
    }
  };
  const auto callback =
      jak2_iso::extract_and_validate(callback_image.file, temp.path / "callback-staging", options);
  CHECK(!callback);
  CHECK(callback.error().code == jak2_iso::ValidationErrorCode::callback_failed);
  CHECK(callback.error().reader_error);
  CHECK(callback.error().reader_error->code == iso_file::ErrorCode::cancelled);
  CHECK(!fs::exists(temp.path / "callback-staging"));

  const auto existing = temp.path / "existing-staging";
  CHECK(fs::create_directory(existing));
  write_bytes(existing / "sentinel", "keep");
  OpenFile existing_image(image_path);
  const auto preexisting = jak2_iso::extract_and_validate(existing_image.file, existing);
  CHECK(!preexisting);
  CHECK(preexisting.error().reader_error);
  CHECK(preexisting.error().reader_error->code == iso_file::ErrorCode::output_create_failed);
  CHECK(read_text(existing / "sentinel") == "keep");
  return true;
}

bool progress_path_replacement_preserves_external_directory() {
  TemporaryDirectory temp;
  const auto image_path = temp.path / "progress.iso";
  const auto image = make_progress_iso();
  write_bytes(image_path, image);

  const auto staging = temp.path / "staging";
  const auto moved_owned_stage = temp.path / "moved-owned-stage";
  const auto external = temp.path / "external";
  CHECK(fs::create_directories(external / "nested"));
  write_bytes(external / "PAYLOAD.BIN", "external payload");
  write_bytes(external / "nested" / "sentinel", "preserve me");

  bool moved = false;
  bool move_failed = false;
  iso_file::Options options;
  options.should_cancel = [&] { return moved; };
  options.on_progress = [&](const iso_file::Progress& progress) {
    if (!moved && progress.bytes_completed > 0) {
      std::error_code error;
      fs::rename(staging, moved_owned_stage, error);
      if (!error) {
        fs::rename(external, staging, error);
      }
      move_failed = bool(error);
      moved = !move_failed;
    }
  };

  OpenFile input(image_path);
  CHECK(input.file);
  const auto result = jak2_iso::extract_and_validate(input.file, staging, options);
  CHECK(moved);
  CHECK(!move_failed);
  CHECK(!result);
  CHECK(result.error().code == jak2_iso::ValidationErrorCode::cancelled);
  CHECK(result.error().cleanup_error);
  CHECK(read_text(staging / "PAYLOAD.BIN") == "external payload");
  CHECK(read_text(staging / "nested" / "sentinel") == "preserve me");
  CHECK(fs::is_directory(moved_owned_stage));
  CHECK(fs::is_empty(moved_owned_stage));
  return true;
}

bool optionally_matches_extracted_retail_oracle() {
  const auto* configured_root = std::getenv("OPENGOAL_JAK2_EXTRACTED_ISO_ROOT");
  if (!configured_root || !*configured_root) {
    return true;
  }
  const auto root = fs::canonical(configured_root);
  uint64_t combined_hash = 0;
  uint32_t file_count = 0;
  std::optional<std::string> serial;
  std::optional<uint64_t> elf_hash;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    std::error_code error;
    if (entry.symlink_status(error).type() != fs::file_type::regular || error ||
        entry.path().filename() == "buildinfo.json") {
      continue;
    }
    const auto hash = hash_file(entry.path());
    CHECK(hash);
    combined_hash ^= *hash;
    ++file_count;
    if (entry.path().parent_path() == root) {
      if (const auto candidate = serial_from_executable(entry.path().filename().string())) {
        CHECK(!serial);
        serial = candidate;
        elf_hash = hash;
      }
    }
  }
  CHECK(serial);
  CHECK(elf_hash);
  const jak2_iso::Fingerprint fingerprint{*serial, *elf_hash,
                                          XXH64(&combined_hash, sizeof(uint64_t), 0), file_count};
  const auto result = jak2_iso::match_supported_revision(fingerprint);
  CHECK(result);
  CHECK(result.value().revision.contents_hash == 18208811100399420450U);
  CHECK(result.value().revision.canonical_name == "Jak II (NTSC-U v2)");
  return true;
}

}  // namespace

int main() {
  const std::array tests = {
      catalog_preserves_desktop_values,
      fingerprint_matching_fails_closed,
      aggregate_matches_desktop_algorithm,
      layout_structure_and_hash_failures_are_typed,
      file_identity_mapping_matches_exact_extraction_order,
      buildinfo_checkpoint_is_atomic_and_desktop_compatible,
      reader_failures_and_cancellation_leave_no_staging,
      progress_path_replacement_preserves_external_directory,
      optionally_matches_extracted_retail_oracle,
  };
  for (const auto test : tests) {
    if (!test()) {
      return 1;
    }
  }
  std::cout << "Jak II ISO validation tests passed\n";
  return 0;
}
