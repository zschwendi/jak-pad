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

enum Event { VblankHandler = 1, HostVsync = 2 };

int g_failures = 0;
int g_chain_calls = 0;
int g_vsync_calls = 0;
int g_sync_path_calls = 0;
int g_desired_level_calls = 0;
int g_active_level_calls = 0;
float g_alpha = -1.f;
std::vector<int> g_order;
std::vector<std::string> g_desired_levels;
std::vector<std::string> g_active_levels;

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
  g_order.push_back(VblankHandler);
  return 99;
}

void send_chain(const void* ee_base, uint32_t chain_offset) {
  expect(ee_base == g_ee_main_mem && chain_offset != 0,
         "Jak 2 send-chain receives the live EE arena and GOAL offset");
  g_chain_calls++;
}

uint32_t vsync() {
  g_order.push_back(HostVsync);
  return (uint32_t)(1 - (g_vsync_calls++ & 1));
}

uint32_t sync_path() {
  g_sync_path_calls++;
  return 29;
}

void desired_levels(const char* const* names, int count) {
  g_desired_level_calls++;
  g_desired_levels.clear();
  for (int i = 0; i < count; i++) {
    g_desired_levels.emplace_back(names[i]);
  }
}

void active_levels(const char* const* names, int count) {
  g_active_level_calls++;
  g_active_levels.clear();
  for (int i = 0; i < count; i++) {
    g_active_levels.emplace_back(names[i]);
  }
}

void set_alpha(float alpha) {
  g_alpha = alpha;
}

goal_kernel_core_status install_host() {
  goal_gfx_host host = {};
  host.send_chain = send_chain;
  host.vsync = vsync;
  host.sync_path = sync_path;
  host.set_levels = desired_levels;
  host.set_pmode_alp = set_alpha;
  host.set_active_levels = active_levels;
  return goal_gfx_host_install(&host);
}

bool initialize_kernel() {
  return goal_kernel_core_initialize() == GOAL_KERNEL_CORE_OK &&
         goal_kernel_core_stub_machine_layer(0) == GOAL_KERNEL_CORE_OK;
}

bool allocate_level_list(const char* label,
                         const char* const (&names)[jak2::LEVEL_MAX],
                         uint32_t* out) {
  if (goal_kernel_core_global_alloc(jak2::LEVEL_MAX * sizeof(u32), label, out) !=
      GOAL_KERNEL_CORE_OK) {
    return false;
  }
  for (int i = 0; i < jak2::LEVEL_MAX; i++) {
    *Ptr<u32>(*out + i * sizeof(u32)) = (u32)jak2::make_string_from_c(names[i]);
  }
  return true;
}

void reset_observations() {
  g_chain_calls = 0;
  g_vsync_calls = 0;
  g_sync_path_calls = 0;
  g_desired_level_calls = 0;
  g_active_level_calls = 0;
  g_alpha = -1.f;
  g_order.clear();
  g_desired_levels.clear();
  g_active_levels.clear();
}

}  // namespace

int main() {
  expect(install_host() == GOAL_KERNEL_CORE_NOT_INITIALIZED,
         "graphics host rejects installation before the kernel owns an EE arena");
  if (!initialize_kernel()) {
    std::printf("FAIL: setup: %s\n", goal_kernel_core_last_error());
    return 1;
  }

  const uint32_t flush_before = lookup("flush-cache");
  const uint32_t mouse_before = lookup("mouse-get-data");
  expect(flush_before && mouse_before,
         "shared flush-cache and inactive mouse are installed before graphics");

  uint32_t mouse_address = 0;
  expect(goal_kernel_core_global_alloc(128, "jak2-mouse-test", &mouse_address) ==
             GOAL_KERNEL_CORE_OK,
         "allocated a Jak 2 mouse object in the EE arena");
  std::memset(Ptr<u8>(mouse_address).c(), 0xab, 128);
  expect(goal_aot_call(mouse_before, mouse_address, 0, 0) == mouse_address,
         "inactive mouse returns the same object pointer");
  const auto* mouse = Ptr<jak2::MouseInfo>(mouse_address).c();
  expect(mouse->active == s7.offset && mouse->cursor == s7.offset &&
             mouse->valid == s7.offset && mouse->status == 0 && mouse->button0 == 0 &&
             mouse->deltax == 0 && mouse->deltay == 0 && mouse->wheel == 0 &&
             mouse->posx == 0 && mouse->posy == 0,
         "inactive mouse is deterministic, hidden, invalid and button-free");
  expect(mouse->id == 0xab && mouse->pad2[0] == 0xabababab,
         "inactive mouse preserves unrelated opaque state and history");
  expect(install_host() == GOAL_KERNEL_CORE_OK,
         "installed a copied, renderer-neutral Jak 2 graphics host");
  expect(lookup("flush-cache") == flush_before && lookup("mouse-get-data") == mouse_before,
         "graphics host does not replace shared flush-cache or inactive mouse");

  const uint32_t send = lookup("__send-gfx-dma-chain");
  const uint32_t syncv = lookup("syncv");
  const uint32_t syncp = lookup("sync-path");
  const uint32_t set_desired = lookup("__pc-set-levels");
  const uint32_t set_active = lookup("__pc-set-active-levels");
  const uint32_t display = lookup("put-display-env");
  expect(send && syncv && syncp && set_desired && set_active && display,
         "all Jak 2 sync and residency functions hold native implementations");

  goal_aot_call(send, 0x10009000, mouse_address, 0);
  expect(g_chain_calls == 1, "Jak 2 DMA chain forwards once through the common host");

  MasterExit = RuntimeExitStatus::RUNNING;
  vblank_interrupt_handler = goal_game_make_function_from_native((void*)retained_handler);
  g_order.clear();
  expect(goal_aot_call(syncp, 0, 0, 0) == 29 && g_sync_path_calls == 1 && g_order.empty(),
         "sync-path returns its host result without dispatching the vblank handler");

  const char* desired_names[jak2::LEVEL_MAX] = {"title", "none", "#f", "", "city", "title"};
  const char* active_names[jak2::LEVEL_MAX] = {"hideout", "none", "#f", "", "nest", "hideout"};
  uint32_t desired_list = 0;
  uint32_t active_list = 0;
  expect(allocate_level_list("jak2-desired-levels", desired_names, &desired_list) &&
             allocate_level_list("jak2-active-levels", active_names, &active_list),
         "allocated distinct six-entry desired and active arrays");
  goal_aot_call(set_desired, desired_list, 0, 0);
  goal_aot_call(set_desired, desired_list, 0, 0);
  goal_aot_call(set_active, active_list, 0, 0);
  goal_aot_call(set_active, active_list, 0, 0);
  expect(g_desired_levels == std::vector<std::string>({"title", "city", "title"}) &&
             g_desired_level_calls == 2,
         "desired levels preserve order and duplicates while filtering placeholders");
  expect(g_active_levels == std::vector<std::string>({"hideout", "nest", "hideout"}) &&
             g_active_level_calls == 2,
         "active levels remain separate and unchanged calls still reach the renderer");

  goal_aot_call(display, 128, 0, 0);
  expect(std::fabs(g_alpha - 128.f / 255.f) < 0.0001f,
         "Jak 2 display adapter normalizes the direct alpha argument");

  g_order.clear();
  const uint64_t odd = goal_aot_call(syncv, 0, 0, 0);
  const uint64_t even = goal_aot_call(syncv, 0, 0, 0);
  expect(odd == 1 && even == 0 &&
             g_order == std::vector<int>({VblankHandler, HostVsync, VblankHandler, HostVsync}),
         "syncv ignores the handler return, orders it before host pacing, and returns host parity");
  MasterExit = RuntimeExitStatus::EXIT;
  g_order.clear();
  expect(goal_aot_call(syncv, 0, 0, 0) == 1 &&
             g_order == std::vector<int>({HostVsync}),
         "syncv skips handler 3 after runtime exit but still reaches host pacing");

  goal_gfx_host_stats stats = {};
  goal_gfx_host_stats_get(&stats);
  expect(stats.vsyncs == 3 && stats.sync_paths == 1 && stats.level_sets == 1 &&
             stats.active_level_sets == 1 &&
             std::strcmp(stats.last_levels, "title+city+title") == 0 &&
             std::strcmp(stats.last_active_levels, "hideout+nest+hideout") == 0,
         "host counters and desired/active snapshots reflect the first kernel lifetime");

  goal_kernel_core_shutdown();
  expect(vblank_interrupt_handler == 0,
         "kernel shutdown clears the retained vblank handler before arena teardown");

  reset_observations();
  if (!initialize_kernel()) {
    std::printf("FAIL: reinitialize: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  const uint32_t second_flush = lookup("flush-cache");
  const uint32_t second_mouse = lookup("mouse-get-data");
  expect(second_flush && second_mouse && install_host() == GOAL_KERNEL_CORE_OK,
         "graphics host installs after a complete kernel reinitialization");
  expect(lookup("flush-cache") == second_flush && lookup("mouse-get-data") == second_mouse,
         "reinitialized graphics host still preserves shared machine functions");

  stats = {};
  goal_gfx_host_stats_get(&stats);
  expect(stats.vsyncs == 0 && stats.sync_paths == 0 && stats.level_sets == 0 &&
             stats.active_level_sets == 0 && std::strcmp(stats.last_levels, "") == 0 &&
             std::strcmp(stats.last_active_levels, "") == 0,
         "installation resets counters and level snapshots for the new kernel lifetime");

  const uint32_t second_syncv = lookup("syncv");
  const uint32_t second_desired = lookup("__pc-set-levels");
  const uint32_t second_active = lookup("__pc-set-active-levels");
  desired_list = 0;
  active_list = 0;
  expect(second_syncv && second_desired && second_active &&
             allocate_level_list("jak2-desired-levels-2", desired_names, &desired_list) &&
             allocate_level_list("jak2-active-levels-2", active_names, &active_list),
         "recreated native functions and level arrays in the new EE arena");
  goal_aot_call(second_desired, desired_list, 0, 0);
  goal_aot_call(second_active, active_list, 0, 0);
  MasterExit = RuntimeExitStatus::RUNNING;
  vblank_interrupt_handler = goal_game_make_function_from_native((void*)retained_handler);
  g_order.clear();
  expect(goal_aot_call(second_syncv, 0, 0, 0) == 1 &&
             g_order == std::vector<int>({VblankHandler, HostVsync}),
         "reinitialized syncv again orders the handler before host pacing");
  goal_gfx_host_stats_get(&stats);
  expect(stats.vsyncs == 1 && stats.level_sets == 1 && stats.active_level_sets == 1 &&
             g_desired_level_calls == 1 && g_active_level_calls == 1,
         "reinitialized callbacks and counters start from one without stale host state");

  goal_kernel_core_shutdown();
  std::printf("\n%s (%d failures)\n",
              g_failures ? "JAK 2 GFX HOST TEST FAILED" : "JAK 2 GFX HOST TEST PASSED",
              g_failures);
  return g_failures ? 1 : 0;
}
