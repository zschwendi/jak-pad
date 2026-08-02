#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>

#include "common/custom_data/Jak1PublicOutputGraph.h"
#include "common/util/FileUtil.h"

#include "goalc/make/Jak1OutputRecipeMakeSystem.h"
#include "goalc/make/MakeSystem.h"

namespace {

class InspectOnlyCompilerTool final : public Tool {
 public:
  InspectOnlyCompilerTool() : Tool("goalc") {}
  bool run(const ToolInput&, const PathMap&) override { return false; }
};

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
    const auto inspected = jak1_output_recipe_generator::inspect_make_system(make_system);
    if (!inspected) {
      std::fprintf(stderr, "Graph inspection failed: %s\n", inspected.error().message.c_str());
      return 1;
    }
    const auto embedded = jak1_public_output_graph::decode();
    if (!embedded) {
      std::fprintf(stderr, "Embedded graph decode failed: %s\n", embedded.error().message.c_str());
      return 1;
    }
    if (embedded.value() != inspected.value()) {
      std::fputs("The embedded public graph differs from the live MakeSystem projection.\n",
                 stderr);
      return 1;
    }
    bool found_tsz_plat_ag = false;
    for (const auto& archive : embedded.value().archives) {
      if (archive.destination_basename != "TSZ.DGO") {
        continue;
      }
      for (const auto& object : archive.objects) {
        if (object.prepared_basename != "plat-ag.go") {
          continue;
        }
        found_tsz_plat_ag = true;
        if (object.retail_source_archive != "DGO/ROB.DGO") {
          std::fprintf(stderr, "TSZ plat-ag.go source mismatch: expected DGO/ROB.DGO, found %s.\n",
                       object.retail_source_archive.c_str());
          return 1;
        }
      }
    }
    if (!found_tsz_plat_ag) {
      std::fputs("The embedded graph does not contain the TSZ plat-ag.go collision case.\n",
                 stderr);
      return 1;
    }
    const auto encoded = jak1_output_graph::encode(inspected.value());
    if (!encoded || !std::equal(encoded.value().begin(), encoded.value().end(),
                                jak1_public_output_graph::wire_data().begin(),
                                jak1_public_output_graph::wire_data().end())) {
      std::fputs("The live graph does not reproduce the embedded wire data exactly.\n", stderr);
      return 1;
    }
    std::printf(
        "Embedded Jak 1 public graph exactly matches %zu sources, %zu archives, %zu flat "
        "copies, and %zu generated flat files.\n",
        embedded.value().ordered_source_files.size(), embedded.value().archives.size(),
        embedded.value().flat_file_copies.size(), embedded.value().generated_flat_files.size());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Graph exactness test failed: %s\n", error.what());
    return 1;
  }
}
