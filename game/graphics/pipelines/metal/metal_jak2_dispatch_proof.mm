#include <array>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_jak2_synthetic_chain.h"
#include "game/graphics/pipelines/metal/metal_renderer.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>

namespace {

int failures = 0;

std::vector<u8> make_policy_inventory_chain() {
  using BucketId = jak2::BucketId;
  constexpr std::array<std::pair<BucketId, u16>, 12> kPayloads = {{
      {BucketId::GMERC_L0_ALPHA, 5},
      {BucketId::GMERC_L0_WATER, 4},
      {BucketId::GMERC_L0_TFRAG, 5},
      {BucketId::GMERC_L0_SHRUB, 5},
      {BucketId::GMERC_LCOM_TFRAG, 5},
      {BucketId::GMERC_LCOM_PRIS, 5},
      {BucketId::MERC_L0_SHRUB, 2},
      {BucketId::MERC_LCOM_TFRAG, 2},
      {BucketId::MERC_LCOM_PRIS, 2},
      {BucketId::SHADOW, 3},
      {BucketId::GMERC_L5_PRIS2, 2},
      {BucketId::DEBUG3, 1},
  }};

  std::vector<u8> chain((metal_renderer::kJak2SyntheticBucketCount + 1) * 16, 0);
  for (std::size_t bucket = 0; bucket < metal_renderer::kJak2SyntheticBucketCount; bucket++) {
    metal_renderer::put_jak2_synthetic_tag(chain, bucket * 16, DmaTag::Kind::CNT);
  }
  metal_renderer::put_jak2_synthetic_tag(
      chain, metal_renderer::kJak2SyntheticBucketCount * 16, DmaTag::Kind::END);

  for (const auto& [bucket, qwc] : kPayloads) {
    const auto bucket_id = static_cast<std::size_t>(bucket);
    const auto payload_offset = chain.size();
    chain.resize(payload_offset + 16 + qwc * 16 + 16, 0);
    metal_renderer::put_jak2_synthetic_tag(chain, bucket_id * 16, DmaTag::Kind::NEXT, 0,
                                           static_cast<u32>(payload_offset));
    metal_renderer::put_jak2_synthetic_tag(chain, payload_offset, DmaTag::Kind::CNT, qwc);
    metal_renderer::put_jak2_synthetic_tag(chain, payload_offset + 16 + qwc * 16,
                                           DmaTag::Kind::NEXT, 0,
                                           static_cast<u32>((bucket_id + 1) * 16));
  }
  return chain;
}

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    failures++;
  }
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::printf("FAIL: no default Metal device is available\n");
      return 1;
    }

    TexturePool texture_pool(GameVersion::Jak2);
    MetalRenderer renderer;
    check(renderer.init(device), "initialized the existing Metal renderer without a display");
    if (failures) {
      return 1;
    }

    renderer.init_bucket_renderers(&texture_pool, GameVersion::Jak2);

    MetalRenderOptions options;
    std::array<u8, 16> early_refe = {};
    bool rejected_early_refe = false;
    try {
      renderer.render_chain_frame(options, nil, early_refe.data(), 0, early_refe.size());
    } catch (const std::runtime_error&) {
      rejected_early_refe = true;
    }
    check(rejected_early_refe,
          "a valid REFE before all 327 buckets fails closed before Metal dispatch");

    const auto chain = metal_renderer::make_jak2_synthetic_metal_chain();

    const bool acquired = renderer.render_chain_frame(options, nil, chain.data(), 0, chain.size());
    const auto stats = renderer.chain_stats();

    check(!acquired, "nil CAMetalLayer acquires no drawable");
    check(stats.chains_rendered == 1 &&
              stats.last_buckets_dispatched == metal_renderer::kJak2SyntheticBucketCount,
          "the real Jak 2 dispatcher consumed all 327 synthetic slots");
    check(stats.command_buffers_committed == 0 && stats.command_buffers_completed == 0 &&
              stats.command_buffer_errors == 0,
          "nil-layer dispatch commits and completes no Metal command buffer");
    check(stats.ocean_command_buffers_committed == 0 &&
              stats.ocean_command_buffers_completed == 0 &&
              stats.ocean_command_buffer_errors == 0,
          "deferred OCEAN buckets report no hidden private command buffers");
    check(stats.drawables_acquired == 0 && stats.drawable_misses == 0,
          "nil-layer dispatch performs no drawable acquisition attempt");
    check(stats.submissions == 0 && stats.presentations_completed == 0 &&
              stats.presentation_drops == 0,
          "nil-layer dispatch records zero submissions and presentations");
    check(stats.skipped_bucket_bytes == 16,
          "one DeferredSkip slot consumes exactly its 16-byte synthetic payload");
    check(stats.last_skipped_bucket_count == 1 &&
              stats.last_skipped_bucket_ids[0] ==
                  metal_renderer::kJak2SyntheticDeferredBucket &&
              stats.last_skipped_bucket_bytes[0] == 16,
          "the last-frame deferred inventory identifies the exact bucket and payload bytes");
    check(stats.draw_calls == 0 && stats.triangles == 0 && stats.jak2_screen_filter_draws == 0 &&
              stats.jak2_screen_filter_triangles == 0,
          "the SCREEN_FILTER Direct binding traverses its NOP payload without drawing");
    check(stats.direct_unsupported_blends == 0,
          "the SCREEN_FILTER Direct binding traverses its NOP payload without unsupported blends");
    check(metal_renderer::jak2_metal_bucket_table_fingerprint() ==
              metal_renderer::kJak2MetalBucketExpectedFingerprint,
          "the dispatcher links the reviewed 327-slot policy table");

    const auto inventory_chain = make_policy_inventory_chain();
    renderer.render_chain_frame(options, nil, inventory_chain.data(), 0, inventory_chain.size());
    const auto inventory = renderer.chain_stats();
    check(inventory.skipped_bucket_bytes == 16 + (3 + 2 + 1) * 16,
          "cumulative deferred bytes exclude implemented Merc and Generic2 buckets");
    check(inventory.last_skipped_bucket_count == 3 &&
              inventory.last_skipped_bucket_ids[0] ==
                  static_cast<u32>(jak2::BucketId::SHADOW) &&
              inventory.last_skipped_bucket_bytes[0] == 3 * 16 &&
              inventory.last_skipped_bucket_ids[1] ==
                  static_cast<u32>(jak2::BucketId::GMERC_L5_PRIS2) &&
              inventory.last_skipped_bucket_bytes[1] == 2 * 16 &&
              inventory.last_skipped_bucket_ids[2] ==
                  static_cast<u32>(jak2::BucketId::DEBUG3) &&
              inventory.last_skipped_bucket_bytes[2] == 1 * 16,
          "the last-frame deferred inventory excludes the implemented Generic2 buckets");
    check(inventory.generic_unexpected_dma == 6 && inventory.generic_draws == 0 &&
              inventory.generic_triangles == 0,
          "every routed normal-GMerc family rejects malformed DMA without drawing");
    check(inventory.merc_malformed_dma == 3 && inventory.merc_draws == 0 &&
              inventory.merc_triangles == 0,
          "every newly routed Merc family rejects malformed DMA without drawing");

    if (failures) {
      std::printf("FAIL: %d Jak 2 nil-layer Metal dispatcher checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak 2 dispatched 327 policy slots with no submission or presentation\n");
    return 0;
  }
}
