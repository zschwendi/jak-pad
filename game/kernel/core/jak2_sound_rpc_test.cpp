/*!
 * @file jak2_sound_rpc_test.cpp
 * Behavioral coverage for the narrow Jak 2 sound-loader version handshake.
 */

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "common/goal_constants.h"
#include "common/log/log.h"

#include "game/kernel/common/kmalloc.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/sound_rpc_jak2.h"
#include "game/overlord/jak2/srpc.h"
#include "game/runtime.h"

namespace {

constexpr u32 kCommandSize = 0x50;
constexpr u32 kGuardSize = 16;
int g_failures = 0;

void check(bool condition, const char* what) {
  std::printf("  %s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    g_failures++;
  }
}

void check_u32(u32 got, u32 expected, const char* what) {
  if (got == expected) {
    std::printf("  ok   %-42s #x%08x\n", what, got);
  } else {
    std::printf("  FAIL %-42s got #x%08x, expected #x%08x\n", what, got, expected);
    g_failures++;
  }
}

struct GuardedCommand {
  Ptr<u8> allocation;
  Ptr<jak2::SoundRpcCommand> command;
};

GuardedCommand guarded_command(const char* name) {
  auto allocation = kmalloc(kglobalheap, kCommandSize + 2 * kGuardSize,
                            KMALLOC_MEMSET | KMALLOC_ALIGN_16, name);
  if (!allocation.offset) {
    check(false, "allocate a guarded RPC command");
    return {};
  }
  memset(allocation.c(), 0xa5, kCommandSize + 2 * kGuardSize);
  return {allocation, (allocation + kGuardSize).cast<jak2::SoundRpcCommand>()};
}

void check_guards(GuardedCommand& buffer, const char* what) {
  bool intact = true;
  for (u32 i = 0; i < kGuardSize; i++) {
    intact &= buffer.allocation.c()[i] == 0xa5;
    intact &= buffer.allocation.c()[kGuardSize + kCommandSize + i] == 0xa5;
  }
  check(intact, what);
}

using GoalEightArgumentFunction =
    u64 (*)(u64, u64, u64, u64, u64, u64, u64, u64);
using GoalOneArgumentFunction = u64 (*)(u64);

template <typename Function>
Function native_entry(const char* name, u32* object_out = nullptr) {
  u32 object = 0;
  if (goal_kernel_core_lookup(name, nullptr, &object) != GOAL_KERNEL_CORE_OK || !object) {
    check(false, name);
    return nullptr;
  }
  uintptr_t entry = 0;
  memcpy(&entry, Ptr<u8>(object).c(), sizeof(entry));
  if (object_out) {
    *object_out = object;
  }
  check(entry != 0, name);
  return reinterpret_cast<Function>(entry);
}

void reset_command(GuardedCommand& buffer, jak2::Jak2SoundCommand command, u32 ee_addr) {
  memset(buffer.command.c(), 0, kCommandSize);
  buffer.command->rsvd1 = 0x5aa5;
  buffer.command->j2command = command;
  buffer.command->irx_version.major = 0xdeadbeef;
  buffer.command->irx_version.minor = 0xcafef00d;
  buffer.command->irx_version.ee_addr = ee_addr;
  memset(buffer.command->max_size + sizeof(SoundRpcGetIrxVersion), 0x3c,
         sizeof(buffer.command->max_size) - sizeof(SoundRpcGetIrxVersion));
}

std::array<u8, kCommandSize> snapshot(GuardedCommand& buffer) {
  std::array<u8, kCommandSize> result;
  memcpy(result.data(), buffer.command.c(), result.size());
  return result;
}

void check_version_reply(GuardedCommand& buffer, u32 ee_addr, const char* prefix) {
  check_u32((u32)buffer.command->j2command, 16, prefix);
  check_u32(buffer.command->irx_version.major, 4, "reply major version");
  check_u32(buffer.command->irx_version.minor, 0, "reply minor version");
  check_u32(buffer.command->irx_version.ee_addr, ee_addr, "reply preserves the EE info address");
  check_u32(buffer.command->rsvd1, 0x5aa5, "reply preserves the reserved header");
}

}  // namespace

int main() {
  lg::set_stdout_level(lg::level::warn);
  lg::set_flush_level(lg::level::warn);
  lg::initialize();

  check(goal_jak2_sound_rpc_install() == GOAL_KERNEL_CORE_NOT_INITIALIZED,
        "installation rejects an uninitialized kernel");
  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL kernel initialization: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  check(goal_kernel_core_stub_machine_layer(0) == GOAL_KERNEL_CORE_OK,
        "reporting-mode machine stubs install");

  u32 stub_call = 0;
  u32 stub_busy = 0;
  goal_kernel_core_lookup("rpc-call", nullptr, &stub_call);
  goal_kernel_core_lookup("rpc-busy?", nullptr, &stub_busy);
  check(goal_jak2_sound_rpc_install() == GOAL_KERNEL_CORE_OK, "Jak 2 sound handshake installs");

  u32 installed_call = 0;
  u32 installed_busy = 0;
  auto rpc_call = native_entry<GoalEightArgumentFunction>("rpc-call", &installed_call);
  auto rpc_busy = native_entry<GoalOneArgumentFunction>("rpc-busy?", &installed_busy);
  check(installed_call != stub_call, "rpc-call replaces its reporting stub");
  check(installed_busy != stub_busy, "rpc-busy? replaces its reporting stub");

  auto send = guarded_command("jak2-sound-rpc-send");
  auto recv = guarded_command("jak2-sound-rpc-recv");
  if (!send.command.offset || !recv.command.offset || !rpc_call || !rpc_busy) {
    goal_kernel_core_shutdown();
    return 1;
  }

  std::printf("\n== separate send and receive buffers ==\n");
  reset_command(send, jak2::Jak2SoundCommand::get_irx_version, 0x12345678);
  const auto original = snapshot(send);
  check_u32((u32)rpc_call(1, 0, 1, send.command.offset, kCommandSize, recv.command.offset,
                         kCommandSize, 0),
            0, "rpc-call returns synchronously");
  check(snapshot(send) == original, "the EE send buffer remains untouched");
  check_version_reply(recv, 0x12345678, "reply preserves command 16");
  check(memcmp(original.data() + 16, recv.command.cast<u8>().c() + 16, kCommandSize - 16) == 0,
        "bytes outside the version fields are preserved");
  check_guards(send, "send-buffer canaries stay intact");
  check_guards(recv, "receive-buffer canaries stay intact");

  std::printf("\n== in-place reply, as check-irx-version uses it ==\n");
  reset_command(send, jak2::Jak2SoundCommand::get_irx_version, 0x23456789);
  check_u32((u32)rpc_call(1, 99, 0, send.command.offset, kCommandSize, send.command.offset,
                         kCommandSize, 0),
            0, "loader fno and async mode do not change the handler");
  check_version_reply(send, 0x23456789, "in-place reply preserves command 16");
  check_u32((u32)rpc_busy(1), 0, "the synchronous loader channel is never busy");

  goal_jak2_sound_rpc_stats stats;
  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.version_requests, 2, "two version requests were handled");
  check_u32(stats.info_ee, 0x23456789, "the latest EE info address is retained");

  std::printf("\n== unsupported requests remain unimplemented ==\n");
  reset_command(send, jak2::Jak2SoundCommand::load_bank, 0x3456789a);
  memset(recv.command.c(), 0xcc, kCommandSize);
  const auto unsupported_send = snapshot(send);
  const auto unsupported_recv = snapshot(recv);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, recv.command.offset, kCommandSize, 0);
  check(snapshot(send) == unsupported_send && snapshot(recv) == unsupported_recv,
        "an unimplemented loader command mutates neither buffer");
  rpc_call(0, 0, 1, send.command.offset, kCommandSize, recv.command.offset, kCommandSize, 0);
  rpc_call(5, 0, 1, send.command.offset, kCommandSize, recv.command.offset, kCommandSize, 0);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize - 1, recv.command.offset, kCommandSize, 0);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, recv.command.offset, kCommandSize - 1, 0);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, 0, kCommandSize, 0);
  rpc_call(1, 0, 1, 0, kCommandSize, recv.command.offset, kCommandSize, 0);
  rpc_call(1, 0, 1, EE_MAIN_MEM_SIZE + 16, kCommandSize, recv.command.offset, kCommandSize, 0);
  rpc_call(1, 0, 1, EE_MAIN_MEM_SIZE - kCommandSize + 1, kCommandSize, recv.command.offset,
           kCommandSize, 0);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize,
           EE_MAIN_MEM_SIZE - kCommandSize + 1, kCommandSize, 0);
  rpc_call(1, 0, 1, send.command.offset, (u64)-1, recv.command.offset, kCommandSize, 0);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, recv.command.offset, (u64)-1, 0);
  check(snapshot(send) == unsupported_send && snapshot(recv) == unsupported_recv,
        "rejected channels and malformed buffers do not mutate memory");
  check_u32((u32)rpc_busy(3), 0, "an unsupported busy query still returns not-busy");
  check_guards(send, "rejected-call send canaries stay intact");
  check_guards(recv, "rejected-call receive canaries stay intact");

  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.version_requests, 2, "rejected calls do not count as handshakes");
  check_u32(stats.rejected_calls, 13, "every unsupported request is reported");

  goal_kernel_core_shutdown();
  std::printf("\n%s: Jak 2 sound-RPC version seam\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
