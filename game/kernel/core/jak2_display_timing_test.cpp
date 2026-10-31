#include <algorithm>
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

struct Elapsed {
  double logical_seconds = 0.0;
  double sound_seconds = 0.0;
  unsigned max_dispatcher_frames = 0;
};

// Replays callbacks from `start`, one per interval, cycling through `intervals` (in seconds).
// The first callback has no history and is defined as one logical frame of the domain for both
// clocks, so the time that matches the replay is the wall span plus 1/rate seconds.
Elapsed replay(int rate, double start, const double* intervals, int interval_count, int callbacks) {
  goal_jak2_display_timing timing = {};
  goal_jak2_display_timing_init(&timing, rate);
  Elapsed out;
  double timestamp = start;
  for (int i = 0; i < callbacks; ++i) {
    if (i > 0) {
      timestamp += intervals[(i - 1) % interval_count];
    }
    const auto step = advance(&timing, timestamp);
    out.logical_seconds += static_cast<double>(step.dispatcher_frames) / rate;
    out.sound_seconds += step.sound_frames / 60.0;
    out.max_dispatcher_frames = std::max(out.max_dispatcher_frames, step.dispatcher_frames);
  }
  out.logical_seconds -= 1.0 / rate;
  out.sound_seconds -= 1.0 / rate;
  return out;
}

bool tracks_wall_time(int rate, const double* intervals, int interval_count, int callbacks) {
  double wall = 0.0;
  for (int i = 1; i < callbacks; ++i) {
    wall += intervals[(i - 1) % interval_count];
  }
  // A ProMotion host clock is large (seconds since boot), so exercise realistic magnitudes.
  const auto r = replay(rate, 86400.0, intervals, interval_count, callbacks);
  const double frame = 1.0 / rate;
  // Less than one frame of each clock may still be pending as fractional debt.
  return std::fabs(r.logical_seconds - wall) <= frame &&
         std::fabs(r.sound_seconds - wall) <= 1.0 / 60.0 + 1e-9;
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
             first.sound_frames + second.sound_frames + third.sound_frames == 1 &&
             first.sound_before_dispatch_mask == 0 && second.sound_before_dispatch_mask == 1 &&
             third.sound_before_dispatch_mask == 0,
         "120 Hz dispatches every callback while the IOP clock remains 60 Hz");

  goal_jak2_display_timing_set_target_frame_rate(&timing, 120);
  const auto sixty_first = advance(&timing, 20.0);
  const auto sixty_second = advance(&timing, 20.0 + 1.0 / 60.0);
  expect(sixty_first.dispatcher_frames == 1 && sixty_second.dispatcher_frames == 2 &&
             sixty_first.sound_frames + sixty_second.sound_frames == 1 &&
             sixty_second.sound_before_dispatch_mask == 1,
         "a 60 Hz delivery fallback preserves a 120 Hz logical domain with one 60 Hz sound frame");

  goal_jak2_display_timing_set_target_frame_rate(&timing, 120);
  const auto slow_first = advance(&timing, 30.0);
  const auto slow_second = advance(&timing, 30.0 + 1.0 / 24.0);
  expect(slow_first.dispatcher_frames == 1 && slow_second.dispatcher_frames == 5 &&
             slow_first.sound_frames + slow_second.sound_frames == 3 &&
             slow_second.sound_before_dispatch_mask == 0b10101,
         "24 Hz delivery is bounded to five logical frames and preserves fractional sound debt");

  goal_jak2_display_timing_set_target_frame_rate(&timing, 60);
  const auto baseline_first = advance(&timing, 40.0);
  const auto baseline_second = advance(&timing, 40.0 + 1.0 / 60.0);
  expect(baseline_first.dispatcher_frames == 1 && baseline_second.dispatcher_frames == 1 &&
             baseline_first.sound_frames == 1 && baseline_second.sound_frames == 1 &&
             baseline_first.sound_before_dispatch_mask == 1 &&
             baseline_second.sound_before_dispatch_mask == 1,
         "60 Hz retains one dispatcher and one sound frame per callback");

  goal_jak2_display_timing_set_target_frame_rate(&timing, 60);
  const auto thirty_first = advance(&timing, 45.0);
  const auto thirty_second = advance(&timing, 45.0 + 1.0 / 30.0);
  expect(thirty_first.sound_before_dispatch_mask == 1 && thirty_second.dispatcher_frames == 2 &&
             thirty_second.sound_frames == 2 && thirty_second.sound_before_dispatch_mask == 0b11,
         "a low-cadence 60 Hz batch interleaves each sound frame before its dispatcher frame");

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

  const double vsync120 = 1.0 / 120.0;
  const double alternating[] = {vsync120, 2 * vsync120};
  expect(tracks_wall_time(60, alternating, 2, 2001),
         "60 Hz logical and sound time track callbacks alternating 8.333/16.667 ms");
  const double steady25[] = {3 * vsync120};
  expect(tracks_wall_time(60, steady25, 1, 1201),
         "60 Hz logical and sound time track steady 25 ms callbacks");
  const double ideal60[] = {1.0 / 60.0};
  const double ideal120[] = {vsync120};
  const double delivered80[] = {1.0 / 80.0};
  const double delivered40[] = {1.0 / 40.0};
  const double delivered30[] = {1.0 / 30.0};
  expect(tracks_wall_time(60, ideal60, 1, 3601) && tracks_wall_time(120, ideal120, 1, 7201) &&
             tracks_wall_time(120, ideal60, 1, 3601) &&
             tracks_wall_time(120, delivered80, 1, 4801) &&
             tracks_wall_time(120, delivered40, 1, 2401) &&
             tracks_wall_time(60, delivered30, 1, 1801),
         "established 60/120 Hz deliveries still track wall time over a minute");
  const double delivered90[] = {1.0 / 90.0};
  expect(tracks_wall_time(60, delivered90, 1, 5401),
         "60 Hz logical and sound time track a 90 Hz delivered cadence over a minute");
  expect(tracks_wall_time(120, delivered90, 1, 5401),
         "120 Hz logical and sound time track a 90 Hz delivered cadence over a minute");
  const auto ideal_run = replay(60, 86400.0, ideal60, 1, 3601);
  const auto ideal120_run = replay(120, 86400.0, ideal120, 1, 7201);
  expect(ideal_run.max_dispatcher_frames == 1 && ideal120_run.max_dispatcher_frames == 1,
         "on-cadence delivery at host-clock magnitudes never batches frames");

  goal_jak2_display_timing_set_target_frame_rate(&timing, 60);
  advance(&timing, 80.0);
  const auto repeated = advance(&timing, 80.0);
  const auto after_repeat = advance(&timing, 80.0 + 1.0 / 60.0);
  expect(repeated.dispatcher_frames == 0 && repeated.sound_frames == 0 &&
             after_repeat.dispatcher_frames == 1 && after_repeat.sound_frames == 1,
         "a repeated timestamp grants no elapsed time and keeps the cadence history");

  goal_jak2_display_timing_set_target_frame_rate(&timing, 60);
  advance(&timing, 90.0);
  const auto stalled = advance(&timing, 90.5);
  const auto recovered = advance(&timing, 90.5 + 1.0 / 60.0);
  expect(stalled.dispatcher_frames == 4 && recovered.dispatcher_frames == 1,
         "the catch-up bound still drops a long stall's excess instead of replaying it later");

  expect(goal_jak2_display_timing_normalize_target_frame_rate(90) == 60 &&
             goal_jak2_display_timing_normalize_target_frame_rate(120) == 120,
         "unsupported rates fail closed to the 60 Hz domain");
  return g_failures == 0 ? 0 : 1;
}
