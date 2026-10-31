/*!
 * @file gfx_host_presentation_test.cpp
 * The optional presentation hooks are forwarded exactly, are no-ops without a host or hook, and
 * never let a host exception escape.
 */

#include <cstdio>
#include <stdexcept>
#include <vector>

#include "game/kernel/core/gfx_host.h"
#include "game/kernel/core/kernel_core.h"

namespace {

int g_failures = 0;
std::vector<int> g_calls;

void expect(bool condition, const char* message) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
  if (!condition) {
    ++g_failures;
  }
}

void record_deferred(int deferred) {
  g_calls.push_back(10 + deferred);
}

void record_finish(int allow_present) {
  g_calls.push_back(20 + allow_present);
}

void throw_standard(int) {
  throw std::runtime_error("host failure");
}

void throw_other(int) {
  throw 7;
}

}  // namespace

static_assert(noexcept(goal_gfx_host_set_presentation_deferred(0)));
static_assert(noexcept(goal_gfx_host_finish_presentation_batch(0)));

int main() {
  goal_gfx_host_set_presentation_deferred(1);
  goal_gfx_host_finish_presentation_batch(1);
  expect(g_calls.empty(), "the wrappers are no-ops before any host is installed");

  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK ||
      goal_kernel_core_stub_machine_layer(0) != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: setup: %s\n", goal_kernel_core_last_error());
    return 1;
  }

  goal_gfx_host without_hooks = {};
  expect(goal_gfx_host_install(&without_hooks) == GOAL_KERNEL_CORE_OK,
         "a host without presentation hooks installs");
  goal_gfx_host_set_presentation_deferred(1);
  goal_gfx_host_finish_presentation_batch(0);
  expect(g_calls.empty(), "a host without presentation hooks keeps both wrappers inert");

  goal_gfx_host with_hooks = {};
  with_hooks.set_presentation_deferred = record_deferred;
  with_hooks.finish_presentation_batch = record_finish;
  expect(goal_gfx_host_install(&with_hooks) == GOAL_KERNEL_CORE_OK,
         "a host with presentation hooks installs");
  goal_gfx_host_set_presentation_deferred(1);
  goal_gfx_host_set_presentation_deferred(0);
  goal_gfx_host_finish_presentation_batch(1);
  goal_gfx_host_finish_presentation_batch(0);
  expect(g_calls == std::vector<int>{11, 10, 21, 20},
         "install copies both hooks and the wrappers forward each argument in order");

  goal_gfx_host throwing = {};
  throwing.set_presentation_deferred = throw_standard;
  throwing.finish_presentation_batch = throw_other;
  goal_gfx_host_install(&throwing);
  bool escaped = false;
  try {
    goal_gfx_host_set_presentation_deferred(1);
    goal_gfx_host_finish_presentation_batch(0);
  } catch (...) {
    escaped = true;
  }
  expect(!escaped, "standard and non-standard host exceptions do not escape the wrappers");

  g_calls.clear();
  goal_gfx_host_install(&with_hooks);
  goal_gfx_host_install(nullptr);
  goal_gfx_host_set_presentation_deferred(1);
  goal_gfx_host_finish_presentation_batch(1);
  expect(g_calls.empty(), "installing no host clears both hooks");

  std::printf("%s: gfx host presentation hooks\n", g_failures ? "FAILED" : "PASSED");
  return g_failures ? 1 : 0;
}
