/*!
 * @file desktop_seams.cpp
 * Definitions for the desktop-only symbols that the portable kernel subset still references
 * at link time. Game-neutral: the per-game pieces reach this file through kernel_game.h.
 *
 * Almost every function in this file is a STUB. A stub is not implemented and does not return a
 * plausible-looking failure value: it aborts with a message naming the missing subsystem. They
 * exist so that the real kernel translation units can be linked without also linking the desktop
 * windowing, IOP, sound, and debugger-transport code.
 *
 * If you hit one of these at runtime, the answer is to implement the subsystem, not to soften the
 * stub.
 *
 * The exceptions, each marked where it is defined, are the ones with nothing platform-specific
 * left in them once the PS2 hardware is gone: host file I/O (`ee::sceOpen` and friends, against
 * the configured data directory), `__mem-move`, `__read-ee-timer`, `__pc-get-mips2c`, and the
 * `scf-get-*` readers of the PS2 system configuration.
 *
 * Subsystems intentionally not in this library:
 *   - game/kernel/{common,jak1}/kmachine.cpp   : IOP boot, video, pads, PC-port functions (SDL,
 *                                                OpenGL, Discord, sqlite)
 *   - game/kernel/{common,jak1}/ksound.cpp     : 989snd / overlord sound
 *   - game/kernel/jak1/kboot.cpp               : desktop boot + GOAL kernel dispatch loop
 *   - game/sce/sif_ee.cpp                      : the EE<->IOP RPC bridge (the file calls it also
 *                                                declares are implemented below)
 *   - game/sce/deci2.cpp, game/system/**       : DECI2 debugger transport and sockets
 *   - game/mips2c/mips2c_table.cpp             : names all four games; core/mips2c_seam.cpp
 *                                                registers the Jak 1 functions instead
 */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/Timer.h"

#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kboot.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/kernel_game.h"
#include "game/mips2c/mips2c_table.h"
#include "game/sce/deci2.h"
#include "game/sce/libscf.h"
#include "game/sce/sif_ee.h"

namespace {
[[noreturn]] void missing(const char* subsystem, const char* symbol) {
  lg::error("[kernel-core] {} is not part of this build; {} cannot be used.", subsystem, symbol);
  ASSERT_NOT_REACHED_MSG("kernel-core stub called");
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// game/kernel/common/kmachine.cpp
// ---------------------------------------------------------------------------------------------

u32 vif1_interrupt_handler = 0;
u32 vblank_interrupt_handler = 0;

/*!
 * Upstream's CacheFlush is already a no-op on PC (the PS2 cache instructions have no equivalent),
 * so this is a faithful implementation rather than a stub. It is duplicated here only because the
 * rest of kmachine.cpp is not portable.
 */
void CacheFlush(void* mem, int size) {
  (void)mem;
  (void)size;
}

void InstallHandler(u32 handler_idx, u32 handler_func) {
  switch (handler_idx) {
    case 3:
      vblank_interrupt_handler = handler_func;
      break;
    case 5:
      vif1_interrupt_handler = handler_func;
      break;
    default:
      lg::error("unknown handler: {}\n", handler_idx);
      ASSERT(false);
  }
}

// ---------------------------------------------------------------------------------------------
// game/kernel/jak1/kmachine.cpp
// ---------------------------------------------------------------------------------------------

namespace {

/*!
 * The loudly-failing machine layer.
 *
 * Each game's kernel_game_jakN.cpp lists every GOAL symbol its real InitMachineScheme would fill
 * in - the PS2 library shims, the pad and file-stream entry points, the system-config readers, the
 * sound RPC, and the PC port's own functions - and installs the list through
 * goal_kernel_core_install_machine_stubs below.
 *
 * They are stubs, and they exist so that GOAL calling one reports which function it wanted instead
 * of reading a symbol that holds 0 and faulting in the guard page with no name attached. The ones
 * that goal_kernel_core_install_implemented_machine_functions overwrites are the exception.
 */
constexpr int kMaxMachineStubs = 192;
const char* const* g_machine_stub_names = nullptr;
int g_machine_stub_count = 0;

bool g_machine_stubs_abort = true;
bool g_machine_stub_reported[kMaxMachineStubs];

u64 machine_function_called(int index) {
  if (g_machine_stubs_abort) {
    missing("the machine layer (kmachine.cpp)", g_machine_stub_names[index]);
  }
  if (!g_machine_stub_reported[index]) {
    g_machine_stub_reported[index] = true;
    std::fprintf(stdout, "  MISSING MACHINE FUNCTION: %s\n", g_machine_stub_names[index]);
    std::fflush(stdout);
  }
  return 0;
}

// one distinct native entry point per name, so the stub knows which symbol was called
template <int Index>
u64 machine_function_stub() {
  return machine_function_called(Index);
}

template <int... Index>
void collect_machine_stub_entries(void* (&out)[kMaxMachineStubs],
                                  std::integer_sequence<int, Index...>) {
  ((out[Index] = (void*)&machine_function_stub<Index>), ...);
}

void* const* machine_stub_entry_points() {
  static void* entries[kMaxMachineStubs];
  static bool built = false;
  if (!built) {
    collect_machine_stub_entries(entries, std::make_integer_sequence<int, kMaxMachineStubs>{});
    built = true;
  }
  return entries;
}

/*!
 * The few functions in the list above that are not machine-specific at all: they move memory, read
 * a clock, and read the PS2's system configuration. They are implemented rather than stubbed
 * because nothing loads without them - the PC port's `ultimate-memcpy` is a call to `__mem-move`,
 * so a stub there means every data object in a DGO gets linked against zeroes - and because
 * nothing platform-dependent is left in them once the PS2 hardware is gone. Everything else in the
 * list stays a stub.
 */
u64 pc_mem_move(u32 dst, u32 src, u32 size) {
  memmove(Ptr<u8>(dst).c(), Ptr<u8>(src).c(), size);
  return 0;
}

u64 read_ee_timer() {
  // The PS2's EE timer runs at 300 MHz. GOAL only ever takes differences of it.
  static Timer ee_clock;
  return (ee_clock.getNs() * 3) / 10;
}

/*! GOAL's `def-mips2c` asks for a hand-translated PS2 function by name. See mips2c_seam.cpp. */
u64 pc_get_mips2c(u32 name) {
  return Mips2C::gLinkedFunctionTable.get(Ptr<String>(name).c()->data());
}

/*!
 * The system-configuration readers, from game/kernel/common/kmachine.cpp's `Decode*`. They read
 * the `masterConfig` block that kernel_core.cpp fills in on behalf of the absent
 * game/kernel/jak1/kboot.cpp.
 *
 * These are not machine-specific: on any platform without a PS2 system configuration they are the
 * boot defaults, which is exactly what upstream's desktop port returns. A stub here is worse than
 * missing, because GOAL believes the zero it gets: `scf-get-volume` of 0 is what settings.gc turns
 * into a muted ambient sound group and into ambient speech scaling every other group to zero.
 */
u64 decode_language() {
  return masterConfig.language;
}

u64 decode_aspect() {
  return masterConfig.aspect;
}

u64 decode_volume() {
  return masterConfig.volume;
}

u64 decode_territory() {
  return GAME_TERRITORY_SCEA;
}

u64 decode_timeout() {
  return masterConfig.timeout;
}

u64 decode_inactive_timeout() {
  return masterConfig.inactive_timeout;
}

void decode_time(u32 ptr) {
  ee::sceCdReadClock(Ptr<ee::sceCdCLOCK>(ptr).c());
}

}  // namespace

void goal_kernel_core_install_machine_stubs(const char* const* names, int count) {
  ASSERT_MSG(count <= kMaxMachineStubs, "more machine functions than stub entry points");
  g_machine_stub_names = names;
  g_machine_stub_count = count;
  memset(g_machine_stub_reported, 0, sizeof(g_machine_stub_reported));
  for (int i = 0; i < count; i++) {
    goal_game_make_function_symbol(names[i], machine_stub_entry_points()[i]);
  }
}

void goal_kernel_core_install_implemented_machine_functions() {
  goal_game_make_function_symbol("__mem-move", (void*)pc_mem_move);
  goal_game_make_function_symbol("__read-ee-timer", (void*)read_ee_timer);
  goal_game_make_function_symbol("__pc-get-mips2c", (void*)pc_get_mips2c);
  goal_game_make_function_symbol("scf-get-language", (void*)decode_language);
  goal_game_make_function_symbol("scf-get-time", (void*)decode_time);
  goal_game_make_function_symbol("scf-get-aspect", (void*)decode_aspect);
  goal_game_make_function_symbol("scf-get-volume", (void*)decode_volume);
  goal_game_make_function_symbol("scf-get-territory", (void*)decode_territory);
  goal_game_make_function_symbol("scf-get-timeout", (void*)decode_timeout);
  goal_game_make_function_symbol("scf-get-inactive-timeout", (void*)decode_inactive_timeout);
  goal_game_make_function_symbol("install-handler", (void*)InstallHandler);
}

void goal_kernel_core_set_machine_stub_mode(bool abort_when_called) {
  g_machine_stubs_abort = abort_when_called;
}

/*!
 * Report a machine-layer call that has no implementation here, the same way the stubs above do:
 * abort, or - in the reporting mode the probes use - name it once and return 0.
 *
 * This exists for a symbol that is only partly implemented, so the part that is missing still says
 * so. `rpc-call` is one: the DGO channel is answered for real and every other channel is not.
 */
u64 goal_kernel_core_machine_stub_report(const char* what) {
  if (g_machine_stubs_abort) {
    missing("the machine layer (kmachine.cpp)", what);
  }
  static std::vector<std::string> reported;
  for (const auto& seen : reported) {
    if (seen == what) {
      return 0;
    }
  }
  reported.emplace_back(what);
  std::fprintf(stdout, "  MISSING MACHINE FUNCTION: %s\n", what);
  std::fflush(stdout);
  return 0;
}

// The per-game InitMachineScheme - the name list and the stack constants - lives in
// kernel_game_jak1.cpp / kernel_game_jak2.cpp and drives the two installers above.

// ---------------------------------------------------------------------------------------------
// game/sce/sif_ee.cpp - host file I/O and the EE<->IOP RPC bridge
//
// The file calls are implemented, because the runtime cannot load anything without them. They are
// ordinary POSIX file descriptors, and every name is resolved under the data directory the host
// application configured (goal_kernel_core_set_data_directory). Nothing here knows a path of its
// own, so a build with no data directory set can open nothing at all.
//
// The RPC calls are still stubs: the IOP/overlord thread emulation is not part of this library.
// Its one job that this platform needs - streaming a DGO off the disc - is done synchronously in
// dgo_loader.cpp instead. See that file for why.
// ---------------------------------------------------------------------------------------------

namespace ee {

s32 sceOpen(const char* filename, s32 flag) {
  if (!filename) {
    return -1;
  }
  char path[1024];
  if (goal_kernel_core_resolve_data_path(filename, path, sizeof(path)) != GOAL_KERNEL_CORE_OK) {
    lg::error("[kernel-core] cannot open '{}': {}", filename, goal_kernel_core_last_error());
    return -1;
  }

  // SCE_RDWR is SCE_RDONLY | SCE_WRONLY, so the access mode has to be compared, not masked.
  int posix_flags;
  switch (flag & SCE_RDWR) {
    case SCE_RDWR:
      posix_flags = O_RDWR;
      break;
    case SCE_WRONLY:
      posix_flags = O_WRONLY;
      break;
    default:
      posix_flags = O_RDONLY;
      break;
  }
  if (flag & SCE_CREAT) {
    posix_flags |= O_CREAT;
  }
  if (flag & SCE_TRUNC) {
    posix_flags |= O_TRUNC;
  }
  if (flag & SCE_APPEND) {
    posix_flags |= O_APPEND;
  }

  const int fd = ::open(path, posix_flags, 0644);
  if (fd < 0) {
    lg::error("[kernel-core] cannot open '{}': {}", path, strerror(errno));
    return -1;
  }
  return fd;
}

s32 sceClose(s32 fd) {
  return fd < 0 ? -1 : ::close(fd);
}

s32 sceRead(s32 fd, void* buf, s32 nbyte) {
  if (fd < 0 || !buf || nbyte < 0) {
    return -1;
  }
  // A short read is normal for a pipe and never expected for a regular file, so this loops rather
  // than returning a count the caller would have to know to check.
  s32 done = 0;
  while (done < nbyte) {
    const ssize_t got = ::read(fd, (u8*)buf + done, (size_t)(nbyte - done));
    if (got < 0) {
      return -1;
    }
    if (got == 0) {
      break;
    }
    done += (s32)got;
  }
  return done;
}

s32 sceWrite(s32 fd, const void* buf, s32 nbyte) {
  if (fd < 0 || !buf || nbyte < 0) {
    return -1;
  }
  s32 done = 0;
  while (done < nbyte) {
    const ssize_t put = ::write(fd, (const u8*)buf + done, (size_t)(nbyte - done));
    if (put <= 0) {
      return -1;
    }
    done += (s32)put;
  }
  return done;
}

s32 sceLseek(s32 fd, s32 offset, s32 where) {
  if (fd < 0) {
    return -1;
  }
  int whence;
  switch (where) {
    case SCE_SEEK_SET:
      whence = SEEK_SET;
      break;
    case SCE_SEEK_CUR:
      whence = SEEK_CUR;
      break;
    case SCE_SEEK_END:
      whence = SEEK_END;
      break;
    default:
      return -1;
  }
  return (s32)::lseek(fd, offset, whence);
}

s32 sceSifCallRpc(sceSifClientData* bd,
                  u32 fno,
                  u32 mode,
                  void* send,
                  s32 ssize,
                  void* recv,
                  s32 rsize,
                  void* end_func,
                  void* end_para) {
  (void)bd;
  (void)fno;
  (void)mode;
  (void)send;
  (void)ssize;
  (void)recv;
  (void)rsize;
  (void)end_func;
  (void)end_para;
  missing("the IOP (overlord) bridge", "ee::sceSifCallRpc");
}

s32 sceSifCheckStatRpc(sceSifRpcData* bd) {
  (void)bd;
  missing("the IOP (overlord) bridge", "ee::sceSifCheckStatRpc");
}

s32 sceSifBindRpc(sceSifClientData* bd, u32 request, u32 mode) {
  (void)bd;
  (void)request;
  (void)mode;
  missing("the IOP (overlord) bridge", "ee::sceSifBindRpc");
}

// -------------------------------------------------------------------------------------------
// game/sce/deci2.cpp - DECI2 debugger transport
// -------------------------------------------------------------------------------------------

s32 sceDeci2Open(u16 protocol, void* opt, void (*handler)(s32 event, s32 param, void* opt)) {
  (void)protocol;
  (void)opt;
  (void)handler;
  missing("the DECI2 listener transport", "ee::sceDeci2Open");
}

s32 sceDeci2Close(s32 s) {
  (void)s;
  missing("the DECI2 listener transport", "ee::sceDeci2Close");
}

s32 sceDeci2ReqSend(s32 s, char dest) {
  (void)s;
  (void)dest;
  missing("the DECI2 listener transport", "ee::sceDeci2ReqSend");
}

s32 sceDeci2ExRecv(s32 s, void* buf, u16 len) {
  (void)s;
  (void)buf;
  (void)len;
  missing("the DECI2 listener transport", "ee::sceDeci2ExRecv");
}

s32 sceDeci2ExSend(s32 s, void* buf, u16 len) {
  (void)s;
  (void)buf;
  (void)len;
  missing("the DECI2 listener transport", "ee::sceDeci2ExSend");
}

void LIBRARY_sceDeci2_run_sends() {
  missing("the DECI2 listener transport", "ee::LIBRARY_sceDeci2_run_sends");
}

}  // namespace ee

// game/mips2c: the Jak 1 half of the hand-translated PS2 assembly library is in this build. It is
// wired up in mips2c_seam.cpp, which explains why the table itself is not.
