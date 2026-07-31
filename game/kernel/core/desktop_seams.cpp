/*!
 * @file desktop_seams.cpp
 * Definitions for the desktop-only symbols that the portable Jak 1 kernel subset still references
 * at link time.
 *
 * Every function in this file is a STUB. None of them are implemented, and none of them return a
 * plausible-looking failure value: they abort with a message naming the missing subsystem. They
 * exist so that the real kernel translation units can be linked without also linking the desktop
 * windowing, IOP, sound, and debugger-transport code.
 *
 * If you hit one of these at runtime, the answer is to implement the subsystem, not to soften the
 * stub.
 *
 * Subsystems intentionally not in this library:
 *   - game/kernel/{common,jak1}/kmachine.cpp   : IOP boot, video, pads, PC-port functions (SDL,
 *                                                OpenGL, Discord, sqlite)
 *   - game/kernel/{common,jak1}/ksound.cpp     : 989snd / overlord sound
 *   - game/kernel/jak1/kboot.cpp               : desktop boot + GOAL kernel dispatch loop
 *   - game/sce/sif_ee.cpp                      : EE<->IOP bridge and host file I/O (needs the IOP
 *                                                thread emulation and a desktop file layout)
 *   - game/sce/deci2.cpp, game/system/**       : DECI2 debugger transport and sockets
 *   - game/mips2c/**                           : hand-translated PS2 VU/asm renderer + collision
 */

#include <cstdio>
#include <utility>

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/kernel/common/kmachine.h"
#include "game/kernel/jak1/kmachine.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/mips2c_table.h"
#include "game/sce/deci2.h"
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

/*!
 * Upstream's CacheFlush is already a no-op on PC (the PS2 cache instructions have no equivalent),
 * so this is a faithful implementation rather than a stub. It is duplicated here only because the
 * rest of kmachine.cpp is not portable.
 */
void CacheFlush(void* mem, int size) {
  (void)mem;
  (void)size;
}

// ---------------------------------------------------------------------------------------------
// game/kernel/jak1/kmachine.cpp
// ---------------------------------------------------------------------------------------------

namespace {

/*!
 * Every GOAL symbol the real jak1::InitMachineScheme fills in: the PS2 library shims, the pad and
 * file-stream entry points, the system-config readers, the sound RPC, and the PC port's own
 * functions. Taken from game/kernel/jak1/kmachine.cpp, game/kernel/jak1/ksound.cpp and
 * init_common_pc_port_functions in game/kernel/common/kmachine.cpp.
 *
 * None of them are implemented here. They exist so that GOAL calling one reports which function it
 * wanted, instead of reading a symbol that holds 0 and faulting in the guard page with no name
 * attached.
 */
const char* const kMachineFunctionNames[] = {
    "__pc-set-levels",
    "pc-discord-rpc-update",
    "put-display-env",
    "syncv",
    "sync-path",
    "reset-path",
    "reset-graph",
    "dma-sync",
    "gs-put-imr",
    "gs-get-imr",
    "gs-store-image",
    "flush-cache",
    "cpad-open",
    "cpad-get-data",
    "install-handler",
    "install-debug-handler",
    "file-stream-open",
    "file-stream-close",
    "file-stream-length",
    "file-stream-seek",
    "file-stream-read",
    "file-stream-write",
    "scf-get-language",
    "scf-get-time",
    "scf-get-aspect",
    "scf-get-volume",
    "scf-get-territory",
    "scf-get-timeout",
    "scf-get-inactive-timeout",
    "dma-to-iop",
    "kernel-shutdown",
    "aybabtu",
    "rpc-call",
    "rpc-busy?",
    "test-load-dgo-c",
    "pc-sound-set-flava-hack",
    "pc-sound-set-fade-hack",
    "__read-ee-timer",
    "__mem-move",
    "__send-gfx-dma-chain",
    "__pc-texture-upload-now",
    "__pc-texture-relocate",
    "__pc-get-mips2c",
    "__pc-force-reload-all-levels",
    "__pc-force-reload-level",
    "__pc-force-reload-common-level",
    "pc-get-display-id",
    "pc-set-display-id!",
    "pc-get-display-name",
    "pc-get-display-mode",
    "pc-set-display-mode!",
    "pc-get-display-count",
    "pc-get-active-display-size",
    "pc-get-active-display-refresh-rate",
    "pc-get-window-size",
    "pc-get-window-scale",
    "pc-set-window-size!",
    "pc-get-num-resolutions",
    "pc-get-resolution",
    "pc-is-supported-resolution?",
    "pc-get-controller-name",
    "pc-get-current-bind",
    "pc-get-controller-count",
    "pc-get-controller-index",
    "pc-set-controller!",
    "pc-get-keyboard-enabled?",
    "pc-set-keyboard-enabled!",
    "pc-set-mouse-options!",
    "pc-set-mouse-camera-sens!",
    "pc-ignore-background-controller-events!",
    "pc-current-controller-has-led?",
    "pc-current-controller-has-rumble?",
    "pc-set-controller-led!",
    "pc-waiting-for-bind?",
    "pc-set-waiting-for-bind!",
    "pc-stop-waiting-for-bind!",
    "pc-reset-bindings-to-defaults!",
    "pc-set-auto-hide-cursor!",
    "pc-get-pressure-sensitivity-enabled?",
    "pc-set-pressure-sensitivity-enabled!",
    "pc-set-axis-scale!",
    "pc-get-axis-scale",
    "pc-current-controller-has-pressure-sensitivity?",
    "pc-current-controller-has-trigger-effect-support?",
    "pc-get-trigger-effects-enabled?",
    "pc-set-trigger-effects-enabled!",
    "pc-clear-trigger-effect!",
    "pc-send-trigger-effect-feedback!",
    "pc-send-trigger-effect-vibrate!",
    "pc-send-trigger-effect-weapon!",
    "pc-send-trigger-rumble!",
    "pc-set-vsync",
    "pc-set-msaa",
    "pc-set-frame-rate",
    "pc-set-game-resolution",
    "pc-set-brightness-contrast",
    "pc-set-letterbox",
    "pc-renderer-tree-set-lod",
    "pc-set-collision-mode",
    "pc-set-collision-mask",
    "pc-get-collision-mask",
    "pc-set-collision-wireframe",
    "pc-set-collision",
    "pc-set-gfx-hack",
    "pc-get-os",
    "pc-get-unix-timestamp",
    "pc-treat-pad0-as-pad1",
    "pc-is-imgui-visible?",
    "pc-filepath-exists?",
    "pc-mkdir-file-path",
    "pc-discord-rpc-set",
    "pc-prof",
    "pc-rand",
    "pc-encode-utf8-string",
    "pc-filter-debug-string?",
    "pc-screen-shot",
    "pc-register-screen-shot-settings",
};
constexpr int kMachineFunctionCount = int(sizeof(kMachineFunctionNames) / sizeof(const char*));

bool g_machine_stubs_abort = true;
bool g_machine_stub_reported[kMachineFunctionCount];

u64 machine_function_called(int index) {
  if (g_machine_stubs_abort) {
    missing("the machine layer (kmachine.cpp)", kMachineFunctionNames[index]);
  }
  if (!g_machine_stub_reported[index]) {
    g_machine_stub_reported[index] = true;
    std::fprintf(stdout, "  MISSING MACHINE FUNCTION: %s\n", kMachineFunctionNames[index]);
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
void install_machine_function_stubs(std::integer_sequence<int, Index...>) {
  (jak1::make_function_symbol_from_c(kMachineFunctionNames[Index],
                                     (void*)&machine_function_stub<Index>),
   ...);
}

}  // namespace

void goal_kernel_core_set_machine_stub_mode(bool abort_when_called) {
  g_machine_stubs_abort = abort_when_called;
}

namespace jak1 {
/*!
 * The machine layer is not in this library, so this installs a loudly-failing GOAL function object
 * for every symbol it would define, and sets the three stack constants, which are plain facts about
 * where GOAL's own stack lives (see docs/aot-stack-model.md).
 *
 * This is not an implementation of the machine layer and does not pretend to be one: calling any of
 * these functions from GOAL either aborts or, in the reporting mode the boot probe uses, prints the
 * function's name and returns 0. A run that continues past one of those messages is measuring how
 * far the loader gets, not demonstrating that anything works.
 */
void InitMachineScheme() {
  install_machine_function_stubs(std::make_integer_sequence<int, kMachineFunctionCount>{});
  intern_from_c("*stack-top*")->value = 0x07ffc000;
  intern_from_c("*stack-base*")->value = 0x07ffffff;
  intern_from_c("*stack-size*")->value = 0x4000;
}
}  // namespace jak1

// ---------------------------------------------------------------------------------------------
// game/sce/sif_ee.cpp - host file I/O and the EE<->IOP RPC bridge
// ---------------------------------------------------------------------------------------------

namespace ee {

s32 sceOpen(const char* filename, s32 flag) {
  (void)filename;
  (void)flag;
  missing("host file I/O", "ee::sceOpen");
}

s32 sceClose(s32 fd) {
  (void)fd;
  missing("host file I/O", "ee::sceClose");
}

s32 sceRead(s32 fd, void* buf, s32 nbyte) {
  (void)fd;
  (void)buf;
  (void)nbyte;
  missing("host file I/O", "ee::sceRead");
}

s32 sceWrite(s32 fd, const void* buf, s32 nbyte) {
  (void)fd;
  (void)buf;
  (void)nbyte;
  missing("host file I/O", "ee::sceWrite");
}

s32 sceLseek(s32 fd, s32 offset, s32 where) {
  (void)fd;
  (void)offset;
  (void)where;
  missing("host file I/O", "ee::sceLseek");
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

// ---------------------------------------------------------------------------------------------
// game/mips2c/mips2c_table.cpp
//
// The mips2c table names every hand-translated PS2 renderer/collision function for all three
// games, so linking it pulls in the whole game/mips2c tree. The link callback map is empty here,
// which means no mips2c function is ever registered, and any object file that asks the linker for
// one aborts below instead of silently linking against nothing.
// ---------------------------------------------------------------------------------------------

namespace Mips2C {

LinkedFunctionTable gLinkedFunctionTable;
PerGameVersion<std::unordered_map<std::string, std::vector<void (*)()>>> gMips2CLinkCallbacks = {
    {}, {}, {}, {}};

void LinkedFunctionTable::reg(const std::string& name, u64 (*exec)(void*), u32 goal_stack_size) {
  (void)exec;
  (void)goal_stack_size;
  lg::error("[kernel-core] mips2c function {} tried to register.", name);
  missing("the mips2c function library", "Mips2C::LinkedFunctionTable::reg");
}

u32 LinkedFunctionTable::get(const std::string& name) {
  lg::error("[kernel-core] mips2c function {} was requested by the linker.", name);
  missing("the mips2c function library", "Mips2C::LinkedFunctionTable::get");
}

}  // namespace Mips2C
