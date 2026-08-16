/*!
 * @file thread_switch_test.cpp
 * Test the native ARM64 implementations of GOAL's thread switch, catch frames and state entry.
 *
 * Two layers:
 *
 *  1. The assembly primitives on their own (game/kernel/core/goal_thread_arm64.s): capturing a
 *     machine context and coming back to it, and running a function on another stack. The
 *     callee-saved registers are filled with sentinels first, by goal_thread_test_arm64.s, because
 *     "the callee-saved registers survive the transfer" is the whole property and C cannot state it.
 *
 *  2. The GOAL routines built on them, driven through real GOAL. test/goalc/aot/thread_switch_test.gc
 *     is compiled on top of the real kernel of whichever game this binary is linked against
 *     (jak1-kernel-core or jak2-kernel-core) and loaded into it, and it spawns processes, throws, goes to a state and suspends the way the game does.
 *     Between the two resumes of the suspended thread this file overwrites the thread's whole live
 *     stack, so a suspend that did not really copy it out cannot pass.
 *
 * See docs/aot-stack-model.md.
 */

#include <cinttypes>
#include <cstdio>
#include <cstring>

extern "C" {
#include "aot_boot_manifest.h"
}

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/symbols.h"

#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/kernel_core.h"
#include "game/runtime.h"

extern "C" {
// goal_thread_arm64.s, the primitives under test
uint64_t goal_context_save_and_call(void (*fn)(void*, uint64_t), uint64_t arg);
[[noreturn]] void goal_context_restore(const void* ctx, uint64_t value);
uint64_t goal_call_on_stack_arm64(void* new_sp,
                                  void* fn,
                                  uint64_t a0,
                                  uint64_t a1,
                                  uint64_t a2,
                                  uint64_t a3,
                                  uint64_t a4,
                                  uint64_t a5);
uint64_t goal_read_stack_pointer(void);

uint64_t goal_native_thread_suspend(uint64_t,
                                    uint64_t,
                                    uint64_t,
                                    uint64_t,
                                    uint64_t,
                                    uint64_t);

// goal_thread_test_arm64.s
uint64_t goal_test_saved_registers(uint64_t (*fn)(uint64_t), uint64_t arg, uint64_t* out);
}

namespace {

static_assert(offsetof(goal_thread_stack_watermark_report, current_used) ==
                  offsetof(goal_thread_stack_watermark_report, fullest_name) + sizeof(const char*),
              "stack diagnostics must remain tail-appended to the C ABI");

int g_failures = 0;

// gkernel's AOT function list keeps the native thread-suspend implementation at index 22. The
// product crash reached this function object through cpu-thread.suspend-hook, but its native entry
// had become zero.
constexpr int kThreadSuspendFunctionIndex = 22;
u32 g_thread_suspend_object = 0;
uintptr_t g_thread_suspend_entry = 0;

void fail(const char* what) {
  std::printf("  FAIL %s\n", what);
  std::fflush(stdout);
  g_failures++;
}

void check_u64(const char* what, u64 got, u64 expected) {
  if (got != expected) {
    std::printf("  FAIL %-40s got #x%" PRIx64 ", expected #x%" PRIx64 "\n", what, got, expected);
    g_failures++;
  } else {
    std::printf("  ok   %-40s #x%" PRIx64 "\n", what, got);
  }
  std::fflush(stdout);
}

void check_s64(const char* what, s64 got, s64 expected) {
  if (got != expected) {
    std::printf("  FAIL %-40s got %" PRId64 ", expected %" PRId64 "\n", what, got, expected);
    g_failures++;
  } else {
    std::printf("  ok   %-40s %" PRId64 "\n", what, got);
  }
  std::fflush(stdout);
}

/*! The 18 sentinels goal_test_saved_registers installs, in the order it writes them back. */
void check_saved_registers(const char* what, const u64* observed) {
  constexpr u64 base = 0xa5a5000000000000ull;
  int wrong = 0;
  for (int i = 0; i < 10; i++) {
    if (observed[i] != base + (u64)(19 + i)) {
      std::printf("  FAIL %s: x%d came back as #x%" PRIx64 "\n", what, 19 + i, observed[i]);
      wrong++;
    }
  }
  for (int i = 0; i < 8; i++) {
    if (observed[10 + i] != base + (u64)(8 + i)) {
      std::printf("  FAIL %s: d%d came back as #x%" PRIx64 "\n", what, 8 + i, observed[10 + i]);
      wrong++;
    }
  }
  if (wrong) {
    g_failures++;
  } else {
    std::printf("  ok   %-40s x19-x28 and d8-d15 all preserved\n", what);
  }
  std::fflush(stdout);
}

u32 symbol_value(const char* name) {
  uint32_t value = 0;
  if (goal_kernel_core_lookup(name, nullptr, &value) != GOAL_KERNEL_CORE_OK) {
    fail(name);
    return 0;
  }
  return value;
}

bool check_thread_suspend_function(const char* stage, bool capture) {
  const u32 object = goal_aot_function_object("gkernel", kThreadSuspendFunctionIndex);
  uintptr_t entry = 0;
  if (object && object <= EE_MAIN_MEM_SIZE - sizeof(entry)) {
    std::memcpy(&entry, g_ee_main_mem + object, sizeof(entry));
  }

  const uintptr_t expected = reinterpret_cast<uintptr_t>(&goal_native_thread_suspend);
  if (!object || !entry || entry != expected ||
      (!capture && (object != g_thread_suspend_object || entry != g_thread_suspend_entry))) {
    std::printf(
        "  FAIL %-40s object #x%x, native #x%" PRIxPTR ", expected (#x%x, #x%" PRIxPTR
        ")\n",
        stage, object, entry, capture ? object : g_thread_suspend_object,
        capture ? expected : g_thread_suspend_entry);
    std::fflush(stdout);
    g_failures++;
    return false;
  }

  if (capture) {
    g_thread_suspend_object = object;
    g_thread_suspend_entry = entry;
  }
  std::printf("  ok   %-40s object #x%x, native #x%" PRIxPTR "\n", stage, object, entry);
  std::fflush(stdout);
  return true;
}

/*!
 * Call a GOAL function by symbol on GOAL's own stack. These functions spawn processes and build
 * catch frames, which only work with a GOAL-memory stack pointer.
 */
u64 call_on_goal_stack(const char* name) {
  uint32_t value = 0;
  if (goal_kernel_core_lookup(name, nullptr, &value) != GOAL_KERNEL_CORE_OK || !value) {
    fail(name);
    return 0;
  }
  // call_goal_on_stack passes no arguments, so the fixture hands the process it is driving over in
  // *tsw-proc* instead.
  return call_goal_on_stack(Ptr<Function>(value), goal_kernel_stack_top(), s7.offset,
                            g_ee_main_mem);
}

// ---------------------------------------------------------------------------------------------
// layer 1: the assembly primitives
// ---------------------------------------------------------------------------------------------

u32 g_primitive_stack_region = 0;
s32 g_primitive_stack_size = 0;
u64 g_observed_inner_sp = 0;
u64 g_observed_outer_sp = 0;

void restore_immediately(void* ctx, uint64_t value) {
  goal_context_restore(ctx, value);
}

u64 context_round_trip(u64 value) {
  return goal_context_save_and_call(&restore_immediately, value);
}

/*! Runs on the switched-to stack. */
u64 note_stack_pointer(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
  g_observed_inner_sp = goal_read_stack_pointer();
  return a0 + a1 + a2 + a3 + a4 + a5;
}

void test_primitives() {
  std::printf("\n== the ARM64 machine-context primitives ==\n");

  check_u64("a context round trip returns the value", context_round_trip(0x1234abcd), 0x1234abcd);

  u64 observed[18] = {0};
  const u64 result = goal_test_saved_registers(&context_round_trip, 0xfeedface, observed);
  check_u64("...with sentinels in the saved regs", result, 0xfeedface);
  check_saved_registers("a context round trip", observed);

  // running on a stack inside GOAL memory, which is where GOAL threads live
  const s32 size = 32 * 1024;
  const Ptr<u8> region = kmalloc(kglobalheap, size, KMALLOC_MEMSET | KMALLOC_ALIGN_16, "tsw-stack");
  if (!region.offset) {
    fail("kmalloc(tsw-stack)");
    return;
  }
  g_primitive_stack_region = region.offset;
  g_primitive_stack_size = size;
  void* top = g_ee_main_mem + region.offset + size;

  g_observed_outer_sp = goal_read_stack_pointer();
  const u64 sum = goal_call_on_stack_arm64(top, (void*)&note_stack_pointer, 1, 2, 3, 4, 5, 6);
  check_u64("a call on another stack passes 6 arguments", sum, 21);
  if (g_observed_inner_sp > (u64)(uintptr_t)top ||
      g_observed_inner_sp <= (u64)(uintptr_t)(g_ee_main_mem + region.offset)) {
    fail("the callee did not run on the new stack");
  } else {
    std::printf("  ok   %-40s #x%" PRIx64 ", %" PRId64 " bytes below the top\n",
                "the callee ran on the new stack", g_observed_inner_sp,
                (s64)((u64)(uintptr_t)top - g_observed_inner_sp));
  }
  check_u64("the caller's stack pointer came back", goal_read_stack_pointer(),
            g_observed_outer_sp);
}

// ---------------------------------------------------------------------------------------------
// layer 2: the GOAL routines
// ---------------------------------------------------------------------------------------------

// the bits test/goalc/aot/thread_switch_test.gc sets in *tsw-log*
constexpr u32 TSW_CATCH_RETURNED = 1;
constexpr u32 TSW_THREW = 4;
constexpr u32 TSW_INIT_RAN = 8;
constexpr u32 TSW_STATE_CODE_RAN = 16;
constexpr u32 TSW_BEFORE_SUSPEND = 32;
constexpr u32 TSW_AFTER_SUSPEND = 64;
constexpr u32 TSW_STATE_CODE_FINISHED = 128;
constexpr u32 TSW_TEMP_THREAD_RAN = 256;
constexpr u32 TSW_AFTER_ABANDON_UNREACHABLE = 512;

u32 tsw_log() {
  return symbol_value("*tsw-log*");
}

void check_log(const char* what, u32 required, u32 forbidden) {
  const u32 log = tsw_log();
  if ((log & required) != required || (log & forbidden) != 0) {
    std::printf("  FAIL %-40s trace #x%x, wanted #x%x set and #x%x clear\n", what, log, required,
                forbidden);
    g_failures++;
  } else {
    std::printf("  ok   %-40s trace #x%x\n", what, log);
  }
  std::fflush(stdout);
}

void test_catch_and_throw() {
  std::printf("\n== GOAL catch frames and throw ==\n");
  call_on_goal_stack("tsw-reset");
  call_on_goal_stack("tsw-run-catch-tests");

  check_log("both catch frames ran", TSW_CATCH_RETURNED | TSW_THREW,
            TSW_AFTER_ABANDON_UNREACHABLE);
  check_s64("a catch frame returns its function's value", (s32)symbol_value("*tsw-catch-result*"),
            63);
  check_s64("a throw returns the thrown value", (s32)symbol_value("*tsw-throw-result*"), 3);
  check_s64("locals live across the throw survived",
            (s32)symbol_value("*tsw-live-across-throw*"), 123450);
}

void test_thread_suspend_and_resume() {
  std::printf("\n== GOAL states, suspend and resume ==\n");
  call_on_goal_stack("tsw-reset");
  if (!call_on_goal_stack("tsw-spawn-state-proc")) {
    fail("tsw-spawn-state-proc");
    return;
  }
  const u64 hook = call_on_goal_stack("tsw-thread-suspend-hook");
  check_u64("new thread copied the suspend function", hook, g_thread_suspend_object);
  if (hook != g_thread_suspend_object ||
      !check_thread_suspend_function("after process construction", false)) {
    return;
  }
  check_log("the init function went to a state", TSW_INIT_RAN,
            TSW_STATE_CODE_RAN | TSW_AFTER_ABANDON_UNREACHABLE);

  call_on_goal_stack("tsw-resume");
  if (!check_thread_suspend_function("after the first suspend", false)) {
    return;
  }
  check_log("the state's code ran and suspended", TSW_STATE_CODE_RAN | TSW_BEFORE_SUSPEND,
            TSW_AFTER_SUSPEND | TSW_STATE_CODE_FINISHED);

  const s32 used = (s32)call_on_goal_stack("tsw-thread-stack-used");
  const u32 sp = (u32)call_on_goal_stack("tsw-thread-stack-pointer");
  std::printf("  the suspended thread had %d bytes of stack live at GOAL #x%x\n", used, sp);
  if (used <= 0) {
    fail("the suspended thread recorded no live stack");
    return;
  }
  goal_thread_stack_watermark_report watermark = {};
  goal_thread_stack_watermark(&watermark);
  check_s64("the watermark records the current usage", watermark.current_used, used);
  if (watermark.current_size < watermark.current_used) {
    fail("the watermark reports an undersized current backup");
  } else {
    std::printf("  ok   %-40s %d of %d bytes\n", "the current backup contains the stack",
                watermark.current_used, watermark.current_size);
  }
  check_s64("no stack overflow was observed", watermark.overflow_failures, 0);
  check_s64("no stack validation failed", watermark.validation_failures, 0);

  // Overwrite everything the thread had live. If thread-suspend did not really copy it into the
  // backup buffer, or thread-resume does not really copy it back, nothing below can work.
  std::memset(g_ee_main_mem + sp, 0x5a, (size_t)used);

  call_on_goal_stack("tsw-resume");
  check_log("it resumed and ran to the end", TSW_AFTER_SUSPEND | TSW_STATE_CODE_FINISHED,
            TSW_AFTER_ABANDON_UNREACHABLE);
  check_s64("stack values survived the scribble",
            (s32)symbol_value("*tsw-stack-across-suspend*"), 5579);
  check_u64("the process deactivated itself", call_on_goal_stack("tsw-process-dead?"), 1);
}

void test_temporary_threads() {
  std::printf("\n== reset-and-call and return-from-thread ==\n");
  call_on_goal_stack("tsw-reset");
  if (!call_on_goal_stack("tsw-spawn-state-proc")) {
    fail("tsw-spawn-state-proc");
    return;
  }
  call_on_goal_stack("tsw-reset");
  check_s64("a temporary thread's value comes back",
            (s32)call_on_goal_stack("tsw-run-temp-return"), 90210);
  check_log("the temporary thread ran", TSW_TEMP_THREAD_RAN, TSW_AFTER_ABANDON_UNREACHABLE);

  call_on_goal_stack("tsw-reset");
  call_on_goal_stack("tsw-run-temp-abandon");
  check_log("abandon-thread returns to the kernel", TSW_TEMP_THREAD_RAN,
            TSW_AFTER_ABANDON_UNREACHABLE);
}

}  // namespace

int main() {
  lg::set_stdout_level(lg::level::warn);
  lg::set_flush_level(lg::level::warn);
  lg::initialize();

  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  goal_kernel_core_stub_machine_layer(0);

  std::printf("== loading the kernel and the fixture ==\n");
  for (int i = 0; i < goal_aot_boot_file_count; i++) {
    const auto& entry = goal_aot_boot_files[i];
    goal_aot_object_file file = {entry.tag,      entry.statics,          *entry.static_count,
                                 entry.functions, *entry.function_count, entry.link};
    if (goal_aot_load(&file) != GOAL_KERNEL_CORE_OK) {
      std::printf("FAIL load %s: %s\n", entry.source, goal_kernel_core_last_error());
      return 1;
    }
    if (std::strcmp(entry.tag, "gkernel") == 0 &&
        !check_thread_suspend_function("when gkernel allocated it", true)) {
      return 1;
    }
    if (goal_aot_run_top_level(entry.tag, nullptr) != GOAL_KERNEL_CORE_OK) {
      std::printf("FAIL top-level %s: %s\n", entry.source, goal_kernel_core_last_error());
      return 1;
    }
    std::printf("  %s\n", entry.source);
  }

  if (!check_thread_suspend_function("after fixture initialization", false)) {
    return 1;
  }

  test_primitives();
  test_catch_and_throw();
  test_thread_suspend_and_resume();
  test_temporary_threads();

  goal_aot_reset();
  goal_kernel_core_shutdown();

  if (g_failures) {
    std::printf("\nTHREAD SWITCH TEST FAILED (%d failures)\n", g_failures);
    return 1;
  }
  std::printf("\nTHREAD SWITCH TEST PASSED\n");
  return 0;
}
