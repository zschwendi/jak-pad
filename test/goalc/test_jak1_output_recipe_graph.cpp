#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "common/util/FileUtil.h"

#include "goalc/make/Jak1OutputRecipeGenerator.h"
#include "goalc/make/MakeSystem.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace {

class InspectOnlyCompilerTool final : public Tool {
 public:
  InspectOnlyCompilerTool() : Tool("goalc") {}
  bool run(const ToolInput&, const PathMap&) override { return false; }
};

bool contains_destination(const jak1_output_recipe_generator::Graph& graph,
                          const std::string& basename) {
  return std::any_of(
             graph.archives.begin(), graph.archives.end(),
             [&](const auto& archive) { return archive.destination_basename == basename; }) ||
         std::any_of(graph.flat_file_copies.begin(), graph.flat_file_copies.end(),
                     [&](const auto& copy) { return copy.destination_basename == basename; }) ||
         std::any_of(
             graph.generated_flat_files.begin(), graph.generated_flat_files.end(),
             [&](const auto& generated) { return generated.destination_basename == basename; });
}

jak1_output_recipe::RevisionProvenance initial_revision() {
  const auto& revision = jak1_iso::default_revision();
  return {std::string(revision.serial),
          revision.elf_hash,
          revision.contents_hash,
          revision.file_count,
          std::string(revision.decomp_config_version),
          revision.territory,
          revision.black_label};
}

std::string synthetic_manifest(const std::vector<std::string>& sources) {
  std::string rows;
  std::uint64_t seed = 0x1000;
  for (const auto& source : sources) {
    auto tag = std::filesystem::path(source).stem().string();
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

std::string synthetic_retail_path(const std::string& destination_basename) {
  return (destination_basename.ends_with(".CGO") ? "CGO/" : "DGO/") + destination_basename;
}

struct SyntheticInputs {
  jak1_output_recipe_generator::VerifiedInputs verified;
  std::size_t retail_occurrences = 0;
};

SyntheticInputs make_synthetic_inputs(const jak1_output_recipe_generator::Graph& graph) {
  namespace generator = jak1_output_recipe_generator;

  SyntheticInputs result;
  result.verified.revision = initial_revision();
  result.verified.extracted_iso_root = "iso_data/jak1";

  std::set<std::string> verified_flat_paths;
  for (const auto& copy : graph.flat_file_copies) {
    const auto relative = std::filesystem::path(copy.source_path)
                              .lexically_relative(result.verified.extracted_iso_root)
                              .generic_string();
    if (!relative.empty()) {
      verified_flat_paths.emplace(relative);
    }
  }
  result.verified.verified_extracted_iso_relative_paths.assign(verified_flat_paths.begin(),
                                                                verified_flat_paths.end());

  std::uint64_t seed = 0x100000;
  for (const auto& archive : graph.archives) {
    for (std::uint32_t object_index = 0; object_index < archive.objects.size(); ++object_index) {
      const auto& object = archive.objects[object_index];
      if (object.producer != generator::ObjectProducerKind::verified_retail) {
        continue;
      }
      auto unique_name = object.prepared_basename;
      if (unique_name.ends_with(".go")) {
        unique_name.resize(unique_name.size() - 3);
      }
      result.verified.retail_catalog.push_back(
          {synthetic_retail_path(archive.destination_basename), object_index, object.internal_name,
           std::move(unique_name), 4, seed, seed + 1});
      ++result.retail_occurrences;
      seed += 0x100;
    }
  }
  return result;
}

bool verify_synthetic_graph_complete_resolution(
    const jak1_output_recipe_generator::Graph& graph) {
  namespace generator = jak1_output_recipe_generator;
  namespace recipe = jak1_output_recipe;

  // These ideal entries are derived from the public graph itself. This proves structural coverage
  // and deterministic selection only; it does not prove compatibility with real retail unique names.
  auto inputs = make_synthetic_inputs(graph);
  if (inputs.retail_occurrences == 0 ||
      inputs.retail_occurrences != inputs.verified.retail_catalog.size()) {
    std::fputs("Synthetic graph coverage found no complete retail input set.\n", stderr);
    return false;
  }

  const auto manifest = synthetic_manifest(graph.ordered_source_files);
  const auto forward = generator::generate_from_graph(graph, manifest, inputs.verified);
  if (!forward) {
    std::fprintf(stderr, "Synthetic forward recipe generation failed: %s\n",
                 forward.error().message.c_str());
    return false;
  }

  std::reverse(inputs.verified.retail_catalog.begin(), inputs.verified.retail_catalog.end());
  const auto reversed = generator::generate_from_graph(graph, manifest, inputs.verified);
  if (!reversed) {
    std::fprintf(stderr, "Synthetic reversed recipe generation failed: %s\n",
                 reversed.error().message.c_str());
    return false;
  }
  if (forward.value() != reversed.value()) {
    std::fputs("Synthetic catalog order changed the generated recipe.\n", stderr);
    return false;
  }

  std::size_t resolved_retail = 0;
  if (forward.value().archives.size() != graph.archives.size()) {
    std::fputs("Synthetic recipe did not retain every graph archive.\n", stderr);
    return false;
  }
  for (std::size_t archive_index = 0; archive_index < graph.archives.size(); ++archive_index) {
    const auto& graph_archive = graph.archives[archive_index];
    const auto& output_archive = forward.value().archives[archive_index];
    if (output_archive.destination_basename != graph_archive.destination_basename ||
        output_archive.objects.size() != graph_archive.objects.size()) {
      std::fputs("Synthetic recipe changed an archive identity or object count.\n", stderr);
      return false;
    }
    for (std::size_t object_index = 0; object_index < graph_archive.objects.size(); ++object_index) {
      const auto& graph_object = graph_archive.objects[object_index];
      const auto& output_object = output_archive.objects[object_index];
      if (graph_object.producer != generator::ObjectProducerKind::verified_retail) {
        continue;
      }
      const auto* retail = std::get_if<recipe::VerifiedRetailObject>(&output_object.source);
      if (!retail || output_object.internal_name != graph_object.internal_name ||
          retail->source_archive_relative_path !=
              synthetic_retail_path(graph_archive.destination_basename) ||
          retail->archive_object_index != object_index) {
        std::fputs("Synthetic recipe did not resolve an expected retail graph occurrence.\n",
                   stderr);
        return false;
      }
      ++resolved_retail;
    }
  }
  if (resolved_retail != inputs.retail_occurrences) {
    std::fputs("Synthetic recipe did not resolve every expected retail graph occurrence.\n", stderr);
    return false;
  }

  std::printf("Synthetic graph-complete retail coverage resolved %zu occurrences; real catalog "
              "compatibility is not covered.\n",
              resolved_retail);
  return true;
}

}  // namespace

int main() {
  if (!file_util::setup_project_path(fs::current_path(), true)) {
    std::fputs("Could not locate the OpenGOAL project root.\n", stderr);
    return 1;
  }

  try {
    MakeSystem make_system(std::nullopt);
    make_system.add_tool(std::make_shared<InspectOnlyCompilerTool>());
    make_system.load_project_file(file_util::get_file_path({"goal_src/jak1/game.gp"}));
    const auto result = jak1_output_recipe_generator::inspect_make_system(make_system);
    if (!result) {
      std::fprintf(stderr, "Graph inspection failed: %s\n", result.error().message.c_str());
      return 1;
    }
    const auto& graph = result.value();
    if (graph.ordered_source_files.size() != 518 || graph.archives.size() != 28 ||
        graph.flat_file_copies.size() != 247 || graph.generated_flat_files.size() != 14 ||
        contains_destination(graph, "SAVEGAME.ICO")) {
      std::fprintf(stderr,
                   "Unexpected graph: %zu sources, %zu archives, %zu copies, %zu generated.\n",
                   graph.ordered_source_files.size(), graph.archives.size(),
                   graph.flat_file_copies.size(), graph.generated_flat_files.size());
      return 1;
    }
    const auto bea =
        std::find_if(graph.archives.begin(), graph.archives.end(),
                     [](const auto& archive) { return archive.destination_basename == "BEA.DGO"; });
    if (bea == graph.archives.end() || bea->objects.size() < 2 ||
        bea->objects[0].prepared_basename != "mistycannon.o" ||
        bea->objects[1].prepared_basename != "babak-with-cannon.o") {
      std::fputs("BEA.DGO object order did not match its checked description.\n", stderr);
      return 1;
    }
    if (!verify_synthetic_graph_complete_resolution(graph)) {
      return 1;
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Graph audit threw: %s\n", error.what());
    return 1;
  }

  std::puts("Jak 1 GROUP:iso graph audit passed.");
  return 0;
}
