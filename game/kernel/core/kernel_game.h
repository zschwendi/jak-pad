#pragma once

/*!
 * @file kernel_game.h
 * The per-game seam of the portable kernel core.
 *
 * The kernel core's shared translation units (kernel_core.cpp, desktop_seams.cpp, aot_loader.cpp,
 * mips2c_seam.cpp, goal_native_kernel.cpp) never name a game. Everything that differs between the
 * games - which kernel globals to initialize, how a symbol is interned and read, how a function
 * object is built, where the process fields live - goes through these functions instead.
 *
 * Each kernel-core library compiles exactly one implementation: kernel_game_jak1.cpp for
 * jak1-kernel-core and kernel_game_jak2.cpp for jak2-kernel-core. One game per library, chosen at
 * link time, the same way upstream keys its kernel translation units by GameVersion.
 */

#include <cstdint>

#include "common/versions/versions.h"

/*! Process field offsets relative to the basic's object pointer (deftype offset minus 4). The
 * thread, cpu-thread and catch-frame layouts are identical across the supported games; the process
 * grew between jak1 and jak2, so its fields move. */
struct GoalGameProcessOffsets {
  int status;
  int main_thread;
  int top_thread;
  int stack_frame_top;
};

/*! cpu-thread.suspend-hook relative to the basic's object pointer. The thread layout is shared by
 * the supported games; unlike process, this field does not need a per-game seam. */
inline constexpr int kGoalCpuThreadSuspendHookOffset = 12;

// ------------------------------------------------------------------------------------------
// identity and initialization
// ------------------------------------------------------------------------------------------

GameVersion goal_game_version();

/*! The per-game kernel *_init_globals calls, plus the common ones they depend on. */
void goal_game_init_kernel_globals();

/*! The game's InitSymbolAndTypes: symbol table and fundamental types on the global heap. */
int32_t goal_game_init_symbol_and_types();

/*! Install the loudly-failing machine layer (this library has no real one) and the stack
 * constants. See desktop_seams.cpp for what the stubs do. */
void goal_game_init_machine_scheme();

/*! Register the game's mips2c function library, or reserve the seam with nothing registered when
 * the game's library has not been ported: a lookup then fails loudly by name. */
void goal_game_register_mips2c();

/*! Release per-game services that must not outlive the common kernel arena. */
void goal_game_shutdown();

/*! Install the selected game's GOAL-facing graphics adapters. gfx_host.cpp owns the host
 * callbacks; these adapters only decode the game's argument layouts. */
void goal_game_install_gfx_adapters();

/*! Run the selected game's work immediately before the host's display-paced sync callback. */
void goal_game_gfx_before_vsync();

// ------------------------------------------------------------------------------------------
// audio output
// ------------------------------------------------------------------------------------------

/*! The fixed output rate of the selected game's 989snd instance. */
int goal_game_sound_sample_rate();

/*! Pull interleaved stereo signed 16-bit frames from the selected game's installed sound owner. */
int goal_game_sound_pull_audio(int16_t* out, int frames);

// ------------------------------------------------------------------------------------------
// symbols and types
// ------------------------------------------------------------------------------------------

/*! Intern and return the symbol's GOAL address. */
uint32_t goal_game_intern(const char* name);

/*! Native pointer to the symbol's 32-bit value slot, interning the symbol. */
int32_t* goal_game_symbol_slot(const char* name);

uint32_t goal_game_symbol_value(const char* name);
void goal_game_set_symbol_value(const char* name, uint32_t value);

/*! Existing symbol's address, or 0. Fills *value_out when the symbol exists. */
uint32_t goal_game_find_symbol(const char* name, uint32_t* value_out);

/*! The name of a GOAL symbol, or "" when the address does not look like one. */
const char* goal_game_symbol_name(uint32_t symbol_address);

/*! Intern the type, creating its vtable if this is the first reference (typelink semantics). */
uint32_t goal_game_intern_type(const char* name, int method_count);

/*! The value of the fixed `function` type symbol, for building function objects. */
uint32_t goal_game_function_type_value();

/*! The type name a symbol holds, or nullptr when the symbol is missing or holds no type. */
const char* goal_game_type_name_of_symbol(const char* name);

// fixed symbols, for state reporting
uint32_t goal_game_empty_pair_offset();
uint32_t goal_game_false_offset();
uint32_t goal_game_true_offset();

// ------------------------------------------------------------------------------------------
// function objects
// ------------------------------------------------------------------------------------------

/*! A GOAL function object holding a native entry point; returns its GOAL address. */
uint32_t goal_game_make_function_from_native(void* func);

/*! Intern the symbol and set it to a new function object for `func`. */
void goal_game_make_function_symbol(const char* name, void* func);

// ------------------------------------------------------------------------------------------
// layouts
// ------------------------------------------------------------------------------------------

const GoalGameProcessOffsets& goal_game_process_offsets();

// ------------------------------------------------------------------------------------------
// shared machine-stub machinery (desktop_seams.cpp), for the per-game InitMachineScheme
// ------------------------------------------------------------------------------------------

/*!
 * Install one loudly-failing GOAL function per name. Each stub aborts, or - in the reporting mode
 * the boot probes use - prints its own name once and returns 0. `names` must stay alive for the
 * run. See desktop_seams.cpp.
 */
void goal_kernel_core_install_machine_stubs(const char* const* names, int count);

/*!
 * Overwrite the stubs that are not machine-specific at all with real implementations: __mem-move,
 * __read-ee-timer, __pc-get-mips2c, pc-rand, and the scf-get-* readers of the boot configuration.
 */
void goal_kernel_core_install_implemented_machine_functions();

/*! Install the portable file and PC-settings subset currently exercised by the Jak 2 host. */
void goal_kernel_core_install_portable_pc_settings_functions();
