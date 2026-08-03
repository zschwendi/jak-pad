#pragma once

/*!
 * @file sound_rpc_jak2.h
 * The first Jak 2 sound-RPC seams: command-aware startup state, checked sound-bank loading,
 * language selection, the IRX-version handshake, and ordinary STR files.
 */

#include <stdint.h>

#include "game/kernel/core/kernel_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct goal_jak2_sound_rpc_stats {
  uint32_t version_requests;
  uint32_t info_ee;
  uint32_t language_requests;
  uint32_t language_failures;
  uint32_t language_id;
  uint32_t player_batches;
  uint32_t player_commands;
  uint32_t player_failures;
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

typedef struct goal_jak2_sound_player_state {
  int32_t master_volumes[32];
  int32_t midi_registers[17];
  uint32_t midi_register_mask;
  uint32_t reverb_seen;
  uint32_t reverb_core;
  int32_t reverb_type;
  uint32_t reverb_left;
  uint32_t reverb_right;
  uint32_t fps;
  uint32_t ear_transform_seen;
  int32_t ear_trans1[3];
  int32_t ear_trans0[3];
  int32_t camera_trans[3];
  int32_t camera_angle;
} goal_jak2_sound_player_state;

/*!
 * Replace Jak 2's rpc-call/rpc-busy? machine stubs with the synchronous startup-state, loader and
 * STR responders. The kernel and machine-stub symbol table must already be initialized. This owns
 * one portable, output-backend-free 989snd instance until shutdown. Player commands 12, 22, 23,
 * 24 and 28 configure state only; MIDI handling is limited to startup registers 3, 4, 14 and 16.
 * Playback, music and streaming remain unsupported. No-reply RPC returns report synchronous
 * transport completion, while the stats report semantic failures.
 */
goal_kernel_core_status goal_jak2_sound_rpc_install(void);

/*! Stop 989snd and free every loaded bank. Safe to call when not installed. */
void goal_jak2_sound_rpc_shutdown(void);

int goal_jak2_sound_rpc_is_installed(void);

void goal_jak2_sound_rpc_stats_get(goal_jak2_sound_rpc_stats* out);

void goal_jak2_sound_player_state_get(goal_jak2_sound_player_state* out);

#ifdef __cplusplus
}  // extern "C"
#endif
