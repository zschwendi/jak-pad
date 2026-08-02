#include "Jak1PublicOutputGraph.h"

#include <array>

namespace jak1_public_output_graph {
namespace {

constexpr std::uint8_t kWireData[] = {
#include <Jak1PublicOutputGraphData.inc>
};

}  // namespace

std::span<const std::uint8_t> wire_data() {
  return kWireData;
}

jak1_output_graph::Result<jak1_output_graph::Graph> decode(
    const jak1_output_graph::Options& options) {
  return jak1_output_graph::decode(wire_data(), options);
}

}  // namespace jak1_public_output_graph
