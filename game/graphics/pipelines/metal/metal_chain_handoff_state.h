#pragma once

#include <string>
#include <utility>

#include "common/common_types.h"

namespace metal_renderer {

struct MetalChainHandoffState {
  bool pending = false;
  u64 rejected_chains = 0;
  std::string last_error;

  void queue() { pending = true; }

  void complete() { pending = false; }

  void reject(std::string error) {
    pending = false;
    rejected_chains++;
    last_error = std::move(error);
  }
};

}  // namespace metal_renderer
