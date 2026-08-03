/*!
 * @file jak2_dma_boundary_test.cpp
 * Prove the headless Jak 2 graphics frontier recognizes its exact 327-entry bucket array. The
 * synthetic chains contain no game data, and the seam measures and drops them without rendering.
 */

#include <cstdio>
#include <cstring>

#include "common/dma/dma.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/dma_capture.h"
#include "game/kernel/core/gfx_host.h"
#include "game/kernel/core/kernel_core.h"
#include "game/runtime.h"

namespace {

constexpr int kBucketCount = (int)jak2::BucketId::MAX_BUCKETS;
static_assert(kBucketCount == 327);

u32 g_chain = 0;
u32 g_payload_chain = 0;
int g_failures = 0;

void expect(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) {
    g_failures++;
  }
}

void put_tag(u32 at, DmaTag::Kind kind, u16 qwc, u32 addr, u32 vif0 = 0, u32 vif1 = 0) {
  const u64 tag = (u64)qwc | ((u64)kind << 28) | ((u64)addr << 32);
  auto* memory = (u8*)g_ee_main_mem;
  std::memcpy(memory + at, &tag, sizeof(tag));
  std::memcpy(memory + at + 8, &vif0, sizeof(vif0));
  std::memcpy(memory + at + 12, &vif1, sizeof(vif1));
}

void build_empty_bucket_chain() {
  auto* memory = (u8*)g_ee_main_mem;
  std::memset(memory + g_chain, 0, 0x5000);
  for (int bucket = 0; bucket < kBucketCount; bucket++) {
    put_tag(g_chain + (u32)bucket * 16, DmaTag::Kind::CNT, 0, 0);
  }
  put_tag(g_chain + (u32)kBucketCount * 16, DmaTag::Kind::END, 0, 0);
}

void build_payload_bucket_chain() {
  build_empty_bucket_chain();

  // Jak 2's bucket array starts directly at the chain pointer. Bucket 0 jumps to content, which
  // returns to bucket 1; there is no Jak 1 default-register CALL/RET envelope.
  put_tag(g_chain, DmaTag::Kind::NEXT, 0, g_payload_chain);
  const u32 pc_port_vif0 = (u32)VifCode::Kind::PC_PORT << 24;
  put_tag(g_payload_chain, DmaTag::Kind::CNT, 1, 0, pc_port_vif0, 3);
  const u64 texture_page = g_chain;
  const u64 mode = 0;
  auto* memory = (u8*)g_ee_main_mem;
  std::memcpy(memory + g_payload_chain + 16, &texture_page, sizeof(texture_page));
  std::memcpy(memory + g_payload_chain + 24, &mode, sizeof(mode));
  put_tag(g_payload_chain + 32, DmaTag::Kind::NEXT, 0, g_chain + 16);
}

void build_short_bucket_chain() {
  build_empty_bucket_chain();
  put_tag(g_chain + (u32)(kBucketCount - 1) * 16, DmaTag::Kind::END, 0, 0);
}

}  // namespace

int main() {
  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: initialize: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  if (goal_kernel_core_stub_machine_layer(1) != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: machine seams: %s\n", goal_kernel_core_last_error());
    goal_kernel_core_shutdown();
    return 1;
  }

  u32 stub_send_chain = 0;
  expect(goal_kernel_core_lookup("__send-gfx-dma-chain", nullptr, &stub_send_chain) ==
                 GOAL_KERNEL_CORE_OK &&
             stub_send_chain != 0,
         "machine layer first installed the diagnostic graphics-DMA stub");

  if (goal_kernel_core_global_alloc(0x5000, "jak2-dma-boundary-test", &g_chain) !=
      GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: chain allocation: %s\n", goal_kernel_core_last_error());
    goal_kernel_core_shutdown();
    return 1;
  }
  g_payload_chain = g_chain + 0x4000;

  goal_gfx_dma_install();
  u32 send_chain = 0;
  expect(goal_kernel_core_lookup("__send-gfx-dma-chain", nullptr, &send_chain) ==
                 GOAL_KERNEL_CORE_OK &&
             send_chain != 0 && send_chain != stub_send_chain,
         "installed the game-neutral graphics-DMA measurement function");
  if (!send_chain) {
    goal_kernel_core_shutdown();
    return 1;
  }

  build_empty_bucket_chain();
  expect(goal_gfx_dma_capture_chain_now(g_ee_main_mem, g_chain, 1, "must-not-write.gpdma") == 0,
         "refused to write a Jak 1-only GPDMACAP v2 file for Jak 2");
  goal_aot_call(send_chain, 0x10009000, g_chain, 0);

  goal_gfx_dma_frame_summary frame = {};
  expect(goal_gfx_dma_get_frame(1, &frame) && frame.well_formed &&
             frame.tags == kBucketCount + 1 && frame.payload_bytes == 0 &&
             frame.copied_bytes > 0 && frame.buckets == kBucketCount,
         "validated exactly 327 empty Jak 2 buckets through END");

  build_payload_bucket_chain();
  goal_aot_call(send_chain, 0x10009000, g_chain, 0);
  expect(goal_gfx_dma_get_frame(2, &frame) && frame.well_formed &&
             frame.tags == kBucketCount + 3 && frame.payload_bytes == 16 &&
             frame.texture_uploads == 1 && frame.copied_bytes > 0 &&
             frame.buckets == kBucketCount,
         "measured payload inside one of the 327 Jak 2 buckets");

  goal_gfx_dma_stats stats = {};
  goal_gfx_dma_get_stats(&stats);
  expect(stats.chains == 2 && stats.well_formed_chains == 2 && stats.malformed_chains == 0,
         "reported two well-formed chains and no malformed chains");
  expect(stats.last_payload_bytes == 16 && stats.last_texture_uploads == 1 &&
             stats.last_bytes != 0 && stats.captures == 0 && stats.captured_bytes == 0,
         "retained DMA measurements without writing a Jak 2 capture");

  build_short_bucket_chain();
  goal_aot_call(send_chain, 0x10009000, g_chain, 0);
  expect(goal_gfx_dma_get_frame(3, &frame) && !frame.well_formed &&
             frame.tags == kBucketCount && frame.buckets == 0,
         "rejected a chain that ended before all 327 buckets completed");

  goal_gfx_dma_get_stats(&stats);
  expect(stats.chains == 3 && stats.well_formed_chains == 2 && stats.malformed_chains == 1 &&
             stats.chains == stats.well_formed_chains + stats.malformed_chains,
         "accounted for every accepted and malformed chain exactly once");

  goal_gfx_dma_reset();
  goal_gfx_host host = {};
  host.send_chain = goal_gfx_dma_observe_chain;
  expect(goal_gfx_host_install(&host) == GOAL_KERNEL_CORE_OK,
         "installed DMA measurement as a graphics-host observer");
  expect(goal_kernel_core_lookup("__send-gfx-dma-chain", nullptr, &send_chain) ==
                 GOAL_KERNEL_CORE_OK &&
             send_chain != 0,
         "graphics host owns the send-chain symbol while the observer measures behind it");
  build_empty_bucket_chain();
  goal_aot_call(send_chain, 0x10009000, g_chain, 0);
  goal_gfx_dma_get_stats(&stats);
  expect(stats.chains == 1 && stats.well_formed_chains == 1 && stats.malformed_chains == 0,
         "host observer measured one well-formed 327-bucket chain without symbol competition");

  goal_kernel_core_shutdown();
  std::printf("\n%s (%d failures)\n",
              g_failures ? "JAK 2 DMA BOUNDARY TEST FAILED" : "JAK 2 DMA BOUNDARY TEST PASSED",
              g_failures);
  return g_failures ? 1 : 0;
}
