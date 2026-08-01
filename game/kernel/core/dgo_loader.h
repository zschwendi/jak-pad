#pragma once

/*!
 * @file dgo_loader.h
 * Load one of the game's DGO archives out of the player's own data directory.
 *
 * A DGO holds two very different kinds of object file, and this platform treats them differently.
 * See dgo_loader.cpp for the rule and why it is the only one that can work here.
 */

#include <stdint.h>

#include "game/kernel/core/kernel_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*! What a DGO load actually did, so a caller can report it instead of assuming it. */
typedef struct goal_dgo_load_stats {
  int objects;       /*! objects in the archive */
  int code_objects;  /*! OpenGOAL v3 objects, whose code came from the AOT path */
  int data_objects;  /*! v2/v4 objects, linked out of the archive by the real linker */
  int reused_code;   /*! code objects whose translation unit an earlier DGO had already loaded */
  uint32_t heap_used_before;
  uint32_t heap_used_after;
} goal_dgo_load_stats;

/*!
 * Load and link `name` ("KERNEL", "GAME", "VI1", or a name with an explicit extension) from
 * `<data directory>/iso/`. `link_flags` are the kernel's LINK_FLAG_* bits; LINK_FLAG_EXECUTE runs
 * each object's top-level as it is linked, which is what a boot does.
 *
 * `buffer_size` is the size of each of the two load buffers taken from the top of the global heap,
 * and must be at least as large as the archive's largest object. Upstream uses 0x400000.
 */
goal_kernel_core_status goal_dgo_load(const char* name,
                                      uint32_t link_flags,
                                      int32_t buffer_size,
                                      goal_dgo_load_stats* out);

/*! Print every object as it is read, with where it landed and how it was classified. */
void goal_dgo_set_verbose(int on);

/*! Describe the last DGO failure. Never NULL. */
const char* goal_dgo_last_error(void);

#ifdef __cplusplus
}  // extern "C"
#endif
