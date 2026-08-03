#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "common/dma/dma.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_renderer.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>

namespace {

constexpr std::size_t kBucketCount = static_cast<std::size_t>(jak2::BucketId::MAX_BUCKETS);
constexpr std::size_t kChainBase = 0x100;
constexpr std::size_t kTerminalOffset = kChainBase + kBucketCount * 16;
constexpr std::size_t kPayloadBase = kTerminalOffset + 16;
constexpr std::size_t kPayloadStride = 48;
constexpr std::size_t kMarkerCount = 3;
constexpr std::size_t kChainSize = kPayloadBase + kMarkerCount * kPayloadStride;
constexpr std::array<jak2::BucketId, kMarkerCount> kMarkers = {
    jak2::BucketId::SKY_DRAW,
    jak2::BucketId::SHADOW,
    jak2::BucketId::DEBUG3,
};

static_assert(kBucketCount == 327);
static_assert(kChainSize <= std::numeric_limits<u32>::max());

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

DmaTag read_tag(const std::vector<u8>& chain, std::size_t offset) {
  u64 value = 0;
  std::memcpy(&value, chain.data() + offset, sizeof(value));
  return DmaTag(value);
}

std::vector<u8> make_chain(bool with_markers) {
  std::vector<u8> chain(kChainSize, 0);
  for (std::size_t bucket = 0; bucket < kBucketCount; bucket++) {
    put_tag(chain, kChainBase + bucket * 16, DmaTag::Kind::CNT);
  }
  put_tag(chain, kTerminalOffset, DmaTag::Kind::END);

  if (with_markers) {
    for (std::size_t marker = 0; marker < kMarkers.size(); marker++) {
      const auto bucket = static_cast<std::size_t>(kMarkers[marker]);
      const auto slot = kChainBase + bucket * 16;
      const auto payload = kPayloadBase + marker * kPayloadStride;
      put_tag(chain, slot, DmaTag::Kind::NEXT, 0, static_cast<u32>(payload));
      put_tag(chain, payload, DmaTag::Kind::CNT, 1);
      std::memset(chain.data() + payload + 16, static_cast<int>(0xa0 + marker), 16);
      put_tag(chain, payload + 32, DmaTag::Kind::NEXT, 0, static_cast<u32>(slot + 16));
    }
  }
  return chain;
}

bool validate_chain_addresses(const std::vector<u8>& chain, bool with_markers) {
  if (kChainBase == 0 || kChainBase % 16 != 0 || kTerminalOffset + 16 > chain.size()) {
    return false;
  }
  const auto terminal = read_tag(chain, kTerminalOffset);
  if (terminal.kind != DmaTag::Kind::END || terminal.qwc != 0) {
    return false;
  }
  if (!with_markers) {
    return true;
  }
  for (std::size_t marker = 0; marker < kMarkers.size(); marker++) {
    const auto bucket = static_cast<std::size_t>(kMarkers[marker]);
    const auto slot = kChainBase + bucket * 16;
    const auto payload = kPayloadBase + marker * kPayloadStride;
    if (bucket >= kBucketCount || slot + 16 > kTerminalOffset ||
        payload + kPayloadStride > chain.size()) {
      return false;
    }
    const auto from_slot = read_tag(chain, slot);
    const auto payload_tag = read_tag(chain, payload);
    const auto return_tag = read_tag(chain, payload + 32);
    if (from_slot.kind != DmaTag::Kind::NEXT || from_slot.addr != payload ||
        payload_tag.kind != DmaTag::Kind::CNT || payload_tag.qwc != 1 ||
        return_tag.kind != DmaTag::Kind::NEXT || return_tag.addr != slot + 16) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::printf("FAIL: no default Metal device is available\n");
      return 1;
    }

    const auto& table = metal_renderer::jak2_metal_bucket_table();
    check(table.size() == kBucketCount, "the product table contains exactly 327 slots");
    for (const auto marker : kMarkers) {
      check(metal_renderer::jak2_metal_bucket_allows_content(static_cast<std::size_t>(marker)),
            "each marker bucket is an audited DeferredSkip slot");
    }

    auto empty_chain = make_chain(false);
    auto marker_chain = make_chain(true);
    check(validate_chain_addresses(empty_chain, false),
          "the all-empty chain uses a valid nonzero base and terminal END");
    check(validate_chain_addresses(marker_chain, true),
          "all marker jumps, payloads, returns, and the terminal END are in bounds");
    if (failures) {
      return 1;
    }

    MetalRenderer renderer;
    check(renderer.init(device), "initialized the existing Metal renderer without a display");
    if (failures) {
      return 1;
    }

    TexturePool texture_pool(GameVersion::Jak2);
    renderer.init_bucket_renderers(&texture_pool, GameVersion::Jak2);

    MetalRenderOptions options;
    const bool empty_acquired =
        renderer.render_chain_frame(options, nil, empty_chain.data(), kChainBase);
    auto stats = renderer.chain_stats();
    check(!empty_acquired, "the all-empty nil-layer chain acquires no drawable");
    check(renderer.wait_for_last_frame(),
          "the all-empty nil-layer command buffer completes successfully");
    check(stats.chains_rendered == 1 && stats.last_buckets_dispatched == kBucketCount,
          "the first chain dispatches all 327 product slots");
    check(stats.command_buffers_committed == 1 && stats.drawable_misses == 1,
          "the first chain commits its offscreen pass and records the nil drawable miss");
    check(stats.skipped_bucket_bytes == 0 && stats.draw_calls == 0 && stats.triangles == 0,
          "the all-empty chain skips no payload and issues no draws");

    options.host_tick_id = 2;
    options.chain_ordinal = 1;
    const bool marker_acquired =
        renderer.render_chain_frame(options, nil, marker_chain.data(), kChainBase);
    stats = renderer.chain_stats();
    check(!marker_acquired, "the marker nil-layer chain acquires no drawable");
    check(renderer.wait_for_last_frame(),
          "the marker nil-layer command buffer completes successfully");
    check(stats.chains_rendered == 2 && stats.last_buckets_dispatched == kBucketCount,
          "the second chain also dispatches exactly 327 product slots");
    check(stats.command_buffers_committed == 2 && stats.drawable_misses == 2 &&
              stats.drawables_acquired == 0,
          "both nil-layer chains commit offscreen without acquiring a drawable");
    check(stats.submissions == 0 && stats.presentations_completed == 0 &&
              stats.presentation_drops == 0,
          "nil-layer dispatch records zero presentation submissions and presentations");
    check(stats.draw_calls == 0 && stats.triangles == 0 && stats.tex_uploads == 0 &&
              stats.direct_unsupported_blends == 0,
          "the policy-only dispatcher performs no rendering or texture work");
    check(stats.skipped_bucket_bytes == 48,
          "three DeferredSkip markers consume exactly 48 synthetic payload bytes");

    metal_renderer::FramePixels pixels;
    check(renderer.read_game_frame(&pixels),
          "the second nil-layer command buffer completes and its offscreen target is readable");
    check(metal_renderer::jak2_metal_bucket_table_fingerprint() ==
              metal_renderer::kJak2MetalBucketExpectedFingerprint,
          "the dispatcher links the reviewed 327-slot policy table unchanged");

    if (failures) {
      std::printf("FAIL: %d Jak 2 nil-layer Metal dispatcher checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak 2 dispatched two 327-slot policy chains with no presentation\n");
    return 0;
  }
}
