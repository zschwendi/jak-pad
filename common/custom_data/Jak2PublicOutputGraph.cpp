#include "Jak2PublicOutputGraph.h"

#include <algorithm>

#include "common/custom_data/Jak1OutputRecipe.h"

namespace jak2_public_output_graph {
namespace {

constexpr std::uint8_t kWireData[] = {
#include <Jak2PublicOutputGraphData.inc>
};

jak1_output_graph::Options jak2_options(const jak1_output_graph::Options& options) {
  auto result = options;
  result.wire_game = jak1_output_graph::WireGame::jak2;
  return result;
}

}  // namespace

jak1_output_graph::Options default_options() {
  jak1_output_graph::Options options;
  options.limits.max_archives = kArchiveLimit;
  options.wire_game = jak1_output_graph::WireGame::jak2;
  return options;
}

std::span<const std::uint8_t> wire_data() {
  return kWireData;
}

jak1_output_graph::Result<jak1_output_graph::Graph> decode(
    const jak1_output_graph::Options& options) {
  return jak1_output_graph::decode(wire_data(), jak2_options(options));
}

jak1_output_graph::Result<jak1_output_graph::Graph> decode_base_retail(
    const jak1_output_graph::Options& options) {
  auto decoded = decode(options);
  if (!decoded) {
    return decoded;
  }

  auto graph = decoded.take_value();
  const auto test_zone =
      std::find_if(graph.archives.begin(), graph.archives.end(),
                   [](const auto& archive) { return archive.destination_basename == "TSZ.DGO"; });
  if (test_zone == graph.archives.end()) {
    return jak1_output_graph::Result<jak1_output_graph::Graph>::failure(
        {jak1_output_graph::ErrorCode::invalid_graph,
         0,
         {},
         {},
         "The embedded Jak II graph does not contain the expected test-zone archive."});
  }

  bool has_custom_actor = false;
  bool has_custom_level = false;
  bool has_projected_source = false;
  for (const auto& object : test_zone->objects) {
    has_custom_actor |= object.producer == jak1_output_graph::ObjectProducerKind::custom_actor;
    has_custom_level |= object.producer == jak1_output_graph::ObjectProducerKind::custom_level;
    has_projected_source |=
        object.producer == jak1_output_graph::ObjectProducerKind::bundled_source &&
        object.internal_name == jak1_output_recipe::kBaseRetailProjectedSourceTag &&
        object.prepared_basename == jak1_output_recipe::kBaseRetailProjectedBundlePath;
  }
  if (!has_custom_actor || !has_custom_level || !has_projected_source) {
    return jak1_output_graph::Result<jak1_output_graph::Graph>::failure(
        {jak1_output_graph::ErrorCode::invalid_graph,
         0,
         {},
         {},
         "The embedded Jak II test-zone archive lacks its checked projected outputs."});
  }
  graph.archives.erase(test_zone);

  for (std::uint32_t archive_index = 0; archive_index < graph.archives.size(); ++archive_index) {
    const auto& archive = graph.archives[archive_index];
    for (std::uint32_t object_index = 0; object_index < archive.objects.size(); ++object_index) {
      const auto producer = archive.objects[object_index].producer;
      if (producer == jak1_output_graph::ObjectProducerKind::custom_actor ||
          producer == jak1_output_graph::ObjectProducerKind::custom_level) {
        return jak1_output_graph::Result<jak1_output_graph::Graph>::failure(
            {jak1_output_graph::ErrorCode::invalid_graph, 0, archive_index, object_index,
             "The Jak II base-retail graph contains a custom output."});
      }
    }
  }

  const auto validated = jak1_output_graph::encode(graph, jak2_options(options));
  if (!validated) {
    return jak1_output_graph::Result<jak1_output_graph::Graph>::failure(validated.error());
  }
  return jak1_output_graph::Result<jak1_output_graph::Graph>::success(std::move(graph));
}

}  // namespace jak2_public_output_graph
