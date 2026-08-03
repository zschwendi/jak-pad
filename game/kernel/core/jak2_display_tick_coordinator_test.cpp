/*!
 * @file jak2_display_tick_coordinator_test.cpp
 * Prove the host display gate drives one headless Jak 2 frame per accepted display callback.
 */

#include <cstdio>
#include <cstring>
#include <vector>

#include "common/dma/dma.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/display_tick_coordinator.h"
#include "game/kernel/core/dma_capture.h"
#include "game/kernel/core/gfx_host.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/kernel_game.h"
#include "game/runtime.h"

namespace {

constexpr int kBucketCount = (int)jak2::BucketId::MAX_BUCKETS;
static_assert(kBucketCount == 327);

enum Event {
  FrameStart = 1,
  SendChain = 2,
  SyncPath = 3,
  VblankHandler = 4,
  HostVsync = 5,
  FrameEnd = 6,
};

int g_failures = 0;
int g_frame_calls = 0;
int g_rendered_frames = 0;
int g_presented_frames = 0;
u32 g_chain = 0;
u32 g_send_chain = 0;
u32 g_sync_path = 0;
u32 g_syncv = 0;
std::vector<double> g_target_times;
std::vector<int> g_events;

void expect(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) {
    g_failures++;
  }
}

u32 lookup(const char* name) {
  u32 value = 0;
  return goal_kernel_core_lookup(name, nullptr, &value) == GOAL_KERNEL_CORE_OK ? value : 0;
}

void put_tag(u32 at, DmaTag::Kind kind, u16 qwc, u32 addr) {
  const u64 tag = (u64)qwc | ((u64)kind << 28) | ((u64)addr << 32);
  auto* memory = (u8*)g_ee_main_mem;
  const u32 vif = 0;
  std::memcpy(memory + at, &tag, sizeof(tag));
  std::memcpy(memory + at + 8, &vif, sizeof(vif));
  std::memcpy(memory + at + 12, &vif, sizeof(vif));
}

void build_empty_bucket_chain() {
  std::memset((u8*)g_ee_main_mem + g_chain, 0, 0x2000);
  for (int bucket = 0; bucket < kBucketCount; bucket++) {
    put_tag(g_chain + (u32)bucket * 16, DmaTag::Kind::CNT, 0, 0);
  }
  put_tag(g_chain + (u32)kBucketCount * 16, DmaTag::Kind::END, 0, 0);
}

u64 retained_handler() {
  g_events.push_back(VblankHandler);
  return 0;
}

void observe_chain(const void* ee_base, u32 chain_offset) {
  g_events.push_back(SendChain);
  goal_gfx_dma_observe_chain(ee_base, chain_offset);
}

u32 sync_path() {
  g_events.push_back(SyncPath);
  return 0;
}

u32 vsync() {
  g_events.push_back(HostVsync);
  return (u32)(g_frame_calls & 1);
}

void run_frame(double target_presentation_time, void*) {
  g_frame_calls++;
  g_target_times.push_back(target_presentation_time);
  g_events.push_back(FrameStart);
  goal_aot_call(g_send_chain, 0x10009000, g_chain, 0);
  goal_aot_call(g_sync_path, 0, 0, 0);
  goal_aot_call(g_syncv, 0, 0, 0);
  g_events.push_back(FrameEnd);
}

std::vector<int> expected_events(int frame_count) {
  std::vector<int> result;
  for (int frame = 0; frame < frame_count; frame++) {
    result.insert(result.end(),
                  {FrameStart, SendChain, SyncPath, VblankHandler, HostVsync, FrameEnd});
  }
  return result;
}

}  // namespace

int main() {
  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK ||
      goal_kernel_core_stub_machine_layer(0) != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: setup: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  if (goal_kernel_core_global_alloc(0x2000, "jak2-display-tick-test", &g_chain) !=
      GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: chain allocation: %s\n", goal_kernel_core_last_error());
    goal_kernel_core_shutdown();
    return 1;
  }
  build_empty_bucket_chain();

  goal_gfx_dma_reset();
  goal_gfx_host host = {};
  host.send_chain = observe_chain;
  host.sync_path = sync_path;
  host.vsync = vsync;
  expect(goal_gfx_host_install(&host) == GOAL_KERNEL_CORE_OK,
         "installed the synchronous headless graphics host");

  g_send_chain = lookup("__send-gfx-dma-chain");
  g_sync_path = lookup("sync-path");
  g_syncv = lookup("syncv");
  expect(g_send_chain && g_sync_path && g_syncv,
         "resolved the real send-chain and synchronization functions");

  MasterExit = RuntimeExitStatus::RUNNING;
  vblank_interrupt_handler = goal_game_make_function_from_native((void*)retained_handler);

  goal_display_tick_coordinator coordinator = {};
  goal_display_tick_coordinator_init(&coordinator, run_frame, nullptr);
  goal_display_tick_coordinator_set_foreground(&coordinator, 1);
  expect(goal_display_tick_coordinator_tick(&coordinator, 1.0) == 1 &&
             goal_display_tick_coordinator_tick(&coordinator, 1.016) == 1 &&
             g_frame_calls == 2,
         "foreground display callbacks each run exactly one frame");

  goal_gfx_host_stats host_before_pause = {};
  goal_gfx_dma_stats dma_before_pause = {};
  goal_gfx_host_stats_get(&host_before_pause);
  goal_gfx_dma_get_stats(&dma_before_pause);

  goal_display_tick_coordinator_set_foreground(&coordinator, 0);
  bool dropped_all = true;
  for (int tick = 0; tick < 100; tick++) {
    dropped_all &= goal_display_tick_coordinator_tick(&coordinator, 2.0 + tick) == 0;
  }
  goal_gfx_host_stats host_after_pause = {};
  goal_gfx_dma_stats dma_after_pause = {};
  goal_gfx_host_stats_get(&host_after_pause);
  goal_gfx_dma_get_stats(&dma_after_pause);
  expect(dropped_all && g_frame_calls == 2 && host_after_pause.chains == host_before_pause.chains &&
             host_after_pause.sync_paths == host_before_pause.sync_paths &&
             host_after_pause.vsyncs == host_before_pause.vsyncs &&
             dma_after_pause.chains == dma_before_pause.chains,
         "background drops display callbacks without host or DMA work");

  goal_display_tick_coordinator_set_foreground(&coordinator, 1);
  expect(goal_display_tick_coordinator_tick(&coordinator, 1000.0) == 1 && g_frame_calls == 3,
         "resume runs one frame without catching up the paused callbacks");
  expect(goal_display_tick_coordinator_tick(&coordinator, 2000.0) == 1 && g_frame_calls == 4,
         "a later timestamp still drives only one frame");

  goal_display_tick_stats coordinator_stats = {};
  goal_display_tick_coordinator_get_stats(&coordinator, &coordinator_stats);
  expect(coordinator_stats.display_ticks == 104 && coordinator_stats.accepted_ticks == 4 &&
             coordinator_stats.paused_ticks == 100,
         "coordinator accounts for four accepted and one hundred discarded ticks");
  expect(g_target_times == std::vector<double>({1.0, 1.016, 1000.0, 2000.0}),
         "accepted target timestamps pass through unchanged");
  expect(g_events == expected_events(4),
         "every frame orders vblank before host sync and completes before the next tick");

  goal_gfx_host_stats host_stats = {};
  goal_gfx_dma_stats dma_stats = {};
  goal_gfx_host_stats_get(&host_stats);
  goal_gfx_dma_get_stats(&dma_stats);
  expect(host_stats.chains == 4 && host_stats.sync_paths == 4 && host_stats.vsyncs == 4,
         "graphics host receives one chain and both sync calls per accepted tick");
  expect(dma_stats.chains == 4 && dma_stats.well_formed_chains == 4 &&
             dma_stats.malformed_chains == 0 && host_stats.chains == dma_stats.chains &&
             dma_stats.chains == (int)coordinator_stats.accepted_ticks,
         "host and DMA counts agree with all four accepted frames");
  bool all_frames_well_formed = true;
  for (int frame_number = 1; frame_number <= 4; frame_number++) {
    goal_gfx_dma_frame_summary frame = {};
    all_frames_well_formed &= goal_gfx_dma_get_frame(frame_number, &frame) && frame.well_formed &&
                              frame.tags == kBucketCount + 1 && frame.buckets == kBucketCount &&
                              frame.payload_bytes == 0;
  }
  expect(all_frames_well_formed, "each accepted frame measures one complete synthetic chain");
  expect(g_rendered_frames == 0 && g_presented_frames == 0,
         "headless coordinator proof renders and presents zero frames");

  vblank_interrupt_handler = 0;
  goal_kernel_core_shutdown();
  std::printf("\n%s: accepted=%llu paused=%llu host=%d dma=%d rendered=%d presented=%d\n",
              g_failures ? "JAK 2 DISPLAY TICK TEST FAILED" : "JAK 2 DISPLAY TICK TEST PASSED",
              (unsigned long long)coordinator_stats.accepted_ticks,
              (unsigned long long)coordinator_stats.paused_ticks, host_stats.chains,
              dma_stats.chains, g_rendered_frames, g_presented_frames);
  return g_failures ? 1 : 0;
}
