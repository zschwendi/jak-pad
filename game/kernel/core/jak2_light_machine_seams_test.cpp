/*!
 * @file jak2_light_machine_seams_test.cpp
 * Prove the Jak 2 machine functions that need neither a renderer nor a desktop input system.
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

uint32_t find_function(const char* name) {
  uint32_t function = 0;
  expect(goal_kernel_core_lookup(name, nullptr, &function) == GOAL_KERNEL_CORE_OK && function != 0,
         name);
  return function;
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

  const uint32_t pc_prof = find_function("pc-prof");
  const uint32_t flush_cache = find_function("flush-cache");
  const uint32_t mouse_get_data = find_function("mouse-get-data");
  if (!pc_prof || !flush_cache || !mouse_get_data) {
    goal_kernel_core_shutdown();
    return 1;
  }

  auto& profiler = prof();
  profiler.clear();
  profiler.set_enable(false);
  const uint64_t disabled_name = jak2::make_string_from_c("disabled");
  goal_aot_call(pc_prof, disabled_name, ProfNode::INSTANT, 0);
  expect(profiler.get_next_idx() == 0, "pc-prof leaves the disabled profiler untouched");

  const uint64_t enabled_name = jak2::make_string_from_c("enabled");
  profiler.set_enable(true);
  goal_aot_call(pc_prof, enabled_name, ProfNode::BEGIN, 0);
  goal_aot_call(pc_prof, enabled_name, ProfNode::END, 0);
  goal_aot_call(pc_prof, enabled_name, ProfNode::INSTANT, 0);
  expect(profiler.get_next_idx() == 3, "pc-prof forwards begin, end and instant events");
  profiler.set_enable(false);
  profiler.clear();

  expect(goal_aot_call(flush_cache, 0, 0, 0) == 0,
         "flush-cache is the upstream host no-op");

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
               mouse->deltay == 0 && mouse->wheel == 0 && mouse->posx == 0.f && mouse->posy == 0.f,
           "mouse-get-data clears stale desktop input state");
  }

  goal_kernel_core_shutdown();
  std::printf("\n%s (%d failures)\n",
              g_failures ? "JAK 2 LIGHT MACHINE SEAMS TEST FAILED"
                         : "JAK 2 LIGHT MACHINE SEAMS TEST PASSED",
              g_failures);
  return g_failures ? 1 : 0;
}
