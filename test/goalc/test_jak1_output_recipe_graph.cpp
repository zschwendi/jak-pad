#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>

#include "common/util/FileUtil.h"

#include "goalc/make/Jak1OutputRecipeGenerator.h"
#include "goalc/make/MakeSystem.h"

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
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Graph audit threw: %s\n", error.what());
    return 1;
  }

  std::puts("Jak 1 GROUP:iso graph audit passed.");
  return 0;
}
