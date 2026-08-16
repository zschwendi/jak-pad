#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include "common/custom_data/Jak1OutputGraph.h"
#include "common/custom_data/Jak2PublicOutputGraph.h"
#include "common/util/FileUtil.h"

#include "goalc/make/Jak1OutputRecipeMakeSystem.h"
#include "goalc/make/MakeSystem.h"

namespace {

class InspectOnlyCompilerTool final : public Tool {
 public:
  InspectOnlyCompilerTool() : Tool("goalc") {}
  bool run(const ToolInput&, const PathMap&) override { return false; }
};

bool write_binary(const fs::path& destination, std::span<const std::uint8_t> bytes) {
  auto stage = destination;
  stage += ".stage";
  std::error_code error;
  fs::remove(stage, error);
  error.clear();

  std::ofstream output(stage, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.flush();
  output.close();
  if (!output) {
    fs::remove(stage, error);
    return false;
  }
  fs::rename(stage, destination, error);
  if (error) {
    fs::remove(stage, error);
    return false;
  }
  return true;
}

jak1_output_recipe_generator::Options jak2_options() {
  jak1_output_recipe_generator::Options options;
  options.limits.max_graph_archives = jak2_public_output_graph::kArchiveLimit;
  options.expected_source_object_count = 840;
  options.public_provenance.game_name = "Jak II";
  options.public_provenance.all_objects_path = "goal_src/jak2/build/all_objs.json";
  options.public_provenance.decompiler_inputs_path = "decompiler/config/jak2/ntsc_v1/inputs.jsonc";
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fputs("usage: jak2-public-output-graph-generator <output.bin>\n", stderr);
    return 2;
  }
  if (!file_util::setup_project_path(fs::current_path(), true)) {
    std::fputs("Could not locate the OpenGOAL project root.\n", stderr);
    return 1;
  }
  try {
    MakeSystem make_system(std::nullopt);
    make_system.add_tool(std::make_shared<InspectOnlyCompilerTool>());
    make_system.load_project_file(file_util::get_file_path({"goal_src/jak2/game.gp"}));
    const auto graph =
        jak1_output_recipe_generator::inspect_make_system(make_system, jak2_options());
    if (!graph) {
      std::fprintf(stderr, "Graph inspection failed: %s", graph.error().message.c_str());
      if (graph.error().archive_index) {
        std::fprintf(stderr, " archive=%u", *graph.error().archive_index);
      }
      if (graph.error().object_index) {
        std::fprintf(stderr, " object=%u", *graph.error().object_index);
      }
      std::fputc('\n', stderr);
      return 1;
    }
    jak1_output_graph::Options encode_options;
    encode_options.limits.max_archives = jak2_public_output_graph::kArchiveLimit;
    encode_options.wire_game = jak1_output_graph::WireGame::jak2;
    const auto encoded = jak1_output_graph::encode(graph.value(), encode_options);
    if (!encoded) {
      std::fprintf(stderr, "Graph encoding failed: %s\n", encoded.error().message.c_str());
      return 1;
    }
    if (!write_binary(argv[1], encoded.value())) {
      std::fputs("Could not atomically write the graph artifact.\n", stderr);
      return 1;
    }
    std::printf("Wrote %zu checked Jak II graph bytes to %s\n", encoded.value().size(), argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Graph generation failed: %s\n", error.what());
    return 1;
  }
}
