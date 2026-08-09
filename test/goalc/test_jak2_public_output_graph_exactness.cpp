#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

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

jak1_output_recipe_generator::Options jak2_options() {
  jak1_output_recipe_generator::Options options;
  options.limits.max_graph_archives = jak2_public_output_graph::kArchiveLimit;
  options.expected_source_object_count = 840;
  options.public_provenance.game_name = "Jak II";
  options.public_provenance.all_objects_path = "goal_src/jak2/build/all_objs.json";
  options.public_provenance.decompiler_inputs_path = "decompiler/config/jak2/ntsc_v1/inputs.jsonc";
  return options;
}

std::size_t encoded_string_size(std::string_view value) {
  return sizeof(std::uint32_t) + value.size();
}

std::size_t public_metadata_wire_size(const jak1_output_graph::Graph& graph) {
  std::size_t result = jak1_output_graph::kJak2Magic.size() + sizeof(std::uint32_t) +
                       sizeof(std::uint64_t) + sizeof(std::uint64_t);

  result += sizeof(std::uint32_t);
  for (const auto& source : graph.ordered_source_files) {
    result += encoded_string_size(source);
  }
  result += sizeof(std::uint32_t);
  for (const auto& archive : graph.archives) {
    result += encoded_string_size(archive.destination_basename) + sizeof(std::uint32_t);
    for (const auto& object : archive.objects) {
      result += encoded_string_size(object.prepared_basename);
      result += encoded_string_size(object.internal_name);
      result += sizeof(std::uint8_t);
      result += encoded_string_size(object.producer ==
                                            jak1_output_graph::ObjectProducerKind::verified_retail
                                        ? std::string_view(object.retail_source_archive)
                                        : std::string_view("-"));
    }
  }
  result += sizeof(std::uint32_t);
  for (const auto& copy : graph.flat_file_copies) {
    result += encoded_string_size(copy.source_path);
    result += encoded_string_size(copy.destination_basename);
  }
  result += sizeof(std::uint32_t);
  for (const auto& generated : graph.generated_flat_files) {
    result += sizeof(std::uint8_t) + encoded_string_size(generated.destination_basename);
  }
  return result;
}

bool valid_archive_name(std::string_view value) {
  return value.ends_with(".DGO") || value.ends_with(".CGO");
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
    make_system.load_project_file(file_util::get_file_path({"goal_src/jak2/game.gp"}));
    const auto inspected =
        jak1_output_recipe_generator::inspect_make_system(make_system, jak2_options());
    if (!inspected) {
      std::fprintf(stderr, "Graph inspection failed: %s\n", inspected.error().message.c_str());
      return 1;
    }
    if (inspected.value().ordered_source_files.size() != 840) {
      std::fprintf(stderr, "Expected 840 ordered sources, found %zu.\n",
                   inspected.value().ordered_source_files.size());
      return 1;
    }

    const auto embedded = jak2_public_output_graph::decode();
    if (!embedded) {
      std::fprintf(stderr, "Embedded graph decode failed: %s\n", embedded.error().message.c_str());
      return 1;
    }
    if (embedded.value() != inspected.value()) {
      std::fputs("The embedded public graph differs from the live MakeSystem projection.\n",
                 stderr);
      return 1;
    }
    const auto base_retail = jak2_public_output_graph::decode_base_retail();
    if (!base_retail) {
      std::fprintf(stderr, "Base-retail graph projection failed: %s\n",
                   base_retail.error().message.c_str());
      return 1;
    }
    if (base_retail.value().archives.size() + 1 != embedded.value().archives.size() ||
        std::any_of(
            base_retail.value().archives.begin(), base_retail.value().archives.end(),
            [](const auto& archive) { return archive.destination_basename == "TSZ.DGO"; })) {
      std::fputs("The base-retail graph did not remove exactly the custom test-zone archive.\n",
                 stderr);
      return 1;
    }
    if (embedded.value().archives.empty() || embedded.value().flat_file_copies.empty() ||
        embedded.value().generated_flat_files.empty()) {
      std::fputs("The embedded graph does not cover archives and both flat-output classes.\n",
                 stderr);
      return 1;
    }
    if (!std::all_of(embedded.value().archives.begin(), embedded.value().archives.end(),
                     [](const auto& archive) {
                       return valid_archive_name(archive.destination_basename) &&
                              !archive.objects.empty();
                     })) {
      std::fputs("The embedded graph contains an invalid or empty archive.\n", stderr);
      return 1;
    }
    for (const auto& archive : base_retail.value().archives) {
      for (const auto& object : archive.objects) {
        if (object.producer == jak1_output_graph::ObjectProducerKind::custom_actor ||
            object.producer == jak1_output_graph::ObjectProducerKind::custom_level) {
          std::fputs("The base-retail graph contains a custom output producer.\n", stderr);
          return 1;
        }
        if (object.producer == jak1_output_graph::ObjectProducerKind::verified_retail &&
            !valid_archive_name(object.retail_source_archive)) {
          std::fputs("A retail object lacks public archive provenance.\n", stderr);
          return 1;
        }
      }
    }

    const auto encode_options = jak2_public_output_graph::default_options();
    const auto encoded = jak1_output_graph::encode(inspected.value(), encode_options);
    if (!encoded || encoded.value().size() != jak2_public_output_graph::wire_data().size() ||
        !std::equal(encoded.value().begin(), encoded.value().end(),
                    jak2_public_output_graph::wire_data().begin())) {
      std::fputs("The live graph does not reproduce the embedded wire data exactly.\n", stderr);
      return 1;
    }
    if (public_metadata_wire_size(embedded.value()) !=
        jak2_public_output_graph::wire_data().size()) {
      std::fputs("The embedded artifact contains bytes outside the checked metadata schema.\n",
                 stderr);
      return 1;
    }
    const auto wrong_game = jak1_output_graph::decode(jak2_public_output_graph::wire_data());
    if (wrong_game || wrong_game.error().code != jak1_output_graph::ErrorCode::wrong_magic) {
      std::fputs("The Jak II graph was not separated from the Jak 1 wire identity.\n", stderr);
      return 1;
    }

    std::printf(
        "Embedded Jak II public graph exactly matches %zu sources, %zu archives, %zu flat "
        "copies, %zu generated flat files, and %zu checked metadata bytes.\n",
        embedded.value().ordered_source_files.size(), embedded.value().archives.size(),
        embedded.value().flat_file_copies.size(), embedded.value().generated_flat_files.size(),
        jak2_public_output_graph::wire_data().size());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Graph exactness test failed: %s\n", error.what());
    return 1;
  }
}
