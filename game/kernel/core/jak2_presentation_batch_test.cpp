/*!
 * @file jak2_presentation_batch_test.cpp
 * The catch-up presentation batch runs every frame in order, defers presentation only before the
 * last frame, presents once when the batch completes, and only cancels when it stops or throws.
 */

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "game/kernel/core/gfx_host.h"
#include "game/kernel/core/jak2_presentation_batch.h"
#include "game/kernel/core/kernel_core.h"

namespace {

int g_failures = 0;
std::vector<std::string> g_events;
int g_deferred = 0;
bool g_throw_from_finish = false;

void expect(bool condition, const std::string& message) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", message.c_str());
  if (!condition) {
    ++g_failures;
  }
}

void host_set_deferred(int deferred) {
  g_deferred = deferred;
  g_events.push_back(deferred ? "defer" : "undefer");
}

void host_finish(int allow_present) {
  g_deferred = 0;
  g_events.push_back(allow_present ? "present" : "cancel");
  if (g_throw_from_finish) {
    throw std::runtime_error("finish failed");
  }
}

// A chain as the host sees it: which frame sent it and whether presentation was deferred.
void send_chain(uint32_t frame) {
  g_events.push_back("chain" + std::to_string(frame) + (g_deferred ? "d" : ""));
}

enum class Exit { Normal, Stop, StandardThrow, OtherThrow };

const char* exit_name(Exit exit) {
  switch (exit) {
    case Exit::Normal:
      return "normal";
    case Exit::Stop:
      return "stop (MasterExit / preview failure)";
    case Exit::StandardThrow:
      return "std::exception";
    case Exit::OtherThrow:
      return "non-standard throw";
  }
  return "";
}

struct Outcome {
  bool completed = false;
  bool threw = false;
  uint32_t frames_run = 0;
};

// Frame `stop_at` ends the batch the given way after sending its chains.
Outcome run_batch(uint32_t frames,
                  Exit exit,
                  uint32_t stop_at,
                  uint32_t chains_in_last_frame = 1) {
  g_events.clear();
  g_deferred = 0;
  Outcome outcome;
  try {
    outcome.completed = jak2_presentation_batch::run(frames, [&](uint32_t frame) {
      outcome.frames_run++;
      g_events.push_back("frame" + std::to_string(frame));
      const uint32_t chains = frame + 1 == frames ? chains_in_last_frame : 1;
      for (uint32_t chain = 0; chain < chains; chain++) {
        send_chain(frame);
      }
      if (frame == stop_at) {
        switch (exit) {
          case Exit::Normal:
            break;
          case Exit::Stop:
            return false;
          case Exit::StandardThrow:
            throw std::runtime_error("frame failed");
          case Exit::OtherThrow:
            throw 42;
        }
      }
      return true;
    });
  } catch (...) {
    outcome.threw = true;
  }
  return outcome;
}

std::vector<std::string> expected_events(uint32_t frames, uint32_t frames_run, bool completed) {
  std::vector<std::string> events;
  for (uint32_t frame = 0; frame < frames_run; frame++) {
    const bool last = frame + 1 == frames;
    if (frames >= 2) {
      events.push_back(last ? "undefer" : "defer");
    }
    events.push_back("frame" + std::to_string(frame));
    events.push_back("chain" + std::to_string(frame) + (frames >= 2 && !last ? "d" : ""));
  }
  if (frames >= 2) {
    events.push_back(completed ? "present" : "cancel");
  }
  return events;
}

int count(const std::string& event) {
  int total = 0;
  for (const auto& e : g_events) {
    total += e == event;
  }
  return total;
}

void check_matrix() {
  for (uint32_t frames = 0; frames <= 5; frames++) {
    for (Exit exit : {Exit::Normal, Exit::Stop, Exit::StandardThrow, Exit::OtherThrow}) {
      // Every failure boundary: the batch can end on any of its frames.
      const uint32_t last_stop = exit == Exit::Normal ? 0 : (frames ? frames - 1 : 0);
      for (uint32_t stop_at = 0; stop_at <= last_stop; stop_at++) {
        const Outcome outcome = run_batch(frames, exit, stop_at);
        const bool fails = exit != Exit::Normal && frames > 0;
        const uint32_t frames_run = fails ? stop_at + 1 : frames;
        const std::string name = "N=" + std::to_string(frames) + " " + exit_name(exit) +
                                 (fails ? " at frame " + std::to_string(stop_at) : "");
        const bool threw_as_expected =
            outcome.threw == (fails && exit != Exit::Stop);
        expect(outcome.frames_run == frames_run && outcome.completed == !fails &&
                   threw_as_expected &&
                   g_events == expected_events(frames, frames_run, !fails) &&
                   count("present") + count("cancel") == (frames >= 2 ? 1 : 0) &&
                   (!fails || count("present") == 0),
               name + ": frames, chains and hooks in order, one finish, no present on failure");
      }
    }
  }
}

void check_chain_shapes() {
  run_batch(3, Exit::Normal, 0, 2);
  expect(g_events == std::vector<std::string>{"defer", "frame0", "chain0d", "defer", "frame1",
                                              "chain1d", "undefer", "frame2", "chain2", "chain2",
                                              "present"},
         "a final frame with two chains presents both inline, then finishes once");

  run_batch(3, Exit::Normal, 0, 0);
  expect(g_events == std::vector<std::string>{"defer", "frame0", "chain0d", "defer", "frame1",
                                              "chain1d", "undefer", "frame2", "present"},
         "a final frame with no chain still finishes with a present request");
}

void check_hostile_finish() {
  g_throw_from_finish = true;
  const Outcome outcome = run_batch(3, Exit::StandardThrow, 1);
  g_throw_from_finish = false;
  expect(outcome.threw && count("cancel") == 1 && count("present") == 0,
         "a host that throws while cancelling during unwind does not terminate the tick");
}

}  // namespace

int main() {
  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK ||
      goal_kernel_core_stub_machine_layer(0) != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: setup: %s\n", goal_kernel_core_last_error());
    return 1;
  }

  goal_gfx_host without_hooks = {};
  goal_gfx_host_install(&without_hooks);
  const Outcome plain = run_batch(4, Exit::Normal, 0);
  const Outcome plain_throw = run_batch(4, Exit::OtherThrow, 2);
  expect(plain.completed && plain.frames_run == 4 && plain_throw.threw &&
             plain_throw.frames_run == 3,
         "a host without presentation hooks runs every frame unchanged");

  goal_gfx_host host = {};
  host.set_presentation_deferred = host_set_deferred;
  host.finish_presentation_batch = host_finish;
  goal_gfx_host_install(&host);
  check_matrix();
  check_chain_shapes();
  check_hostile_finish();

  std::printf("%s: jak2 presentation batch\n", g_failures ? "FAILED" : "PASSED");
  return g_failures ? 1 : 0;
}
