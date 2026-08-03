#pragma once

/*!
 * @file sound_rpc_jak2.h
 * The first Jak 2 sound-RPC seams: command-aware loader framing, checked sound-bank loading, the
 * IRX-version handshake, and ordinary STR files.
 */

#include <stdint.h>

#include "game/kernel/core/kernel_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct goal_jak2_sound_rpc_stats {
  uint32_t version_requests;
  uint32_t info_ee;
  uint32_t bank_requests;
  uint32_t banks_loaded;
  uint32_t bank_reuses;
  uint32_t bank_failures;
  uint32_t str_requests;
  uint32_t str_reads;
  uint32_t str_failures;
  uint32_t str_bytes;
  uint32_t rejected_calls;
} goal_jak2_sound_rpc_stats;

/*!
 * Replace Jak 2's rpc-call/rpc-busy? machine stubs with the synchronous loader and STR responders.
 * The kernel and machine-stub symbol table must already be initialized. This owns one portable,
 * output-backend-free 989snd instance until shutdown.
 */
goal_kernel_core_status goal_jak2_sound_rpc_install(void);

/*! Stop 989snd and free every loaded bank. Safe to call when not installed. */
void goal_jak2_sound_rpc_shutdown(void);

int goal_jak2_sound_rpc_is_installed(void);

void goal_jak2_sound_rpc_stats_get(goal_jak2_sound_rpc_stats* out);

#ifdef __cplusplus
}  // extern "C"
#endif
