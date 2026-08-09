/*!
 * @file jak2_apple_input.mm
 * Apple GameController polling adapter for the Jak II development app.
 */

#include "game/kernel/core/jak2_apple_input.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "game/kernel/core/pad.h"
#import <GameController/GameController.h>

namespace {

enum PressureIndex {
  kPressureRight = 0,
  kPressureLeft = 1,
  kPressureUp = 2,
  kPressureDown = 3,
  kPressureTriangle = 4,
  kPressureCircle = 5,
  kPressureX = 6,
  kPressureSquare = 7,
  kPressureL1 = 8,
  kPressureR1 = 9,
  kPressureL2 = 10,
  kPressureR2 = 11,
};

struct AdapterState {
  std::mutex mutex;
  goal_jak2_apple_input_metrics metrics = {};
};

AdapterState g_adapter;

goal_pad_state disconnected_pad() {
  goal_pad_state pad;
  goal_pad_state_neutral(&pad);
  pad.connected = 0;
  return pad;
}

uint8_t axis_byte(float value) {
  const float clamped = std::clamp(value, -1.0f, 1.0f);
  const float scaled = clamped < 0.0f ? 127.0f + clamped * 127.0f : 127.0f + clamped * 128.0f;
  return static_cast<uint8_t>(std::lround(scaled));
}

uint8_t pressure_byte(float value) {
  return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

void add_controller_button(goal_pad_state* pad,
                           GCControllerButtonInput* input,
                           uint32_t button,
                           PressureIndex pressure) {
  if (!input) {
    return;
  }
  pad->pressure[pressure] = std::max(pad->pressure[pressure], pressure_byte(input.value));
  if (input.isPressed) {
    pad->buttons |= button;
  }
}

void add_keyboard_button(goal_pad_state* pad,
                         GCKeyboardInput* input,
                         GCKeyCode key,
                         uint32_t button,
                         PressureIndex pressure) API_AVAILABLE(macos(11.0), ios(14.0)) {
  GCDeviceButtonInput* key_input = [input buttonForKeyCode:key];
  if (key_input.isPressed) {
    pad->buttons |= button;
    pad->pressure[pressure] = 255;
  }
}

bool key_down(GCKeyboardInput* input, GCKeyCode key) API_AVAILABLE(macos(11.0), ios(14.0)) {
  return [input buttonForKeyCode:key].isPressed;
}

uint8_t keyboard_axis(bool negative, bool positive, uint8_t fallback) {
  if (negative) {
    return 0;
  }
  return positive ? 255 : fallback;
}

GCExtendedGamepad* first_extended_gamepad() {
  for (GCController* controller in GCController.controllers) {
    GCExtendedGamepad* gamepad = controller.extendedGamepad;
    if (gamepad) {
      return gamepad;
    }
  }
  return nil;
}

void sample_controller(GCExtendedGamepad* gamepad, goal_pad_state* pad) {
  add_controller_button(pad, gamepad.dpad.right, GOAL_PAD_RIGHT, kPressureRight);
  add_controller_button(pad, gamepad.dpad.left, GOAL_PAD_LEFT, kPressureLeft);
  add_controller_button(pad, gamepad.dpad.up, GOAL_PAD_UP, kPressureUp);
  add_controller_button(pad, gamepad.dpad.down, GOAL_PAD_DOWN, kPressureDown);
  add_controller_button(pad, gamepad.buttonY, GOAL_PAD_TRIANGLE, kPressureTriangle);
  add_controller_button(pad, gamepad.buttonB, GOAL_PAD_CIRCLE, kPressureCircle);
  add_controller_button(pad, gamepad.buttonA, GOAL_PAD_X, kPressureX);
  add_controller_button(pad, gamepad.buttonX, GOAL_PAD_SQUARE, kPressureSquare);
  add_controller_button(pad, gamepad.leftShoulder, GOAL_PAD_L1, kPressureL1);
  add_controller_button(pad, gamepad.rightShoulder, GOAL_PAD_R1, kPressureR1);
  add_controller_button(pad, gamepad.leftTrigger, GOAL_PAD_L2, kPressureL2);
  add_controller_button(pad, gamepad.rightTrigger, GOAL_PAD_R2, kPressureR2);

  if (@available(macOS 10.15, iOS 13.0, *)) {
    if (gamepad.buttonMenu.isPressed) {
      pad->buttons |= GOAL_PAD_START;
    }
    if (gamepad.buttonOptions.isPressed) {
      pad->buttons |= GOAL_PAD_SELECT;
    }
  }
  if (@available(macOS 10.14.1, iOS 12.1, *)) {
    if (gamepad.leftThumbstickButton.isPressed) {
      pad->buttons |= GOAL_PAD_L3;
    }
    if (gamepad.rightThumbstickButton.isPressed) {
      pad->buttons |= GOAL_PAD_R3;
    }
  }

  pad->left_x = axis_byte(gamepad.leftThumbstick.xAxis.value);
  pad->left_y = axis_byte(-gamepad.leftThumbstick.yAxis.value);
  pad->right_x = axis_byte(gamepad.rightThumbstick.xAxis.value);
  pad->right_y = axis_byte(-gamepad.rightThumbstick.yAxis.value);
}

void sample_keyboard(GCKeyboardInput* keyboard, goal_pad_state* pad)
    API_AVAILABLE(macos(11.0), ios(14.0)) {
  add_keyboard_button(pad, keyboard, GCKeyCodeUpArrow, GOAL_PAD_UP, kPressureUp);
  add_keyboard_button(pad, keyboard, GCKeyCodeRightArrow, GOAL_PAD_RIGHT, kPressureRight);
  add_keyboard_button(pad, keyboard, GCKeyCodeDownArrow, GOAL_PAD_DOWN, kPressureDown);
  add_keyboard_button(pad, keyboard, GCKeyCodeLeftArrow, GOAL_PAD_LEFT, kPressureLeft);
  add_keyboard_button(pad, keyboard, GCKeyCodeKeyR, GOAL_PAD_TRIANGLE, kPressureTriangle);
  add_keyboard_button(pad, keyboard, GCKeyCodeKeyE, GOAL_PAD_CIRCLE, kPressureCircle);
  add_keyboard_button(pad, keyboard, GCKeyCodeSpacebar, GOAL_PAD_X, kPressureX);
  add_keyboard_button(pad, keyboard, GCKeyCodeKeyF, GOAL_PAD_SQUARE, kPressureSquare);
  add_keyboard_button(pad, keyboard, GCKeyCodeKeyQ, GOAL_PAD_L1, kPressureL1);
  add_keyboard_button(pad, keyboard, GCKeyCodeKeyO, GOAL_PAD_R1, kPressureR1);
  add_keyboard_button(pad, keyboard, GCKeyCodeOne, GOAL_PAD_L2, kPressureL2);
  add_keyboard_button(pad, keyboard, GCKeyCodeKeyP, GOAL_PAD_R2, kPressureR2);

  if (key_down(keyboard, GCKeyCodeReturnOrEnter)) {
    pad->buttons |= GOAL_PAD_START;
  }
  if (key_down(keyboard, GCKeyCodeQuote)) {
    pad->buttons |= GOAL_PAD_SELECT;
  }
  if (key_down(keyboard, GCKeyCodeComma)) {
    pad->buttons |= GOAL_PAD_L3;
  }
  if (key_down(keyboard, GCKeyCodePeriod)) {
    pad->buttons |= GOAL_PAD_R3;
  }

  pad->left_x = keyboard_axis(key_down(keyboard, GCKeyCodeKeyA), key_down(keyboard, GCKeyCodeKeyD),
                              pad->left_x);
  pad->left_y = keyboard_axis(key_down(keyboard, GCKeyCodeKeyW), key_down(keyboard, GCKeyCodeKeyS),
                              pad->left_y);
  pad->right_x = keyboard_axis(key_down(keyboard, GCKeyCodeKeyL), key_down(keyboard, GCKeyCodeKeyJ),
                               pad->right_x);
  pad->right_y = keyboard_axis(key_down(keyboard, GCKeyCodeKeyI), key_down(keyboard, GCKeyCodeKeyK),
                               pad->right_y);
}

void record_push(goal_jak2_apple_input_metrics* metrics,
                 const goal_pad_state& pad,
                 goal_kernel_core_status status) {
  metrics->connected = pad.connected;
  metrics->last_push_status = status;
  metrics->last_buttons = pad.buttons;
  metrics->last_left_x = pad.left_x;
  metrics->last_left_y = pad.left_y;
  metrics->last_right_x = pad.right_x;
  metrics->last_right_y = pad.right_y;
  if (status != GOAL_KERNEL_CORE_OK) {
    metrics->push_failures++;
  }
}

}  // namespace

void goal_jak2_apple_input_start(void) {
  @autoreleasepool {
    std::lock_guard<std::mutex> lock(g_adapter.mutex);
    if (g_adapter.metrics.started) {
      return;
    }

    g_adapter.metrics = {};
    g_adapter.metrics.started = 1;
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];

    const goal_pad_state pad = disconnected_pad();
    record_push(&g_adapter.metrics, pad, goal_pad_set_state(0, &pad));
  }
}

goal_kernel_core_status goal_jak2_apple_input_sample_and_push(void) {
  @autoreleasepool {
    std::lock_guard<std::mutex> lock(g_adapter.mutex);
    if (!g_adapter.metrics.started) {
      return GOAL_KERNEL_CORE_NOT_INITIALIZED;
    }

    goal_pad_state pad;
    goal_pad_state_neutral(&pad);
    uint32_t sources = GOAL_JAK2_APPLE_INPUT_SOURCE_NONE;

    GCExtendedGamepad* gamepad = first_extended_gamepad();
    if (gamepad) {
      sources |= GOAL_JAK2_APPLE_INPUT_SOURCE_CONTROLLER;
      sample_controller(gamepad, &pad);
    }
    if (@available(macOS 11.0, iOS 14.0, *)) {
      GCKeyboardInput* keyboard = GCKeyboard.coalescedKeyboard.keyboardInput;
      if (keyboard) {
        sources |= GOAL_JAK2_APPLE_INPUT_SOURCE_KEYBOARD;
        sample_keyboard(keyboard, &pad);
      }
    }

    pad.connected = sources != GOAL_JAK2_APPLE_INPUT_SOURCE_NONE;
    const goal_kernel_core_status status = goal_pad_set_state(0, &pad);
    g_adapter.metrics.samples++;
    g_adapter.metrics.active_sources = sources;
    record_push(&g_adapter.metrics, pad, status);
    return status;
  }
}

void goal_jak2_apple_input_stop(void) {
  @autoreleasepool {
    std::lock_guard<std::mutex> lock(g_adapter.mutex);
    [GCController stopWirelessControllerDiscovery];

    const goal_pad_state pad = disconnected_pad();
    record_push(&g_adapter.metrics, pad, goal_pad_set_state(0, &pad));
    g_adapter.metrics.started = 0;
    g_adapter.metrics.active_sources = GOAL_JAK2_APPLE_INPUT_SOURCE_NONE;
  }
}

goal_kernel_core_status goal_jak2_apple_input_get_metrics(goal_jak2_apple_input_metrics* out) {
  if (!out) {
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(g_adapter.mutex);
  *out = g_adapter.metrics;
  return GOAL_KERNEL_CORE_OK;
}
