#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/custom_data/Jak1OutputMaterializer.h"

#include "decompiler/extractor/jak1_checked_dgo.h"
#include "decompiler/extractor/jak1_checked_dgo_writer.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace {

namespace fs = std::filesystem;
using namespace jak1_output_materializer;

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": " #condition << '\n'; \
      return false;                                                                              \
    }                                                                                            \
  } while (false)

void write_u32(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value) {
  (*bytes)[offset] = static_cast<std::uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
  (*bytes)[offset + 2] = static_cast<std::uint8_t>(value >> 16);
  (*bytes)[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

std::vector<std::uint8_t> make_v4_object(std::uint8_t seed) {
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

std::uint64_t hash_of(std::span<const std::uint8_t> bytes) {
  return XXH64(bytes.data(), bytes.size(), 0);
}

bool write_bytes(const fs::path& path, std::span<const std::uint8_t> bytes) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.flush();
  return static_cast<bool>(output);
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct TempDirectory {
  TempDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      path = fs::temp_directory_path() / ("jak1-output-materializer-test-" + std::to_string(nonce) +
                                          "-" + std::to_string(attempt));
      std::error_code error;
      if (fs::create_directory(path, error)) {
        return;
      }
    }
    throw std::runtime_error("could not create materializer test directory");
  }

  ~TempDirectory() {
    std::error_code error;
    fs::remove_all(path, error);
  }

  fs::path path;
};

jak1_output_recipe::RevisionProvenance provenance_of(const jak1_iso::Revision& revision) {
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

struct Fixture {
  TempDirectory temp;
  fs::path source_root = temp.path / "source-pack";
  fs::path iso_root = temp.path / "extracted";
  fs::path generated_root = temp.path / "generated";
  fs::path fr3_root = temp.path / "prepared-fr3";
  fs::path recipe_file = temp.path / "recipe.bin";
  fs::path destination = temp.path / "installed";
  std::vector<std::uint8_t> bundled = {1, 2, 3, 4};
  std::vector<std::uint8_t> retail = make_v4_object(7);
  std::vector<std::uint8_t> generated = {9, 8, 7, 6, 5};
  std::vector<std::uint8_t> flat = {0x10, 0x20, 0x30};
  std::vector<std::uint8_t> generated_flat = {0x40, 0x50};
  std::vector<std::uint8_t> fr3 = {0x60, 0x70, 0x80};
  jak1_output_recipe::SourceObjectPackIdentity source_pack = {1, 0x123456789abcdef0ULL};
  jak1_output_recipe::Recipe recipe;
  Inputs inputs;
  Options options;

  bool setup() {
    const auto revision = provenance_of(jak1_iso::default_revision());
    recipe.revision = revision;
    recipe.source_object_pack = source_pack;
    recipe.archives = {{
        "OUT.DGO",
        {
            {"bundled", jak1_output_recipe::BundledSourceObject{"objects/bundled.o", bundled.size(),
                                                                hash_of(bundled)}},
            {"retail", jak1_output_recipe::VerifiedRetailObject{"DGO/RETAIL.DGO", 0, 4,
                                                                retail.size(), hash_of(retail)}},
            {"tpage-dir",
             jak1_output_recipe::GeneratedData{
                 jak1_output_recipe::GeneratedDataKind::directory_tpages}},
        },
    }};
    recipe.flat_file_copies = {{"DATA.BIN", "DATA.BIN"}};
    recipe.generated_flat_files = {
        {jak1_output_recipe::GeneratedFlatFileKind::game_text, "0COMMON.TXT"}};
    recipe.expected_fr3_basenames = {"level.fr3"};

    jak1_output_recipe::Options recipe_options;
    recipe_options.expected_revision = revision;
    recipe_options.expected_source_object_pack = source_pack;
    auto encoded = jak1_output_recipe::encode(recipe, recipe_options);
    if (!encoded || !write_bytes(recipe_file, encoded.value()) ||
        !write_bytes(source_root / "objects/bundled.o", bundled) ||
        !write_bytes(iso_root / "DATA.BIN", flat) ||
        !write_bytes(generated_root / "objects/tpage-dir.o", generated) ||
        !write_bytes(generated_root / "flat/0COMMON.TXT", generated_flat) ||
        !write_bytes(fr3_root / "level.fr3", fr3)) {
      return false;
    }

    const std::array<jak1_checked_dgo_writer::ObjectRecord, 1> retail_objects = {
        jak1_checked_dgo_writer::ObjectRecord{"retail", retail},
    };
    auto retail_dgo = jak1_checked_dgo_writer::build("RETAIL.DGO", retail_objects);
    if (!retail_dgo || !write_bytes(iso_root / "DGO/RETAIL.DGO", retail_dgo.value())) {
      return false;
    }

    inputs.recipe_file = recipe_file;
    inputs.source_object_pack_root = source_root;
    inputs.extracted_iso_root = iso_root;
    inputs.generated_artifact_root = generated_root;
    inputs.prepared_fr3_root = fr3_root;
    inputs.generated_objects = {{
        jak1_output_recipe::GeneratedDataKind::directory_tpages,
        "tpage-dir",
        "objects/tpage-dir.o",
        generated.size(),
        hash_of(generated),
    }};
    inputs.generated_flat_files = {{
        jak1_output_recipe::GeneratedFlatFileKind::game_text,
        "0COMMON.TXT",
        "flat/0COMMON.TXT",
        generated_flat.size(),
        hash_of(generated_flat),
    }};
    options.expected_revision = revision;
    options.expected_source_object_pack = source_pack;
    return true;
  }

  bool rewrite_recipe() {
    jak1_output_recipe::Options recipe_options;
    recipe_options.expected_revision = recipe.revision;
    recipe_options.expected_source_object_pack = source_pack;
    auto encoded = jak1_output_recipe::encode(recipe, recipe_options);
    return encoded && write_bytes(recipe_file, encoded.value());
  }

  bool stage_absent() const { return !fs::exists(fs::path(destination.string() + ".stage")); }
};

bool materializes_checked_desktop_layout() {
  Fixture fixture;
  CHECK(fixture.setup());
  CHECK(!fixture.options.compressed_trailing_alignment_bytes);
  std::vector<Progress> progress;
  fixture.options.on_progress = [&](const Progress& update) { progress.push_back(update); };
  const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
  CHECK(result);
  CHECK(result.value().archives_written == 1);
  CHECK(result.value().objects_written == 3);
  CHECK(result.value().flat_files_written == 2);
  CHECK(result.value().fr3_files_written == 1);
  CHECK(result.value().output_bytes > 0);
  CHECK(fixture.stage_absent());
  CHECK(read_bytes(fixture.destination / "iso/DATA.BIN") == fixture.flat);
  CHECK(read_bytes(fixture.destination / "iso/0COMMON.TXT") == fixture.generated_flat);
  CHECK(read_bytes(fixture.destination / "fr3/level.fr3") == fixture.fr3);

  const auto output_dgo = read_bytes(fixture.destination / "iso/OUT.DGO");
  const auto decoded = jak1_checked_dgo::read(output_dgo, "OUT.DGO");
  CHECK(decoded);
  CHECK(decoded.value().objects.size() == 3);
  CHECK(decoded.value().objects[0].internal_name == "bundled");
  CHECK(decoded.value().objects[0].data == fixture.bundled);
  CHECK(decoded.value().objects[1].internal_name == "retail");
  CHECK(decoded.value().objects[1].data == fixture.retail);
  CHECK(decoded.value().objects[2].internal_name == "tpage-dir");
  CHECK(decoded.value().objects[2].data == fixture.generated);
  CHECK(!progress.empty());
  CHECK(progress.front().phase == Phase::validating);
  CHECK(progress.back().phase == Phase::installing);
  return true;
}

bool rejects_mismatched_checked_inputs_and_cleans_stage() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.bundled[0] ^= 1;
    CHECK(write_bytes(fixture.source_root / "objects/bundled.o", fixture.bundled));
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::source_object_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    auto changed = make_v4_object(19);
    const std::array<jak1_checked_dgo_writer::ObjectRecord, 1> objects = {
        jak1_checked_dgo_writer::ObjectRecord{"retail", changed},
    };
    const auto dgo = jak1_checked_dgo_writer::build("RETAIL.DGO", objects);
    CHECK(dgo);
    CHECK(write_bytes(fixture.iso_root / "DGO/RETAIL.DGO", dgo.value()));
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::retail_object_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.generated[0] ^= 1;
    CHECK(write_bytes(fixture.generated_root / "objects/tpage-dir.o", fixture.generated));
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::generated_artifact_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

bool preserves_typed_recipe_identity_failures() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.options.expected_revision =
        provenance_of(*(jak1_iso::supported_revisions().begin() + 1));
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::revision_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.options.expected_source_object_pack.aggregate_xxh64 ^= 1;
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::source_pack_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

bool rejects_inexact_generated_and_fr3_catalogs() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.inputs.generated_objects.push_back({
        jak1_output_recipe::GeneratedDataKind::custom_actor,
        "test-actor",
        "objects/test-actor-ag.go",
        1,
        1,
    });
    fixture.inputs.generated_objects.push_back({
        jak1_output_recipe::GeneratedDataKind::custom_level,
        "test-zone",
        "objects/test-zone.go",
        1,
        1,
    });
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::generated_catalog_invalid);
    CHECK(result.error().message.find("unreferenced") != std::string::npos);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    const std::array<std::uint8_t, 1> extra = {1};
    CHECK(write_bytes(fixture.fr3_root / "extra.fr3", extra));
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::fr3_set_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

bool preserves_existing_destination_and_output_budget() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    const std::array<std::uint8_t, 2> sentinel = {0xaa, 0xbb};
    CHECK(write_bytes(fixture.destination / "sentinel", sentinel));
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::destination_exists);
    CHECK(read_bytes(fixture.destination / "sentinel") ==
          std::vector<std::uint8_t>(sentinel.begin(), sentinel.end()));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.options.limits.max_total_output_bytes = 128;
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::output_limit_exceeded);
    CHECK(result.error().message.find("checked DGO writer rejected") != std::string::npos);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

bool cancellation_and_callback_failure_are_atomic() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    bool cancel = false;
    fixture.options.on_progress = [&](const Progress& update) {
      if (update.phase == Phase::writing_archives) {
        cancel = true;
      }
    };
    fixture.options.should_cancel = [&] { return cancel; };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::cancelled);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.options.on_progress = [](const Progress&) { throw std::runtime_error("test"); };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::callback_failed);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

bool rejects_symlinked_input() {
  Fixture fixture;
  CHECK(fixture.setup());
  std::error_code error;
  fs::remove(fixture.source_root / "objects/bundled.o", error);
  CHECK(!error);
  fs::create_symlink(fixture.iso_root / "DATA.BIN", fixture.source_root / "objects/bundled.o",
                     error);
  if (error) {
    std::cout << "symlink test skipped: " << error.message() << '\n';
    return true;
  }
  const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::unsafe_path);
  CHECK(!fs::exists(fixture.destination));
  CHECK(fixture.stage_absent());
  return true;
}

bool rejects_dangling_destination_symlink() {
  Fixture fixture;
  CHECK(fixture.setup());
  std::error_code error;
  fs::create_symlink(fixture.temp.path / "missing", fixture.destination, error);
  if (error) {
    std::cout << "destination symlink test skipped: " << error.message() << '\n';
    return true;
  }
  const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::destination_exists);
  CHECK(fs::symlink_status(fixture.destination).type() == fs::file_type::symlink);
  CHECK(fixture.stage_absent());
  return true;
}

}  // namespace

int main() {
  const std::array tests = {
      materializes_checked_desktop_layout,
      rejects_mismatched_checked_inputs_and_cleans_stage,
      preserves_typed_recipe_identity_failures,
      rejects_inexact_generated_and_fr3_catalogs,
      preserves_existing_destination_and_output_budget,
      cancellation_and_callback_failure_are_atomic,
      rejects_symlinked_input,
      rejects_dangling_destination_symlink,
  };
  for (const auto test : tests) {
    if (!test()) {
      return 1;
    }
  }
  std::cout << "Jak 1 output materializer tests passed\n";
  return 0;
}
