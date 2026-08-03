#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/custom_data/Jak1OutputRecipe.h"

#include "goalc/make/Jak1OutputRecipeGenerator.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace generator = jak1_output_recipe_generator;
namespace recipe = jak1_output_recipe;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

recipe::RevisionProvenance initial_revision() {
  const auto& revision = jak1_iso::default_revision();
  return {std::string(revision.serial),
          revision.elf_hash,
          revision.contents_hash,
          revision.file_count,
          std::string(revision.decomp_config_version),
          revision.territory,
          revision.black_label};
}

std::string make_manifest(const std::vector<std::string>& sources) {
  std::string rows;
  uint64_t seed = 0x1000;
  for (const auto& source : sources) {
    const auto slash = source.find_last_of('/');
    auto tag = source.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto dot = tag.find_last_of('.');
    tag.resize(dot);
    rows += source + "\t" + tag + "\t" + tag + ".o\t" + std::to_string(seed) + "\t";
    char hash[17]{};
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(seed + 1));
    rows += hash;
    rows += '\n';
    seed += 0x100;
  }
  const auto aggregate = XXH64(rows.data(), rows.size(), 0);
  char aggregate_text[17]{};
  std::snprintf(aggregate_text, sizeof(aggregate_text), "%016llx",
                static_cast<unsigned long long>(aggregate));
  return "FORMAT\tgoalc-source-object-pack-v1\nCOUNT\t" + std::to_string(sources.size()) +
         "\nAGGREGATE_XXH64\t" + aggregate_text + "\nSOURCE\tTAG\tFILE\tBYTES\tXXH64\n" + rows;
}

generator::Graph valid_graph() {
  generator::Graph graph;
  graph.ordered_source_files = {"goal_src/jak1/code-a.gc", "goal_src/jak1/code-b.gc"};
  graph.archives = {
      {"A.DGO",
       {{"code-a.o", "code-a", generator::ObjectProducerKind::bundled_source, {}},
        {"art-ag.go", "art", generator::ObjectProducerKind::verified_retail, "DGO/A.DGO"}}},
      {"B.CGO",
       {{"code-b.o", "code-b", generator::ObjectProducerKind::bundled_source, {}},
        {"dir-tpages.go", "dir-tpages", generator::ObjectProducerKind::directory_tpages, {}},
        {"game-cnt.go", "game-cnt", generator::ObjectProducerKind::game_count, {}},
        {"test-actor-ag.go", "test-actor", generator::ObjectProducerKind::custom_actor, {}},
        {"test-zone.go", "test-zone", generator::ObjectProducerKind::custom_level, {}}}},
  };
  graph.flat_file_copies = {{"/verified/iso/MUS/TWEAKVAL.MUS", "TWEAKVAL.MUS"}};
  graph.generated_flat_files = {
      {recipe::GeneratedFlatFileKind::game_text, "0COMMON.TXT"},
      {recipe::GeneratedFlatFileKind::game_subtitle, "0SUBTIT.TXT"},
  };
  return graph;
}

generator::VerifiedInputs valid_inputs() {
  generator::VerifiedInputs inputs;
  inputs.revision = initial_revision();
  inputs.extracted_iso_root = "/verified/iso";
  inputs.verified_extracted_iso_relative_paths = {"MUS/TWEAKVAL.MUS"};
  inputs.retail_catalog = {
      {"DGO/A.DGO", 4, "art", "art-ag", 4, 4096, 0xaaaa},
      {"DGO/Z.DGO", 9, "art", "art-ag", 4, 8192, 0xbbbb},
  };
  inputs.expected_fr3_basenames = {"test-zone.fr3"};
  return inputs;
}

generator::Options test_options() {
  generator::Options options;
  options.expected_source_object_count = 2;
  return options;
}

void test_success_and_round_trip() {
  const auto graph = valid_graph();
  const auto manifest = make_manifest(graph.ordered_source_files);
  const auto result =
      generator::generate_from_graph(graph, manifest, valid_inputs(), test_options());
  check(bool(result), "valid graph generates a recipe");
  if (!result) {
    return;
  }
  const auto& output = result.value();
  check(output.archives.size() == 2, "recipe keeps two archives");
  check(output.archives[0].objects.size() == 2, "recipe keeps exact A.DGO object count");
  check(output.archives[0].objects[0].internal_name == "code-a" &&
            output.archives[0].objects[1].internal_name == "art",
        "recipe preserves DGO object order");
  const auto& retail = std::get<recipe::VerifiedRetailObject>(output.archives[0].objects[1].source);
  check(retail.source_archive_relative_path == "DGO/A.DGO" && retail.archive_object_index == 4,
        "matching source archive disambiguates divergent catalog entries");
  check(output.generated_flat_files.size() == 2, "recipe carries all generated flat outputs");

  recipe::Options schema_options;
  schema_options.expected_revision = output.revision;
  schema_options.expected_source_object_pack = output.source_object_pack;
  const auto encoded = recipe::encode(output, schema_options);
  check(bool(encoded), "generated recipe encodes");
  if (encoded) {
    const auto decoded = recipe::decode(encoded.value(), schema_options);
    check(bool(decoded) && decoded.value() == output, "generated recipe round-trips exactly");
  }
}

void test_manifest_failures() {
  const auto graph = valid_graph();
  auto manifest = make_manifest(graph.ordered_source_files);
  manifest[manifest.size() - 2] = manifest[manifest.size() - 2] == '1' ? '2' : '1';
  auto result = generator::generate_from_graph(graph, manifest, valid_inputs(), test_options());
  check(!result && result.error().code == generator::ErrorCode::manifest_hash_mismatch,
        "mutated manifest rows fail their aggregate");

  auto reordered = graph;
  std::swap(reordered.ordered_source_files[0], reordered.ordered_source_files[1]);
  result = generator::generate_from_graph(reordered, make_manifest(graph.ordered_source_files),
                                          valid_inputs(), test_options());
  check(!result && result.error().code == generator::ErrorCode::manifest_graph_mismatch,
        "manifest source order must match GROUP:all-code");

  const auto no_newline = make_manifest(graph.ordered_source_files)
                              .substr(0, make_manifest(graph.ordered_source_files).size() - 1);
  const auto parsed = generator::parse_source_object_pack_manifest(no_newline);
  check(!parsed, "manifest requires its exact final newline");
}

void test_retail_selection() {
  auto graph = valid_graph();
  graph.archives[0].destination_basename = "X.DGO";
  auto result = generator::generate_from_graph(graph, make_manifest(graph.ordered_source_files),
                                               valid_inputs(), test_options());
  check(bool(result), "public source provenance does not depend on output archive identity");
  if (result) {
    const auto& selected =
        std::get<recipe::VerifiedRetailObject>(result.value().archives[1].objects[1].source);
    check(selected.source_archive_relative_path == "DGO/A.DGO",
          "explicit public provenance selects the intended divergent retail source");
  }

  graph = valid_graph();
  graph.archives[0].objects[1].retail_source_archive = "DGO/Z.DGO";
  result = generator::generate_from_graph(graph, make_manifest(graph.ordered_source_files),
                                          valid_inputs(), test_options());
  check(bool(result), "another explicit public source can be selected exactly");
  if (result) {
    const auto& selected =
        std::get<recipe::VerifiedRetailObject>(result.value().archives[0].objects[1].source);
    check(selected.source_archive_relative_path == "DGO/Z.DGO",
          "selection follows public provenance rather than catalog order or hashes");
  }

  graph.archives[0].objects[1].retail_source_archive = "DGO/Y.DGO";
  result = generator::generate_from_graph(graph, make_manifest(graph.ordered_source_files),
                                          valid_inputs(), test_options());
  check(!result && result.error().code == generator::ErrorCode::missing_retail_object,
        "missing public retail provenance fails closed");

  graph = valid_graph();
  graph.archives[0].objects[1].retail_source_archive.clear();
  result = generator::generate_from_graph(graph, make_manifest(graph.ordered_source_files),
                                          valid_inputs(), test_options());
  check(!result && result.error().code == generator::ErrorCode::invalid_graph,
        "missing graph provenance fails closed before catalog selection");
}

void test_provenance_and_callbacks() {
  auto inputs = valid_inputs();
  inputs.revision.config_version = "ntsc_v2";
  auto result = generator::generate_from_graph(
      valid_graph(), make_manifest(valid_graph().ordered_source_files), inputs, test_options());
  check(!result && result.error().code == generator::ErrorCode::unsupported_revision,
        "non-ntsc_v1 revisions are rejected");

  inputs = valid_inputs();
  inputs.verified_extracted_iso_relative_paths.clear();
  result = generator::generate_from_graph(
      valid_graph(), make_manifest(valid_graph().ordered_source_files), inputs, test_options());
  check(!result && result.error().code == generator::ErrorCode::unverified_flat_source,
        "flat files must be present in the verified extracted-ISO inventory");

  generator::Options options;
  options.expected_source_object_count = 2;
  options.should_cancel = [] { return true; };
  result = generator::generate_from_graph(
      valid_graph(), make_manifest(valid_graph().ordered_source_files), valid_inputs(), options);
  check(!result && result.error().code == generator::ErrorCode::cancelled,
        "cancellation is surfaced");

  options.should_cancel = []() -> bool { throw std::runtime_error("callback"); };
  result = generator::generate_from_graph(
      valid_graph(), make_manifest(valid_graph().ordered_source_files), valid_inputs(), options);
  check(!result && result.error().code == generator::ErrorCode::callback_failed,
        "callback exceptions are contained");
}

void test_reserved_savegame_icon_destination() {
  auto graph = valid_graph();
  graph.archives[0].destination_basename = "savegame.ico";
  auto result = generator::generate_from_graph(graph, make_manifest(graph.ordered_source_files),
                                               valid_inputs(), test_options());
  check(!result && result.error().code == generator::ErrorCode::invalid_graph,
        "reserved archive destination is rejected case-insensitively");

  graph = valid_graph();
  graph.flat_file_copies[0].destination_basename = "SaveGame.Ico";
  result = generator::generate_from_graph(graph, make_manifest(graph.ordered_source_files),
                                          valid_inputs(), test_options());
  check(!result && result.error().code == generator::ErrorCode::invalid_graph,
        "reserved flat-file destination is rejected case-insensitively");

  graph = valid_graph();
  graph.generated_flat_files[0].destination_basename = "SAVEGAME.ICO";
  result = generator::generate_from_graph(graph, make_manifest(graph.ordered_source_files),
                                          valid_inputs(), test_options());
  check(!result && result.error().code == generator::ErrorCode::invalid_graph,
        "reserved generated destination is rejected case-insensitively");
}

}  // namespace

int main(int argc, char** argv) {
  test_success_and_round_trip();
  test_manifest_failures();
  test_retail_selection();
  test_provenance_and_callbacks();
  test_reserved_savegame_icon_destination();
  if (argc == 2) {
    std::ifstream input(argv[1], std::ios::binary);
    const std::string manifest((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    const auto parsed = generator::parse_source_object_pack_manifest(manifest);
    check(bool(parsed), "real source-object-pack manifest parses");
    if (parsed) {
      check(parsed.value().identity.object_count == 518 &&
                parsed.value().identity.aggregate_xxh64 == 0xf1770cae6287a359,
            "real source-object-pack manifest has the expected checked identity");
    }
  }
  if (failures) {
    std::fprintf(stderr, "%d Jak 1 output-recipe generator checks failed.\n", failures);
    return 1;
  }
  std::puts("All Jak 1 output-recipe generator checks passed.");
  return 0;
}
