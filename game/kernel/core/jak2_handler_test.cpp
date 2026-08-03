/*!
 * @file jak2_handler_test.cpp
 * Prove Jak 2's portable install-handler binding without game data or interrupt dispatch.
 */

#include <cstdio>

#include "game/kernel/common/kmachine.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/kernel_game.h"

namespace {

int g_failures = 0;

u64 handler_a() {
  return 0;
}

u64 handler_b() {
  return 0;
}

void expect(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) {
    g_failures++;
  }
}

}  // namespace

int main() {
  expect(vblank_interrupt_handler == 0 && vif1_interrupt_handler == 0,
         "interrupt handler storage begins clear");

  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: initialize: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  if (goal_kernel_core_stub_machine_layer(0) != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: machine seams: %s\n", goal_kernel_core_last_error());
    goal_kernel_core_shutdown();
    return 1;
  }

  uint32_t install_handler = 0;
  expect(goal_kernel_core_lookup("install-handler", nullptr, &install_handler) ==
                 GOAL_KERNEL_CORE_OK &&
             install_handler != 0,
         "Jak 2 binds install-handler to a real GOAL function object");

  const uint32_t first = goal_game_make_function_from_native((void*)handler_a);
  const uint32_t second = goal_game_make_function_from_native((void*)handler_b);
  expect(first != 0 && second != 0 && first != second,
         "the test handlers are distinct GOAL function objects");

  goal_aot_call(install_handler, 3, first, 0);
  expect(vblank_interrupt_handler == first && vif1_interrupt_handler == 0,
         "handler ID 3 stores the vblank function and leaves VIF1 clear");

  goal_aot_call(install_handler, 5, second, 0);
  expect(vblank_interrupt_handler == first && vif1_interrupt_handler == second,
         "handler ID 5 stores the VIF1 function independently");

  goal_aot_call(install_handler, 3, second, 0);
  expect(vblank_interrupt_handler == second,
         "installing ID 3 again replaces the prior vblank function");

  goal_aot_call(install_handler, 5, 0, 0);
  expect(vif1_interrupt_handler == 0 && vblank_interrupt_handler == second,
         "installing zero clears only the selected handler");

  goal_aot_call(install_handler, 5, first, 0);
  goal_kernel_core_shutdown();
  expect(vblank_interrupt_handler == 0 && vif1_interrupt_handler == 0,
         "kernel shutdown clears both retained GOAL references");

  expect(goal_kernel_core_initialize() == GOAL_KERNEL_CORE_OK,
         "the kernel can initialize again after shutdown");
  expect(vblank_interrupt_handler == 0 && vif1_interrupt_handler == 0,
         "a fresh kernel lifecycle begins with both handlers clear");
  goal_kernel_core_shutdown();

  std::printf("\n%s (%d failures)\n",
              g_failures ? "JAK 2 HANDLER TEST FAILED" : "JAK 2 HANDLER TEST PASSED",
              g_failures);
  return g_failures ? 1 : 0;
}
