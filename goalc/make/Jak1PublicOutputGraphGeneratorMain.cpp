#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include "common/custom_data/Jak1OutputGraph.h"
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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fputs("usage: jak1-public-output-graph-generator <output.bin>\n", stderr);
    return 2;
  }
  if (!file_util::setup_project_path(fs::current_path(), true)) {
    std::fputs("Could not locate the OpenGOAL project root.\n", stderr);
    return 1;
  }
  try {
    MakeSystem make_system(std::nullopt);
    make_system.add_tool(std::make_shared<InspectOnlyCompilerTool>());
    make_system.load_project_file(file_util::get_file_path({"goal_src/jak1/game.gp"}));
    const auto graph = jak1_output_recipe_generator::inspect_make_system(make_system);
    if (!graph) {
      std::fprintf(stderr, "Graph inspection failed: %s\n", graph.error().message.c_str());
      return 1;
    }
    const auto encoded = jak1_output_graph::encode(graph.value());
    if (!encoded) {
      std::fprintf(stderr, "Graph encoding failed: %s\n", encoded.error().message.c_str());
      return 1;
    }
    if (!write_binary(argv[1], encoded.value())) {
      std::fputs("Could not atomically write the graph artifact.\n", stderr);
      return 1;
    }
    std::printf("Wrote %zu checked graph bytes to %s\n", encoded.value().size(), argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Graph generation failed: %s\n", error.what());
    return 1;
  }
}
