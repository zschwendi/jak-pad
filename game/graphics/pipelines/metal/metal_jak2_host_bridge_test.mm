#include <cstdio>
#include <cstring>

#include "common/dma/dma.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"
#include "game/kernel/core/kernel_core.h"
#include "game/runtime.h"

namespace {

constexpr u32 kChainOffset = 0x100000;
constexpr u32 kBucketCount = static_cast<u32>(jak2::BucketId::MAX_BUCKETS);

int failures = 0;

void check(bool condition, const char* message) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
  if (!condition) {
    failures++;
  }
}

void put_tag(u32 offset, DmaTag::Kind kind) {
  const u64 value = static_cast<u64>(kind) << 28;
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + offset, &value, sizeof(value));
}

void make_empty_chain() {
  static_assert(kBucketCount == 327);
  std::memset(static_cast<u8*>(g_ee_main_mem) + kChainOffset, 0,
              (kBucketCount + 1) * 16);
  for (u32 bucket = 0; bucket < kBucketCount; bucket++) {
    put_tag(kChainOffset + bucket * 16, DmaTag::Kind::CNT);
  }
  put_tag(kChainOffset + kBucketCount * 16, DmaTag::Kind::END);
}

}  // namespace

int main() {
  check(goal_kernel_core_initialize() == GOAL_KERNEL_CORE_OK,
        "initialized the Jak 2 kernel arena for the copied chain");
  if (failures) {
    return 1;
  }
  make_empty_chain();

  goal_jak2_metal_host* host = goal_jak2_metal_host_create();
  check(host != nullptr, "created the process-singleton Jak 2 Metal host");
  check(goal_jak2_metal_host_create() == nullptr,
        "rejected a second live Jak 2 Metal host");

  goal_gfx_host callbacks = {};
  check(goal_jak2_metal_host_copy_gfx_host(host, &callbacks),
        "copied the app-owned graphics callback table");
  check(callbacks.send_chain && callbacks.sync_path && callbacks.vsync,
        "the copied host contains every required synchronous callback");

  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  callbacks.sync_path();
  callbacks.vsync();
  goal_jak2_metal_host_metrics metrics = {};
  check(goal_jak2_metal_host_get_metrics(host, &metrics),
        "copied the host metrics after dispatch");
  check(metrics.chains == 1 && metrics.completed_command_buffers == 1 &&
            metrics.failed_chains == 0 && metrics.last_buckets_dispatched == kBucketCount,
        "one copied 327-bucket chain completed offscreen");
  check(metrics.command_buffers_committed == 1 && metrics.drawables_acquired == 0 &&
            metrics.draws == 0 && metrics.triangles == 0 && metrics.submissions == 0 &&
            metrics.presentations == 0,
        "nil-layer lifecycle committed without drawing, submitting, or presenting");

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
