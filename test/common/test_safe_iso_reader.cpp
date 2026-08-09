#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "common/util/read_iso_file.h"

namespace {

constexpr size_t kSectorSize = 2048;

struct SyntheticIso {
  std::vector<uint8_t> bytes;
  size_t root_file_record = 0;
};

void write_le16(uint8_t* out, uint16_t value) {
  out[0] = value & 0xff;
  out[1] = value >> 8;
}

void write_be16(uint8_t* out, uint16_t value) {
  out[0] = value >> 8;
  out[1] = value & 0xff;
}

void write_both16(uint8_t* out, uint16_t value) {
  write_le16(out, value);
  write_be16(out + 2, value);
}

void write_le32(uint8_t* out, uint32_t value) {
  out[0] = value & 0xff;
  out[1] = (value >> 8) & 0xff;
  out[2] = (value >> 16) & 0xff;
  out[3] = value >> 24;
}

void write_be32(uint8_t* out, uint32_t value) {
  out[0] = value >> 24;
  out[1] = (value >> 16) & 0xff;
  out[2] = (value >> 8) & 0xff;
  out[3] = value & 0xff;
}

void write_both32(uint8_t* out, uint32_t value) {
  write_le32(out, value);
  write_be32(out + 4, value);
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

std::vector<uint8_t> identifier(const std::string& text) {
  return {text.begin(), text.end()};
}

SyntheticIso make_synthetic_iso(const std::string& root_file_name = "SAFE.TXT;1",
                                const std::string& nested_directory_name = "NEST") {
  constexpr uint32_t sectors = 28;
  constexpr uint32_t root_sector = 20;
  constexpr uint32_t main_file_sector = 21;
  constexpr uint32_t nested_directory_sector = 23;
  constexpr uint32_t nested_file_sector = 24;
  constexpr uint32_t main_file_size = 3000;
  constexpr uint32_t nested_file_size = 5;

  SyntheticIso iso;
  iso.bytes.resize(size_t(sectors) * kSectorSize);

  auto* primary = iso.bytes.data() + 16 * kSectorSize;
  primary[0] = 1;
  memcpy(primary + 1, "CD001", 5);
  primary[6] = 1;
  write_both32(primary + 80, sectors);
  write_both16(primary + 128, kSectorSize);
  write_record(&iso.bytes, 16 * kSectorSize + 156, root_sector, kSectorSize, true, {0});

  auto* terminator = iso.bytes.data() + 17 * kSectorSize;
  terminator[0] = 255;
  memcpy(terminator + 1, "CD001", 5);
  terminator[6] = 1;

  size_t root_offset = root_sector * kSectorSize;
  root_offset += write_record(&iso.bytes, root_offset, root_sector, kSectorSize, true, {0});
  root_offset += write_record(&iso.bytes, root_offset, root_sector, kSectorSize, true, {1});
  iso.root_file_record = root_offset;
  root_offset += write_record(&iso.bytes, root_offset, main_file_sector, main_file_size, false,
                              identifier(root_file_name));
  write_record(&iso.bytes, root_offset, nested_directory_sector, kSectorSize, true,
               identifier(nested_directory_name));

  size_t nested_offset = nested_directory_sector * kSectorSize;
  nested_offset +=
      write_record(&iso.bytes, nested_offset, nested_directory_sector, kSectorSize, true, {0});
  nested_offset += write_record(&iso.bytes, nested_offset, root_sector, kSectorSize, true, {1});
  write_record(&iso.bytes, nested_offset, nested_file_sector, nested_file_size, false,
               identifier("TINY.BIN;1"));

  for (uint32_t i = 0; i < main_file_size; ++i) {
    iso.bytes[main_file_sector * kSectorSize + i] = static_cast<uint8_t>(i % 251);
  }
  const std::array<uint8_t, nested_file_size> tiny = {'h', 'e', 'l', 'l', 'o'};
  std::copy(tiny.begin(), tiny.end(), iso.bytes.begin() + nested_file_sector * kSectorSize);
  return iso;
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("opengoal-safe-iso-test-" + std::to_string(stamp));
    std::filesystem::create_directory(path);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }

  std::filesystem::path path;
};

bool write_image(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return bool(output);
}

std::vector<uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

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

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": " #condition << '\n'; \
      return false;                                                                              \
    }                                                                                            \
  } while (false)

bool inspect_valid_image() {
  TemporaryDirectory temp;
  const auto fixture = make_synthetic_iso();
  const auto image = temp.path / "fixture.iso";
  CHECK(write_image(image, fixture.bytes));
  OpenFile input(image);
  CHECK(input.file);

  const auto result = iso_file::inspect(input.file);
  CHECK(result);
  CHECK(result.value().root.children.size() == 2);
  CHECK(result.value().root.children[0].name == "SAFE.TXT");
  CHECK(result.value().root.children[0].size == 3000);
  CHECK(result.value().root.children[1].is_dir);
  CHECK(result.value().root.children[1].children.size() == 1);
  CHECK(result.value().root.children[1].children[0].name == "TINY.BIN");
  return true;
}

bool extract_in_bounded_chunks_with_progress_and_hashes() {
  TemporaryDirectory temp;
  const auto fixture = make_synthetic_iso();
  const auto image = temp.path / "fixture.iso";
  const auto staging = temp.path / "staging";
  CHECK(write_image(image, fixture.bytes));
  OpenFile input(image);
  CHECK(input.file);

  iso_file::Options options;
  options.read_chunk_bytes = 257;
  options.hash_files = true;
  uint64_t last_bytes = 0;
  uint32_t progress_calls = 0;
  bool monotonic = true;
  iso_file::Progress final_progress;
  options.on_progress = [&](const iso_file::Progress& progress) {
    monotonic &= progress.bytes_completed >= last_bytes;
    last_bytes = progress.bytes_completed;
    final_progress = progress;
    progress_calls++;
  };

  const auto result = iso_file::extract_to_staging(input.file, staging, options);
  CHECK(result);
  CHECK(monotonic);
  CHECK(progress_calls > 10);
  CHECK(final_progress.bytes_completed == 3005);
  CHECK(final_progress.bytes_total == 3005);
  CHECK(final_progress.files_completed == 2);
  CHECK(final_progress.files_total == 2);
  CHECK(result.value().files_extracted == 2);
  CHECK(result.value().hashes.size() == 2);
  CHECK(result.value().hashes[0] == 862502553176757176ull);
  CHECK(result.value().hashes[1] == 2794345569481354659ull);
  CHECK(read_bytes(staging / "SAFE.TXT").size() == 3000);
  CHECK(read_bytes(staging / "NEST" / "TINY.BIN") ==
        std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}));
  return true;
}

bool water_animation_name_is_patched() {
  TemporaryDirectory temp;
  const auto fixture = make_synthetic_iso("WATER_AN.CGO;1");
  const auto image = temp.path / "fixture.iso";
  const auto staging = temp.path / "staging";
  CHECK(write_image(image, fixture.bytes));
  OpenFile input(image);
  CHECK(input.file);

  const auto result = iso_file::extract_to_staging(input.file, staging);
  CHECK(result);
  CHECK(result.value().root.children[0].name == "WATER_AN.CGO");
  CHECK(std::filesystem::exists(staging / "WATER-AN.CGO"));
  CHECK(!std::filesystem::exists(staging / "WATER_AN.CGO"));
  CHECK(read_bytes(staging / "WATER-AN.CGO").size() == 3000);
  return true;
}

bool cancellation_removes_staging() {
  TemporaryDirectory temp;
  const auto fixture = make_synthetic_iso();
  const auto image = temp.path / "fixture.iso";
  const auto staging = temp.path / "staging";
  CHECK(write_image(image, fixture.bytes));
  OpenFile input(image);
  CHECK(input.file);

  iso_file::Options options;
  options.read_chunk_bytes = 128;
  bool cancel = false;
  options.should_cancel = [&] { return cancel; };
  options.on_progress = [&](const iso_file::Progress& progress) {
    if (progress.bytes_completed > 0) {
      cancel = true;
    }
  };
  const auto result = iso_file::extract_to_staging(input.file, staging, options);
  CHECK(!result);
  CHECK(result.error().code == iso_file::ErrorCode::cancelled);
  CHECK(!std::filesystem::exists(staging));
  return true;
}

bool rejects_invalid_descriptor() {
  TemporaryDirectory temp;
  auto fixture = make_synthetic_iso();
  fixture.bytes[16 * kSectorSize + 1] = 'X';
  const auto image = temp.path / "fixture.iso";
  CHECK(write_image(image, fixture.bytes));
  OpenFile input(image);
  const auto result = iso_file::inspect(input.file);
  CHECK(!result);
  CHECK(result.error().code == iso_file::ErrorCode::invalid_descriptor);
  return true;
}

bool rejects_out_of_bounds_extent() {
  TemporaryDirectory temp;
  auto fixture = make_synthetic_iso();
  write_both32(fixture.bytes.data() + fixture.root_file_record + 2, 1000);
  const auto image = temp.path / "fixture.iso";
  CHECK(write_image(image, fixture.bytes));
  OpenFile input(image);
  const auto result = iso_file::inspect(input.file);
  CHECK(!result);
  CHECK(result.error().code == iso_file::ErrorCode::extent_out_of_bounds);
  return true;
}

bool rejects_malformed_directory_record() {
  TemporaryDirectory temp;
  auto fixture = make_synthetic_iso();
  fixture.bytes[fixture.root_file_record] = 20;
  const auto image = temp.path / "fixture.iso";
  CHECK(write_image(image, fixture.bytes));
  OpenFile input(image);
  const auto result = iso_file::inspect(input.file);
  CHECK(!result);
  CHECK(result.error().code == iso_file::ErrorCode::invalid_directory_record);
  return true;
}

bool rejects_unsafe_path() {
  TemporaryDirectory temp;
  const auto fixture = make_synthetic_iso("..;1");
  const auto image = temp.path / "fixture.iso";
  CHECK(write_image(image, fixture.bytes));
  OpenFile input(image);
  const auto result = iso_file::inspect(input.file);
  CHECK(!result);
  CHECK(result.error().code == iso_file::ErrorCode::unsafe_path);

  const auto collision = make_synthetic_iso("SAFE.TXT;1", "safe.txt");
  const auto collision_image = temp.path / "collision.iso";
  CHECK(write_image(collision_image, collision.bytes));
  OpenFile collision_input(collision_image);
  const auto collision_result = iso_file::inspect(collision_input.file);
  CHECK(!collision_result);
  CHECK(collision_result.error().code == iso_file::ErrorCode::unsafe_path);
  return true;
}

bool enforces_depth_entry_and_size_limits() {
  TemporaryDirectory temp;
  const auto fixture = make_synthetic_iso();
  const auto image = temp.path / "fixture.iso";
  CHECK(write_image(image, fixture.bytes));

  {
    OpenFile input(image);
    iso_file::Options options;
    options.max_depth = 0;
    const auto result = iso_file::inspect(input.file, options);
    CHECK(!result);
    CHECK(result.error().code == iso_file::ErrorCode::depth_limit_exceeded);
  }
  {
    OpenFile input(image);
    iso_file::Options options;
    options.max_entries = 1;
    const auto result = iso_file::inspect(input.file, options);
    CHECK(!result);
    CHECK(result.error().code == iso_file::ErrorCode::entry_limit_exceeded);
  }
  {
    OpenFile input(image);
    iso_file::Options options;
    options.max_image_bytes = fixture.bytes.size() - 1;
    const auto result = iso_file::inspect(input.file, options);
    CHECK(!result);
    CHECK(result.error().code == iso_file::ErrorCode::file_size_limit_exceeded);
  }
  {
    OpenFile input(image);
    iso_file::Options options;
    options.max_directory_bytes = kSectorSize - 1;
    const auto result = iso_file::inspect(input.file, options);
    CHECK(!result);
    CHECK(result.error().code == iso_file::ErrorCode::file_size_limit_exceeded);
  }
  {
    OpenFile input(image);
    iso_file::Options options;
    options.max_file_bytes = 1024;
    const auto result = iso_file::inspect(input.file, options);
    CHECK(!result);
    CHECK(result.error().code == iso_file::ErrorCode::file_size_limit_exceeded);
  }
  {
    OpenFile input(image);
    iso_file::Options options;
    options.max_total_output_bytes = 3001;
    const auto result = iso_file::inspect(input.file, options);
    CHECK(!result);
    CHECK(result.error().code == iso_file::ErrorCode::total_size_limit_exceeded);
  }
  {
    OpenFile input(image);
    iso_file::Options options;
    options.max_name_bytes = 4;
    const auto result = iso_file::inspect(input.file, options);
    CHECK(!result);
    CHECK(result.error().code == iso_file::ErrorCode::unsafe_path);
  }
  {
    OpenFile input(image);
    iso_file::Options options;
    options.read_chunk_bytes = 1024 * 1024 + 1;
    const auto result = iso_file::inspect(input.file, options);
    CHECK(!result);
    CHECK(result.error().code == iso_file::ErrorCode::invalid_argument);
  }
  return true;
}

bool existing_staging_is_preserved() {
  TemporaryDirectory temp;
  const auto fixture = make_synthetic_iso();
  const auto image = temp.path / "fixture.iso";
  const auto staging = temp.path / "staging";
  CHECK(write_image(image, fixture.bytes));
  std::filesystem::create_directory(staging);
  std::ofstream(staging / "keep.txt") << "keep";
  OpenFile input(image);

  const auto result = iso_file::extract_to_staging(input.file, staging);
  CHECK(!result);
  CHECK(result.error().code == iso_file::ErrorCode::output_create_failed);
  CHECK(read_bytes(staging / "keep.txt") == std::vector<uint8_t>({'k', 'e', 'e', 'p'}));
  return true;
}

#ifndef _WIN32
bool owned_staging_rejects_and_preserves_unexpected_entries() {
  TemporaryDirectory temp;
  const auto fixture = make_synthetic_iso();
  const auto image = temp.path / "fixture.iso";
  const auto staging = temp.path / "staging";
  CHECK(write_image(image, fixture.bytes));
  OpenFile input(image);
  CHECK(input.file);

  iso_file::OwnedStagingDirectory owned_staging;
  const auto result = iso_file::extract_to_owned_staging(input.file, staging, &owned_staging);
  CHECK(result);
  std::ofstream(staging / "unexpected.txt") << "preserve";
  CHECK(!owned_staging.is_linked());
  const auto cleanup_error = owned_staging.cleanup();
  CHECK(cleanup_error);
  CHECK(read_bytes(staging / "unexpected.txt") ==
        std::vector<uint8_t>({'p', 'r', 'e', 's', 'e', 'r', 'v', 'e'}));
  CHECK(!std::filesystem::exists(staging / "SAFE.TXT"));
  CHECK(!std::filesystem::exists(staging / "NEST"));
  return true;
}

bool owned_staging_rejects_in_place_progress_mutation() {
  TemporaryDirectory temp;
  const auto fixture = make_synthetic_iso();
  const auto image = temp.path / "fixture.iso";
  const auto staging = temp.path / "staging";
  const auto sibling = temp.path / "sibling.txt";
  CHECK(write_image(image, fixture.bytes));
  std::ofstream(sibling) << "outside";

  bool mutated = false;
  bool mutation_failed = false;
  iso_file::Options options;
  options.read_chunk_bytes = 128;
  options.hash_files = true;
  options.on_progress = [&](const iso_file::Progress& progress) {
    if (!mutated && progress.current_path == "SAFE.TXT" && progress.files_completed == 1) {
      std::fstream current(staging / "SAFE.TXT", std::ios::binary | std::ios::in | std::ios::out);
      current.seekp(0);
      current.put('!');
      current.close();
      mutation_failed = !current;
      if (!mutation_failed) {
        std::ofstream(staging / "unexpected.txt") << "preserve";
      }
      mutated = !mutation_failed;
    }
  };

  OpenFile input(image);
  CHECK(input.file);
  iso_file::OwnedStagingDirectory owned_staging;
  const auto result =
      iso_file::extract_to_owned_staging(input.file, staging, &owned_staging, options);
  CHECK(mutated);
  CHECK(!mutation_failed);
  CHECK(!result);
  CHECK(result.error().code == iso_file::ErrorCode::output_write_failed);
  CHECK(!std::filesystem::exists(staging / "SAFE.TXT"));
  CHECK(!std::filesystem::exists(staging / "NEST"));
  CHECK(read_bytes(staging / "unexpected.txt") ==
        std::vector<uint8_t>({'p', 'r', 'e', 's', 'e', 'r', 'v', 'e'}));
  CHECK(read_bytes(sibling) == std::vector<uint8_t>({'o', 'u', 't', 's', 'i', 'd', 'e'}));
  return true;
}

bool owned_staging_rejects_replaced_parent_path() {
  TemporaryDirectory temp;
  const auto fixture = make_synthetic_iso();
  const auto image = temp.path / "fixture.iso";
  const auto staging_parent = temp.path / "staging-parent";
  const auto moved_staging_parent = temp.path / "moved-staging-parent";
  const auto external_parent = temp.path / "external-parent";
  const auto staging = staging_parent / "staging";
  CHECK(write_image(image, fixture.bytes));
  CHECK(std::filesystem::create_directory(staging_parent));
  CHECK(std::filesystem::create_directories(external_parent / "staging" / "nested"));
  std::ofstream(external_parent / "staging" / "external.txt") << "preserve";
  std::ofstream(external_parent / "staging" / "nested" / "sentinel") << "external";

  bool replaced = false;
  bool replacement_failed = false;
  iso_file::Options options;
  options.read_chunk_bytes = 128;
  options.on_progress = [&](const iso_file::Progress& progress) {
    if (!replaced && progress.bytes_completed > 0) {
      std::error_code error;
      std::filesystem::rename(staging_parent, moved_staging_parent, error);
      if (!error) {
        std::filesystem::rename(external_parent, staging_parent, error);
      }
      replacement_failed = bool(error);
      replaced = !replacement_failed;
    }
  };

  OpenFile input(image);
  CHECK(input.file);
  iso_file::OwnedStagingDirectory owned_staging;
  const auto result =
      iso_file::extract_to_owned_staging(input.file, staging, &owned_staging, options);
  CHECK(result);
  CHECK(replaced);
  CHECK(!replacement_failed);
  CHECK(std::filesystem::is_regular_file(moved_staging_parent / "staging" / "SAFE.TXT"));
  CHECK(std::filesystem::is_directory(moved_staging_parent / "staging" / "NEST"));
  CHECK(!owned_staging.keep());
  const auto cleanup_error = owned_staging.cleanup();
  CHECK(!cleanup_error);
  CHECK(read_bytes(staging / "external.txt") ==
        std::vector<uint8_t>({'p', 'r', 'e', 's', 'e', 'r', 'v', 'e'}));
  CHECK(read_bytes(staging / "nested" / "sentinel") ==
        std::vector<uint8_t>({'e', 'x', 't', 'e', 'r', 'n', 'a', 'l'}));
  CHECK(std::filesystem::is_directory(moved_staging_parent));
  CHECK(std::filesystem::is_empty(moved_staging_parent));
  return true;
}
#endif

bool desktop_adapter_preserves_behavior_and_throws_typed_errors() {
  TemporaryDirectory temp;
  const auto fixture = make_synthetic_iso();
  const auto image = temp.path / "fixture.iso";
  const auto output = temp.path / "output";
  CHECK(write_image(image, fixture.bytes));

  {
    OpenFile input(image);
    auto layout = find_files_in_iso(input.file);
    CHECK(layout.root.children.size() == 2);
    layout.shouldHash = true;
    unpack_iso_files(input.file, layout, fs::path(output.string()));
    CHECK(layout.files_extracted == 2);
    CHECK(layout.hashes.size() == 2);
    CHECK(read_bytes(output / "NEST" / "TINY.BIN") ==
          std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}));
  }

  auto invalid = fixture;
  invalid.bytes[16 * kSectorSize + 1] = 'X';
  const auto invalid_image = temp.path / "invalid.iso";
  CHECK(write_image(invalid_image, invalid.bytes));
  OpenFile invalid_input(invalid_image);
  try {
    (void)find_files_in_iso(invalid_input.file);
    CHECK(false);
  } catch (const iso_file::Exception& error) {
    CHECK(error.error().code == iso_file::ErrorCode::invalid_descriptor);
  }
  return true;
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, bool (*)()>> tests = {
      {"inspect_valid_image", inspect_valid_image},
      {"extract_in_bounded_chunks_with_progress_and_hashes",
       extract_in_bounded_chunks_with_progress_and_hashes},
      {"water_animation_name_is_patched", water_animation_name_is_patched},
      {"cancellation_removes_staging", cancellation_removes_staging},
      {"rejects_invalid_descriptor", rejects_invalid_descriptor},
      {"rejects_out_of_bounds_extent", rejects_out_of_bounds_extent},
      {"rejects_malformed_directory_record", rejects_malformed_directory_record},
      {"rejects_unsafe_path", rejects_unsafe_path},
      {"enforces_depth_entry_and_size_limits", enforces_depth_entry_and_size_limits},
      {"existing_staging_is_preserved", existing_staging_is_preserved},
#ifndef _WIN32
      {"owned_staging_rejects_and_preserves_unexpected_entries",
       owned_staging_rejects_and_preserves_unexpected_entries},
      {"owned_staging_rejects_in_place_progress_mutation",
       owned_staging_rejects_in_place_progress_mutation},
      {"owned_staging_rejects_replaced_parent_path", owned_staging_rejects_replaced_parent_path},
#endif
      {"desktop_adapter_preserves_behavior_and_throws_typed_errors",
       desktop_adapter_preserves_behavior_and_throws_typed_errors},
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
  std::cout << "Passed " << passed << " deterministic synthetic ISO tests.\n";
  return 0;
}
