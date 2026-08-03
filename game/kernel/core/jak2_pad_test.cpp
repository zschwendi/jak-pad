/*!
 * @file jak2_pad_test.cpp
 * Prove the portable pad seam through the Jak 2 symbol table without game data or an input device.
 */

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kernel_types.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/pad.h"

namespace {

int g_failures = 0;

// Jak 2's cpad-info has this same prefix, then old-rightx/righty/leftx/lefty (two bytes each).
// CPadOpen and CPadGetData are the common kmachine.cpp functions and never access that tail.
constexpr size_t kJak2OldAxisBytes = 8;
static_assert(offsetof(CPadInfo, number) == 32);
static_assert(offsetof(CPadInfo, state) == 80);
static_assert(offsetof(CPadInfo, change_time) == 124);
static_assert(sizeof(CPadInfo) == 132);

void expect(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) {
    g_failures++;
  }
}

}  // namespace

int main() {
  expect(goal_pad_install() == GOAL_KERNEL_CORE_NOT_INITIALIZED,
         "the pad seam refuses to install before the kernel exists");
  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: initialize: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  if (goal_kernel_core_stub_machine_layer(0) != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: machine stubs: %s\n", goal_kernel_core_last_error());
    goal_kernel_core_shutdown();
    return 1;
  }

  uint32_t open_stub = 0;
  uint32_t get_data_stub = 0;
  expect(goal_kernel_core_lookup("cpad-open", nullptr, &open_stub) == GOAL_KERNEL_CORE_OK &&
             open_stub != 0,
         "the Jak 2 machine layer initially exposes a named cpad-open stub");
  expect(goal_kernel_core_lookup("cpad-get-data", nullptr, &get_data_stub) == GOAL_KERNEL_CORE_OK &&
             get_data_stub != 0,
         "the Jak 2 machine layer initially exposes a named cpad-get-data stub");

  expect(goal_pad_install() == GOAL_KERNEL_CORE_OK,
         "the portable pad seam installs through the Jak 2 symbol table");
  uint32_t open = 0;
  uint32_t get_data = 0;
  expect(goal_kernel_core_lookup("cpad-open", nullptr, &open) == GOAL_KERNEL_CORE_OK && open != 0 &&
             open != open_stub,
         "cpad-open replaced its Jak 2 machine stub");
  expect(goal_kernel_core_lookup("cpad-get-data", nullptr, &get_data) == GOAL_KERNEL_CORE_OK &&
             get_data != 0 && get_data != get_data_stub,
         "cpad-get-data replaced its Jak 2 machine stub");

  uint32_t pad_offset = 0;
  if (goal_kernel_core_global_alloc(sizeof(CPadInfo) + kJak2OldAxisBytes, "jak2-pad-test",
                                    &pad_offset) != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: pad allocation: %s\n", goal_kernel_core_last_error());
    goal_kernel_core_shutdown();
    return 1;
  }
  auto* pad = Ptr<CPadInfo>(pad_offset).c();
  std::memset(pad, 0, sizeof(*pad));
  auto* old_axes = Ptr<uint8_t>(pad_offset + sizeof(*pad)).c();
  std::memset(old_axes, 0xa5, kJak2OldAxisBytes);
  constexpr int kPort = 0;
  pad->number = kPort;

  expect(goal_aot_call(open, pad_offset, kPort, 0) == pad_offset,
         "cpad-open returns the Jak 2 cpad-info it was given");
  expect(pad->cpad_file == kPort + 1 && pad->new_pad == 1 && pad->state == 0,
         "cpad-open opens the requested port and marks the pad new");

  goal_aot_call(get_data, pad_offset, 0, 0);
  expect((pad->valid & 0x80) != 0, "a Jak 2 pad port with no host state is reported disconnected");

  goal_pad_state host;
  goal_pad_state_neutral(&host);
  host.buttons = GOAL_PAD_START | GOAL_PAD_X;
  host.left_x = 200;
  host.left_y = 12;
  expect(goal_pad_set_state(kPort, &host) == GOAL_KERNEL_CORE_OK,
         "the host can push a controller into the shared seam");

  int frames_to_live = 0;
  for (int frame = 1; frame <= 8 && !frames_to_live; frame++) {
    goal_aot_call(get_data, pad_offset, 0, 0);
    if ((pad->valid & 0x80) == 0) {
      frames_to_live = frame;
    }
  }
  expect(frames_to_live == 4 && pad->state == 99,
         "the common pad state machine reaches its running state");
  expect((pad->status >> 4) == 7, "the controller reports DualShock 2 mode");
  expect(pad->button0 == (GOAL_PAD_START | GOAL_PAD_X),
         "Jak 2 receives the shared PS2 button bits");
  expect(pad->leftx == 200 && pad->lefty == 12, "Jak 2 receives the host's analog stick bytes");

  pad->direct[0] = 1;
  pad->direct[1] = 173;
  goal_aot_call(get_data, pad_offset, 0, 0);
  uint8_t large = 0;
  uint8_t small = 0;
  expect(goal_pad_get_rumble(kPort, &large, &small) == GOAL_KERNEL_CORE_OK && large == 1 &&
             small == 173,
         "Jak 2 vibration reaches the host-facing seam");
  expect(goal_pad_read_count(kPort) > 0, "the Jak 2 calls read through the host pad state");

  host.connected = 0;
  goal_pad_set_state(kPort, &host);
  goal_aot_call(get_data, pad_offset, 0, 0);
  expect((pad->valid & 0x80) != 0 && pad->state == 0,
         "disconnecting the controller is reported and resets the pad");
  bool old_axes_unchanged = true;
  for (size_t i = 0; i < kJak2OldAxisBytes; i++) {
    old_axes_unchanged &= old_axes[i] == 0xa5;
  }
  expect(old_axes_unchanged, "the common functions leave Jak 2's old-axis tail untouched");

  goal_kernel_core_shutdown();
  std::printf("\n%s (%d failures)\n",
              g_failures ? "JAK 2 PAD TEST FAILED" : "JAK 2 PAD TEST PASSED", g_failures);
  return g_failures ? 1 : 0;
}
