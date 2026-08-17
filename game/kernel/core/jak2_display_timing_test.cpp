#include <cmath>
#include <cstdio>

#include "game/kernel/core/jak2_display_timing.h"

namespace {

int g_failures = 0;

void expect(bool condition, const char* message) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
  if (!condition) {
    ++g_failures;
  }
}

goal_jak2_display_timing_step advance(goal_jak2_display_timing* timing, double timestamp) {
  return goal_jak2_display_timing_advance(timing, timestamp);
}

}  // namespace

int main() {
  goal_jak2_display_timing timing = {};
  goal_jak2_display_timing_init(&timing, 120);
  expect(timing.target_frame_rate == 120, "120 Hz is an explicit Jak II timing domain");

  const auto first = advance(&timing, 10.0);
  const auto second = advance(&timing, 10.0 + 1.0 / 120.0);
  const auto third = advance(&timing, 10.0 + 2.0 / 120.0);
  expect(first.dispatcher_frames == 1 && second.dispatcher_frames == 1 &&
             third.dispatcher_frames == 1 &&
             first.sound_frames + second.sound_frames + third.sound_frames == 1,
         "120 Hz dispatches every callback while the IOP clock remains 60 Hz");

  goal_jak2_display_timing_set_target_frame_rate(&timing, 120);
  const auto sixty_first = advance(&timing, 20.0);
  const auto sixty_second = advance(&timing, 20.0 + 1.0 / 60.0);
  expect(sixty_first.dispatcher_frames == 1 && sixty_second.dispatcher_frames == 2 &&
             sixty_first.sound_frames + sixty_second.sound_frames == 1,
         "a 60 Hz delivery fallback preserves a 120 Hz logical domain with one 60 Hz sound frame");

  goal_jak2_display_timing_set_target_frame_rate(&timing, 120);
  const auto slow_first = advance(&timing, 30.0);
  const auto slow_second = advance(&timing, 30.0 + 1.0 / 24.0);
  expect(slow_first.dispatcher_frames == 1 && slow_second.dispatcher_frames == 5 &&
             slow_first.sound_frames + slow_second.sound_frames == 3,
         "24 Hz delivery is bounded to five logical frames and preserves fractional sound debt");

  goal_jak2_display_timing_set_target_frame_rate(&timing, 60);
  const auto baseline_first = advance(&timing, 40.0);
  const auto baseline_second = advance(&timing, 40.0 + 1.0 / 60.0);
  expect(baseline_first.dispatcher_frames == 1 && baseline_second.dispatcher_frames == 1 &&
             baseline_first.sound_frames == 1 && baseline_second.sound_frames == 1,
         "60 Hz retains one dispatcher and one sound frame per callback");

  goal_jak2_display_timing_set_target_frame_rate(&timing, 120);
  const auto switch_first = advance(&timing, 50.0);
  const auto switch_second = advance(&timing, 50.0 + 1.0 / 120.0);
  goal_jak2_display_timing_set_target_frame_rate(&timing, 60);
  const auto switch_back = advance(&timing, 51.0);
  expect(switch_first.dispatcher_frames == 1 && switch_second.dispatcher_frames == 1 &&
             switch_back.dispatcher_frames == 1 && switch_back.sound_frames == 1,
         "a live 120-to-60 switch resets both timing debts");

  goal_jak2_display_timing_set_target_frame_rate(&timing, 120);
  advance(&timing, 60.0);
  const auto before_pause = advance(&timing, 60.0 + 1.0 / 120.0);
  goal_jak2_display_timing_reset_presentation(&timing);
  const auto after_pause = advance(&timing, 600.0);
  expect(before_pause.sound_frames == 1 && after_pause.dispatcher_frames == 1 &&
             after_pause.sound_frames == 0,
         "a lifecycle reset drops stale elapsed time without discarding sound debt");

  goal_jak2_display_timing_set_target_frame_rate(&timing, 120);
  advance(&timing, 70.0);
  const auto fractional_delivery = advance(&timing, 70.0 + 1.0 / 90.0);
  const auto non_monotonic_delivery = advance(&timing, 69.0);
  expect(fractional_delivery.dispatcher_frames == 1 &&
             non_monotonic_delivery.dispatcher_frames == 1,
         "a non-monotonic timestamp drops fractional display debt instead of replaying a frame");

  expect(goal_jak2_display_timing_normalize_target_frame_rate(90) == 60 &&
             goal_jak2_display_timing_normalize_target_frame_rate(120) == 120,
         "unsupported rates fail closed to the 60 Hz domain");
  return g_failures == 0 ? 0 : 1;
}
