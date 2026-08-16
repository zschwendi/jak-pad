#include <array>
#include <cstdio>
#include <cstring>

#include "common/dma/dma.h"

#include "game/graphics/pipelines/metal/metal_jak2_chain_validation.h"
#include "game/graphics/pipelines/metal/metal_jak2_synthetic_chain.h"

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
  const auto complete = metal_renderer::make_jak2_synthetic_metal_chain();
  expect(static_cast<bool>(
             metal_renderer::validate_jak2_metal_dma_chain(complete.data(), complete.size(), 0)),
         "complete synthetic chain reaches all 327 bucket boundaries");

  std::array<u8, 16> early_refe = {};
  const auto early =
      metal_renderer::validate_jak2_metal_dma_chain(early_refe.data(), early_refe.size(), 0);
  expect(!early && early.error == metal_renderer::Jak2MetalChainValidationError::EarlyTermination &&
             early.failed_bucket == 0,
         "structurally valid REFE before bucket 1 fails closed");

  std::array<u8, 16> cycle = {};
  const u64 next_self = static_cast<u64>(DmaTag::Kind::NEXT) << 28;
  std::memcpy(cycle.data(), &next_self, sizeof(next_self));
  const auto cyclic =
      metal_renderer::validate_jak2_metal_dma_chain(cycle.data(), cycle.size(), 0);
  expect(!cyclic && cyclic.error == metal_renderer::Jak2MetalChainValidationError::DmaChain &&
             cyclic.dma.error == DmaChainValidationError::Cycle,
         "cyclic chain fails structural validation before bucket traversal");

  if (failures) {
    std::printf("FAIL: %d Jak 2 Metal DMA boundary checks failed\n", failures);
    return 1;
  }
  std::printf("PASS: Jak 2 Metal DMA boundary validation\n");
  return 0;
}
