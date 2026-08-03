/*!
 * @file display_tick_coordinator.cpp
 * The display host owns pacing. This file only gates one callback to one frame.
 */

#include "game/kernel/core/display_tick_coordinator.h"

extern "C" {

void goal_display_tick_coordinator_init(goal_display_tick_coordinator* coordinator,
                                        goal_display_tick_frame_callback frame_callback,
                                        void* context) {
  if (!coordinator) {
    return;
  }
  *coordinator = {};
  coordinator->frame_callback = frame_callback;
  coordinator->context = context;
}

void goal_display_tick_coordinator_set_foreground(goal_display_tick_coordinator* coordinator,
                                                  int foreground) {
  if (coordinator) {
    coordinator->foreground = foreground != 0;
  }
}

int goal_display_tick_coordinator_tick(goal_display_tick_coordinator* coordinator,
                                       double target_presentation_time) {
  if (!coordinator) {
    return 0;
  }
  coordinator->stats.display_ticks++;
  if (!coordinator->foreground || !coordinator->frame_callback) {
    coordinator->stats.paused_ticks++;
    return 0;
  }

  coordinator->stats.accepted_ticks++;
  coordinator->frame_callback(target_presentation_time, coordinator->context);
  return 1;
}

void goal_display_tick_coordinator_get_stats(const goal_display_tick_coordinator* coordinator,
                                             goal_display_tick_stats* out) {
  if (coordinator && out) {
    *out = coordinator->stats;
  }
}

}  // extern "C"
