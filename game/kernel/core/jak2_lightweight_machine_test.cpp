/*!
 * @file jak2_lightweight_machine_test.cpp
 * Check the small portable Jak II machine functions that do not belong to a host subsystem.
 */

#include <cstdio>

#include "common/global_profiler/GlobalProfiler.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/jak2/kmachine.h"
#include "game/kernel/jak2/kscheme.h"

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) {
    g_failures++;
  }
}

bool lookup_function(const char* name, uint32_t* out) {
  return goal_kernel_core_lookup(name, nullptr, out) == GOAL_KERNEL_CORE_OK && *out != 0;
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

  uint32_t pc_prof = 0;
  uint32_t flush_cache = 0;
  uint32_t mouse_get_data = 0;
  expect(lookup_function("pc-prof", &pc_prof), "pc-prof holds a portable implementation");
  expect(lookup_function("flush-cache", &flush_cache),
         "flush-cache holds its signed-AOT no-op implementation");
  expect(lookup_function("mouse-get-data", &mouse_get_data),
         "mouse-get-data holds its inactive-pointer implementation");

  auto& profiler = prof();
  profiler.clear();
  profiler.set_enable(true);
  const uint32_t name = (uint32_t)jak2::make_string_from_c("goalpad-jak2-profiler");
  const uint32_t empty = (uint32_t)jak2::make_string_from_c("");
  goal_aot_call(pc_prof, name, ProfNode::BEGIN, 0);
  goal_aot_call(pc_prof, name, ProfNode::INSTANT, 0);
  goal_aot_call(pc_prof, empty, ProfNode::END, 0);
  profiler.set_enable(false);
  expect(profiler.get_next_idx() == 3,
         "pc-prof forwards begin, instant and end events to the portable profiler");
  goal_aot_call(pc_prof, name, ProfNode::INSTANT, 0);
  expect(profiler.get_next_idx() == 3, "pc-prof preserves the profiler's disabled behavior");

  expect(goal_aot_call(flush_cache, 0, 0, 0) == 0 &&
             goal_aot_call(flush_cache, 2, 0, 0) == 0 &&
             goal_aot_call(flush_cache, UINT32_MAX, 0, 0) == 0,
         "flush-cache accepts every upstream mode without executable-memory work");

  auto mouse_mem =
      kmalloc(kglobalheap, sizeof(jak2::MouseInfo), KMALLOC_MEMSET, "inactive-mouse-test");
  expect(mouse_mem.offset != 0, "allocated synthetic mouse-info");
  if (mouse_mem.offset) {
    auto* mouse = reinterpret_cast<jak2::MouseInfo*>(mouse_mem.c());
    mouse->active = 0xffffffff;
    mouse->valid = 0xffffffff;
    mouse->cursor = 0xffffffff;
    mouse->status = 0xffff;
    mouse->button0 = 0xffff;
    mouse->deltax = 7;
    mouse->deltay = -7;
    mouse->wheel = 9;
    mouse->posx = 123.f;
    mouse->posy = -123.f;

    expect(goal_aot_call(mouse_get_data, mouse_mem.offset, 0, 0) == mouse_mem.offset,
           "mouse-get-data returns its mouse-info");
    expect(mouse->active == s7.offset && mouse->valid == s7.offset &&
               mouse->cursor == s7.offset,
           "mouse-get-data reports no pointer device");
    expect(mouse->status == 0 && mouse->button0 == 0 && mouse->deltax == 0 &&
               mouse->deltay == 0 && mouse->wheel == 0 && mouse->posx == 0.f &&
               mouse->posy == 0.f,
           "mouse-get-data clears stale desktop input state");
  }

  goal_kernel_core_shutdown();

  pc_prof = 0;
  flush_cache = 0;
  mouse_get_data = 0;
  const bool initialized = goal_kernel_core_initialize() == GOAL_KERNEL_CORE_OK;
  const bool reinitialized =
      initialized && goal_kernel_core_stub_machine_layer(1) == GOAL_KERNEL_CORE_OK;
  expect(reinitialized && lookup_function("pc-prof", &pc_prof) &&
             lookup_function("flush-cache", &flush_cache) &&
             lookup_function("mouse-get-data", &mouse_get_data),
         "all lightweight implementations survive kernel reinitialization");
  if (reinitialized) {
    goal_aot_call(flush_cache, 0, 0, 0);
  }
  if (initialized) {
    goal_kernel_core_shutdown();
  }

  std::printf("\n%s (%d failures)\n",
              g_failures ? "JAK 2 LIGHTWEIGHT MACHINE TEST FAILED"
                         : "JAK 2 LIGHTWEIGHT MACHINE TEST PASSED",
              g_failures);
  return g_failures ? 1 : 0;
}
