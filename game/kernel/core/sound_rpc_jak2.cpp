/*!
 * @file sound_rpc_jak2.cpp
 * Answer Jak 2's sound-loader version handshake without pulling in the IOP or sound engine.
 *
 * `check-irx-version` sends one 0x50-byte command on loader channel 1. Upstream's Jak 2 overlord
 * writes version 4.0 into that command, remembers the requested EE info-block address, and returns
 * the command as the RPC reply. Nothing else in the Jak 2 sound protocol is implemented here.
 */

#include <cstddef>
#include <cstring>

#include "common/goal_constants.h"

#include "game/kernel/core/sound_rpc_jak2.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/overlord/jak2/srpc.h"
#include "game/runtime.h"

// Defined beside the machine stubs in desktop_seams.cpp.
u64 goal_kernel_core_machine_stub_report(const char* what);

namespace {

constexpr s32 kLoaderChannel = 1;
constexpr s32 kCommandSize = 0x50;
constexpr u32 kIrxMajor = 4;
constexpr u32 kIrxMinor = 0;

static_assert(sizeof(jak2::SoundRpcCommand) == kCommandSize);
static_assert(offsetof(jak2::SoundRpcCommand, j2command) == 2);
static_assert(offsetof(jak2::SoundRpcCommand, irx_version) == 4);
static_assert(offsetof(SoundRpcGetIrxVersion, major) == 0);
static_assert(offsetof(SoundRpcGetIrxVersion, minor) == 4);
static_assert(offsetof(SoundRpcGetIrxVersion, ee_addr) == 8);

goal_jak2_sound_rpc_stats g_stats;

bool readable_ee_span(u32 address, u32 size) {
  return g_ee_main_mem && address >= (u32)EE_MAIN_MEM_LOW_PROTECT &&
         address <= (u32)EE_MAIN_MEM_SIZE && size <= (u32)EE_MAIN_MEM_SIZE - address;
}

u64 reject(const char* what) {
  g_stats.rejected_calls++;
  return goal_kernel_core_machine_stub_report(what);
}

u64 rpc_call(u64* args) {
  if (!args) {
    return reject("rpc-call (Jak 2 sound, missing arguments)");
  }

  const s32 channel = (s32)args[0];
  const u32 send_buffer = (u32)args[3];
  const s32 send_size = (s32)args[4];
  const u32 recv_buffer = (u32)args[5];
  const s32 recv_size = (s32)args[6];

  if (channel != kLoaderChannel) {
    return reject("rpc-call (Jak 2 sound, unimplemented channel)");
  }
  if (send_size != kCommandSize || recv_size != kCommandSize ||
      !readable_ee_span(send_buffer, kCommandSize) ||
      !readable_ee_span(recv_buffer, kCommandSize)) {
    return reject("rpc-call (Jak 2 sound, malformed version handshake)");
  }

  jak2::SoundRpcCommand command;
  memcpy(&command, Ptr<u8>(send_buffer).c(), sizeof(command));
  if (command.j2command != jak2::Jak2SoundCommand::get_irx_version) {
    return reject("rpc-call (Jak 2 sound, unimplemented loader command)");
  }

  command.irx_version.major = kIrxMajor;
  command.irx_version.minor = kIrxMinor;
  g_stats.version_requests++;
  g_stats.info_ee = command.irx_version.ee_addr;

  // Upstream mutates its IOP-side loader buffer, then SIF copies the returned 0x50 bytes to the EE
  // receive buffer. The game's check-irx-version aliases send and receive, but separate EE send
  // buffers must remain untouched.
  memcpy(Ptr<u8>(recv_buffer).c(), &command, sizeof(command));
  return 0;
}

u64 rpc_busy(u64 channel) {
  if ((s32)channel != kLoaderChannel) {
    return reject("rpc-busy? (Jak 2 sound, unimplemented channel)");
  }
  return 0;
}

template <u64 (*Function)(u64*)>
u64 stack_arg_shim(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
  u64 args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
  return Function(args);
}

}  // namespace

extern "C" {

goal_kernel_core_status goal_jak2_sound_rpc_install(void) {
  if (!goal_kernel_core_is_initialized()) {
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }

  g_stats = {};
  jak2::make_stack_arg_function_symbol_from_c("rpc-call", (void*)stack_arg_shim<rpc_call>);
  jak2::make_function_symbol_from_c("rpc-busy?", (void*)rpc_busy);
  return GOAL_KERNEL_CORE_OK;
}

void goal_jak2_sound_rpc_stats_get(goal_jak2_sound_rpc_stats* out) {
  if (out) {
    *out = g_stats;
  }
}

}  // extern "C"
