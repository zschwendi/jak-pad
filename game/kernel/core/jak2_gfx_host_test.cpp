/*!
 * @file jak2_gfx_host_test.cpp
 * Prove the portable graphics host preserves Jak 2's machine-layer contracts without a renderer.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/goal_constants.h"
#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/gfx_host.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/kernel_game.h"
#include "game/kernel/jak2/kmachine.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/runtime.h"

namespace {

int g_failures = 0;
std::vector<int> g_order;
std::vector<std::string> g_desired_levels;
std::vector<std::string> g_active_levels;
float g_alpha = -1.f;
int g_chain_calls = 0;
uint32_t g_sync_path_calls = 0;

void expect(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) {
    g_failures++;
  }
}

uint32_t lookup(const char* name) {
  uint32_t value = 0;
  return goal_kernel_core_lookup(name, nullptr, &value) == GOAL_KERNEL_CORE_OK ? value : 0;
}

u64 retained_handler() {
  g_order.push_back(1);
  return 0;
}

void send_chain(const void* ee_base, uint32_t chain_offset) {
  expect(ee_base == g_ee_main_mem && chain_offset != 0,
         "Jak 2 send-chain receives the live EE arena and GOAL offset");
  g_chain_calls++;
}

uint32_t vsync() {
  g_order.push_back(2);
  return 1;
}

uint32_t sync_path() {
  g_sync_path_calls++;
  return 29;
}

void desired_levels(const char* const* names, int count) {
  g_desired_levels.assign(names, names + count);
}

void active_levels(const char* const* names, int count) {
  g_active_levels.assign(names, names + count);
}

void set_alpha(float alpha) {
  g_alpha = alpha;
}

}  // namespace

int main() {
  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK ||
      goal_kernel_core_stub_machine_layer(0) != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: setup: %s\n", goal_kernel_core_last_error());
    return 1;
  }

  const uint32_t flush_before = lookup("flush-cache");
  const uint32_t mouse_get_data = lookup("mouse-get-data");
  expect(flush_before && mouse_get_data,
         "Jak 2 lightweight flush and inactive mouse implementations are installed");

  uint32_t mouse_address = 0;
  expect(goal_kernel_core_global_alloc(128, "jak2-mouse-test", &mouse_address) ==
             GOAL_KERNEL_CORE_OK,
         "allocated a Jak 2 mouse object in the EE arena");
  std::memset(Ptr<u8>(mouse_address).c(), 0xab, 128);
  expect(goal_aot_call(mouse_get_data, mouse_address, 0, 0) == mouse_address,
         "inactive mouse returns the same object pointer");
  const auto* mouse = Ptr<jak2::MouseInfo>(mouse_address).c();
  expect(mouse->active == s7.offset && mouse->cursor == s7.offset &&
             mouse->valid == s7.offset && mouse->status == 0 && mouse->button0 == 0 &&
             mouse->deltax == 0 && mouse->deltay == 0 && mouse->wheel == 0 && mouse->posx == 0 &&
             mouse->posy == 0,
         "inactive mouse is deterministic, hidden, invalid and button-free");
  expect(mouse->id == 0xab && mouse->pad2[0] == 0xabababab,
         "inactive mouse preserves unrelated opaque state and history");

  goal_gfx_host host = {};
  host.send_chain = send_chain;
  host.vsync = vsync;
  host.sync_path = sync_path;
  host.set_levels = desired_levels;
  host.set_pmode_alp = set_alpha;
  host.set_active_levels = active_levels;
  expect(goal_gfx_host_install(&host) == GOAL_KERNEL_CORE_OK, "installed the Jak 2 graphics host");
  expect(lookup("flush-cache") == flush_before,
         "graphics host leaves Jak 2's validated flush-cache implementation intact");

  const uint32_t send = lookup("__send-gfx-dma-chain");
  const uint32_t syncv = lookup("syncv");
  const uint32_t syncp = lookup("sync-path");
  const uint32_t set_desired = lookup("__pc-set-levels");
  const uint32_t set_active = lookup("__pc-set-active-levels");
  const uint32_t display = lookup("put-display-env");
  expect(send && syncv && syncp && set_desired && set_active && display,
         "all Jak 2 host functions hold native implementations");

  goal_aot_call(send, 0x10009000, mouse_address, 0);
  expect(g_chain_calls == 1, "Jak 2 DMA chain forwards once through the common host");
  expect(goal_aot_call(syncp, 0, 0, 0) == 29 && g_sync_path_calls == 1,
         "Jak 2 sync-path forwards its host result");

  uint32_t desired_list = 0;
  uint32_t active_list = 0;
  expect(goal_kernel_core_global_alloc(jak2::LEVEL_MAX * sizeof(u32), "jak2-desired-levels",
                                       &desired_list) == GOAL_KERNEL_CORE_OK &&
             goal_kernel_core_global_alloc(jak2::LEVEL_MAX * sizeof(u32), "jak2-active-levels",
                                           &active_list) == GOAL_KERNEL_CORE_OK,
         "allocated distinct desired and active level arrays");
  const char* desired_names[jak2::LEVEL_MAX] = {"city", "stadium", "palace", "port", "strip",
                                                "slums"};
  const char* active_names[jak2::LEVEL_MAX] = {"hideout", "nest", "sew", "forest", "dig", "drill"};
  for (int i = 0; i < jak2::LEVEL_MAX; i++) {
    *Ptr<u32>(desired_list + i * sizeof(u32)) = (u32)jak2::make_string_from_c(desired_names[i]);
    *Ptr<u32>(active_list + i * sizeof(u32)) = (u32)jak2::make_string_from_c(active_names[i]);
  }
  goal_aot_call(set_desired, desired_list, 0, 0);
  goal_aot_call(set_active, active_list, 0, 0);
  expect(g_desired_levels.size() == jak2::LEVEL_MAX && g_desired_levels.front() == "city" &&
             g_desired_levels.back() == "slums",
         "Jak 2 desired-level adapter forwards all six names");
  expect(g_active_levels.size() == jak2::LEVEL_MAX && g_active_levels.front() == "hideout" &&
             g_active_levels.back() == "drill",
         "Jak 2 active-level adapter independently forwards all six names");

  goal_aot_call(display, 128, 0, 0);
  expect(std::fabs(g_alpha - 128.f / 255.f) < 0.0001f,
         "Jak 2 display adapter uses the direct alpha argument");

  MasterExit = RuntimeExitStatus::RUNNING;
  vblank_interrupt_handler = goal_game_make_function_from_native((void*)retained_handler);
  g_order.clear();
  expect(goal_aot_call(syncv, 0, 0, 0) == 1 && g_order == std::vector<int>({1, 2}),
         "Jak 2 syncv dispatches retained handler 3 once before host vsync");
  vblank_interrupt_handler = 0;
  g_order.clear();
  expect(goal_aot_call(syncv, 0, 0, 0) == 1 && g_order == std::vector<int>({2}),
         "Jak 2 syncv reaches the host directly when handler 3 is clear");

  goal_gfx_host_stats stats = {};
  goal_gfx_host_stats_get(&stats);
  expect(stats.chains == 1 && stats.vsyncs == 2 && stats.sync_paths == 1 &&
             stats.level_sets == 1 && stats.active_level_sets == 1 &&
             std::strcmp(stats.last_levels, "city+stadium+palace+port+strip+slums") == 0 &&
             std::strcmp(stats.last_active_levels, "hideout+nest+sew+forest+dig+drill") == 0,
         "Jak 2 stats keep desired and active level sets separate");

  vblank_interrupt_handler = goal_game_make_function_from_native((void*)retained_handler);
  goal_kernel_core_shutdown();
  expect(vblank_interrupt_handler == 0,
         "kernel shutdown clears the retained handler after graphics-host use");

  std::printf("\n%s (%d failures)\n",
              g_failures ? "JAK 2 GFX HOST TEST FAILED" : "JAK 2 GFX HOST TEST PASSED",
              g_failures);
  return g_failures ? 1 : 0;
}
