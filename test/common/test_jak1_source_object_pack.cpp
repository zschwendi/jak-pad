#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/custom_data/Jak1PublicOutputGraph.h"
#include "common/custom_data/Jak1SourceObjectPack.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace {

namespace pack = jak1_source_object_pack;
namespace fs = std::filesystem;

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": " #condition << '\n'; \
      return false;                                                                              \
    }                                                                                            \
  } while (false)

struct Row {
  std::string source;
  std::string tag;
  std::string file;
  std::string bytes;
  std::uint64_t hash = 0;
};

std::string manifest_text(const std::vector<Row>& rows, pack::Summary* expected = nullptr) {
  std::string row_text;
  std::uint64_t total = 0;
  for (const auto& row : rows) {
    char hash[17]{};
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(row.hash));
    row_text += row.source + "\t" + row.tag + "\t" + row.file + "\t" +
                std::to_string(row.bytes.size()) + "\t" + hash + "\n";
    total += row.bytes.size();
  }
  const auto aggregate = XXH64(row_text.data(), row_text.size(), 0);
  char aggregate_text[17]{};
  std::snprintf(aggregate_text, sizeof(aggregate_text), "%016llx",
                static_cast<unsigned long long>(aggregate));
  if (expected) {
    expected->identity = {static_cast<std::uint32_t>(rows.size()), aggregate};
    expected->total_object_bytes = total;
  }
  return "FORMAT\tgoalc-source-object-pack-v1\nCOUNT\t" + std::to_string(rows.size()) +
         "\nAGGREGATE_XXH64\t" + aggregate_text + "\nSOURCE\tTAG\tFILE\tBYTES\tXXH64\n" + row_text;
}

void write_file(const fs::path& path, std::string_view bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error("could not write a synthetic source-pack file");
  }
}

class Fixture {
 public:
  Fixture() {
    const auto decoded = jak1_public_output_graph::decode();
    if (!decoded || decoded.value().ordered_source_files.size() != pack::kExpectedObjectCount) {
      throw std::runtime_error("embedded public graph is unavailable");
    }
    const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int suffix = 0; suffix < 100; ++suffix) {
      root = fs::temp_directory_path() / ("opengoal-jak1-source-pack-test-" + std::to_string(seed) +
                                          "-" + std::to_string(suffix));
      std::error_code error;
      if (fs::create_directory(root, error)) {
        break;
      }
      root.clear();
    }
    if (root.empty()) {
      throw std::runtime_error("could not create a synthetic source-pack root");
    }

    rows.reserve(pack::kExpectedObjectCount);
    for (std::size_t index = 0; index < decoded.value().ordered_source_files.size(); ++index) {
      const auto& source = decoded.value().ordered_source_files[index];
      const auto tag = fs::path(source).stem().string();
      std::string bytes = "synthetic-public-object-" + std::to_string(index);
      bytes.push_back('\0');
      const auto hash = XXH64(bytes.data(), bytes.size(), 0);
      rows.push_back({source, tag, tag + ".o", std::move(bytes), hash});
      write_file(root / rows.back().file, rows.back().bytes);
    }
    write_manifest();
  }

  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  ~Fixture() {
    std::error_code error;
    fs::remove_all(root, error);
  }

  void write_manifest() {
    manifest = manifest_text(rows, &expected);
    write_file(root / pack::kManifestName, manifest);
  }

  fs::path root;
  std::vector<Row> rows;
  std::string manifest;
  pack::Summary expected;
};

bool valid_pack_reports_exact_identity_and_progress() {
  Fixture fixture;
  std::uint32_t last_completed = 0;
  std::uint64_t last_bytes = 0;
  pack::Options options;
  options.expected_identity = fixture.expected.identity;
  options.on_progress = [&](const pack::Progress& progress) {
    if (progress.phase == pack::Phase::validating_objects) {
      last_completed = progress.completed;
      last_bytes = progress.bytes_hashed;
    }
  };
  const auto result = pack::validate(fixture.root, options);
  CHECK(result);
  CHECK(result.value().identity == fixture.expected.identity);
  CHECK(result.value().total_object_bytes == fixture.expected.total_object_bytes);
  CHECK(last_completed == pack::kExpectedObjectCount);
  CHECK(last_bytes == fixture.expected.total_object_bytes);

  options.expected_identity->aggregate_xxh64 ^= 1;
  const auto wrong_identity = pack::validate(fixture.root, options);
  CHECK(!wrong_identity);
  CHECK(wrong_identity.error().code == pack::ErrorCode::wrong_identity);
  return true;
}

bool deterministic_fixture_bytes_match() {
  Fixture first;
  Fixture second;
  CHECK(first.manifest == second.manifest);
  CHECK(first.expected.identity == second.expected.identity);
  CHECK(first.rows.size() == second.rows.size());
  for (std::size_t index = 0; index < first.rows.size(); ++index) {
    CHECK(first.rows[index].source == second.rows[index].source);
    CHECK(first.rows[index].file == second.rows[index].file);
    CHECK(first.rows[index].bytes == second.rows[index].bytes);
  }
  return true;
}

bool rejects_malformed_and_truncated_manifests() {
  Fixture fixture;
  write_file(fixture.root / pack::kManifestName,
             std::string_view(fixture.manifest).substr(0, fixture.manifest.size() - 1));
  auto result = pack::validate(fixture.root);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::manifest_invalid);

  write_file(fixture.root / pack::kManifestName,
             std::string_view(fixture.manifest).substr(0, fixture.manifest.size() / 2));
  result = pack::validate(fixture.root);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::manifest_invalid);

  fixture.write_manifest();
  pack::Options options;
  options.limits.max_manifest_bytes = fixture.manifest.size() - 1;
  result = pack::validate(fixture.root, options);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::manifest_too_large);
  return true;
}

bool rejects_order_path_and_directory_mismatches() {
  Fixture fixture;
  std::swap(fixture.rows[0], fixture.rows[1]);
  fixture.write_manifest();
  auto result = pack::validate(fixture.root);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::source_graph_mismatch);

  std::swap(fixture.rows[0], fixture.rows[1]);
  fixture.rows[0].file = "../escape.o";
  fixture.write_manifest();
  result = pack::validate(fixture.root);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::manifest_invalid);

  fixture.rows[0].file = fixture.rows[0].tag + ".o";
  fixture.write_manifest();
  write_file(fixture.root / "unexpected.o", "unexpected");
  result = pack::validate(fixture.root);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::contents_mismatch);

  fs::remove(fixture.root / "unexpected.o");
  fs::remove(fixture.root / fixture.rows[0].file);
  result = pack::validate(fixture.root);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::contents_mismatch);

  write_file(fixture.root / fixture.rows[0].file, fixture.rows[0].bytes);
  fs::create_directory(fixture.root / "unexpected-directory");
  result = pack::validate(fixture.root);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::unsafe_entry);
  return true;
}

bool rejects_hash_and_size_limit_mismatches() {
  Fixture fixture;
  auto changed = fixture.rows[0].bytes;
  changed[0] ^= 1;
  write_file(fixture.root / fixture.rows[0].file, changed);
  auto result = pack::validate(fixture.root);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::object_hash_mismatch);

  changed.pop_back();
  write_file(fixture.root / fixture.rows[0].file, changed);
  result = pack::validate(fixture.root);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::object_hash_mismatch);

  write_file(fixture.root / fixture.rows[0].file, fixture.rows[0].bytes);
  pack::Options options;
  options.limits.max_object_bytes = 1;
  result = pack::validate(fixture.root, options);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::object_too_large);

  options = {};
  options.limits.max_total_object_bytes = fixture.expected.total_object_bytes - 1;
  result = pack::validate(fixture.root, options);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::object_too_large);
  return true;
}

bool rejects_invalid_roots_and_options() {
  auto result = pack::validate("relative-source-pack");
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::invalid_argument);

  Fixture fixture;
  result = pack::validate(fixture.root / "missing");
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::root_missing);

  pack::Options options;
  options.expected_identity = fixture.expected.identity;
  options.expected_identity->object_count = 1;
  result = pack::validate(fixture.root, options);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::invalid_argument);
  return true;
}

bool surfaces_cancellation_and_callback_failures() {
  Fixture fixture;
  pack::Options options;
  options.should_cancel = [] { return true; };
  auto result = pack::validate(fixture.root, options);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::cancelled);

  bool started_hashing = false;
  options.limits.io_chunk_bytes = 1;
  options.should_cancel = [&] { return started_hashing; };
  options.on_progress = [&](const pack::Progress& progress) {
    started_hashing = progress.phase == pack::Phase::validating_objects;
  };
  result = pack::validate(fixture.root, options);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::cancelled);
  CHECK(result.error().entry_index == 0);

  options.should_cancel = []() -> bool { throw std::runtime_error("callback"); };
  result = pack::validate(fixture.root, options);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::callback_failed);

  options.should_cancel = {};
  options.on_progress = [](const pack::Progress&) { throw std::runtime_error("progress"); };
  result = pack::validate(fixture.root, options);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::callback_failed);
  return true;
}

}  // namespace

int main() {
  const std::array tests = {
      valid_pack_reports_exact_identity_and_progress, deterministic_fixture_bytes_match,
      rejects_malformed_and_truncated_manifests,      rejects_order_path_and_directory_mismatches,
      rejects_hash_and_size_limit_mismatches,         rejects_invalid_roots_and_options,
      surfaces_cancellation_and_callback_failures,
  };
  for (const auto test : tests) {
    if (!test()) {
      return 1;
    }
  }
  std::cout << "Jak 1 source-object-pack tests passed\n";
  return 0;
}
