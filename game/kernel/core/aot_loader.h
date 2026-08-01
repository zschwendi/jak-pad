#pragma once

/*!
 * @file aot_loader.h
 * Load an object file produced by the AOT C backend (goalc/aot/CBackend.cpp) into the real GOAL
 * kernel: static data goes into the real global heap, relocations are applied against the real
 * symbol table, and every AOT function gets a real GOAL `function` object on the real heap.
 *
 * This is the runtime counterpart of `goalc/aot/goal_c_runtime.h`. It replaces only the parts of
 * `game/kernel/jak1/klink.cpp` that copy and relocate an object file; nothing here writes
 * executable memory.
 *
 * Function-object representation
 * ------------------------------
 * A GOAL function object is an ordinary basic on the GOAL heap whose type is `function`. Upstream
 * writes machine code at the object's address and jumps there. AOT code lives in `__TEXT`, which
 * no 32-bit GOAL pointer can reach, so GOALPad stores the 64-bit native entry point at the same
 * address instead. Emitted call sites load it (`GOAL_FN` in goal_c_runtime.h) and so does
 * `call_goal`, which keeps one uniform representation for AOT functions and for the kernel's own
 * C functions.
 */

#include <stdint.h>

#include "game/kernel/core/kernel_core.h"
#include "goalc/aot/goal_c_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * One AOT translation unit. Every field except `tag` comes straight out of the generated header:
 *
 *   goal_aot_object_file f = {"gcommon", goal_gcommon_statics, goal_gcommon_static_count,
 *                             goal_gcommon_functions, goal_gcommon_function_count,
 *                             goal_gcommon_link};
 *
 * `tag` must match the `--tag` goalc-cbackend was run with, because the emitted code looks its own
 * statics and functions up by that name.
 */
typedef struct goal_aot_object_file {
  const char* tag;
  const goal_static_desc* statics;
  int static_count;
  const void* const* functions;
  int function_count;
  void (*link)(void);
} goal_aot_object_file;

/*!
 * Place `file`'s static data and function objects in the real global heap and relocate them. The
 * kernel must already be initialized. Loading the same tag twice fails.
 */
goal_kernel_core_status goal_aot_load(const goal_aot_object_file* file);

/*!
 * Record that `file` is the native translation of the DGO object named `object_name`, so the DGO
 * loader can find it when that object comes off the disc. `object_name` is the name in the DGO's
 * object header, which is the GOAL source's base name without its extension: "hud-classes-pc" for
 * goal_src/jak1/pc/hud-classes-pc.gc.
 *
 * `file` is copied. Registering a name twice replaces the earlier registration.
 */
goal_kernel_core_status goal_aot_register_object(const char* object_name,
                                                 const goal_aot_object_file* file);

/*! The translation unit registered for `object_name`, or NULL. */
const goal_aot_object_file* goal_aot_registered_object(const char* object_name);

/*! Non-zero if goal_aot_load has already placed `tag` in the heap. */
int goal_aot_is_loaded(const char* tag);

/*!
 * GOAL address of the function object for `index` in `tag`, or 0 if unknown.
 */
uint32_t goal_aot_function_object(const char* tag, int index);

/*!
 * GOAL address of the file's top-level function object. goalc appends the top-level function to a
 * file's function list last (FileEnv::add_top_level_function), so this is the last entry.
 */
uint32_t goal_aot_top_level_object(const char* tag);

/*!
 * Call a GOAL function object through the runtime's own call path (`call_goal`). `func` is a GOAL
 * pointer to a function object.
 */
uint64_t goal_aot_call(uint32_t func, uint64_t a0, uint64_t a1, uint64_t a2);

/*!
 * Native address of the top of GOAL's own stack, at the top of EE main memory, 16-byte aligned for
 * ARM64. This is where upstream's KernelCheckAndDispatch enters GOAL from.
 */
uint64_t goal_kernel_stack_top(void);

/*!
 * Run a loaded file's `top-level`, the way a DGO load's EXECUTE step runs an object file's.
 *
 * It runs on GOAL's own stack, because `(new 'stack ...)` gives a C local a GOAL address and
 * GOAL stores some of those - a process spawn's catch-frame, for one - in 32-bit fields, where a
 * native stack address would be truncated to nonsense. See docs/aot-stack-model.md.
 *
 * `*enable-method-set*` is raised around it, exactly as InitHeapAndSymbol raises it around the
 * kernel DGO and InitMachineScheme around the engine DGO. A top-level's `defmethod` on a type
 * whose subtypes already exist only reaches those subtypes while it is raised.
 */
goal_kernel_core_status goal_aot_run_top_level(const char* tag, uint64_t* out_result);

/*!
 * Look the symbol up in the real symbol table and call whatever function object it holds, through
 * `call_goal`. Returns GOAL_KERNEL_CORE_NOT_FOUND if the symbol does not exist or is empty.
 */
goal_kernel_core_status goal_aot_call_symbol(const char* name,
                                             uint64_t a0,
                                             uint64_t a1,
                                             uint64_t a2,
                                             uint64_t* out_result);

/*!
 * Forget every loaded file. Called by goal_kernel_core_shutdown; the heap memory goes away with
 * the kernel.
 */
void goal_aot_reset(void);

#ifdef __cplusplus
}  // extern "C"
#endif
