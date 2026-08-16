#pragma once

/*!
 * @file sound_rpc_jak2.h
 * Jak 2's direct sound-RPC seam: checked sound/music-bank lifecycle, player controls, language
 * selection, the IRX-version handshake, ordinary/chunked STR files, and the channel-5 stream state
 * consumed by GOAL's GUI loader.
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
  uint32_t play_requests;
  uint32_t sounds_started;
  uint32_t sound_updates;
  uint32_t sounds_missing;
  uint32_t bank_requests;
  uint32_t banks_loaded;
  uint32_t bank_reuses;
  uint32_t bank_failures;
  uint32_t music_requests;
  uint32_t music_loaded;
  uint32_t music_failures;
  uint32_t music_starts;
  uint32_t music_unloads;
  uint32_t str_requests;
  uint32_t str_reads;
  uint32_t str_failures;
  uint32_t str_bytes;
  uint32_t stream_batches;
  uint32_t stream_commands;
  uint32_t stream_queue_requests;
  uint32_t stream_play_requests;
  uint32_t stream_stop_requests;
  uint32_t stream_failures;
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

enum { GOAL_JAK2_SOUND_STREAM_SLOT_COUNT = 4 };

typedef struct goal_jak2_sound_stream_slot_state {
  char name[48];
  int32_t id;
  int32_t published_position;
  uint32_t published_status;
  int32_t primary_voice;
  int32_t secondary_voice;
  uint32_t ring_base;
  uint32_t sampled_nax;
  uint32_t last_nax;
  uint32_t total_bytes;
  uint32_t bytes_read;
  uint32_t sample_rate;
  uint32_t chunks_loaded;
  uint64_t played_bytes;
  uint64_t clock_samples;
  uint64_t invalid_nax_samples;
  uint64_t ring_wraps;
  uint64_t position_advances;
  uint64_t position_stalls;
  uint64_t half_transitions;
  uint8_t active;
  uint8_t playing;
  uint8_t paused;
  uint8_t finished;
  uint8_t current_half;
} goal_jak2_sound_stream_slot_state;

typedef struct goal_jak2_sound_stream_state {
  uint64_t pull_calls;
  uint64_t pulled_frames;
  goal_jak2_sound_stream_slot_state slots[GOAL_JAK2_SOUND_STREAM_SLOT_COUNT];
} goal_jak2_sound_stream_state;

/*!
 * Replace Jak 2's rpc-call/rpc-busy? machine stubs with synchronous player, loader, STR and PLAY
 * responders. The kernel and machine-stub symbol table must already be initialized. This owns one
 * portable, output-backend-free 989snd instance until shutdown. Ordinary named SFX and sequenced
 * music use 989snd; all upstream player controls are applied without an IOP. Channel 5 retains the
 * four-name play/stop/queue state and reports it through the GOAL sound-info block. No-reply RPC
 * returns report synchronous transport completion, while the stats report semantic failures.
 */
goal_kernel_core_status goal_jak2_sound_rpc_install(void);

/*! Stop 989snd and free every loaded bank. Safe to call when not installed. */
void goal_jak2_sound_rpc_shutdown(void);

int goal_jak2_sound_rpc_is_installed(void);

/*! Advance the IOP-facing frame counter and copy the 0x250-byte Jak 2 sound-info block to the EE. */
void goal_jak2_sound_frame(void);

void goal_jak2_sound_rpc_stats_get(goal_jak2_sound_rpc_stats* out);

void goal_jak2_sound_player_state_get(goal_jak2_sound_player_state* out);

/*! Snapshot the raw-voice clock and GOAL-published state for each active channel-5 stream. */
void goal_jak2_sound_stream_state_get(goal_jak2_sound_stream_state* out);

/*! Record one completed host mixer pull for the stream-clock diagnostic snapshot. */
void goal_jak2_sound_audio_pull_record(int32_t pulled_frames);

/*!
 * Route one call or busy query through the installed Jak 2 sound responder. These are the
 * composition boundary used by the DGO channel router; callers should keep channels 0, 1, 4 and 5
 * under this responder rather than duplicating their protocol.
 */
uint64_t goal_jak2_sound_rpc_call(const uint64_t* args);

uint64_t goal_jak2_sound_rpc_busy(int32_t channel);

#ifdef __cplusplus
}  // extern "C"
#endif
