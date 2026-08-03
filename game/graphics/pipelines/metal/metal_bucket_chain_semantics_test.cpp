#include "game/graphics/pipelines/metal/metal_bucket_chain_semantics.h"

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    failures++;
  }
}

DmaTag tag(DmaTag::Kind kind, u16 qwc) {
  return DmaTag((u64)qwc | ((u64)kind << 28));
}

}  // namespace

int main() {
  using Layout = metal_renderer::MetalBucketChainLayout;
  const auto jak1 = metal_renderer::bucket_chain_layout(GameVersion::Jak1);
  const auto jak2 = metal_renderer::bucket_chain_layout(GameVersion::Jak2);
  check(jak1 == Layout::Jak1DefaultRegs, "Jak 1 keeps its default-register CALL/RET layout");
  check(jak2 == Layout::Jak2Direct, "Jak 2 uses its direct 327-slot bucket layout");
  check(metal_renderer::bucket_chain_layout(GameVersion::Jak3) == Layout::Unsupported &&
            metal_renderer::bucket_chain_layout(GameVersion::JakX) == Layout::Unsupported,
        "unimplemented future-game layouts fail closed");

  const auto cnt0 = tag(DmaTag::Kind::CNT, 0);
  const auto cnt1 = tag(DmaTag::Kind::CNT, 1);
  const auto next0 = tag(DmaTag::Kind::NEXT, 0);
  check(metal_renderer::is_strict_empty_bucket_tag(jak2, cnt0),
        "Jak 2 accepts its single zero-qwc CNT empty-bucket slot");
  check(!metal_renderer::is_strict_empty_bucket_tag(jak1, cnt0),
        "Jak 1 does not use the later-game strict-empty shape");
  check(!metal_renderer::is_strict_empty_bucket_tag(jak2, cnt1) &&
            !metal_renderer::is_strict_empty_bucket_tag(jak2, next0),
        "Jak 2 rejects payload CNT and NEXT tags as strict-empty buckets");

  if (failures) {
    std::printf("FAIL: %d Metal bucket-chain semantic checks failed\n", failures);
    return 1;
  }
  std::printf("PASS: Metal bucket-chain semantics remain version-specific\n");
  return 0;
}
