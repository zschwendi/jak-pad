#pragma once

/*!
 * @file sound_rpc.h
 * The sound half of the overlord, answered where it is sent, plus the seam a host pulls mixed
 * audio out of.
 *
 * GOAL drives sound through two RPC channels (`engine/sound/gsound.gc`): the loader (channel 1)
 * loads sound banks and music, and the player (channel 0) starts, stops and positions sounds.
 * Upstream those channels reach a pair of IOP threads in `game/overlord/jak1/srpc.cpp`, which call
 * into 989snd. There is no IOP here, so `sound_rpc.cpp` answers both channels inside `rpc-call`,
 * the same way `dgo_loader.cpp` answers the DGO, STR and ramdisk channels.
 *
 * 989snd itself is portable and is used unchanged: it is the sequencer, the SPU voice model and
 * the mixer. What it does not have here is an output device. `goal_sound_pull_audio` is that seam:
 * the host asks for frames when it wants them, and gets the same samples the desktop port's audio
 * callback would have been handed.
 */

#include <stdint.h>

#include "game/kernel/core/kernel_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Start 989snd and answer the sound RPC channels from here on.
 *
 * Call after `goal_kernel_core_stub_machine_layer`, whose `rpc-call` stub this cooperates with -
 * `goal_dgo_install_goal_loader` installs the one `rpc-call`, and it routes the sound channels
 * here. Banks and music are read from `<data directory>/iso/`, so a data directory must be set.
 */
goal_kernel_core_status goal_sound_install(void);

/*! Stop 989snd and free every loaded bank. Safe to call when not installed. */
void goal_sound_shutdown(void);

int goal_sound_is_installed(void);

/*!
 * One EE frame of the work upstream's `VBlank_Handler` does: advance the music fade, and write the
 * `sound-iop-info` block back to the EE address `check-irx-version` asked for. Call once per
 * `kernel-dispatcher` frame, before pulling that frame's audio.
 */
void goal_sound_frame(void);

/*! The rate 989snd's mixer runs at. Its sequencer timing is derived from it, so a host that cannot
 *  open a device at this rate has to resample rather than ask for a different rate. */
int goal_sound_sample_rate(void);

/*!
 * The audio-out seam. Render `frames` interleaved stereo 16-bit frames into `out` and return how
 * many were written - `frames` when the sound system is running, 0 when it is not, in which case
 * `out` is left alone.
 *
 * This is the only thing a host has to provide a device for. A CoreAudio or AVAudioEngine render
 * callback can call it directly; the boot test calls it once per game frame and writes a WAV.
 * 989snd serializes this against the RPC commands with its own lock, so the caller may be the
 * device's thread.
 */
int goal_sound_pull_audio(int16_t* out, int frames);

/*! What GOAL asked the sound RPC for, so a run can report it instead of assuming it. */
typedef struct goal_sound_rpc_stats {
  int loader_commands;  /*! commands received on the loader channel */
  int player_commands;  /*! commands received on the player channel */
  int banks_loaded;     /*! .SBK sound banks 989snd accepted */
  int bank_failures;    /*! .SBK loads that found no file or no bank */
  int music_loaded;     /*! .MUS music banks 989snd accepted */
  int music_failures;   /*! .MUS loads that found no file or no bank */
  int sounds_started;   /*! sounds 989snd gave a voice to */
  int sounds_missing;   /*! play requests naming a sound no loaded bank has */
  int music_starts;     /*! times the loaded music bank was (re)started */
  int spool_requests;   /*! `spool-` play requests: streamed VAG, which is not implemented */
  int play_rpc_calls;   /*! calls on channel 5, the streamed-audio channel, also not implemented */
  int unknown_commands; /*! commands this implementation does not handle */
  int music_group_volume; /*! the volume the music group is at: (game volume * fade >> 16) * tweak >> 7,
                             out of 0x400. What upstream's SetMusicVol computes. */
  int frames;           /*! goal_sound_frame calls */
  int audio_frames;     /*! stereo frames handed to the host */
} goal_sound_rpc_stats;

void goal_sound_rpc_stats_get(goal_sound_rpc_stats* out);

/*! Name every distinct command and sound this run could not play. Never NULL; "" when nothing was
 *  missed. The returned pointer is owned by this file and stays valid until the next call. */
const char* goal_sound_unhandled_report(void);

/*! Print every RPC command as it arrives. */
void goal_sound_set_verbose(int on);

/*! Describe the last sound failure. Never NULL; "" when there has been none. */
const char* goal_sound_last_error(void);

/*!
 * Route one `rpc-call` on a sound channel. `dgo_loader.cpp` calls this for channels 0, 1 and 5;
 * nothing else should. Returns 0 like every other RPC, and reports through the machine-layer stub
 * path when the sound system is not installed.
 */
uint64_t goal_sound_rpc_call(int32_t channel, const uint64_t* args);

#ifdef __cplusplus
}  // extern "C"
#endif
