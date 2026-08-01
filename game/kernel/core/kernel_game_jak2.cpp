/*!
 * @file kernel_game_jak2.cpp
 * The Jak 2 implementation of the kernel core's per-game seam (kernel_game.h).
 * Compiled only into jak2-kernel-core.
 *
 * Also defines, in namespace jak2, the small pieces of game/kernel/jak2/{kboot,kmachine}.cpp that
 * the jak2 kernel translation units reference at link time but whose real homes are not part of
 * this library: the boot globals and KernelDispatch (kboot.cpp is the desktop entry point), and
 * loud stubs for the sqlite debug interface (kmachine.cpp is the desktop machine layer).
 */

#include <cstring>

#include "common/symbols.h"
#include "common/util/Assert.h"

#include "game/kernel/common/fileio.h"
#include "game/kernel/common/kboot.h"
#include "game/kernel/common/kdgo.h"
#include "game/kernel/common/kdsnetm.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/klisten.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kmemcard.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/common/ksocket.h"
#include "game/kernel/core/kernel_game.h"
#include "game/kernel/core/mips2c_seam.h"
#include "game/kernel/jak2/kboot.h"
#include "game/kernel/jak2/kdgo.h"
#include "game/kernel/jak2/klisten.h"
#include "game/kernel/jak2/kmachine.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/runtime.h"

namespace Mips2C {
// mips2c_seam.cpp
void reserve_mips2c_stack();
void forget_mips2c_registrations();
}  // namespace Mips2C

namespace {

/*!
 * Every GOAL symbol the real jak2::InitMachineScheme fills in: the PS2 library shims, the pad,
 * mouse and file-stream entry points, the system-config readers, the sound RPC, and the PC port's
 * own functions. Taken from game/kernel/jak2/kmachine.cpp, game/kernel/jak2/ksound.cpp and
 * init_common_pc_port_functions in game/kernel/common/kmachine.cpp.
 */
const char* const kJak2MachineFunctionNames[] = {
    // game/kernel/jak2/kmachine.cpp InitMachineScheme
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
    "mouse-get-data",
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
    // game/kernel/jak2/kmachine.cpp InitMachine_PCPort
    "__pc-set-levels",
    "__pc-set-active-levels",
    "__pc-get-tex-remap",
    "pc-init-autosplitter-struct",
    "pc-discord-rpc-update",
    "alloc-vagdir-names",
    "pc-fetch-external-speedrun-times",
    "pc-fetch-external-race-times",
    "pc-fetch-external-highscores",
    "pc-get-external-speedrun-time",
    "pc-get-external-race-time",
    "pc-get-external-highscore",
    "pc-get-num-external-speedrun-times",
    "pc-get-num-external-race-times",
    "pc-get-num-external-highscores",
    "pc-sr-mode-get-practice-entries-amount",
    "pc-sr-mode-get-practice-entry-name",
    "pc-sr-mode-get-practice-entry-continue-point",
    "pc-sr-mode-init-practice-info!",
    "pc-sr-mode-get-practice-entry-history-success",
    "pc-sr-mode-get-practice-entry-history-attempts",
    "pc-sr-mode-get-practice-entry-session-success",
    "pc-sr-mode-get-practice-entry-session-attempts",
    "pc-sr-mode-get-practice-entry-avg-time",
    "pc-sr-mode-get-practice-entry-fastest-time",
    "pc-sr-mode-record-practice-entry-attempt!",
    "pc-sr-mode-init-custom-category-info!",
    "pc-sr-mode-get-custom-category-amount",
    "pc-sr-mode-get-custom-category-name",
    "pc-sr-mode-get-custom-category-continue-point",
    "pc-sr-mode-dump-new-custom-category",
    // game/kernel/jak2/ksound.cpp InitSoundScheme
    "rpc-call",
    "rpc-busy?",
    "test-load-dgo-c",
    "pc-sound-set-flava-hack",
    "pc-sound-set-fade-hack",
    // init_common_pc_port_functions in game/kernel/common/kmachine.cpp
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
constexpr int kJak2MachineFunctionCount =
    int(sizeof(kJak2MachineFunctionNames) / sizeof(const char*));

}  // namespace

namespace jak2 {

char DebugBootUser[64];
char DebugBootArtGroup[64];

void kboot_init_globals() {
  memset(DebugBootUser, 0, sizeof(DebugBootUser));
  memset(DebugBootArtGroup, 0, sizeof(DebugBootArtGroup));
  // upstream asks the REPL config for a username; this library has no REPL
  strcpy(DebugBootUser, "unknown");
}

/*!
 * The GOAL kernel's frame, from game/kernel/jak2/kboot.cpp with the desktop-only pieces gone:
 * no throttle sleep (the host paces frames) and no dispatch-time print.
 */
void KernelDispatch(u32 dispatcher_func) {
  // place our stack at the end of EE memory
  u64 goal_stack = u64(g_ee_main_mem) + EE_MAIN_MEM_SIZE - 16;

  // try to get a message from the listener, and process it if needed
  Ptr<char> new_message = WaitForMessageAndAck();
  if (new_message.offset) {
    ProcessListenerMessage(new_message);
  }

  // remember the old listener
  auto old_listener_function = ListenerFunction->value();

  // run the kernel!
  if (MasterUseKernel) {
    call_goal_on_stack(Ptr<Function>(dispatcher_func), goal_stack, s7.offset, g_ee_main_mem);
  } else if (ListenerFunction->value() != s7.offset) {
    call_goal_on_stack(Ptr<Function>(ListenerFunction->value()), goal_stack, s7.offset,
                       g_ee_main_mem);
    ListenerFunction->value() = s7.offset;
  }

  // flush stdout
  ClearPending();

  // now run the extra "kernel function"
  auto bonus_function = KernelFunction->value();
  if (bonus_function != s7.offset) {
    KernelFunction->value() = s7.offset;
    call_goal_on_stack(Ptr<Function>(bonus_function), goal_stack, s7.offset, g_ee_main_mem);
  }

  // send ack to indicate that the listener function has been processed and the result printed
  if (MasterDebug && ListenerFunction->value() != old_listener_function) {
    SendAck();
  }
}

void KernelShutdown(u32 reason) {
  MasterExit = (RuntimeExitStatus)reason;
}

// The sqlite debug interface lives in game/kernel/jak2/kmachine.cpp, which is not part of this
// library. sql_query_sync only reaches these when *debug-segment* is on, which it never is here.
void initialize_sql_db() {
  ASSERT_NOT_REACHED_MSG("the sqlite debug interface is not part of this build");
}

sqlite::GenericResponse run_sql_query(const std::string& /*query*/) {
  ASSERT_NOT_REACHED_MSG("the sqlite debug interface is not part of this build");
}

/*!
 * The machine layer is not in this library, so this installs a loudly-failing GOAL function object
 * for every symbol it would define, sets the stack constants - plain facts about where GOAL's own
 * stack lives in this port (see docs/aot-stack-model.md) - and fills the boot symbols the real
 * InitMachineScheme would fill.
 *
 * This is not an implementation of the machine layer and does not pretend to be one: calling any of
 * these functions from GOAL either aborts or, in the reporting mode the boot probe uses, prints the
 * function's name and returns 0. A run that continues past one of those messages is measuring how
 * far the loader gets, not demonstrating that anything works.
 */
void InitMachineScheme() {
  goal_kernel_core_install_machine_stubs(kJak2MachineFunctionNames, kJak2MachineFunctionCount);
  goal_kernel_core_install_implemented_machine_functions();
  intern_from_c("*stack-top*")->value() = 0x07ffc000;
  intern_from_c("*stack-base*")->value() = 0x07ffffff;
  intern_from_c("*stack-size*")->value() = 0x4000;
  intern_from_c("*kernel-boot-message*")->value() = intern_from_c(DebugBootMessage).offset;
  intern_from_c("*user*")->value() = (u32)make_string_from_c(DebugBootUser);
  if (DiskBoot) {
    intern_from_c("*kernel-boot-mode*")->value() = intern_from_c("boot").offset;
  }
  intern_from_c("*kernel-boot-level*")->value() = intern_from_c(DebugBootLevel).offset;
  intern_from_c("*kernel-boot-art-group*")->value() = (u32)make_string_from_c(DebugBootArtGroup);
  // The DiskBoot branch of the real InitMachineScheme loads GAME.CGO here; the host drives DGO
  // loads itself through goal_dgo_load, so nothing is loaded behind its back.
}

}  // namespace jak2

GameVersion goal_game_version() {
  return GameVersion::Jak2;
}

void goal_game_init_kernel_globals() {
  fileio_init_globals();
  kboot_init_globals_common();
  jak2::kboot_init_globals();
  kdgo_init_globals();
  jak2::kdgo_init_globals();
  kdsnetm_init_globals_common();
  klink_init_globals();
  kscheme_init_globals_common();
  jak2::kscheme_init_globals();
  kmalloc_init_globals_common();
  klisten_init_globals();
  jak2::klisten_init_globals();
  kmemcard_init_globals();
  kprint_init_globals_common();

  // The symbol-name CRC table, normally filled in jak2::goal_main (game/kernel/jak2/kboot.cpp),
  // which is the desktop entry point and is not part of this library. See kernel_game_jak1.cpp
  // for what breaks without it.
  init_crc();
}

int32_t goal_game_init_symbol_and_types() {
  return jak2::InitSymbolAndTypes();
}

void goal_game_init_machine_scheme() {
  jak2::InitMachineScheme();
}

void goal_game_register_mips2c() {
  // The jak2 mips2c function library has not been ported into this build. The seam is still
  // reserved so a `__pc-get-mips2c` lookup fails loudly by name instead of faulting.
  Mips2C::forget_mips2c_registrations();
  Mips2C::reserve_mips2c_stack();
}

uint32_t goal_game_intern(const char* name) {
  return jak2::intern_from_c(name).offset;
}

int32_t* goal_game_symbol_slot(const char* name) {
  return (int32_t*)&jak2::intern_from_c(name)->value();
}

uint32_t goal_game_symbol_value(const char* name) {
  return jak2::intern_from_c(name)->value();
}

void goal_game_set_symbol_value(const char* name, uint32_t value) {
  jak2::intern_from_c(name)->value() = value;
}

uint32_t goal_game_find_symbol(const char* name, uint32_t* value_out) {
  auto sym = jak2::find_symbol_from_c(name);
  if (!sym.offset) {
    return 0;
  }
  if (value_out) {
    *value_out = sym->value();
  }
  return sym.offset;
}

const char* goal_game_symbol_name(uint32_t symbol_address) {
  if (!symbol_address || symbol_address >= EE_MAIN_MEM_SIZE || !g_ee_main_mem) {
    return "";
  }
  return jak2::symbol_name_cstr(*Ptr<Symbol4<u32>>(symbol_address).c());
}

uint32_t goal_game_intern_type(const char* name, int method_count) {
  return jak2::intern_type_from_c(name, (u64)method_count).offset;
}

uint32_t goal_game_function_type_value() {
  return jak2::u32_in_fixed_sym(jak2_symbols::FIX_SYM_FUNCTION_TYPE);
}

const char* goal_game_type_name_of_symbol(const char* name) {
  auto sym = jak2::find_symbol_from_c(name);
  if (!sym.offset) {
    return nullptr;
  }
  auto type = Ptr<jak2::Type>(sym->value());
  if (!type.offset || !type->symbol.offset) {
    return nullptr;
  }
  return jak2::symbol_name_cstr(*type->symbol.c());
}

uint32_t goal_game_empty_pair_offset() {
  return s7.offset + jak2_symbols::S7_OFF_FIX_SYM_EMPTY_PAIR;
}

uint32_t goal_game_false_offset() {
  return s7.offset + jak2_symbols::FIX_SYM_FALSE;
}

uint32_t goal_game_true_offset() {
  return s7.offset + jak2_symbols::FIX_SYM_TRUE;
}

uint32_t goal_game_make_function_from_native(void* func) {
  return jak2::make_function_from_native(func).offset;
}

void goal_game_make_function_symbol(const char* name, void* func) {
  jak2::make_function_symbol_from_c(name, func);
}

const GoalGameProcessOffsets& goal_game_process_offsets() {
  // kernel/gkernel-h.gc, deftype offsets minus 4: the jak2 process-tree gained `clock` and the
  // jak2 process gained `level`, `event-hook` and two pad words ahead of these fields.
  static const GoalGameProcessOffsets offsets = {36, 44, 48, 104};
  return offsets;
}
