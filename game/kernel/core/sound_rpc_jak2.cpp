/*!
 * @file sound_rpc_jak2.cpp
 * Answer Jak 2's sound-loader version handshake and ordinary-file STR requests without an IOP.
 *
 * `check-irx-version` sends one 0x50-byte command on loader channel 1. Upstream's Jak 2 overlord
 * writes version 4.0 into that command, remembers the requested EE info-block address, and returns
 * the command as the RPC reply. Loader command 2 has no receive buffer, but sound-bank loading is
 * still reported as unimplemented. Channel 4 reads an ordinary file from the configured `iso/`
 * directory into EE memory. Chunked STR files and the rest of the Jak 2 sound protocol remain
 * unimplemented.
 */

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

#include "common/goal_constants.h"

#include "game/common/str_rpc_types.h"
#include "game/kernel/core/sound_rpc_jak2.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/overlord/jak2/srpc.h"
#include "game/runtime.h"
#include "game/sce/sif_ee.h"

// Defined beside the machine stubs in desktop_seams.cpp.
u64 goal_kernel_core_machine_stub_report(const char* what);

namespace {

constexpr s32 kLoaderChannel = 1;
constexpr s32 kCommandSize = 0x50;
constexpr s32 kStrChannel = 4;
constexpr u32 kStrFunction = 0;
constexpr s32 kStrRequestSize = 0x40;
constexpr s32 kStrReplySize = 0x20;
constexpr u32 kIrxMajor = 4;
constexpr u32 kIrxMinor = 0;

static_assert(sizeof(jak2::SoundRpcCommand) == kCommandSize);
static_assert(offsetof(jak2::SoundRpcCommand, j2command) == 2);
static_assert(offsetof(jak2::SoundRpcCommand, irx_version) == 4);
static_assert(offsetof(SoundRpcGetIrxVersion, major) == 0);
static_assert(offsetof(SoundRpcGetIrxVersion, minor) == 4);
static_assert(offsetof(SoundRpcGetIrxVersion, ee_addr) == 8);
static_assert(sizeof(RPC_Str_Cmd_Jak2) == 0x50);
static_assert(offsetof(RPC_Str_Cmd_Jak2, basename) == kStrReplySize);

struct StrRequest {
  u16 rsvd;
  u16 result;
  u32 address;
  s32 section;
  u32 maxlen;
  u32 dummy[4];
  char basename[kStrRequestSize - kStrReplySize];
};
static_assert(sizeof(StrRequest) == kStrRequestSize);

struct StrReply {
  u16 rsvd;
  u16 result;
  u32 address;
  s32 section;
  u32 maxlen;
  u32 dummy[4];
};
static_assert(sizeof(StrReply) == kStrReplySize);

goal_jak2_sound_rpc_stats g_stats;

bool readable_ee_span(u32 address, u32 size) {
  return g_ee_main_mem && address >= (u32)EE_MAIN_MEM_LOW_PROTECT &&
         address <= (u32)EE_MAIN_MEM_SIZE && size <= (u32)EE_MAIN_MEM_SIZE - address;
}

u64 reject(const char* what) {
  g_stats.rejected_calls++;
  return goal_kernel_core_machine_stub_report(what);
}

u64 loader_rpc(u32 send_buffer, s32 send_size, u32 recv_buffer, s32 recv_size) {
  if (send_size != kCommandSize || (send_buffer & 0xf) ||
      !readable_ee_span(send_buffer, kCommandSize)) {
    return reject("rpc-call (Jak 2 sound, malformed loader command)");
  }

  jak2::SoundRpcCommand command;
  memcpy(&command, Ptr<u8>(send_buffer).c(), sizeof(command));
  switch (command.j2command) {
    case jak2::Jak2SoundCommand::get_irx_version:
      if (recv_size != kCommandSize || (recv_buffer & 0xf) ||
          !readable_ee_span(recv_buffer, kCommandSize)) {
        return reject("rpc-call (Jak 2 sound, malformed version reply)");
      }

      command.irx_version.major = kIrxMajor;
      command.irx_version.minor = kIrxMinor;
      g_stats.version_requests++;
      g_stats.info_ee = command.irx_version.ee_addr;

      // Upstream mutates its IOP-side loader buffer, then SIF copies the returned 0x50 bytes to the
      // EE receive buffer. The game's check-irx-version aliases send and receive, but separate EE
      // send buffers must remain untouched.
      memcpy(Ptr<u8>(recv_buffer).c(), &command, sizeof(command));
      return 0;
    case jak2::Jak2SoundCommand::load_bank:
      if (recv_size != 0) {
        return reject("rpc-call (Jak 2 sound, load-bank unexpectedly requested a reply)");
      }
      g_stats.bank_load_requests++;
      g_stats.bank_load_unimplemented++;
      return reject("rpc-call (Jak 2 sound, load-bank unimplemented)");
    default:
      return reject("rpc-call (Jak 2 sound, unimplemented loader command)");
  }
}

void write_str_reply(const StrRequest& request, u32 recv_buffer, u16 result, u32 length) {
  StrReply reply;
  memcpy(&reply, &request, sizeof(reply));
  reply.result = result;
  reply.maxlen = length;
  memcpy(Ptr<u8>(recv_buffer).c(), &reply, sizeof(reply));
}

bool uppercase_basename(const StrRequest& request, std::string* out) {
  char basename[sizeof(request.basename) + 1];
  memcpy(basename, request.basename, sizeof(request.basename));
  basename[sizeof(request.basename)] = '\0';

  size_t length = 0;
  while (basename[length]) {
    length++;
  }
  if (!length) {
    return false;
  }

  out->assign(basename, length);
  if (*out == "." || *out == "..") {
    return false;
  }
  for (char& c : *out) {
    if (c == '/' || c == '\\' || c == ':') {
      return false;
    }
    if (c >= 'a' && c <= 'z') {
      c -= 'a' - 'A';
    }
  }
  return true;
}

bool read_str_file(const StrRequest& request, const std::string& basename, u32* length) {
  if (!request.maxlen || !readable_ee_span(request.address, request.maxlen)) {
    return false;
  }

  const std::string relative = "iso/" + basename;
  const s32 fd = ee::sceOpen(relative.c_str(), SCE_RDONLY);
  if (fd < 0) {
    return false;
  }

  const s32 file_size = ee::sceLseek(fd, 0, SCE_SEEK_END);
  if (file_size <= 0 || ee::sceLseek(fd, 0, SCE_SEEK_SET) != 0) {
    ee::sceClose(fd);
    return false;
  }
  const u32 read_size = std::min((u32)file_size, request.maxlen);
  const s32 bytes_read = ee::sceRead(fd, Ptr<u8>(request.address).c(), (s32)read_size);
  ee::sceClose(fd);
  if (bytes_read != (s32)read_size) {
    return false;
  }
  *length = read_size;
  return true;
}

u64 str_rpc(u32 function,
            u32 send_buffer,
            s32 send_size,
            u32 recv_buffer,
            s32 recv_size) {
  if (function != kStrFunction) {
    return reject("rpc-call (Jak 2 STR, unimplemented function)");
  }
  if (send_size != kStrRequestSize || recv_size != kStrReplySize ||
      !readable_ee_span(send_buffer, kStrRequestSize) ||
      !readable_ee_span(recv_buffer, kStrReplySize)) {
    return reject("rpc-call (Jak 2 STR, malformed buffers)");
  }

  StrRequest request;
  memcpy(&request, Ptr<u8>(send_buffer).c(), sizeof(request));
  if (request.section >= 0) {
    return reject("rpc-call (Jak 2 STR, chunked files unimplemented)");
  }

  g_stats.str_requests++;
  std::string basename;
  u32 length = 0;
  if (uppercase_basename(request, &basename) && read_str_file(request, basename, &length)) {
    write_str_reply(request, recv_buffer, STR_RPC_RESULT_DONE, length);
    g_stats.str_reads++;
    g_stats.str_bytes += length;
  } else {
    write_str_reply(request, recv_buffer, STR_RPC_RESULT_ERROR, 0);
    g_stats.str_failures++;
  }
  return 0;
}

u64 rpc_call(u64* args) {
  if (!args) {
    return reject("rpc-call (Jak 2 sound, missing arguments)");
  }

  const s32 channel = (s32)args[0];
  const u32 function = (u32)args[1];
  const u32 send_buffer = (u32)args[3];
  const s32 send_size = (s32)args[4];
  const u32 recv_buffer = (u32)args[5];
  const s32 recv_size = (s32)args[6];

  if (channel == kLoaderChannel) {
    return loader_rpc(send_buffer, send_size, recv_buffer, recv_size);
  }
  if (channel == kStrChannel) {
    return str_rpc(function, send_buffer, send_size, recv_buffer, recv_size);
  }
  return reject("rpc-call (Jak 2 sound, unimplemented channel)");
}

u64 rpc_busy(u64 channel) {
  if ((s32)channel != kLoaderChannel && (s32)channel != kStrChannel) {
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
