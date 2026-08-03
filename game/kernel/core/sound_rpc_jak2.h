#pragma once

/*!
 * @file sound_rpc_jak2.h
 * The first Jak 2 sound-RPC seams: command-aware loader framing, the IRX-version handshake, and
 * ordinary STR files. Sound-bank loading remains unimplemented.
 */

#include <stdint.h>

#include "game/kernel/core/kernel_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct goal_jak2_sound_rpc_stats {
  uint32_t version_requests;
  uint32_t info_ee;
  uint32_t bank_load_requests;
  uint32_t bank_load_unimplemented;
  uint32_t str_requests;
  uint32_t str_reads;
  uint32_t str_failures;
  uint32_t str_bytes;
  uint32_t rejected_calls;
} goal_jak2_sound_rpc_stats;

/*!
 * Replace Jak 2's rpc-call/rpc-busy? machine stubs with the synchronous version and STR responders.
 * The kernel and machine-stub symbol table must already be initialized.
 */
goal_kernel_core_status goal_jak2_sound_rpc_install(void);

void goal_jak2_sound_rpc_stats_get(goal_jak2_sound_rpc_stats* out);

#ifdef __cplusplus
}  // extern "C"
#endif
