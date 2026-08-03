/*!
 * @file jak2_pc_rand_test.cpp
 * Prove Jak 2's portable pc-rand binding and upstream process-lifetime sequence.
 */

#include <array>
#include <cstdio>

#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/kernel_core.h"

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) {
    g_failures++;
  }
}

bool initialize_and_find_pc_rand(uint32_t* out) {
  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: initialize: %s\n", goal_kernel_core_last_error());
    return false;
  }
  if (goal_kernel_core_stub_machine_layer(0) != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: machine seams: %s\n", goal_kernel_core_last_error());
    goal_kernel_core_shutdown();
    return false;
  }
  if (goal_kernel_core_lookup("pc-rand", nullptr, out) != GOAL_KERNEL_CORE_OK || *out == 0) {
    std::printf("FAIL: pc-rand lookup: %s\n", goal_kernel_core_last_error());
    goal_kernel_core_shutdown();
    return false;
  }
  return true;
}

}  // namespace

int main() {
  constexpr std::array<uint64_t, 4> kExpected = {
      3499211612,
      581869302,
      3890346734,
      3586334585,
  };

  uint32_t pc_rand = 0;
  if (!initialize_and_find_pc_rand(&pc_rand)) {
    return 1;
  }
  expect(goal_aot_call(pc_rand, 0, 0, 0) == kExpected[0],
         "pc-rand returns upstream's first default-seeded value");
  expect(goal_aot_call(pc_rand, 0, 0, 0) == kExpected[1],
         "pc-rand returns upstream's second default-seeded value");
  goal_kernel_core_shutdown();

  if (!initialize_and_find_pc_rand(&pc_rand)) {
    return 1;
  }
  expect(goal_aot_call(pc_rand, 0, 0, 0) == kExpected[2],
         "pc-rand continues its sequence after kernel reinitialization");
  expect(goal_aot_call(pc_rand, 0, 0, 0) == kExpected[3],
         "pc-rand preserves upstream's process-lifetime generator state");
  goal_kernel_core_shutdown();

  std::printf("\n%s (%d failures)\n",
              g_failures ? "JAK 2 PC-RAND TEST FAILED" : "JAK 2 PC-RAND TEST PASSED",
              g_failures);
  return g_failures ? 1 : 0;
}
