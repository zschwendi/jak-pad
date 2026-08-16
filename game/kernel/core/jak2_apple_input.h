#pragma once

/*!
 * @file jak2_apple_input.h
 * Polling GameController adapter for the Jak II Apple development host.
 *
 * The app starts the adapter with its runtime, calls sample-and-push immediately before every
 * runtime tick, and stops it before shutting the runtime down. The header stays C-only so the
 * Objective-C app shell does not need to expose any GameController types.
 */

#include <stdint.h>

#include "game/kernel/core/kernel_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum goal_jak2_apple_input_source {
  GOAL_JAK2_APPLE_INPUT_SOURCE_NONE = 0,
  GOAL_JAK2_APPLE_INPUT_SOURCE_CONTROLLER = 1 << 0,
  GOAL_JAK2_APPLE_INPUT_SOURCE_KEYBOARD = 1 << 1,
} goal_jak2_apple_input_source;

typedef struct goal_jak2_apple_input_metrics {
  int32_t started;
  int32_t connected;
  uint32_t active_sources;
  uint64_t samples;
  uint64_t push_failures;
  goal_kernel_core_status last_push_status;
  uint32_t last_buttons;
  uint8_t last_left_x;
  uint8_t last_left_y;
  uint8_t last_right_x;
  uint8_t last_right_y;
} goal_jak2_apple_input_metrics;

/*! Start controller discovery. Safe to call again while already started. */
void goal_jak2_apple_input_start(void);

/*!
 * Poll the first connected extended controller and the coalesced keyboard, then copy the merged
 * state to GOAL pad port 0. Returns NOT_INITIALIZED if the adapter has not been started.
 */
goal_kernel_core_status goal_jak2_apple_input_sample_and_push(void);

/*!
 * Stop discovery and immediately push a disconnected, neutral state to port 0. Safe to call when
 * already stopped.
 */
void goal_jak2_apple_input_stop(void);

/*! Copy the current lifecycle, connection, sample, and last-input snapshot into `out`. */
goal_kernel_core_status goal_jak2_apple_input_get_metrics(goal_jak2_apple_input_metrics* out);

#ifdef __cplusplus
}  // extern "C"
#endif
