#pragma once

#include <cstdint>
#include <span>

#include "common/custom_data/Jak1OutputGraph.h"

namespace jak1_public_output_graph {

std::span<const std::uint8_t> wire_data();
jak1_output_graph::Result<jak1_output_graph::Graph> decode(
    const jak1_output_graph::Options& options = {});

}  // namespace jak1_public_output_graph
