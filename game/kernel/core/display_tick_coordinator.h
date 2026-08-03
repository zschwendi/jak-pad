#pragma once

/*!
 * @file display_tick_coordinator.h
 * A portable, synchronous gate between a host display callback and one game frame.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*goal_display_tick_frame_callback)(double target_presentation_time, void* context);

typedef struct goal_display_tick_stats {
  uint64_t display_ticks;
  uint64_t accepted_ticks;
  uint64_t paused_ticks;
} goal_display_tick_stats;

typedef struct goal_display_tick_coordinator {
  goal_display_tick_frame_callback frame_callback;
  void* context;
  goal_display_tick_stats stats;
  int foreground;
} goal_display_tick_coordinator;

/*! Initialize a stopped coordinator. The callback is borrowed and invoked synchronously. */
void goal_display_tick_coordinator_init(goal_display_tick_coordinator* coordinator,
                                        goal_display_tick_frame_callback frame_callback,
                                        void* context);

/*! Set whether subsequent display ticks may run frames. This never queues or replays ticks. */
void goal_display_tick_coordinator_set_foreground(goal_display_tick_coordinator* coordinator,
                                                  int foreground);

/*!
 * Offer one host display tick. A foreground tick invokes the supplied frame callback exactly once
 * and returns 1. A background tick is dropped and returns 0. The timestamp is passed through
 * unchanged; this coordinator never derives a frame count from it.
 */
int goal_display_tick_coordinator_tick(goal_display_tick_coordinator* coordinator,
                                       double target_presentation_time);

void goal_display_tick_coordinator_get_stats(const goal_display_tick_coordinator* coordinator,
                                             goal_display_tick_stats* out);

#ifdef __cplusplus
}  // extern "C"
#endif
