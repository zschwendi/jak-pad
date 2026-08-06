#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    failures++;
  }
}

bool has_behavior(jak2::BucketId id, metal_renderer::Jak2MetalBucketBehavior behavior) {
  return metal_renderer::jak2_metal_bucket_table()[static_cast<std::size_t>(id)].behavior ==
         behavior;
}

}  // namespace

int main() {
  using Behavior = metal_renderer::Jak2MetalBucketBehavior;
  const auto& table = metal_renderer::jak2_metal_bucket_table();

  std::size_t deferred = 0;
  std::size_t strict_empty = 0;
  std::size_t direct = 0;
  bool contiguous = true;
  for (std::size_t i = 0; i < table.size(); i++) {
    contiguous &= table[i].id == i;
    deferred += table[i].behavior == Behavior::DeferredSkip;
    strict_empty += table[i].behavior == Behavior::StrictEmpty;
    direct += table[i].behavior == Behavior::Direct;
  }

  check(table.size() == 327 && contiguous, "the Jak 2 table covers 327 contiguous bucket IDs");
  check(deferred == 198, "198 OpenGL-bound buckets remain deferred for Metal");
  check(strict_empty == 127, "127 unbound buckets use strict-empty descriptor policy");
  check(direct == 2, "two reviewed OpenGL-bound buckets are implemented by Metal Direct");
  check(metal_renderer::jak2_metal_bucket_table_fingerprint() ==
            metal_renderer::kJak2MetalBucketExpectedFingerprint,
        "the ordered descriptor policy matches its fixed reference fingerprint");

  check(has_behavior(jak2::BucketId::BUCKET_2, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::SKY_DRAW, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::GMERC_L5_TFRAG, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::GMERC_L5_SHRUB, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::GMERC_L5_ALPHA, Behavior::DeferredSkip),
        "frame setup, sky, and level renderer bindings are deferred");
  check(has_behavior(jak2::BucketId::SHADOW, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::GMERC_L5_PRIS2, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::ETIE_W_L5_WATER, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::DEBUG3, Behavior::DeferredSkip),
        "common, prismatic, water, and tail bindings are deferred");
  check(has_behavior(jak2::BucketId::SCREEN_FILTER, Behavior::Direct) &&
            has_behavior(jak2::BucketId::DEBUG_NO_ZBUF2, Behavior::Direct),
        "SCREEN_FILTER and DEBUG_NO_ZBUF2 are the explicit implemented Direct buckets");
  check(static_cast<std::size_t>(jak2::BucketId::DEBUG_NO_ZBUF2) == 325,
        "DEBUG_NO_ZBUF2 retains its verified Jak 2 bucket ID 325");

  check(has_behavior(jak2::BucketId::BUCKET_0, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::TFRAG_S_L0_TFRAG, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::EMERC_L0_ALPHA, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::DEPTH_CUE, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::BUCKET_323, Behavior::StrictEmpty),
        "representative unbound buckets are strict-empty");

  const auto strict_id = static_cast<std::size_t>(jak2::BucketId::TFRAG_S_L0_TFRAG);
  const auto deferred_id = static_cast<std::size_t>(jak2::BucketId::SKY_DRAW);
  check(!metal_renderer::jak2_metal_bucket_allows_content(strict_id),
        "strict-empty policy does not allow content");
  check(metal_renderer::jak2_metal_bucket_allows_content(deferred_id),
        "deferred policy allows content for later implementation");
  check(metal_renderer::jak2_metal_bucket_allows_content(
            static_cast<std::size_t>(jak2::BucketId::SCREEN_FILTER)),
        "implemented Direct policy allows content");
  check(metal_renderer::jak2_metal_bucket_allows_content(
            static_cast<std::size_t>(jak2::BucketId::DEBUG_NO_ZBUF2)),
        "the promoted DEBUG_NO_ZBUF2 Direct policy allows content");
  check(!metal_renderer::jak2_metal_bucket_allows_content(table.size()),
        "policy rejects an out-of-range bucket ID");

  std::size_t direct_batches = 0;
  for (std::size_t i = 0; i < table.size(); i++) {
    direct_batches += metal_renderer::jak2_metal_direct_batch_size(i) != 0;
  }
  check(direct_batches == 2 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::SCREEN_FILTER)) == 256 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::DEBUG_NO_ZBUF2)) == 0x8000 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::SKY_DRAW)) == 0 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::DEBUG3)) == 0,
        "only the two Direct bindings receive their OpenGL reference batch sizes");
  check(metal_renderer::jak2_metal_direct_batch_size(table.size()) == 0,
        "out-of-range buckets do not receive a Direct binding");

  if (failures) {
    std::printf("FAIL: %d Jak 2 Metal bucket table checks failed\n", failures);
    return 1;
  }
  std::printf("PASS: Jak 2 Metal descriptor policy matches the 327-entry OpenGL reference\n");
  return 0;
}
