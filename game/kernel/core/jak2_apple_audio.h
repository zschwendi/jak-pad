#pragma once

/*!
 * @file jak2_apple_audio.h
 * A small iOS audio-output owner for the game-neutral `goal_game_sound_*` seam.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum goal_jak2_apple_audio_state {
  GOAL_JAK2_APPLE_AUDIO_CLOSED = 0,
  GOAL_JAK2_APPLE_AUDIO_RUNNING = 1,
  GOAL_JAK2_APPLE_AUDIO_SUSPENDED = 2,
} goal_jak2_apple_audio_state;

typedef struct goal_jak2_apple_audio_stats {
  uint64_t start_attempts;
  uint64_t starts;
  uint64_t suspends;
  uint64_t resumes;
  uint64_t render_callbacks;
  uint64_t frames_requested;
  uint64_t frames_rendered;
  uint64_t silent_frames;
  uint64_t underruns;
  uint64_t invalid_buffers;
  int64_t last_error_code;
  int32_t sample_rate;
  int32_t state;
} goal_jak2_apple_audio_stats;

/*!
 * Activate the process audio session and start a RemoteIO output unit. Repeated calls while
 * running succeed without opening another unit; a suspended unit is resumed. The host retains
 * ownership of the AVAudioSession category and route policy.
 */
int goal_jak2_apple_audio_start(void);

/*! Stop callbacks and deactivate the audio session while retaining the initialized output unit. */
int goal_jak2_apple_audio_suspend(void);

/*! Reactivate and restart a suspended unit. A restart failure closes the unit. */
int goal_jak2_apple_audio_resume(void);

/*!
 * Stop callbacks, dispose the output unit, and deactivate the audio session. This is idempotent.
 * Call it before tearing down the game runtime that owns `goal_game_sound_pull_audio`.
 */
int goal_jak2_apple_audio_close(void);

/*! Copy lifecycle and render-callback telemetry. Safe to call while audio is running. */
int goal_jak2_apple_audio_get_stats(goal_jak2_apple_audio_stats* out);

/*! A stable, process-lifetime description of the last lifecycle failure, or an empty string. */
const char* goal_jak2_apple_audio_last_error(void);

#ifdef __cplusplus
}  // extern "C"
#endif
