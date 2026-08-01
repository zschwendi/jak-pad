#pragma once

/*!
 * @file kernel_core.h
 * Narrow C entry point for bringing up the real Jak 1 GOAL kernel state: EE main memory, the
 * global/debug kernel heaps, the GOAL symbol table, and the fundamental GOAL types.
 *
 * This is the boundary an Objective-C++ bridge is expected to call. It deliberately exposes no
 * C++ types and transfers no ownership: every pointer returned by this header is either owned by
 * the kernel (and only valid between initialize and shutdown) or is static storage.
 *
 * This entry point does NOT execute GOAL code and does NOT load game data. See
 * `goal_kernel_core_state::main_memory_executable` and the notes in `desktop_seams.cpp` for the
 * seams that still require the compiler/AOT track.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum goal_kernel_core_status {
  GOAL_KERNEL_CORE_OK = 0,
  GOAL_KERNEL_CORE_ALREADY_INITIALIZED = 1,
  GOAL_KERNEL_CORE_NOT_INITIALIZED = 2,
  GOAL_KERNEL_CORE_MAIN_MEMORY_FAILED = 3,
  GOAL_KERNEL_CORE_SYMBOL_INIT_FAILED = 4,
  GOAL_KERNEL_CORE_INVALID_ARGUMENT = 5,
  GOAL_KERNEL_CORE_NOT_FOUND = 6,
  GOAL_KERNEL_CORE_OUT_OF_MEMORY = 7,
} goal_kernel_core_status;

/*!
 * A read-only snapshot of the initialized kernel. All addresses named `*_offset` are GOAL
 * pointers: byte offsets from `main_memory_address`.
 */
typedef struct goal_kernel_core_state {
  uint64_t main_memory_address;
  uint32_t main_memory_size;
  /*! 1 if EE main memory was mapped with PROT_EXEC. 0 means no native GOAL code can run from the
   *  GOAL heap on this platform without a different execution strategy. */
  int32_t main_memory_executable;

  uint32_t global_heap_base_offset;
  uint32_t global_heap_current_offset;
  uint32_t global_heap_top_offset;
  uint32_t global_heap_top_base_offset;
  uint32_t global_heap_used_bytes;

  uint32_t debug_heap_base_offset;
  uint32_t debug_heap_top_base_offset;

  uint32_t symbol_table_offset;
  uint32_t s7_offset;
  uint32_t last_symbol_offset;
  int32_t symbol_count;

  uint32_t empty_pair_offset;
  uint32_t false_offset;
  uint32_t true_offset;
} goal_kernel_core_state;

/*!
 * Map EE main memory, initialize the kernel heaps, and run the real Jak 1 symbol table and type
 * bootstrap. Safe to call again after `goal_kernel_core_shutdown`.
 */
goal_kernel_core_status goal_kernel_core_initialize(void);

/*!
 * Tear the kernel back down and unmap EE main memory. Safe to call when not initialized.
 */
void goal_kernel_core_shutdown(void);

int goal_kernel_core_is_initialized(void);

/*! Copy the current kernel snapshot into `out`. */
goal_kernel_core_status goal_kernel_core_get_state(goal_kernel_core_state* out);

/*!
 * Allocate `size` bytes from the real global heap. On success writes the GOAL pointer (offset
 * from EE main memory) to `out_offset`. `name` is used for heap logging and may be NULL.
 */
goal_kernel_core_status goal_kernel_core_global_alloc(int32_t size,
                                                      const char* name,
                                                      uint32_t* out_offset);

/*!
 * Intern `name` in the real symbol table, creating it if it does not exist. Writes the GOAL
 * pointer to the symbol to `out_symbol_offset`.
 */
goal_kernel_core_status goal_kernel_core_intern(const char* name, uint32_t* out_symbol_offset);

/*!
 * Look up an existing symbol without creating it. Returns GOAL_KERNEL_CORE_NOT_FOUND if the
 * symbol is not present.
 */
goal_kernel_core_status goal_kernel_core_lookup(const char* name,
                                                uint32_t* out_symbol_offset,
                                                uint32_t* out_value);

/*!
 * Read the name of the GOAL type stored in the symbol `name`, into `buffer`. Used to prove the
 * type bootstrap produced real, linked type objects.
 */
goal_kernel_core_status goal_kernel_core_type_name_of_symbol(const char* name,
                                                             char* buffer,
                                                             size_t buffer_size);

/*!
 * Install a loudly-failing GOAL function object for every symbol the machine layer
 * (`kmachine.cpp`: pads, video, file streams, system config, sound RPC, PC-port functions) would
 * define, and set the `*stack-top*` / `*stack-base*` / `*stack-size*` constants.
 *
 * The machine layer is not part of this library. Without this, GOAL calling one of its functions
 * reads a symbol holding 0 and faults in the guard page with no name attached; with it, the call
 * says which function was wanted.
 *
 * This is a diagnostic, not an implementation. With `abort_when_called` non-zero a call aborts.
 * With it zero the call prints the function's name once and returns 0, which lets a probe find
 * everything that is missing in one run - but nothing that happens after such a message is
 * evidence that the code works.
 */
goal_kernel_core_status goal_kernel_core_stub_machine_layer(int abort_when_called);

/*!
 * Point the runtime at the player's own prepared Jak 1 data. `path` is the directory that holds
 * the `iso/` and `fr3/` directories the OpenGOAL extraction tools produce - what
 * `goal_src/jak1/game.gp` calls `$OUT/iso` and `$OUT/fr3`.
 *
 * No game data ships with this library and none is ever written into it. Until a directory is set,
 * every file open fails with a message saying so. Pass NULL or "" to unset.
 */
goal_kernel_core_status goal_kernel_core_set_data_directory(const char* path);

/*! The configured data directory, or "" if none has been set. Never NULL. */
const char* goal_kernel_core_data_directory(void);

/*!
 * Where the game's save files go. The directory is created on the first save; a missing or empty
 * directory simply means there are no saves yet. Pass NULL or "" to fall back to the upstream
 * desktop location (the OpenGOAL user config directory).
 */
goal_kernel_core_status goal_kernel_core_set_saves_directory(const char* path);

/*! The configured saves directory, or "" if none has been set. Never NULL. */
const char* goal_kernel_core_saves_directory(void);

/*!
 * Resolve a data-relative name such as "iso/KERNEL.CGO" against the data directory. A name that
 * is already absolute is copied through unchanged. Returns GOAL_KERNEL_CORE_NOT_FOUND when no
 * data directory has been set.
 */
goal_kernel_core_status goal_kernel_core_resolve_data_path(const char* name,
                                                           char* out,
                                                           size_t out_size);

/*!
 * How much of GOAL's per-thread backup stacks a run used. Every GOAL thread copies the live part
 * of its execution stack into a small buffer when it suspends, and the size of that buffer comes
 * from numbers in `goal_src` that were measured on the PS2 (see `process-stack-save-size` in
 * `kernel/gkernel-h.gc`). `thread-suspend` aborts when a live stack does not fit; this reports
 * what did fit, so a run can say how close those numbers came to being wrong.
 *
 * The name strings are owned by the kernel and are valid until shutdown.
 */
typedef struct goal_thread_stack_watermark_report {
  int suspends;
  int deepest_used; /*!< the largest live stack any suspend copied */
  int deepest_size;
  const char* deepest_name;
  int fullest_used; /*!< the suspend that came closest to filling its buffer */
  int fullest_size;
  const char* fullest_name;
} goal_thread_stack_watermark_report;

void goal_thread_stack_watermark(goal_thread_stack_watermark_report* out);

/*!
 * Describe the last failure. Never NULL; returns "" when there has been no failure. The returned
 * pointer is owned by the kernel and stays valid until the next failing call.
 */
const char* goal_kernel_core_last_error(void);

#ifdef __cplusplus
}  // extern "C"
#endif
