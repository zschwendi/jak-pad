#pragma once

/*!
 * @file pad.h
 * The controller seam. This library has no windowing system and no input library, so it cannot
 * read a controller: the host application - a test driver, a macOS shell, or the iPad app's
 * GameController bridge - reads one and pushes the result in through `goal_pad_set_state`, once
 * per frame, before the frame runs.
 *
 * `goal_pad_install` then implements the two GOAL machine functions that read a pad, `cpad-open`
 * and `cpad-get-data`, out of that pushed state. What GOAL sees is the PS2 `cpad-info` structure
 * exactly as `engine/ps2/pad.gc` expects it, so `service-cpads`, `cpad-pressed?` and the analog
 * stick work without any GOAL change.
 *
 * Nothing here knows about SDL, GameController, a keyboard, or a window.
 */

#include <stdint.h>

#include "game/kernel/core/kernel_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The buttons, as bits of `goal_pad_state::buttons`. These are the bit numbers of the `pad-buttons`
 * enum in `goal_src/jak1/engine/ps2/pad.gc`, which is the PS2 digital button word: GOAL reads this
 * word straight out of `cpad-info` and tests it with `cpad-pressed?`.
 *
 * A set bit means the button is held. (The real PS2 pad reported the opposite; upstream's pad
 * reader already flipped it, and so does this one.)
 */
typedef enum goal_pad_button {
  GOAL_PAD_SELECT = 1 << 0,
  GOAL_PAD_L3 = 1 << 1,
  GOAL_PAD_R3 = 1 << 2,
  GOAL_PAD_START = 1 << 3,
  GOAL_PAD_UP = 1 << 4,
  GOAL_PAD_RIGHT = 1 << 5,
  GOAL_PAD_DOWN = 1 << 6,
  GOAL_PAD_LEFT = 1 << 7,
  GOAL_PAD_L2 = 1 << 8,
  GOAL_PAD_R2 = 1 << 9,
  GOAL_PAD_L1 = 1 << 10,
  GOAL_PAD_R1 = 1 << 11,
  GOAL_PAD_TRIANGLE = 1 << 12,
  GOAL_PAD_CIRCLE = 1 << 13,
  GOAL_PAD_X = 1 << 14,
  GOAL_PAD_SQUARE = 1 << 15,
} goal_pad_button;

/*! The value of an axis at rest. The game reads a stick as a byte per axis, 0 to 255. */
#define GOAL_PAD_ANALOG_NEUTRAL 127

/*! How many controllers the game reads. `*cpad-list*` in pad.gc holds this many. */
#define GOAL_PAD_PORTS 4

/*! The pressure bytes, in the order the PS2 reported them (`abutton` in `hw-cpad`). */
#define GOAL_PAD_PRESSURE_BYTES 12

/*!
 * One controller, as the host sees it. A host that has no controller pushes nothing, or pushes a
 * state with `connected` zero; either way GOAL is told the port is empty rather than told that a
 * pad with no buttons pressed is attached.
 */
typedef struct goal_pad_state {
  int32_t connected;
  /*! held buttons, as `goal_pad_button` bits */
  uint32_t buttons;
  uint8_t left_x;
  uint8_t left_y;
  uint8_t right_x;
  uint8_t right_y;
  uint8_t pressure[GOAL_PAD_PRESSURE_BYTES];
} goal_pad_state;

/*!
 * Fill `out` with a connected controller holding nothing, sticks centered. The starting point for
 * a host that is about to set the buttons it read.
 */
void goal_pad_state_neutral(goal_pad_state* out);

/*!
 * Implement `cpad-open` and `cpad-get-data` out of the pushed state, replacing the machine-layer
 * stubs. Call after `goal_kernel_core_stub_machine_layer` and before the GOAL code that opens the
 * pads runs - `*cpad-list*`'s top-level, in `engine/ps2/pad.gc`, which GAME.CGO carries.
 */
goal_kernel_core_status goal_pad_install(void);

/*!
 * Set what port `port` is holding right now. The next `cpad-get-data` for that port reports it.
 * Ports the host never sets read as disconnected.
 */
goal_kernel_core_status goal_pad_set_state(int port, const goal_pad_state* state);

/*! Make the host's rumble preference authoritative for GOAL's per-pad vibration gate. */
goal_kernel_core_status goal_pad_set_rumble_enabled(int port, int enabled);

/*!
 * What GOAL last asked the vibration motors of `port` to do: `large` is the on/off motor and
 * `small` is the analog one, both 0 when nothing is buzzing. GOAL writes these into the pad's
 * `direct` bytes and the pad reader hands them to the hardware; here it hands them to the host.
 */
goal_kernel_core_status goal_pad_get_rumble(int port, uint8_t* out_large, uint8_t* out_small);

/*!
 * Consume the strongest motor request GOAL issued since the previous consume. A host that pulls
 * once per presented frame uses this instead of sampling a transient `direct` byte that may
 * already have returned to zero later in the same frame.
 */
goal_kernel_core_status goal_pad_take_rumble(int port, uint8_t* out_large, uint8_t* out_small);

/*! How many times GOAL has read `port`. A host can use this to see that the seam is being used. */
int goal_pad_read_count(int port);

#ifdef __cplusplus
}  // extern "C"
#endif
