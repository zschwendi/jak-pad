#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/custom_data/Jak2SourceObjectPack.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace {

namespace fs = std::filesystem;
namespace pack = jak2_source_object_pack;

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

std::string indexed_name(std::size_t index) {
  char name[32]{};
  std::snprintf(name, sizeof(name), "source-%03zu", index);
  return name;
}

std::string source_for_index(std::size_t index) {
  constexpr std::array shared_sources = {
      "goal_src/jak1/pc/pckernel-h.gc",
      "goal_src/jak1/pc/debug/pc-debug-common.gc",
      "goal_src/jak1/pc/pckernel-common.gc",
  };
  if (index < shared_sources.size()) {
    return shared_sources[index];
  }
  return "goal_src/jak2/synthetic/" + indexed_name(index) + ".gc";
}

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
         "\nAGGREGATE_XXH64\t" + aggregate_text +
         "\nSOURCE\tTAG\tFILE\tBYTES\tXXH64\n" + row_text;
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
    const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int suffix = 0; suffix < 100; ++suffix) {
      root = fs::temp_directory_path() / ("opengoal-jak2-source-pack-test-" +
                                          std::to_string(seed) + "-" + std::to_string(suffix));
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
    expected_sources.reserve(pack::kExpectedObjectCount);
    for (std::size_t index = 0; index < pack::kExpectedObjectCount; ++index) {
      const auto source = source_for_index(index);
      const auto tag = fs::path(source).stem().string();
      std::string bytes = "synthetic-jak2-object-" + std::to_string(index);
      bytes.push_back('\0');
      const auto hash = XXH64(bytes.data(), bytes.size(), 0);
      rows.push_back({source, tag, tag + ".o", std::move(bytes), hash});
      expected_sources.push_back(source);
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
  std::vector<std::string> expected_sources;
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
  const auto result = pack::validate(fixture.root, fixture.expected_sources, options);
  CHECK(result);
  CHECK(result.value().identity == fixture.expected.identity);
  CHECK(result.value().total_object_bytes == fixture.expected.total_object_bytes);
  CHECK(last_completed == pack::kExpectedObjectCount);
  CHECK(last_bytes == fixture.expected.total_object_bytes);

  options.expected_identity->aggregate_xxh64 ^= 1;
  const auto wrong_identity = pack::validate(fixture.root, fixture.expected_sources, options);
  CHECK(!wrong_identity);
  CHECK(wrong_identity.error().code == pack::ErrorCode::wrong_identity);
  return true;
}

bool recorded_bundle_identity_is_pinned_and_fails_closed() {
  Fixture fixture;
  CHECK(pack::kRecordedAggregateXXH64 == 0x97a122324eb71fb3ULL);
  auto result = pack::validate_recorded(fixture.root);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::wrong_identity);

  pack::Options options;
  options.expected_identity = fixture.expected.identity;
  result = pack::validate_recorded(fixture.root, options);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::invalid_argument);
  return true;
}

bool optionally_validates_recorded_bundle_oracle() {
  const auto* root = std::getenv("OPENGOAL_JAK2_RECORDED_SOURCE_PACK");
  if (!root || !*root) {
    return true;
  }
  const auto result = pack::validate_recorded(fs::absolute(root));
  CHECK(result);
  CHECK(result.value().identity.object_count == pack::kExpectedObjectCount);
  CHECK(result.value().identity.aggregate_xxh64 == pack::kRecordedAggregateXXH64);
  return true;
}

bool rejects_wrong_count_and_source_graph() {
  Fixture fixture;
  auto malformed = fixture.manifest;
  const auto count = malformed.find("COUNT\t840");
  CHECK(count != std::string::npos);
  malformed.replace(count, std::string("COUNT\t840").size(), "COUNT\t839");
  write_file(fixture.root / pack::kManifestName, malformed);
  auto result = pack::validate(fixture.root, fixture.expected_sources);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::manifest_invalid);

  fixture.write_manifest();
  std::swap(fixture.expected_sources[0], fixture.expected_sources[1]);
  result = pack::validate(fixture.root, fixture.expected_sources);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::source_graph_mismatch);

  fixture.expected_sources.pop_back();
  result = pack::validate(fixture.root, fixture.expected_sources);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::invalid_argument);

  fixture.expected_sources = {};
  fixture.expected_sources.reserve(fixture.rows.size());
  for (const auto& row : fixture.rows) {
    fixture.expected_sources.push_back(row.source);
  }
  fixture.expected_sources[0] = "goal_src/jak1/pc/not-part-of-jak2.gc";
  result = pack::validate(fixture.root, fixture.expected_sources);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::invalid_argument);
  return true;
}

bool rejects_directory_and_object_mismatches() {
  Fixture fixture;
  write_file(fixture.root / "unexpected.o", "unexpected");
  auto result = pack::validate(fixture.root, fixture.expected_sources);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::contents_mismatch);

  fs::remove(fixture.root / "unexpected.o");
  auto changed = fixture.rows[0].bytes;
  changed[0] ^= 1;
  write_file(fixture.root / fixture.rows[0].file, changed);
  result = pack::validate(fixture.root, fixture.expected_sources);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::object_hash_mismatch);
  return true;
}

bool surfaces_cancellation_and_callback_failures() {
  Fixture fixture;
  pack::Options options;
  options.should_cancel = [] { return true; };
  auto result = pack::validate(fixture.root, fixture.expected_sources, options);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::cancelled);

  options.should_cancel = []() -> bool { throw std::runtime_error("callback"); };
  result = pack::validate(fixture.root, fixture.expected_sources, options);
  CHECK(!result);
  CHECK(result.error().code == pack::ErrorCode::callback_failed);
  return true;
}

}  // namespace

int main() {
  const std::array tests = {
      valid_pack_reports_exact_identity_and_progress,
      recorded_bundle_identity_is_pinned_and_fails_closed,
      optionally_validates_recorded_bundle_oracle,
      rejects_wrong_count_and_source_graph,
      rejects_directory_and_object_mismatches,
      surfaces_cancellation_and_callback_failures,
  };
  for (const auto test : tests) {
    if (!test()) {
      return 1;
    }
  }
  std::cout << "Jak II source-object-pack tests passed\n";
  return 0;
}
