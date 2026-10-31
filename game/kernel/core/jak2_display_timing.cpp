/*!
 * @file jak2_display_timing.cpp
 * Kept separate from the runtime so cadence behavior has a data-free deterministic proof.
 */

#include "game/kernel/core/jak2_display_timing.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kIntegralTolerance = 1e-9;

// Each callback's elapsed time is rounded to half a logical frame and the rounding error is
// carried into the next callback, so cadences that are not frame multiples still sum correctly.
constexpr double kQuantization = 0.5;

double frame_ratio(goal_jak2_display_timing* timing, double target_presentation_time) {
  if (!std::isfinite(target_presentation_time)) {
    goal_jak2_display_timing_reset_presentation(timing);
    return 1.0;
  }
  if (!timing->has_previous_target_presentation_time) {
    timing->previous_target_presentation_time = target_presentation_time;
    timing->has_previous_target_presentation_time = 1;
    return 1.0;
  }

  const double delivered_frames =
      (target_presentation_time - timing->previous_target_presentation_time) *
      timing->target_frame_rate;
  if (!std::isfinite(delivered_frames) || delivered_frames < 0.0) {
    goal_jak2_display_timing_reset_presentation(timing);
    return 1.0;
  }
  timing->previous_target_presentation_time = target_presentation_time;

  // Quantizing absorbs floating-point noise; the remainder keeps the rounding from adding or
  // losing time across callbacks.
  const double elapsed_frames = delivered_frames + timing->presentation_remainder;
  const double quantized =
      std::max(0.0, std::round(elapsed_frames / kQuantization) * kQuantization);
  const double maximum = timing->target_frame_rate == 120 ? 5.0 : 4.0;
  if (quantized > maximum) {
    timing->presentation_remainder = 0.0;
    return maximum;
  }
  timing->presentation_remainder = elapsed_frames - quantized;
  return quantized;
}

}  // namespace

extern "C" {

int32_t goal_jak2_display_timing_normalize_target_frame_rate(int32_t target_frame_rate) {
  return target_frame_rate == 120 ? 120 : 60;
}

void goal_jak2_display_timing_init(goal_jak2_display_timing* timing, int32_t target_frame_rate) {
  if (!timing) {
    return;
  }
  *timing = {};
  timing->target_frame_rate = goal_jak2_display_timing_normalize_target_frame_rate(target_frame_rate);
}

void goal_jak2_display_timing_reset_presentation(goal_jak2_display_timing* timing) {
  if (!timing) {
    return;
  }
  timing->previous_target_presentation_time = 0.0;
  timing->has_previous_target_presentation_time = 0;
  // A lifecycle gap or a non-monotonic host timestamp must not replay a fractional display
  // callback on the next delivery. Sound uses its own clock and intentionally keeps its debt.
  timing->dispatcher_debt = 0.0;
  timing->presentation_remainder = 0.0;
}

void goal_jak2_display_timing_set_target_frame_rate(goal_jak2_display_timing* timing,
                                                     int32_t target_frame_rate) {
  if (!timing) {
    return;
  }
  *timing = {};
  timing->target_frame_rate = goal_jak2_display_timing_normalize_target_frame_rate(target_frame_rate);
}

goal_jak2_display_timing_step goal_jak2_display_timing_advance(
    goal_jak2_display_timing* timing,
    double target_presentation_time) {
  if (!timing) {
    return {};
  }

  const double ratio = frame_ratio(timing, target_presentation_time);
  const double display_debt_before = timing->dispatcher_debt;
  const double sound_debt_before = timing->sound_debt;
  timing->dispatcher_debt += ratio;
  const uint32_t dispatcher_frames = static_cast<uint32_t>(
      std::clamp(static_cast<int>(timing->dispatcher_debt + kIntegralTolerance), 0, 5));
  timing->dispatcher_debt -= dispatcher_frames;
  if (timing->dispatcher_debt < 0.0 && timing->dispatcher_debt > -kIntegralTolerance) {
    timing->dispatcher_debt = 0.0;
  }

  timing->sound_debt += ratio * 60.0 / timing->target_frame_rate;
  const uint32_t sound_frames = static_cast<uint32_t>(
      std::clamp(static_cast<int>(timing->sound_debt + kIntegralTolerance), 0, 4));
  timing->sound_debt -= sound_frames;
  if (timing->sound_debt < 0.0 && timing->sound_debt > -kIntegralTolerance) {
    timing->sound_debt = 0.0;
  }

  uint32_t sound_before_dispatch_mask = 0;
  const double sound_per_target_frame = 60.0 / timing->target_frame_rate;
  uint32_t next_dispatch = 0;
  for (uint32_t sound_frame = 1; ratio > 0.0 && sound_frame <= sound_frames; sound_frame++) {
    const double sound_progress =
        (sound_frame - sound_debt_before) / (ratio * sound_per_target_frame);
    while (next_dispatch < dispatcher_frames) {
      const double dispatcher_progress = (next_dispatch + 1 - display_debt_before) / ratio;
      if (dispatcher_progress + kIntegralTolerance >= sound_progress) {
        break;
      }
      next_dispatch++;
    }
    if (next_dispatch < dispatcher_frames) {
      sound_before_dispatch_mask |= 1u << next_dispatch;
    }
  }
  return {dispatcher_frames, sound_frames, sound_before_dispatch_mask};
}

}  // extern "C"
