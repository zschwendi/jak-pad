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
 * Describe the last failure. Never NULL; returns "" when there has been no failure. The returned
 * pointer is owned by the kernel and stays valid until the next failing call.
 */
const char* goal_kernel_core_last_error(void);

#ifdef __cplusplus
}  // extern "C"
#endif
