#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"

#include <array>
#include <cstdio>
#include <cstring>

#include "common/dma/dma.h"
#include "common/dma/gs.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/kernel/core/kernel_core.h"
#include "game/runtime.h"

namespace {

constexpr u32 kChainOffset = 0x100000;
constexpr u32 kBucketCount = static_cast<u32>(jak2::BucketId::MAX_BUCKETS);
constexpr u32 kScreenFilterBucket = static_cast<u32>(jak2::BucketId::SCREEN_FILTER);
constexpr u32 kScreenFilterPayloadOffset = kChainOffset + 0x4000;
constexpr std::size_t kGifQwords = 7;
constexpr std::size_t kGifBytes = kGifQwords * 16;

int failures = 0;

void check(bool condition, const char* message) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
  if (!condition) {
    failures++;
  }
}

void put_tag(u32 offset,
             DmaTag::Kind kind,
             u16 qwc = 0,
             u32 address = 0,
             u32 vif0 = 0,
             u32 vif1 = 0) {
  const u64 value =
      static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) | (static_cast<u64>(address) << 32);
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + offset, &value, sizeof(value));
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + offset + 8, &vif0, sizeof(vif0));
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + offset + 12, &vif1, sizeof(vif1));
}

void make_empty_chain() {
  static_assert(kBucketCount == 327);
  std::memset(static_cast<u8*>(g_ee_main_mem) + kChainOffset, 0, (kBucketCount + 1) * 16);
  for (u32 bucket = 0; bucket < kBucketCount; bucket++) {
    put_tag(kChainOffset + bucket * 16, DmaTag::Kind::CNT);
  }
  put_tag(kChainOffset + kBucketCount * 16, DmaTag::Kind::END);
}

void put_u64(std::array<u8, kGifBytes>& payload, std::size_t offset, u64 value) {
  std::memcpy(payload.data() + offset, &value, sizeof(value));
}

void put_rgbaq(std::array<u8, kGifBytes>& payload, std::size_t offset) {
  constexpr std::array<u32, 4> kGreen = {0, 255, 0, 128};
  std::memcpy(payload.data() + offset, kGreen.data(), 16);
}

void put_xyzf2(std::array<u8, kGifBytes>& payload, std::size_t offset, u32 x, u32 y) {
  constexpr u64 kZ = 0xffffff;
  std::memcpy(payload.data() + offset, &x, sizeof(x));
  std::memcpy(payload.data() + offset + 4, &y, sizeof(y));
  put_u64(payload, offset + 8, kZ << 4);
}

std::array<u8, kGifBytes> make_screen_filter_triangle() {
  std::array<u8, kGifBytes> payload = {};
  constexpr u64 kNloop = 1;
  constexpr u64 kEop = 1ull << 15;
  constexpr u64 kPre = 1ull << 46;
  constexpr u64 kPrim = static_cast<u64>(GsPrim::Kind::TRI) | (1ull << 3) | (1ull << 6);
  constexpr u64 kNreg = 6ull << 60;
  put_u64(payload, 0, kNloop | kEop | kPre | (kPrim << 47) | kNreg);

  constexpr u64 kRgbaq = static_cast<u64>(GifTag::RegisterDescriptor::RGBAQ);
  constexpr u64 kXyzf2 = static_cast<u64>(GifTag::RegisterDescriptor::XYZF2);
  constexpr u64 kRegisters =
      kRgbaq | (kXyzf2 << 4) | (kRgbaq << 8) | (kXyzf2 << 12) | (kRgbaq << 16) | (kXyzf2 << 20);
  put_u64(payload, 8, kRegisters);

  put_rgbaq(payload, 16);
  put_xyzf2(payload, 32, 0x8000, 0x7800);
  put_rgbaq(payload, 48);
  put_xyzf2(payload, 64, 0x7800, 0x8800);
  put_rgbaq(payload, 80);
  put_xyzf2(payload, 96, 0x8800, 0x8800);
  return payload;
}

void make_screen_filter_chain() {
  make_empty_chain();
  const auto payload = make_screen_filter_triangle();
  const u32 bucket_offset = kChainOffset + kScreenFilterBucket * 16;
  const u32 next_bucket_offset = bucket_offset + 16;
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kScreenFilterPayloadOffset);
  const u32 direct = (static_cast<u32>(VifCode::Kind::DIRECT) << 24) | static_cast<u32>(kGifQwords);
  put_tag(kScreenFilterPayloadOffset, DmaTag::Kind::CNT, static_cast<u16>(kGifQwords), 0, 0,
          direct);
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + kScreenFilterPayloadOffset + 16, payload.data(),
              payload.size());
  put_tag(kScreenFilterPayloadOffset + 16 + kGifBytes, DmaTag::Kind::NEXT, 0, next_bucket_offset);
}

bool is_zero(const goal_jak2_metal_frame_summary& summary) {
  return summary.width == 0 && summary.height == 0 && summary.byte_count == 0 &&
         summary.hash == 0 && summary.non_black_pixels == 0;
}

}  // namespace

int main() {
  check(goal_kernel_core_initialize() == GOAL_KERNEL_CORE_OK,
        "initialized the Jak 2 kernel arena for the copied chain");
  if (failures) {
    return 1;
  }
  make_empty_chain();

  check(goal_jak2_metal_host_create_presenting(nullptr) == nullptr,
        "rejected presenting mode without an app-owned CAMetalLayer");
  goal_jak2_metal_host* host = goal_jak2_metal_host_create();
  check(host != nullptr, "created the process-singleton Jak 2 Metal host");
  if (!host) {
    goal_kernel_core_shutdown();
    return 1;
  }
  check(goal_jak2_metal_host_create() == nullptr,
        "rejected a second live Jak 2 Metal host");

  goal_gfx_host callbacks = {};
  check(goal_jak2_metal_host_copy_gfx_host(host, &callbacks),
        "copied the app-owned graphics callback table");
  check(callbacks.send_chain && callbacks.sync_path && callbacks.vsync,
        "the copied host contains every required synchronous callback");

  goal_jak2_metal_frame_summary frame_summary = {1, 1, 1, 1, 1};
  check(!goal_jak2_metal_host_read_last_frame(host, &frame_summary) && is_zero(frame_summary),
        "nil-layer mode safely rejects readback before a frame exists");

  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  callbacks.sync_path();
  callbacks.vsync();
  goal_jak2_metal_host_metrics metrics = {};
  check(goal_jak2_metal_host_get_metrics(host, &metrics), "copied the host metrics after dispatch");
  check(metrics.chains == 1 && metrics.completed_chains == 1 && metrics.failed_chains == 0 &&
            metrics.last_buckets_dispatched == kBucketCount,
        "one copied 327-bucket chain completed policy dispatch");
  check(metrics.command_buffers_committed == 0 && metrics.command_buffers_completed == 0 &&
            metrics.command_buffer_errors == 0 && metrics.drawables_acquired == 0 &&
            metrics.drawable_misses == 0 && metrics.late_present_submissions == 0 &&
            metrics.draws == 0 && metrics.triangles == 0 && metrics.submissions == 0 &&
            metrics.last_screen_filter_draws == 0 && metrics.last_screen_filter_triangles == 0 &&
            metrics.presentations == 0 && metrics.presentation_drops == 0 &&
            metrics.presentation_order_mismatches == 0 && metrics.unsupported_blends == 0,
        "nil-layer lifecycle dispatches without committing, drawing, or presenting");
  check(!goal_jak2_metal_host_wait_for_last_frame(host, 0.01, 0),
        "nil-layer mode rejects a completion wait without changing its dispatch result");

  make_screen_filter_chain();
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(host, &metrics),
        "copied the host metrics after synthetic SCREEN_FILTER dispatch");
  check(metrics.chains == 2 && metrics.completed_chains == 2 && metrics.failed_chains == 0 &&
            metrics.last_buckets_dispatched == kBucketCount,
        "the synthetic SCREEN_FILTER chain completed all 327 policy buckets");
  check(metrics.draws == 1 && metrics.triangles == 1 && metrics.last_screen_filter_draws == 1 &&
            metrics.last_screen_filter_triangles == 1,
        "SCREEN_FILTER records its deterministic Direct draw and triangle");
  check(metrics.command_buffers_committed == 0 && metrics.command_buffers_completed == 0 &&
            metrics.command_buffer_errors == 0 && metrics.drawables_acquired == 0 &&
            metrics.drawable_misses == 0 && metrics.late_present_submissions == 0 &&
            metrics.submissions == 0 && metrics.presentations == 0 &&
            metrics.presentation_drops == 0 && metrics.presentation_order_mismatches == 0,
        "nil-layer SCREEN_FILTER drawing remains submission- and presentation-free");
  frame_summary = {1, 1, 1, 1, 1};
  check(!goal_jak2_metal_host_read_last_frame(host, &frame_summary) && is_zero(frame_summary),
        "nil-layer SCREEN_FILTER encoding still exposes no completed frame readback");

  goal_jak2_metal_host_destroy(host);
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(!goal_jak2_metal_host_get_metrics(host, &metrics),
        "a destroyed host no longer exposes state while stale callbacks remain inert");
  goal_jak2_metal_host* replacement = goal_jak2_metal_host_create();
  check(replacement != nullptr, "host ownership can be re-established after destruction");
  goal_jak2_metal_host_destroy(replacement);
  goal_kernel_core_shutdown();

  if (failures) {
    std::printf("FAIL: %d Jak 2 Metal host lifecycle checks failed\n", failures);
    return 1;
  }
  std::printf("PASS: Jak 2 external Metal host copied, dispatched, synchronized, and released\n");
  return 0;
}
