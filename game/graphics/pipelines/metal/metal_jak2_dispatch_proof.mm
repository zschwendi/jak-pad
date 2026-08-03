#include <cstdio>

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_jak2_test_packets.h"
#include "game/graphics/pipelines/metal/metal_renderer.h"
#include "game/graphics/texture/TexturePool.h"
#import <Metal/Metal.h>

namespace {

constexpr std::size_t kBucketCount = static_cast<std::size_t>(jak2::BucketId::MAX_BUCKETS);
static_assert(kBucketCount == 327);

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

    auto chain = metal_renderer::jak2_test::make_screen_filter_chain(0, true);

    MetalRenderOptions options;
    const bool acquired = renderer.render_chain_frame(options, nil, chain.data(), 0);
    const auto stats = renderer.chain_stats();

    check(!acquired, "nil CAMetalLayer acquires no drawable");
    check(stats.chains_rendered == 1 && stats.last_buckets_dispatched == kBucketCount,
          "the real Jak 2 dispatcher consumed all 327 synthetic slots");
    check(stats.command_buffers_committed == 0,
          "nil-layer dispatch commits no Metal command buffer");
    check(!renderer.wait_for_last_frame().had_command_buffer,
          "nil-layer dispatch leaves no command buffer for the completion barrier");
    check(stats.drawables_acquired == 0 && stats.drawable_misses == 0,
          "nil-layer dispatch performs no drawable acquisition attempt");
    check(stats.submissions == 0 && stats.presentations_completed == 0 &&
              stats.presentation_drops == 0,
          "nil-layer dispatch records zero submissions and presentations");
    check(stats.draw_calls == 1 && stats.triangles == 2 && stats.jak2_screen_filter_draws == 1 &&
              stats.jak2_screen_filter_triangles == 2,
          "SCREEN_FILTER encodes one audited sprite draw and two triangles");
    check(stats.skipped_bucket_bytes == 16,
          "one DeferredSkip slot consumes exactly its 16-byte synthetic payload");
    check(stats.direct_unsupported_blends == 0,
          "the SCREEN_FILTER Direct binding uses only supported blend state");
    check(metal_renderer::jak2_metal_bucket_table_fingerprint() ==
              metal_renderer::kJak2MetalBucketExpectedFingerprint,
          "the dispatcher links the reviewed 327-slot policy table");

    if (failures) {
      std::printf("FAIL: %d Jak 2 nil-layer Metal dispatcher checks failed\n", failures);
      return 1;
    }
    std::printf(
        "PASS: Jak 2 dispatched SCREEN_FILTER through all 327 slots without presentation\n");
    return 0;
  }
}
