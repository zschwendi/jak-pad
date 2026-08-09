#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "common/custom_data/Jak2OutputRecipe.h"
#include "common/custom_data/Jak2PublicOutputGraph.h"
#include "goalc/make/Jak2OutputRecipeGenerator.h"

namespace {

namespace recipe = jak2_output_recipe;
namespace generator = jak2_output_recipe_generator;

#define CHECK(condition)                                                                      \
  do {                                                                                        \
    if (!(condition)) {                                                                       \
      std::cerr << "check failed at line " << __LINE__ << ": " #condition << '\n';          \
      return 1;                                                                               \
    }                                                                                         \
  } while (false)

recipe::Recipe make_recipe() {
  auto result = recipe::make_base_retail_recipe(jak2_iso::default_revision());
  recipe::ArchiveRecord archive;
  archive.destination_basename = "GAME.CGO";
  archive.objects.reserve(jak2_source_object_pack::kExpectedObjectCount + 1);
  for (std::uint32_t index = 0; index < jak2_source_object_pack::kExpectedObjectCount; ++index) {
    const auto name = "source-" + std::to_string(index);
    archive.objects.push_back(
        {name, recipe::BundledSourceObject{name + ".o", 1, index + 1}});
  }
  archive.objects.push_back(
      {"retail-data", recipe::VerifiedRetailObject{"DGO/RETAIL.DGO", 0, 2, 32,
                                                     0xabcddcba12344321ULL}});
  result.archives.push_back(std::move(archive));
  result.expected_fr3_basenames.push_back("GAME.fr3");
  return result;
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

int main() {
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
      graph.value(), "not-a-manifest", jak2_iso::default_revision(), graph_inputs);
  CHECK(!graph_recipe);
  CHECK(graph_recipe.error().code == generator::ErrorCode::invalid_manifest);

  auto expected = make_recipe();
  const auto encoded = recipe::encode(expected, jak2_iso::default_revision());
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

  const auto decoded = recipe::decode(encoded.value(), jak2_iso::default_revision());
  CHECK(decoded);
  CHECK(decoded.value() == expected);
  CHECK(decoded.value().producer == jak1_output_recipe::kJak2ProvenanceId);
  CHECK(decoded.value().game == jak1_output_recipe::kJak2GameId);
  CHECK(decoded.value().profile == recipe::OutputProfile::jak2_base_retail);
  CHECK(decoded.value().projected_source_objects.empty());
  CHECK(decoded.value().source_object_pack == recipe::kRecordedSourceObjectPack);

  jak1_output_recipe::Options jak1_options;
  jak1_options.expected_revision = jak1_revision();
  jak1_options.expected_source_object_pack = recipe::kRecordedSourceObjectPack;
  const auto wrong_game = jak1_output_recipe::decode(encoded.value(), jak1_options);
  CHECK(!wrong_game);
  CHECK(wrong_game.error().code == jak1_output_recipe::ErrorCode::wrong_magic);

  const auto revisions = jak2_iso::supported_revisions();
  CHECK(revisions.size() > 1);
  const auto wrong_revision = recipe::decode(encoded.value(), revisions[1]);
  CHECK(!wrong_revision);
  CHECK(wrong_revision.error().code == recipe::ErrorCode::unsupported_revision);

  auto wrong_profile = expected;
  wrong_profile.profile = recipe::OutputProfile::jak1_base_retail;
  const auto profile_result = recipe::encode(wrong_profile, jak2_iso::default_revision());
  CHECK(!profile_result);
  CHECK(profile_result.error().code == recipe::ErrorCode::wrong_provenance);
  return 0;
}
