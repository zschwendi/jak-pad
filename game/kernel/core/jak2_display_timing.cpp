/*!
 * @file jak2_display_timing.cpp
 * Kept separate from the runtime so cadence behavior has a data-free deterministic proof.
 */

#include "game/kernel/core/jak2_display_timing.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kIntegralTolerance = 1e-9;

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

  const double elapsed_frames =
      (target_presentation_time - timing->previous_target_presentation_time) *
      timing->target_frame_rate;
  timing->previous_target_presentation_time = target_presentation_time;
  if (!std::isfinite(elapsed_frames) || elapsed_frames <= 0.0) {
    goal_jak2_display_timing_reset_presentation(timing);
    return 1.0;
  }

  const double quantization = timing->target_frame_rate > 60 ? 0.5 : 1.0;
  const double quantized = std::round(elapsed_frames / quantization) * quantization;
  const double maximum = timing->target_frame_rate == 120 ? 5.0 : 4.0;
  return std::clamp(quantized, 1.0, maximum);
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
  return {dispatcher_frames, sound_frames};
}

}  // extern "C"
