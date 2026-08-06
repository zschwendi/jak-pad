/*!
 * @file jak1_gfx_host_test.cpp
 * Preserve the Jak 1 graphics-host ABI while the shared seam also serves Jak 2.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/gfx_host.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/kernel_game.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

namespace {

int g_failures = 0;
int g_handler_calls = 0;
int g_chain_calls = 0;
int g_texture_uploads = 0;
int g_texture_moves = 0;
int g_vsync_calls = 0;
int g_sync_path_calls = 0;
int g_active_level_calls = 0;
uint32_t g_chain_offset = 0;
float g_alpha = -1.f;
std::vector<std::string> g_levels;

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
  g_handler_calls++;
  return 0;
}

void send_chain(const void* ee_base, uint32_t chain_offset) {
  expect(ee_base == g_ee_main_mem, "send-chain receives the live EE arena");
  g_chain_calls++;
  g_chain_offset = chain_offset;
}

uint32_t vsync() {
  return (uint32_t)(1 - (g_vsync_calls++ & 1));
}

uint32_t sync_path() {
  g_sync_path_calls++;
  return 37;
}

void texture_upload(const uint8_t* page, int mode, uint32_t s7_ptr) {
  expect(page == Ptr<u8>(g_chain_offset).c() && mode == 5 && s7_ptr == s7.offset,
         "texture upload preserves the Jak 1 pointer, mode and s7 ABI");
  g_texture_uploads++;
}

void texture_move(uint32_t dst, uint32_t src, uint32_t format) {
  expect(dst == 10 && src == 20 && format == 30,
         "texture relocate preserves all three arguments");
  g_texture_moves++;
}

void set_levels(const char* const* names, int count) {
  g_levels.clear();
  for (int i = 0; i < count; i++) {
    g_levels.emplace_back(names[i]);
  }
}

void set_active_levels(const char* const* /*names*/, int /*count*/) {
  g_active_level_calls++;
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

  const uint32_t host_manages_display = lookup("pc-host-manages-display?");
  expect(host_manages_display &&
             goal_aot_call(host_manages_display, 0, 0, 0) == goal_game_true_offset(),
         "portable kernel host reports that it manages display presentation");

  const uint32_t flush_before = lookup("flush-cache");
  goal_gfx_host host = {};
  host.send_chain = send_chain;
  host.vsync = vsync;
  host.sync_path = sync_path;
  host.texture_upload_now = texture_upload;
  host.texture_relocate = texture_move;
  host.set_levels = set_levels;
  host.set_pmode_alp = set_alpha;
  host.set_active_levels = set_active_levels;
  expect(goal_gfx_host_install(&host) == GOAL_KERNEL_CORE_OK, "installed the Jak 1 graphics host");
  expect(flush_before != 0 && lookup("flush-cache") == flush_before,
         "graphics host does not replace shared flush-cache");

  const uint32_t send = lookup("__send-gfx-dma-chain");
  const uint32_t syncv = lookup("syncv");
  const uint32_t syncp = lookup("sync-path");
  const uint32_t upload = lookup("__pc-texture-upload-now");
  const uint32_t relocate = lookup("__pc-texture-relocate");
  const uint32_t levels = lookup("__pc-set-levels");
  const uint32_t display = lookup("put-display-env");
  expect(send && syncv && syncp && upload && relocate && levels && display,
         "all Jak 1 host functions hold native implementations");

  uint32_t scratch = 0;
  expect(goal_kernel_core_global_alloc(64, "jak1-gfx-host-test", &scratch) == GOAL_KERNEL_CORE_OK,
         "allocated host-test memory in the EE arena");
  g_chain_offset = scratch;
  goal_aot_call(send, 0x10009000, scratch, 0);
  goal_aot_call(upload, scratch, 5, 0);
  goal_aot_call(relocate, 10, 20, 30);
  expect(g_chain_calls == 1 && g_texture_uploads == 1 && g_texture_moves == 1,
         "chain and texture calls each forward once");

  const uint32_t level0 = (uint32_t)jak1::make_string_from_c("village1");
  const uint32_t level1 = (uint32_t)jak1::make_string_from_c("none");
  goal_aot_call(levels, level0, level1, 0);
  expect(g_levels == std::vector<std::string>{"village1"},
         "Jak 1 adapter decodes two direct level arguments and filters none");

  auto* env = Ptr<u8>(scratch).c();
  std::memset(env, 0, 64);
  env[1] = 64;
  goal_aot_call(display, scratch, 0, 0);
  expect(std::fabs(g_alpha - 64.f / 255.f) < 0.0001f,
         "Jak 1 adapter reads blackout alpha from display-env byte 1");

  MasterExit = RuntimeExitStatus::RUNNING;
  vblank_interrupt_handler = goal_game_make_function_from_native((void*)retained_handler);
  const uint64_t odd = goal_aot_call(syncv, 0, 0, 0);
  const uint64_t even = goal_aot_call(syncv, 0, 0, 0);
  expect(odd == 1 && even == 0 && g_vsync_calls == 2 && g_handler_calls == 0,
         "Jak 1 syncv preserves host parity without dispatching Jak 2's retained handler");
  expect(goal_aot_call(syncp, 0, 0, 0) == 37 && g_sync_path_calls == 1,
         "Jak 1 sync-path forwards its host completion result");

  goal_gfx_host_stats stats = {};
  goal_gfx_host_stats_get(&stats);
  expect(stats.chains == 1 && stats.vsyncs == 2 && stats.sync_paths == 1 &&
             stats.texture_uploads == 1 && stats.texture_moves == 1 && stats.level_sets == 1 &&
             std::strcmp(stats.last_levels, "village1") == 0 && stats.active_level_sets == 0 &&
             std::strcmp(stats.last_active_levels, "") == 0 && g_active_level_calls == 0,
         "Jak 1 stats retain desired levels and leave active levels unused");

  goal_kernel_core_shutdown();
  std::printf("\n%s (%d failures)\n",
              g_failures ? "JAK 1 GFX HOST TEST FAILED" : "JAK 1 GFX HOST TEST PASSED",
              g_failures);
  return g_failures ? 1 : 0;
}
