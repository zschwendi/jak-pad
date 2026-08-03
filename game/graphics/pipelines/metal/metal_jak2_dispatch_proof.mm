#include <cstdio>
#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_jak2_synthetic_chain.h"
#include "game/graphics/pipelines/metal/metal_renderer.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>

namespace {

int failures = 0;

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

    MetalRenderer renderer;
    check(renderer.init(device), "initialized the existing Metal renderer without a display");
    if (failures) {
      return 1;
    }

    TexturePool texture_pool(GameVersion::Jak2);
    renderer.init_bucket_renderers(&texture_pool, GameVersion::Jak2);

    const auto chain = metal_renderer::make_jak2_synthetic_metal_chain();

    MetalRenderOptions options;
    const bool acquired = renderer.render_chain_frame(options, nil, chain.data(), 0);
    const auto stats = renderer.chain_stats();

    check(!acquired, "nil CAMetalLayer acquires no drawable");
    check(stats.chains_rendered == 1 &&
              stats.last_buckets_dispatched == metal_renderer::kJak2SyntheticBucketCount,
          "the real Jak 2 dispatcher consumed all 327 synthetic slots");
    check(stats.command_buffers_committed == 0 && stats.command_buffers_completed == 0 &&
              stats.command_buffer_errors == 0,
          "nil-layer dispatch commits and completes no Metal command buffer");
    check(stats.drawables_acquired == 0 && stats.drawable_misses == 0,
          "nil-layer dispatch performs no drawable acquisition attempt");
    check(stats.submissions == 0 && stats.presentations_completed == 0 &&
              stats.presentation_drops == 0,
          "nil-layer dispatch records zero submissions and presentations");
    check(stats.draw_calls == 0 && stats.triangles == 0 && stats.skipped_bucket_bytes == 16,
          "one DeferredSkip slot consumes exactly its 16-byte synthetic payload");
    check(stats.direct_unsupported_blends == 0,
          "the DEBUG3 Direct binding traverses NOP payload without unsupported blends");
    check(metal_renderer::jak2_metal_bucket_table_fingerprint() ==
              metal_renderer::kJak2MetalBucketExpectedFingerprint,
          "the dispatcher links the reviewed 327-slot policy table");

    if (failures) {
      std::printf("FAIL: %d Jak 2 nil-layer Metal dispatcher checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak 2 dispatched 327 policy slots with no submission or presentation\n");
    return 0;
  }
}
