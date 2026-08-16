#include <cstdio>

#include "common/common_types.h"

#include "game/graphics/pipelines/metal/metal_chain_handoff_state.h"

namespace {

int failures = 0;

void expect(bool condition, const char* description) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
  if (!condition) {
    failures++;
  }
}

}  // namespace

int main() {
  metal_renderer::MetalChainHandoffState state;
  state.queue();
  expect(state.pending, "queue marks one chain pending");

  state.reject("bounded DMA validation failed");
  expect(!state.pending, "rejection releases the pending chain");
  expect(state.rejected_chains == 1, "rejection is counted exactly once");
  expect(state.last_error == "bounded DMA validation failed", "rejection reason is retained");

  state.queue();
  expect(state.pending, "a valid chain may queue after a rejection");
  state.complete();
  expect(!state.pending, "successful completion releases the next chain");
  expect(state.rejected_chains == 1, "successful completion does not add a rejection");

  if (failures) {
    std::printf("FAIL: %d Metal chain handoff checks failed\n", failures);
    return 1;
  }
  std::printf("PASS: Metal chain handoff rejection state\n");
  return 0;
}
