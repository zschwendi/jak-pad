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

/*!
 * Install the entry points GOAL's own level loader drives a DGO with: the DGO RPC (`rpc-call` /
 * `rpc-busy?`, answered synchronously out of the same reader) and `link-begin` / `link-resume`
 * with this platform's code/data rule applied. See dgo_loader.cpp.
 *
 * Call after `goal_kernel_core_stub_machine_layer`, whose stubs for `rpc-call` and `rpc-busy?`
 * these replace. For Jak 2, install its sound RPC first; this installer becomes the top-level
 * router and delegates sound channels 0, 1 and 4 to it. Calling it clears the counters below.
 */
void goal_dgo_install_goal_loader(void);

/*! What GOAL's own loader asked the RPC for, so a caller can report it instead of assuming it. */
typedef struct goal_dgo_rpc_stats {
  int dgo_archives;          /*! DGO loads GOAL started */
  int dgo_objects;           /*! objects the RPC handed back */
  int dgo_failures;          /*! well-formed DGO RPCs that returned an error */
  int last_dgo_result;       /*! most recent DGO_RPC_RESULT_* value */
  int linked_code_objects;   /*! link-begin calls taken from the AOT path */
  int linked_data_objects;   /*! link-begin calls given to the real linker */
  int str_reads;             /*! files and animation chunks the STR RPC delivered */
  int str_failures;          /*! STR requests that found nothing to read */
  int ramdisk_files;         /*! files the ramdisk RPC loaded (the .VIS of a level) */
  int ramdisk_reads;         /*! windows of one the ramdisk RPC handed back */
  int ramdisk_misses;        /*! ramdisk requests that found nothing to read */
  unsigned level_code_bytes; /*! heap a level's own object files took, in the level's own heap */
  char first_dgo_name[17];   /*! first archive GOAL asked channel 3 to load, uppercased */
  char current_dgo_name[17]; /*! archive still returning objects, or empty when none */
  char last_dgo_name[17];    /*! archive named by the most recent load request */
  char last_dgo_error[256];  /*! diagnostic from the most recent error result */
} goal_dgo_rpc_stats;

void goal_dgo_goal_loader_stats(goal_dgo_rpc_stats* out);

/*! Print every object as it is read, with where it landed and how it was classified. */
void goal_dgo_set_verbose(int on);

/*! Describe the last DGO failure. Never NULL. */
const char* goal_dgo_last_error(void);

#ifdef __cplusplus
}  // extern "C"
#endif
