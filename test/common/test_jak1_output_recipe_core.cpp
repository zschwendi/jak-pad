#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "common/custom_data/Jak1PublicOutputGraph.h"

#include "goalc/make/Jak1OutputRecipeGenerator.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace {

namespace graph = jak1_output_graph;
namespace generator = jak1_output_recipe_generator;

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": " #condition << '\n'; \
      return false;                                                                              \
    }                                                                                            \
  } while (false)

std::string source_tag(std::string_view source) {
  const auto slash = source.find_last_of('/');
  auto basename = source.substr(slash == std::string_view::npos ? 0 : slash + 1);
  const auto dot = basename.find_last_of('.');
  return std::string(basename.substr(0, dot));
}

std::string synthetic_manifest(const std::vector<std::string>& sources) {
  std::string rows;
  std::uint64_t seed = 0x1000;
  for (const auto& source : sources) {
    const auto tag = source_tag(source);
    char hash[17]{};
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(seed + 1));
    rows += source + "\t" + tag + "\t" + tag + ".o\t" + std::to_string(seed) + "\t" + hash + "\n";
    seed += 0x100;
  }
  const auto aggregate = XXH64(rows.data(), rows.size(), 0);
  char aggregate_text[17]{};
  std::snprintf(aggregate_text, sizeof(aggregate_text), "%016llx",
                static_cast<unsigned long long>(aggregate));
  return "FORMAT\tgoalc-source-object-pack-v1\nCOUNT\t" + std::to_string(sources.size()) +
         "\nAGGREGATE_XXH64\t" + aggregate_text + "\nSOURCE\tTAG\tFILE\tBYTES\tXXH64\n" + rows;
}

jak1_output_recipe::RevisionProvenance initial_revision() {
  const auto& revision = jak1_iso::default_revision();
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

generator::VerifiedInputs synthetic_inputs(const graph::Graph& value) {
  generator::VerifiedInputs inputs;
  inputs.revision = initial_revision();
  inputs.extracted_iso_root = "iso_data/jak1";
  inputs.expected_fr3_basenames = {"GAME.fr3", "beach.fr3"};

  constexpr std::string_view kIsoPrefix = "iso_data/jak1/";
  std::set<std::string> flat_paths;
  for (const auto& copy : value.flat_file_copies) {
    if (copy.source_path.starts_with(kIsoPrefix)) {
      flat_paths.emplace(copy.source_path.substr(kIsoPrefix.size()));
    }
  }
  inputs.verified_extracted_iso_relative_paths.assign(flat_paths.begin(), flat_paths.end());

  std::uint64_t seed = 0x100000;
  std::map<std::string, std::uint32_t> catalog_index_by_key;
  std::map<std::string, std::uint32_t> next_index_by_archive;
  for (const auto& archive : value.archives) {
    for (const auto& object : archive.objects) {
      if (object.producer != graph::ObjectProducerKind::verified_retail) {
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

bool embedded_graph_round_trips_exactly() {
  const auto decoded = jak1_public_output_graph::decode();
  CHECK(decoded);
  CHECK(decoded.value().ordered_source_files.size() == 518);
  CHECK(decoded.value().archives.size() == 28);
  CHECK(decoded.value().flat_file_copies.size() == 247);
  CHECK(decoded.value().generated_flat_files.size() == 14);
  const auto encoded = graph::encode(decoded.value());
  const auto encoded_again = graph::encode(decoded.value());
  CHECK(encoded);
  CHECK(encoded_again);
  CHECK(encoded.value() == encoded_again.value());
  CHECK(std::equal(encoded.value().begin(), encoded.value().end(),
                   jak1_public_output_graph::wire_data().begin(),
                   jak1_public_output_graph::wire_data().end()));
  return true;
}

bool rejects_corruption_limits_and_cancellation() {
  const auto wire = jak1_public_output_graph::wire_data();
  CHECK(wire.size() > 64);
  for (const auto length :
       {std::size_t{0}, std::size_t{7}, std::size_t{19}, wire.size() / 2, wire.size() - 1}) {
    const auto result = graph::decode(wire.first(length));
    CHECK(!result);
  }
  auto corrupt = std::vector<std::uint8_t>(wire.begin(), wire.end());
  corrupt[corrupt.size() / 2] ^= 1;
  auto result = graph::decode(corrupt);
  CHECK(!result);
  CHECK(result.error().code == graph::ErrorCode::corrupt_hash);

  graph::Options options;
  options.limits.max_source_files = 517;
  result = graph::decode(wire, options);
  CHECK(!result);

  options = {};
  options.should_cancel = [] { return true; };
  result = graph::decode(wire, options);
  CHECK(!result);
  CHECK(result.error().code == graph::ErrorCode::cancelled);
  return true;
}

bool portable_generation_core_consumes_embedded_graph() {
  const auto decoded = jak1_public_output_graph::decode();
  CHECK(decoded);
  auto inputs = synthetic_inputs(decoded.value());
  CHECK(!inputs.retail_catalog.empty());
  CHECK(inputs.expected_fr3_basenames == std::vector<std::string>({"GAME.fr3", "beach.fr3"}));
  const auto manifest = synthetic_manifest(decoded.value().ordered_source_files);
  const auto forward = generator::generate_from_graph(decoded.value(), manifest, inputs);
  CHECK(forward);
  CHECK(forward.value().expected_fr3_basenames == inputs.expected_fr3_basenames);
  CHECK(std::find(forward.value().expected_fr3_basenames.begin(),
                  forward.value().expected_fr3_basenames.end(),
                  "test-zone.fr3") == forward.value().expected_fr3_basenames.end());

  std::reverse(inputs.retail_catalog.begin(), inputs.retail_catalog.end());
  const auto reversed = generator::generate_from_graph(decoded.value(), manifest, inputs);
  CHECK(reversed);
  CHECK(reversed.value() == forward.value());
  return true;
}

}  // namespace

int main() {
  const std::array tests = {
      embedded_graph_round_trips_exactly,
      rejects_corruption_limits_and_cancellation,
      portable_generation_core_consumes_embedded_graph,
  };
  for (const auto test : tests) {
    if (!test()) {
      return 1;
    }
  }
  std::cout << "Jak 1 output recipe core tests passed\n";
  return 0;
}
