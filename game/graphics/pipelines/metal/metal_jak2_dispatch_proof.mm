#include <cstdio>
#include <cstring>
#include <vector>

#include "common/dma/dma.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
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

void put_tag(std::vector<u8>& chain,
             std::size_t offset,
             DmaTag::Kind kind,
             u16 qwc = 0,
             u32 address = 0) {
  const u64 value = static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                    (static_cast<u64>(address) << 32);
  std::memcpy(chain.data() + offset, &value, sizeof(value));
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

    constexpr std::size_t kDeferredBucket = static_cast<std::size_t>(jak2::BucketId::BUCKET_2);
    constexpr std::size_t kDirectBucket = static_cast<std::size_t>(jak2::BucketId::DEBUG3);
    constexpr std::size_t kDeferredPayloadOffset = (kBucketCount + 1) * 16;
    constexpr std::size_t kDirectPayloadOffset = kDeferredPayloadOffset + 48;
    std::vector<u8> chain(kDirectPayloadOffset + 48, 0);
    for (std::size_t bucket = 0; bucket < kBucketCount; bucket++) {
      put_tag(chain, bucket * 16, DmaTag::Kind::CNT);
    }
    put_tag(chain, kBucketCount * 16, DmaTag::Kind::END);

    // Distinguish an audited DeferredSkip binding from StrictEmpty without
    // requiring any game data: bucket 2 jumps to one qword of synthetic
    // payload and then returns to bucket 3.
    put_tag(chain, kDeferredBucket * 16, DmaTag::Kind::NEXT, 0, kDeferredPayloadOffset);
    put_tag(chain, kDeferredPayloadOffset, DmaTag::Kind::CNT, 1);
    std::memset(chain.data() + kDeferredPayloadOffset + 16, 0xa5, 16);
    put_tag(chain, kDeferredPayloadOffset + 32, DmaTag::Kind::NEXT, 0,
            static_cast<u32>((kDeferredBucket + 1) * 16));

    // DEBUG3 uses the existing Direct renderer. Its private qword contains four
    // VIF NOPs, so this proves Jak 2's Direct chain traversal without encoding
    // a GIF draw or making a pixel claim.
    put_tag(chain, kDirectBucket * 16, DmaTag::Kind::NEXT, 0, kDirectPayloadOffset);
    put_tag(chain, kDirectPayloadOffset, DmaTag::Kind::CNT, 1);
    put_tag(chain, kDirectPayloadOffset + 32, DmaTag::Kind::NEXT, 0,
            static_cast<u32>((kDirectBucket + 1) * 16));

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
