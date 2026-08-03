/*!
 * @file kernel_game_jak1.cpp
 * The Jak 1 implementation of the kernel core's per-game seam (kernel_game.h).
 * Compiled only into jak1-kernel-core.
 */

#include "common/symbols.h"

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
#include "game/kernel/core/kernel_game.h"
#include "game/kernel/core/gfx_host_internal.h"
#include "game/kernel/core/mips2c_seam.h"
#include "game/kernel/core/sound_rpc.h"
#include "game/kernel/jak1/kdgo.h"
#include "game/kernel/jak1/klisten.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

namespace {

/*!
 * Every GOAL symbol the real jak1::InitMachineScheme fills in: the PS2 library shims, the pad and
 * file-stream entry points, the system-config readers, the sound RPC, and the PC port's own
 * functions. Taken from game/kernel/jak1/kmachine.cpp, game/kernel/jak1/ksound.cpp and
 * init_common_pc_port_functions in game/kernel/common/kmachine.cpp.
 */
const char* const kJak1MachineFunctionNames[] = {
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
constexpr int kJak1MachineFunctionCount =
    int(sizeof(kJak1MachineFunctionNames) / sizeof(const char*));

u64 gfx_set_levels(u32 l0, u32 l1) {
  const u32 levels[] = {l0, l1};
  goal_gfx_host_forward_levels(levels, 2, false);
  return 0;
}

u64 gfx_put_display_env(u32 ptr) {
  goal_gfx_host_forward_pmode_alpha(Ptr<u8>(ptr).c()[1] / 255.f);
  return 0;
}

}  // namespace

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
  goal_kernel_core_install_machine_stubs(kJak1MachineFunctionNames, kJak1MachineFunctionCount);
  goal_kernel_core_install_implemented_machine_functions();
  intern_from_c("*stack-top*")->value = 0x07ffc000;
  intern_from_c("*stack-base*")->value = 0x07ffffff;
  intern_from_c("*stack-size*")->value = 0x4000;
}
}  // namespace jak1

GameVersion goal_game_version() {
  return GameVersion::Jak1;
}

void goal_game_shutdown() {
  goal_sound_shutdown();
}

void goal_game_install_gfx_adapters() {
  goal_game_make_function_symbol("put-display-env", (void*)gfx_put_display_env);
  goal_game_make_function_symbol("__pc-set-levels", (void*)gfx_set_levels);
}

void goal_game_gfx_before_vsync() {}

void goal_game_init_kernel_globals() {
  fileio_init_globals();
  kboot_init_globals_common();
  kdgo_init_globals();
  jak1::kdgo_init_globals();
  kdsnetm_init_globals_common();
  klink_init_globals();
  kscheme_init_globals_common();
  jak1::kscheme_init_globals();
  kmalloc_init_globals_common();
  klisten_init_globals();
  jak1::klisten_init_globals();
  kmemcard_init_globals();
  kprint_init_globals_common();

  // GOAL hashes symbol names with its own CRC table, and kscheme_init_globals_common zeroes that
  // table. Upstream fills it in jak1::goal_main (game/kernel/jak1/kboot.cpp), which is the desktop
  // entry point and is not part of this library. Without it every hash is computed from a table of
  // zeroes: symbol interning still works, because it is self-consistent, but `EMPTY_HASH` no
  // longer matches, so `intern_from_c("_empty_")` makes an ordinary symbol instead of returning
  // the empty pair - and every static field holding '() links to it. `(null? ...)` then says no.
  init_crc();
}

int32_t goal_game_init_symbol_and_types() {
  return jak1::InitSymbolAndTypes();
}

void goal_game_init_machine_scheme() {
  jak1::InitMachineScheme();
}

void goal_game_register_mips2c() {
  goal_mips2c_register_jak1();
}

uint32_t goal_game_intern(const char* name) {
  return jak1::intern_from_c(name).offset;
}

int32_t* goal_game_symbol_slot(const char* name) {
  return (int32_t*)&jak1::intern_from_c(name)->value;
}

uint32_t goal_game_symbol_value(const char* name) {
  return jak1::intern_from_c(name)->value;
}

void goal_game_set_symbol_value(const char* name, uint32_t value) {
  jak1::intern_from_c(name)->value = value;
}

uint32_t goal_game_find_symbol(const char* name, uint32_t* value_out) {
  auto sym = jak1::find_symbol_from_c(name);
  if (!sym.offset) {
    return 0;
  }
  if (value_out) {
    *value_out = sym->value;
  }
  return sym.offset;
}

const char* goal_game_symbol_name(uint32_t symbol_address) {
  if (!symbol_address || symbol_address >= EE_MAIN_MEM_SIZE || !g_ee_main_mem) {
    return "";
  }
  return jak1::info(Ptr<jak1::Symbol>(symbol_address))->str->data();
}

uint32_t goal_game_intern_type(const char* name, int method_count) {
  return jak1::intern_type_from_c(name, (u64)method_count).offset;
}

uint32_t goal_game_function_type_value() {
  return *(s7 + jak1_symbols::FIX_SYM_FUNCTION_TYPE);
}

const char* goal_game_type_name_of_symbol(const char* name) {
  auto sym = jak1::find_symbol_from_c(name);
  if (!sym.offset) {
    return nullptr;
  }
  auto type = Ptr<jak1::Type>(sym->value);
  if (!type.offset || !type->symbol.offset) {
    return nullptr;
  }
  return jak1::info(type->symbol)->str->data();
}

uint32_t goal_game_empty_pair_offset() {
  return (s7 + jak1_symbols::FIX_SYM_EMPTY_PAIR).offset;
}

uint32_t goal_game_false_offset() {
  return (s7 + jak1_symbols::FIX_SYM_FALSE).offset;
}

uint32_t goal_game_true_offset() {
  return (s7 + jak1_symbols::FIX_SYM_TRUE).offset;
}

uint32_t goal_game_make_function_from_native(void* func) {
  return jak1::make_function_from_native(func).offset;
}

void goal_game_make_function_symbol(const char* name, void* func) {
  jak1::make_function_symbol_from_c(name, func);
}

const GoalGameProcessOffsets& goal_game_process_offsets() {
  // kernel/gkernel-h.gc, deftype offsets minus 4
  static const GoalGameProcessOffsets offsets = {32, 40, 44, 88};
  return offsets;
}
