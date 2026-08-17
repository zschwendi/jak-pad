#pragma once

/*!
 * @file jak2_display_timing.h
 * Deterministic host-callback accounting for Jak II's frame and 60 Hz IOP clocks.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct goal_jak2_display_timing_step {
  uint32_t dispatcher_frames;
  uint32_t sound_frames;
  /*! Bit N publishes one IOP frame immediately before dispatcher frame N. */
  uint32_t sound_before_dispatch_mask;
} goal_jak2_display_timing_step;

typedef struct goal_jak2_display_timing {
  double previous_target_presentation_time;
  double dispatcher_debt;
  double sound_debt;
  int32_t target_frame_rate;
  int32_t has_previous_target_presentation_time;
} goal_jak2_display_timing;

/*! The runtime has a 60 Hz baseline and an experimental 120 Hz logical timing domain. */
int32_t goal_jak2_display_timing_normalize_target_frame_rate(int32_t target_frame_rate);

/*! Start a fresh timing domain. Invalid rates select the established 60 Hz baseline. */
void goal_jak2_display_timing_init(goal_jak2_display_timing* timing, int32_t target_frame_rate);

/*!
 * Discard timestamp history and fractional display debt after a lifecycle interruption without
 * losing fractional sound debt. The next accepted callback is one logical frame, matching the
 * established 60 Hz behavior.
 */
void goal_jak2_display_timing_reset_presentation(goal_jak2_display_timing* timing);

/*! Replace the domain and reset both dispatcher and sound debt. */
void goal_jak2_display_timing_set_target_frame_rate(goal_jak2_display_timing* timing,
                                                     int32_t target_frame_rate);

/*!
 * Convert one host target-presentation timestamp into bounded logical dispatcher and IOP frames.
 * A dispatcher frame is never replayed after a non-monotonic timestamp. At a lower delivered
 * cadence, accumulated whole target frames are reported together. The caller publishes the
 * mask-selected IOP frames before their matching dispatcher frames and any remaining IOP frames
 * after the batch.
 */
goal_jak2_display_timing_step goal_jak2_display_timing_advance(
    goal_jak2_display_timing* timing,
    double target_presentation_time);

#ifdef __cplusplus
}  // extern "C"
#endif
