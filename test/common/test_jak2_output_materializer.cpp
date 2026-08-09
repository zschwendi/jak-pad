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

#include "common/custom_data/Jak2OutputMaterializer.h"

#include "decompiler/extractor/jak1_checked_dgo.h"
#include "decompiler/extractor/jak1_checked_dgo_writer.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace {

namespace fs = std::filesystem;
namespace materializer = jak2_output_materializer;
namespace recipe = jak2_output_recipe;

#define CHECK(condition)                                                                      \
  do {                                                                                        \
    if (!(condition)) {                                                                       \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                                      \
      return false;                                                                           \
    }                                                                                         \
  } while (false)

void write_u32(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value) {
  (*bytes)[offset] = static_cast<std::uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
  (*bytes)[offset + 2] = static_cast<std::uint8_t>(value >> 16);
  (*bytes)[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

std::vector<std::uint8_t> make_v4_object() {
  std::vector<std::uint8_t> bytes(144);
  write_u32(&bytes, 0, 0xffffffff);
  write_u32(&bytes, 4, 64);
  write_u32(&bytes, 8, 4);
  write_u32(&bytes, 12, 64);
  for (std::size_t index = 16; index < 80; ++index) {
    bytes[index] = static_cast<std::uint8_t>(0x51 + index);
  }
  const std::string marker = "/src/jak2/final/art-group7/retail-ag.go";
  std::copy(marker.begin(), marker.end(), bytes.begin() + 16);
  bytes[16 + marker.size()] = 0;
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
      path = fs::temp_directory_path() /
             ("jak2-output-materializer-test-" + std::to_string(nonce) + "-" +
              std::to_string(attempt));
      std::error_code error;
      if (fs::create_directory(path, error)) {
        return;
      }
    }
    throw std::runtime_error("could not create Jak II materializer test directory");
  }

  ~TempDirectory() {
    std::error_code error;
    fs::remove_all(path, error);
  }

  fs::path path;
};

struct Fixture {
  TempDirectory temp;
  fs::path source_root = temp.path / "source-pack";
  fs::path iso_root = temp.path / "extracted";
  fs::path generated_root = temp.path / "generated";
  fs::path fr3_root = temp.path / "prepared-fr3";
  fs::path recipe_file = temp.path / "recipe.bin";
  fs::path destination = temp.path / "installed";
  std::vector<std::vector<std::uint8_t>> bundled;
  std::vector<std::uint8_t> retail = make_v4_object();
  std::vector<std::uint8_t> flat = {0x10, 0x20, 0x30};
  std::vector<std::uint8_t> generated_flat = {0x40, 0x50};
  std::vector<std::uint8_t> fr3 = {0x60, 0x70, 0x80};
  recipe::Recipe output_recipe;
  materializer::Inputs inputs;
  materializer::Options options;

  bool setup() {
    output_recipe = recipe::make_base_retail_recipe(jak2_iso::default_revision());
    recipe::ArchiveRecord archive;
    archive.destination_basename = "OUT.DGO";
    archive.objects.reserve(jak2_source_object_pack::kExpectedObjectCount + 1);
    bundled.reserve(jak2_source_object_pack::kExpectedObjectCount);
    for (std::uint32_t index = 0; index < jak2_source_object_pack::kExpectedObjectCount; ++index) {
      bundled.push_back({static_cast<std::uint8_t>(index), static_cast<std::uint8_t>(index >> 8),
                         0x5a, 0xa5});
      const auto name = "src" + std::to_string(index);
      const auto relative = name + ".o";
      if (!write_bytes(source_root / relative, bundled.back())) {
        return false;
      }
      archive.objects.push_back(
          {name, recipe::BundledSourceObject{relative, bundled.back().size(),
                                              hash_of(bundled.back())}});
    }
    archive.objects.push_back(
        {"retail", recipe::VerifiedRetailObject{"DGO/RETAIL.DGO", 0, 4, retail.size(),
                                                  hash_of(retail)}});
    output_recipe.archives.push_back(std::move(archive));
    output_recipe.flat_file_copies = {{"DATA.BIN", "DATA.BIN"}};
    output_recipe.generated_flat_files = {{recipe::GeneratedFlatFileKind::game_text,
                                           "0COMMON.TXT"}};
    output_recipe.expected_fr3_basenames = {"GAME.fr3"};

    const auto encoded = recipe::encode(output_recipe, jak2_iso::default_revision());
    if (!encoded || !write_bytes(recipe_file, encoded.value()) ||
        !write_bytes(iso_root / "DATA.BIN", flat) ||
        !write_bytes(generated_root / "flat/0COMMON.TXT", generated_flat) ||
        !write_bytes(fr3_root / "GAME.fr3", fr3)) {
      return false;
    }
    const std::array<jak1_checked_dgo_writer::ObjectRecord, 1> retail_objects = {
        jak1_checked_dgo_writer::ObjectRecord{"retail", retail},
    };
    const auto retail_dgo = jak1_checked_dgo_writer::build("RETAIL.DGO", retail_objects);
    if (!retail_dgo || !write_bytes(iso_root / "DGO/RETAIL.DGO", retail_dgo.value())) {
      return false;
    }

    inputs.recipe_file = recipe_file;
    inputs.source_object_pack_root = source_root;
    inputs.extracted_iso_root = iso_root;
    inputs.generated_artifact_root = generated_root;
    inputs.prepared_fr3_root = fr3_root;
    inputs.generated_flat_files = {{recipe::GeneratedFlatFileKind::game_text,
                                    "0COMMON.TXT",
                                    "flat/0COMMON.TXT",
                                    generated_flat.size(),
                                    hash_of(generated_flat)}};
    return true;
  }

  bool stage_absent() const { return !fs::exists(fs::path(destination.string() + ".stage")); }
};

bool materializes_checked_jak2_layout() {
  Fixture fixture;
  CHECK(fixture.setup());
  const auto result = materializer::materialize(
      fixture.inputs, fixture.destination, jak2_iso::default_revision(), fixture.options);
  CHECK(result);
  CHECK(result.value().archives_written == 1);
  CHECK(result.value().objects_written == jak2_source_object_pack::kExpectedObjectCount + 1);
  CHECK(result.value().flat_files_written == 2);
  CHECK(result.value().fr3_files_written == 1);
  CHECK(fixture.stage_absent());
  CHECK(read_bytes(fixture.destination / "iso/DATA.BIN") == fixture.flat);
  CHECK(read_bytes(fixture.destination / "iso/0COMMON.TXT") == fixture.generated_flat);
  CHECK(read_bytes(fixture.destination / "fr3/GAME.fr3") == fixture.fr3);

  jak1_checked_dgo::Options read_options;
  read_options.game_version = GameVersion::Jak2;
  const auto output = jak1_checked_dgo::read(read_bytes(fixture.destination / "iso/OUT.DGO"),
                                              "OUT.DGO", read_options);
  CHECK(output);
  CHECK(output.value().objects.size() == jak2_source_object_pack::kExpectedObjectCount + 1);
  CHECK(output.value().objects.back().internal_name == "retail");
  CHECK(output.value().objects.back().unique_name == "retail-ag");
  CHECK(output.value().objects.back().data == fixture.retail);
  return true;
}

bool cancellation_is_atomic() {
  Fixture fixture;
  CHECK(fixture.setup());
  bool cancel = false;
  fixture.options.on_progress = [&](const materializer::Progress& progress) {
    if (progress.phase == materializer::Phase::writing_archives) {
      cancel = true;
    }
  };
  fixture.options.should_cancel = [&] { return cancel; };
  const auto result = materializer::materialize(
      fixture.inputs, fixture.destination, jak2_iso::default_revision(), fixture.options);
  CHECK(!result);
  CHECK(result.error().code == materializer::ErrorCode::cancelled);
  CHECK(!fs::exists(fixture.destination));
  CHECK(fixture.stage_absent());
  return true;
}

bool rejects_symlink_and_wrong_game_without_staging() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    std::error_code error;
    fs::remove(fixture.source_root / "src0.o", error);
    CHECK(!error);
    fs::create_symlink(fixture.iso_root / "DATA.BIN", fixture.source_root / "src0.o", error);
    if (!error) {
      const auto result = materializer::materialize(
          fixture.inputs, fixture.destination, jak2_iso::default_revision(), fixture.options);
      CHECK(!result);
      CHECK(result.error().code == materializer::ErrorCode::unsafe_path);
      CHECK(!fs::exists(fixture.destination));
      CHECK(fixture.stage_absent());
    }
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    auto jak1_recipe = fixture.output_recipe;
    const auto& jak1_revision = jak1_iso::default_revision();
    jak1_recipe.producer = jak1_output_recipe::kProvenanceId;
    jak1_recipe.game = jak1_output_recipe::kGameId;
    jak1_recipe.profile = jak1_output_recipe::OutputProfile::full_public;
    jak1_recipe.revision = {std::string(jak1_revision.serial),
                            jak1_revision.elf_hash,
                            jak1_revision.contents_hash,
                            jak1_revision.file_count,
                            std::string(jak1_revision.decomp_config_version),
                            jak1_revision.territory,
                            jak1_revision.black_label};
    jak1_output_recipe::Options jak1_options;
    jak1_options.expected_revision = jak1_recipe.revision;
    jak1_options.expected_source_object_pack = recipe::kRecordedSourceObjectPack;
    const auto encoded = jak1_output_recipe::encode(jak1_recipe, jak1_options);
    CHECK(encoded);
    CHECK(write_bytes(fixture.recipe_file, encoded.value()));
    const auto result = materializer::materialize(
        fixture.inputs, fixture.destination, jak2_iso::default_revision(), fixture.options);
    CHECK(!result);
    CHECK(result.error().code == materializer::ErrorCode::recipe_invalid);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

}  // namespace

int main() {
  const std::array tests = {
      materializes_checked_jak2_layout,
      cancellation_is_atomic,
      rejects_symlink_and_wrong_game_without_staging,
  };
  for (const auto test : tests) {
    if (!test()) {
      return 1;
    }
  }
  std::cout << "Jak II output materializer tests passed\n";
  return 0;
}
