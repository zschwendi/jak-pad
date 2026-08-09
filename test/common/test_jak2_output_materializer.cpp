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
#include <variant>
#include <vector>

#include "common/custom_data/Jak2OutputMaterializer.h"

#include "decompiler/extractor/jak1_checked_dgo.h"
#include "decompiler/extractor/jak1_checked_dgo_writer.h"
#include "decompiler/extractor/jak2_fr3_preparer.h"
#include "third-party/lzokay/lzokay.hpp"

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

void append_u32(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
  const auto offset = bytes->size();
  bytes->resize(offset + 4);
  write_u32(bytes, offset, value);
}

std::vector<std::uint8_t> make_aligned_blzo(std::span<const std::uint8_t> expanded) {
  constexpr std::size_t kBlockSize = 0x8000;
  std::vector<std::uint8_t> output{'o', 'Z', 'l', 'B'};
  append_u32(&output, expanded.size());
  for (std::size_t offset = 0; offset < expanded.size();) {
    const auto block_size = std::min(kBlockSize, expanded.size() - offset);
    std::vector<std::uint8_t> compressed(lzokay::compress_worst_size(block_size));
    auto compressed_size = compressed.size();
    const auto status = lzokay::compress(expanded.data() + offset, block_size, compressed.data(),
                                         compressed.size(), compressed_size);
    if (status != lzokay::EResult::Success || compressed_size >= kBlockSize) {
      if (block_size != kBlockSize) {
        return {};
      }
      append_u32(&output, kBlockSize);
      output.insert(output.end(), expanded.begin() + offset,
                    expanded.begin() + offset + block_size);
    } else {
      append_u32(&output, compressed_size);
      output.insert(output.end(), compressed.begin(), compressed.begin() + compressed_size);
    }
    while (output.size() % 4) {
      output.push_back(0);
    }
    offset += block_size;
  }
  const auto alignment = materializer::kNtscV2CompressedArchiveAlignmentBytes;
  const auto trailing = (alignment - output.size() % alignment) % alignment;
  output.resize(output.size() + trailing, 0);
  return output;
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
  std::vector<checked_file_identity::Identity> extracted_identities;
  std::vector<checked_file_identity::Identity> fr3_identities;
  materializer::Inputs inputs;
  materializer::Options options;

  bool write_retail_archive(std::span<const std::uint8_t> object,
                            const fs::path& relative = "DGO/RETAIL.DGO") {
    const std::array<jak1_checked_dgo_writer::ObjectRecord, 1> retail_objects = {
        jak1_checked_dgo_writer::ObjectRecord{"retail", object},
    };
    const auto retail_dgo = jak1_checked_dgo_writer::build("RETAIL.DGO", retail_objects);
    if (!retail_dgo) {
      return false;
    }
    const auto compressed = make_aligned_blzo(retail_dgo.value());
    return !compressed.empty() && write_bytes(iso_root / relative, compressed);
  }

  bool setup() {
    output_recipe = recipe::make_base_retail_recipe(jak2_iso::import_revision());
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

    const auto encoded = recipe::encode(output_recipe, jak2_iso::import_revision());
    if (!encoded || !write_bytes(recipe_file, encoded.value()) ||
        !write_bytes(iso_root / "DATA.BIN", flat) ||
        !write_bytes(generated_root / "flat/0COMMON.TXT", generated_flat) ||
        !write_bytes(fr3_root / "GAME.fr3", fr3)) {
      return false;
    }
    if (!write_retail_archive(retail)) {
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

  bool bind_validated_identities() {
    const auto archive = read_bytes(iso_root / "DGO/RETAIL.DGO");
    extracted_identities = {
        {"DGO/RETAIL.DGO", archive.size(), hash_of(archive)},
        {"DATA.BIN", flat.size(), hash_of(flat)},
    };
    for (std::size_t index = extracted_identities.size();
         index < jak2_iso::import_revision().file_count; ++index) {
      extracted_identities.push_back(
          {"UNUSED/" + std::to_string(index) + ".BIN", 0, static_cast<std::uint64_t>(index + 1)});
    }
    fr3_identities = {{"GAME.fr3", fr3.size(), hash_of(fr3)}};
    for (std::size_t index = 1; index < materializer::kNtscV2ExpectedFr3Files; ++index) {
      const auto name = "synthetic-" + std::to_string(index) + ".fr3";
      const std::vector<std::uint8_t> bytes = {static_cast<std::uint8_t>(index)};
      if (!write_bytes(fr3_root / name, bytes)) {
        return false;
      }
      output_recipe.expected_fr3_basenames.push_back(name);
      fr3_identities.push_back({name, bytes.size(), hash_of(bytes)});
    }
    std::sort(output_recipe.expected_fr3_basenames.begin(),
              output_recipe.expected_fr3_basenames.end());
    const auto encoded = recipe::encode(output_recipe, jak2_iso::import_revision());
    if (!encoded || !write_bytes(recipe_file, encoded.value())) {
      return false;
    }
    inputs.validated_extracted_files = extracted_identities;
    inputs.validated_fr3_files = fr3_identities;
    options.require_validated_file_identities = true;
    return true;
  }
};

bool materializes_checked_jak2_layout() {
  Fixture fixture;
  CHECK(fixture.setup());
  CHECK(fixture.options.limits.max_validated_extracted_files ==
        jak2_iso::import_revision().file_count);
  CHECK(jak2_fr3::kNtscV2ExpectedExtractedFiles ==
        jak2_iso::import_revision().file_count);
  CHECK(fixture.options.limits.max_validated_fr3_files ==
        materializer::kNtscV2ExpectedFr3Files);
  CHECK(!fixture.options.require_validated_file_identities);
  CHECK(fixture.bind_validated_identities());
  const auto expected_recipe = read_bytes(fixture.recipe_file);
  CHECK(!expected_recipe.empty());
  fixture.options.expected_recipe_bytes = expected_recipe;
  const auto result = materializer::materialize(
      fixture.inputs, fixture.destination, jak2_iso::import_revision(), fixture.options);
  CHECK(result);
  CHECK(result.value().archives_written == 1);
  CHECK(result.value().objects_written == jak2_source_object_pack::kExpectedObjectCount + 1);
  CHECK(result.value().flat_files_written == 2);
  CHECK(result.value().fr3_files_written == materializer::kNtscV2ExpectedFr3Files);
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

bool validated_archive_mutation_is_rejected() {
  Fixture fixture;
  CHECK(fixture.setup());
  CHECK(fixture.bind_validated_identities());
  bool callback_write_ok = true;
  fixture.options.on_progress = [&](const materializer::Progress& progress) {
    if (progress.phase == materializer::Phase::writing_archives) {
      auto archive = read_bytes(fixture.iso_root / "DGO/RETAIL.DGO");
      archive.back() ^= 1;
      callback_write_ok = write_bytes(fixture.iso_root / "DGO/RETAIL.DGO", archive);
    }
  };
  const auto result = materializer::materialize(
      fixture.inputs, fixture.destination, jak2_iso::import_revision(), fixture.options);
  CHECK(callback_write_ok);
  CHECK(!result);
  CHECK(result.error().code == materializer::ErrorCode::input_identity_mismatch);
  CHECK(!fs::exists(fixture.destination));
  CHECK(fixture.stage_absent());
  return true;
}

bool progress_callback_recipe_swap_is_rejected() {
  Fixture fixture;
  CHECK(fixture.setup());
  CHECK(fixture.bind_validated_identities());
  const auto expected_recipe = read_bytes(fixture.recipe_file);
  CHECK(!expected_recipe.empty());

  auto& source = std::get<recipe::BundledSourceObject>(
      fixture.output_recipe.archives.front().objects.front().source);
  source.bundle_relative_path = "alternate-src0.o";
  CHECK(write_bytes(fixture.source_root / source.bundle_relative_path, fixture.bundled.front()));
  const auto replacement = recipe::encode(fixture.output_recipe, jak2_iso::import_revision());
  CHECK(replacement);

  bool swapped = false;
  fixture.options.expected_recipe_bytes = expected_recipe;
  fixture.options.on_progress = [&](const materializer::Progress& progress) {
    if (!swapped && progress.phase == materializer::Phase::validating) {
      swapped = write_bytes(fixture.recipe_file, replacement.value());
    }
  };
  const auto result = materializer::materialize(
      fixture.inputs, fixture.destination, jak2_iso::import_revision(), fixture.options);
  CHECK(swapped);
  CHECK(!result);
  CHECK(result.error().code == materializer::ErrorCode::recipe_mismatch);
  CHECK(!fs::exists(fixture.destination));
  CHECK(fixture.stage_absent());
  return true;
}

bool required_identity_manifests_are_exact_and_bounded() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    CHECK(fixture.bind_validated_identities());
    fixture.extracted_identities.push_back({"EXTRA.BIN", 1, 1});
    fixture.inputs.validated_extracted_files = fixture.extracted_identities;
    const auto result = materializer::materialize(
        fixture.inputs, fixture.destination, jak2_iso::import_revision(), fixture.options);
    CHECK(!result);
    CHECK(result.error().code == materializer::ErrorCode::input_identity_mismatch);
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    CHECK(fixture.bind_validated_identities());
    fixture.extracted_identities.back().relative_path = "dgo/retail.dgo";
    fixture.inputs.validated_extracted_files = fixture.extracted_identities;
    const auto result = materializer::materialize(
        fixture.inputs, fixture.destination, jak2_iso::import_revision(), fixture.options);
    CHECK(!result);
    CHECK(result.error().code == materializer::ErrorCode::input_identity_mismatch);
    CHECK(fixture.stage_absent());
  }
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
      fixture.inputs, fixture.destination, jak2_iso::import_revision(), fixture.options);
  CHECK(!result);
  CHECK(result.error().code == materializer::ErrorCode::cancelled);
  CHECK(!fs::exists(fixture.destination));
  CHECK(fixture.stage_absent());
  return true;
}

bool retail_revalidation_rejects_hash_and_wrong_archive() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    auto changed = fixture.retail;
    changed.back() ^= 1;
    CHECK(fixture.write_retail_archive(changed));
    const auto result = materializer::materialize(
        fixture.inputs, fixture.destination, jak2_iso::import_revision(), fixture.options);
    CHECK(!result);
    CHECK(result.error().code == materializer::ErrorCode::retail_object_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    auto& retail_source = std::get<recipe::VerifiedRetailObject>(
        fixture.output_recipe.archives[0].objects.back().source);
    retail_source.source_archive_relative_path = "DGO/MISSING.DGO";
    const auto encoded = recipe::encode(fixture.output_recipe, jak2_iso::import_revision());
    CHECK(encoded);
    CHECK(write_bytes(fixture.recipe_file, encoded.value()));
    const auto result = materializer::materialize(
        fixture.inputs, fixture.destination, jak2_iso::import_revision(), fixture.options);
    CHECK(!result);
    CHECK(result.error().code == materializer::ErrorCode::input_missing);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

bool rejects_v1_symlink_and_wrong_game_without_staging() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    const auto result = materializer::materialize(
        fixture.inputs, fixture.destination, jak2_iso::default_revision(), fixture.options);
    CHECK(!result);
    CHECK(result.error().code == materializer::ErrorCode::revision_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    std::error_code error;
    fs::remove(fixture.source_root / "src0.o", error);
    CHECK(!error);
    fs::create_symlink(fixture.iso_root / "DATA.BIN", fixture.source_root / "src0.o", error);
    if (!error) {
      const auto result = materializer::materialize(
          fixture.inputs, fixture.destination, jak2_iso::import_revision(), fixture.options);
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
        fixture.inputs, fixture.destination, jak2_iso::import_revision(), fixture.options);
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
      validated_archive_mutation_is_rejected,
      progress_callback_recipe_swap_is_rejected,
      required_identity_manifests_are_exact_and_bounded,
      cancellation_is_atomic,
      retail_revalidation_rejects_hash_and_wrong_archive,
      rejects_v1_symlink_and_wrong_game_without_staging,
  };
  for (const auto test : tests) {
    if (!test()) {
      return 1;
    }
  }
  std::cout << "Jak II output materializer tests passed\n";
  return 0;
}
