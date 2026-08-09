#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/custom_data/Jak2OutputRecipe.h"
#include "common/custom_data/Jak2PublicOutputGraph.h"
#include "goalc/make/Jak2OutputRecipeGenerator.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace {

namespace recipe = jak2_output_recipe;
namespace generator = jak2_output_recipe_generator;
namespace core_generator = jak1_output_recipe_generator;
namespace fs = std::filesystem;

#define CHECK(condition)                                                                      \
  do {                                                                                        \
    if (!(condition)) {                                                                       \
      std::cerr << "check failed at line " << __LINE__ << ": " #condition << '\n';          \
      return 1;                                                                               \
    }                                                                                         \
  } while (false)

recipe::Recipe make_recipe() {
  auto result = recipe::make_base_retail_recipe(jak2_iso::import_revision());
  recipe::ArchiveRecord archive;
  archive.destination_basename = "GAME.CGO";
  archive.objects.reserve(jak2_source_object_pack::kExpectedObjectCount);
  for (std::uint32_t index = 0; index + 1 < jak2_source_object_pack::kExpectedObjectCount;
       ++index) {
    const auto name = "source-" + std::to_string(index);
    archive.objects.push_back(
        {name, recipe::BundledSourceObject{name + ".o", 1, index + 1}});
  }
  archive.objects.push_back(
      {"retail-data", recipe::VerifiedRetailObject{"DGO/RETAIL.DGO", 0, 2, 32,
                                                     0xabcddcba12344321ULL}});
  result.archives.push_back(std::move(archive));
  result.projected_source_objects = {
      {jak1_output_recipe::kBaseRetailProjectedBundlePath, 1,
       jak2_source_object_pack::kExpectedObjectCount}};
  result.expected_fr3_basenames.push_back("GAME.fr3");
  return result;
}

std::string source_tag(std::string_view source) {
  return fs::path(source).stem().string();
}

std::string synthetic_manifest(const std::vector<std::string>& sources) {
  std::string rows;
  std::uint64_t seed = 0x1000;
  for (const auto& source : sources) {
    const auto tag = source_tag(source);
    char hash[17]{};
    std::snprintf(hash, sizeof(hash), "%016llx",
                  static_cast<unsigned long long>(seed + 1));
    rows += source + "\t" + tag + "\t" + tag + ".o\t" + std::to_string(seed) + "\t" +
            hash + "\n";
    seed += 0x100;
  }
  const auto aggregate = XXH64(rows.data(), rows.size(), 0);
  char aggregate_text[17]{};
  std::snprintf(aggregate_text, sizeof(aggregate_text), "%016llx",
                static_cast<unsigned long long>(aggregate));
  return "FORMAT\tgoalc-source-object-pack-v1\nCOUNT\t" + std::to_string(sources.size()) +
         "\nAGGREGATE_XXH64\t" + aggregate_text +
         "\nSOURCE\tTAG\tFILE\tBYTES\tXXH64\n" + rows;
}

core_generator::VerifiedInputs synthetic_inputs(const generator::Graph& graph) {
  core_generator::VerifiedInputs inputs;
  inputs.revision = recipe::revision_provenance(jak2_iso::import_revision());
  inputs.extracted_iso_root = "iso_data/jak2";
  inputs.expected_fr3_basenames = {"GAME.fr3"};

  std::set<std::string> flat_paths;
  for (const auto& copy : graph.flat_file_copies) {
    flat_paths.emplace(
        fs::path(copy.source_path).lexically_relative(inputs.extracted_iso_root).generic_string());
  }
  inputs.verified_extracted_iso_relative_paths.assign(flat_paths.begin(), flat_paths.end());

  std::map<std::string, std::uint32_t> catalog_index_by_key;
  std::map<std::string, std::uint32_t> next_index_by_archive;
  std::uint64_t seed = 0x100000;
  for (const auto& archive : graph.archives) {
    for (const auto& object : archive.objects) {
      if (object.producer != core_generator::ObjectProducerKind::verified_retail) {
        continue;
      }
      auto unique_name = object.prepared_basename;
      unique_name.resize(unique_name.size() - 3);
      const auto key =
          object.retail_source_archive + "\n" + unique_name + "\n" + object.internal_name;
      if (catalog_index_by_key.contains(key)) {
        continue;
      }
      const auto index = next_index_by_archive[object.retail_source_archive]++;
      catalog_index_by_key.emplace(key, index);
      inputs.retail_catalog.push_back({object.retail_source_archive, index, object.internal_name,
                                       std::move(unique_name), 4, seed, seed + 1});
      seed += 0x100;
    }
  }
  return inputs;
}

core_generator::Options core_options() {
  core_generator::Options options;
  options.limits.max_graph_archives = jak2_public_output_graph::kArchiveLimit;
  options.recipe_limits.max_archives = jak2_public_output_graph::kArchiveLimit;
  options.output_profile = recipe::OutputProfile::jak2_base_retail;
  options.wire_game = jak1_output_recipe::WireGame::jak2;
  options.expected_source_object_count = jak2_source_object_pack::kExpectedObjectCount;
  return options;
}

jak1_output_recipe::RevisionProvenance jak1_revision() {
  const auto& source = jak1_iso::default_revision();
  return {std::string(source.serial),
          source.elf_hash,
          source.contents_hash,
          source.file_count,
          std::string(source.decomp_config_version),
          source.territory,
          source.black_label};
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 1 || argc == 2);
  recipe::Options default_options;
  CHECK(default_options.limits.max_archives == jak2_public_output_graph::kArchiveLimit);
  const auto graph = jak2_public_output_graph::decode_base_retail();
  CHECK(graph);
  CHECK(graph.value().ordered_source_files.size() == jak2_source_object_pack::kExpectedObjectCount);
  CHECK(graph.value().archives.size() == generator::kBaseRetailArchiveCount);
  CHECK(graph.value().flat_file_copies.size() == generator::kBaseRetailFlatCopyCount);
  CHECK(graph.value().generated_flat_files.size() == generator::kBaseRetailGeneratedFlatCount);
  CHECK(std::none_of(graph.value().archives.begin(), graph.value().archives.end(),
                     [](const auto& archive) {
                       return archive.destination_basename == "TSZ.DGO";
                     }));

  generator::VerifiedInputs graph_inputs;
  auto graph_recipe = generator::generate_from_graph(
      graph.value(), "not-a-manifest", jak2_iso::import_revision(), graph_inputs);
  CHECK(!graph_recipe);
  CHECK(graph_recipe.error().code == generator::ErrorCode::invalid_manifest);
  graph_recipe = generator::generate_from_graph(
      graph.value(), "not-a-manifest", jak2_iso::default_revision(), graph_inputs);
  CHECK(!graph_recipe);
  CHECK(graph_recipe.error().code == generator::ErrorCode::unsupported_revision);

  auto expected = make_recipe();
  const auto encoded = recipe::encode(expected, jak2_iso::import_revision());
  CHECK(encoded);
  CHECK(encoded.value().size() >= jak1_output_recipe::kJak2Magic.size());
  CHECK(std::equal(jak1_output_recipe::kJak2Magic.begin(),
                   jak1_output_recipe::kJak2Magic.end(), encoded.value().begin()));

  const std::vector<std::uint8_t> retail_payload = {
      0xf1, 0xe2, 0xd3, 0xc4, 0xb5, 0xa6, 0x97, 0x88, 0x79, 0x6a, 0x5b,
      0x4c, 0x3d, 0x2e, 0x1f, 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76,
      0x87, 0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f, 0x11,
  };
  CHECK(std::search(encoded.value().begin(), encoded.value().end(), retail_payload.begin(),
                    retail_payload.end()) == encoded.value().end());

  const auto decoded = recipe::decode(encoded.value(), jak2_iso::import_revision());
  CHECK(decoded);
  CHECK(decoded.value() == expected);
  CHECK(decoded.value().producer == jak1_output_recipe::kJak2ProvenanceId);
  CHECK(decoded.value().game == jak1_output_recipe::kJak2GameId);
  CHECK(decoded.value().profile == recipe::OutputProfile::jak2_base_retail);
  CHECK(decoded.value().projected_source_objects.size() == 1);
  CHECK(decoded.value().projected_source_objects.front().bundle_relative_path ==
        jak1_output_recipe::kBaseRetailProjectedBundlePath);
  CHECK(decoded.value().source_object_pack == recipe::kRecordedSourceObjectPack);

  jak1_output_recipe::Options core_jak2_options;
  core_jak2_options.expected_revision =
      recipe::revision_provenance(jak2_iso::import_revision());
  core_jak2_options.expected_source_object_pack = recipe::kRecordedSourceObjectPack;
  core_jak2_options.wire_game = jak1_output_recipe::WireGame::jak2;
  CHECK(jak1_output_recipe::decode(encoded.value(), core_jak2_options));
  core_jak2_options.expected_revision =
      recipe::revision_provenance(jak2_iso::default_revision());
  const auto rejected_v1_core = jak1_output_recipe::decode(encoded.value(), core_jak2_options);
  CHECK(!rejected_v1_core);
  CHECK(rejected_v1_core.error().code == jak1_output_recipe::ErrorCode::invalid_argument);

  jak1_output_recipe::Options jak1_options;
  jak1_options.expected_revision = jak1_revision();
  jak1_options.expected_source_object_pack = recipe::kRecordedSourceObjectPack;
  const auto wrong_game = jak1_output_recipe::decode(encoded.value(), jak1_options);
  CHECK(!wrong_game);
  CHECK(wrong_game.error().code == jak1_output_recipe::ErrorCode::wrong_magic);

  const auto wrong_revision = recipe::decode(encoded.value(), jak2_iso::default_revision());
  CHECK(!wrong_revision);
  CHECK(wrong_revision.error().code == recipe::ErrorCode::unsupported_revision);

  auto wrong_profile = expected;
  wrong_profile.profile = recipe::OutputProfile::jak1_base_retail;
  const auto profile_result = recipe::encode(wrong_profile, jak2_iso::import_revision());
  CHECK(!profile_result);
  CHECK(profile_result.error().code == recipe::ErrorCode::wrong_provenance);

  auto wrong_projection = expected;
  wrong_projection.projected_source_objects.clear();
  auto projection_result = recipe::encode(wrong_projection, jak2_iso::import_revision());
  CHECK(!projection_result);
  CHECK(projection_result.error().code == recipe::ErrorCode::wrong_source_pack);
  wrong_projection = expected;
  wrong_projection.projected_source_objects.front().bundle_relative_path = "other.o";
  projection_result = recipe::encode(wrong_projection, jak2_iso::import_revision());
  CHECK(!projection_result);
  CHECK(projection_result.error().code == recipe::ErrorCode::wrong_source_pack);
  wrong_projection = expected;
  wrong_projection.projected_source_objects.push_back({"other.o", 1, 1});
  projection_result = recipe::encode(wrong_projection, jak2_iso::import_revision());
  CHECK(!projection_result);
  CHECK(projection_result.error().code == recipe::ErrorCode::wrong_source_pack);

  auto custom_output = expected;
  custom_output.archives.front().objects.push_back(
      {"custom", recipe::GeneratedData{recipe::GeneratedDataKind::custom_actor}});
  const auto custom_result = recipe::encode(custom_output, jak2_iso::import_revision());
  CHECK(!custom_result);
  CHECK(custom_result.error().code == recipe::ErrorCode::wrong_provenance);

  const auto manifest = synthetic_manifest(graph.value().ordered_source_files);
  const auto inputs = synthetic_inputs(graph.value());
  const auto generated = core_generator::generate_from_graph(
      graph.value(), manifest, inputs, core_options());
  if (!generated) {
    std::cerr << "embedded Jak II recipe generation failed: " << generated.error().message << '\n';
  }
  CHECK(generated);
  CHECK(generated.value().projected_source_objects.size() == 1);
  CHECK(generated.value().projected_source_objects.front().bundle_relative_path ==
        jak1_output_recipe::kBaseRetailProjectedBundlePath);
  std::set<std::string> referenced_bundled_sources;
  for (const auto& archive : generated.value().archives) {
    for (const auto& object : archive.objects) {
      if (const auto* bundled = std::get_if<recipe::BundledSourceObject>(&object.source)) {
        referenced_bundled_sources.emplace(bundled->bundle_relative_path);
      }
    }
  }
  CHECK(referenced_bundled_sources.size() == 839);
  CHECK(!referenced_bundled_sources.contains(
      jak1_output_recipe::kBaseRetailProjectedBundlePath));

  auto wrong_graph = graph.value();
  std::map<std::string, std::size_t> bundled_occurrences;
  for (const auto& archive : wrong_graph.archives) {
    for (const auto& object : archive.objects) {
      if (object.producer == core_generator::ObjectProducerKind::bundled_source) {
        ++bundled_occurrences[object.prepared_basename];
      }
    }
  }
  bool replaced = false;
  for (auto& archive : wrong_graph.archives) {
    for (auto& object : archive.objects) {
      if (object.producer == core_generator::ObjectProducerKind::bundled_source &&
          bundled_occurrences[object.prepared_basename] == 1) {
        object.prepared_basename = jak1_output_recipe::kBaseRetailProjectedBundlePath;
        object.internal_name = jak1_output_recipe::kBaseRetailProjectedSourceTag;
        replaced = true;
        break;
      }
    }
    if (replaced) {
      break;
    }
  }
  CHECK(replaced);
  const auto rejected =
      core_generator::generate_from_graph(wrong_graph, manifest, inputs, core_options());
  CHECK(!rejected);
  CHECK(rejected.error().code == core_generator::ErrorCode::manifest_graph_mismatch);

  if (argc == 2) {
    std::ifstream manifest_input(argv[1], std::ios::binary);
    CHECK(manifest_input.is_open());
    const std::string recorded_manifest((std::istreambuf_iterator<char>(manifest_input)),
                                        std::istreambuf_iterator<char>());
    generator::VerifiedInputs recorded_inputs;
    recorded_inputs.extracted_iso_root = inputs.extracted_iso_root;
    recorded_inputs.verified_extracted_iso_relative_paths =
        inputs.verified_extracted_iso_relative_paths;
    recorded_inputs.retail_catalog = inputs.retail_catalog;
    recorded_inputs.expected_fr3_basenames = inputs.expected_fr3_basenames;
    const auto recorded = generator::generate_from_graph(
        graph.value(), recorded_manifest, jak2_iso::import_revision(), recorded_inputs);
    if (!recorded) {
      std::cerr << "recorded Jak II manifest oracle failed: " << recorded.error().message << '\n';
    }
    CHECK(recorded);
    CHECK(recorded.value().source_object_pack == recipe::kRecordedSourceObjectPack);
    CHECK(recorded.value().projected_source_objects.size() == 1);
    CHECK(recorded.value().projected_source_objects.front().bundle_relative_path ==
          jak1_output_recipe::kBaseRetailProjectedBundlePath);
    CHECK(recorded.value().projected_source_objects.front().size == 5539);
    CHECK(recorded.value().projected_source_objects.front().xxh64 == 0x8efb63413b6e7c24ULL);
  }
  return 0;
}
