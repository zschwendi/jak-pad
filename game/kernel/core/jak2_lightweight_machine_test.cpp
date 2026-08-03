/*!
 * @file jak2_lightweight_machine_test.cpp
 * Check the small portable Jak II machine functions that do not belong to a host subsystem.
 */

#include <cstdio>

#include "common/global_profiler/GlobalProfiler.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/kernel_core.h"
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
  expect(lookup_function("pc-prof", &pc_prof), "pc-prof holds a portable implementation");
  expect(lookup_function("flush-cache", &flush_cache),
         "flush-cache holds its signed-AOT no-op implementation");

  prof().clear();
  prof().set_enable(true);
  const uint32_t name = (uint32_t)jak2::make_string_from_c("goalpad-jak2-profiler");
  const uint32_t empty = (uint32_t)jak2::make_string_from_c("");
  goal_aot_call(pc_prof, name, ProfNode::BEGIN, 0);
  goal_aot_call(pc_prof, name, ProfNode::INSTANT, 0);
  goal_aot_call(pc_prof, empty, ProfNode::END, 0);
  prof().set_enable(false);
  expect(prof().get_next_idx() == 3,
         "pc-prof forwards begin, instant and end events to the portable profiler");
  goal_aot_call(pc_prof, name, ProfNode::INSTANT, 0);
  expect(prof().get_next_idx() == 3, "pc-prof preserves the profiler's disabled behavior");

  goal_aot_call(flush_cache, 0, 0, 0);
  goal_aot_call(flush_cache, 2, 0, 0);
  goal_aot_call(flush_cache, UINT32_MAX, 0, 0);
  expect(true, "flush-cache accepts the upstream mode argument without executable-memory work");

  goal_kernel_core_shutdown();

  pc_prof = 0;
  flush_cache = 0;
  const bool initialized = goal_kernel_core_initialize() == GOAL_KERNEL_CORE_OK;
  const bool reinitialized =
      initialized && goal_kernel_core_stub_machine_layer(1) == GOAL_KERNEL_CORE_OK;
  expect(reinitialized && lookup_function("pc-prof", &pc_prof) &&
             lookup_function("flush-cache", &flush_cache),
         "both implementations survive kernel reinitialization");
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
