/*!
 * @file aot_boot_test.cpp
 * Load Jak 1's object files into the real kernel in the real build order and run each one's
 * top-level, going as far as the runtime can currently get.
 *
 * The file list and every file's static data, function table and link step come from
 * aot_boot_manifest.h, which goalc-cbackend-sweep generates by walking goal_src/jak1/game.gp
 * through the make system. So this is the game's own build order, not a hand-picked list.
 *
 * This is a progress probe, not a pass/fail conformance test: it reports exactly how many files
 * loaded, how many top-levels ran, and what stopped it. Nothing here is allowed to skip a step to
 * get further.
 */

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" {
#include "aot_boot_manifest.h"
}

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/symbols.h"

#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

namespace {

/*! Everything is flushed as it happens: if a top-level crashes, the last line printed says where. */
void say(const char* format, ...) __attribute__((format(printf, 1, 2)));
void say(const char* format, ...) {
  va_list args;
  va_start(args, format);
  std::vfprintf(stdout, format, args);
  va_end(args);
  std::fflush(stdout);
}

int untranslated_functions(const goal_aot_boot_entry& entry) {
  int count = 0;
  for (int i = 0; i < *entry.function_count; i++) {
    if (!entry.functions[i]) {
      count++;
    }
  }
  return count;
}

void report_heaps() {
  goal_kernel_core_state state;
  if (goal_kernel_core_get_state(&state) == GOAL_KERNEL_CORE_OK) {
    say("  global heap at #x%x, %d symbols\n", state.global_heap_current_offset,
        state.symbol_count);
  }
}

/*! Anything GOAL printed since the last call, so a failing top-level's own message is visible. */
void drain_goal_print_buffer() {
  const char* printed = Ptr<char>(PrintBufArea.offset + sizeof(ListenerMessageHeader)).c();
  if (printed[0]) {
    say("  GOAL said: %s\n", printed);
    clear_print();
  }
}

}  // namespace

int main() {
  lg::set_stdout_level(lg::level::warn);
  lg::set_flush_level(lg::level::warn);
  lg::initialize();

  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    say("FAIL: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  {
    goal_kernel_core_state state;
    goal_kernel_core_get_state(&state);
    say("kernel up: s7 #x%x, %d symbols, EE main memory executable: %s\n", state.s7_offset,
        state.symbol_count, state.main_memory_executable ? "YES" : "NO");
  }
  clear_print();

  // The machine layer is not in this library. Reporting mode names each of its functions the first
  // time GOAL asks for one and returns 0, so one run enumerates everything a boot needs; a file
  // that loads after such a message has not been shown to work.
  goal_kernel_core_stub_machine_layer(0);

  int loaded = 0;
  int ran = 0;
  int missing_functions = 0;
  const char* stopped_by = nullptr;

  // JAK1_AOT_BOOT_FRONTIER is where this probe stops, and it is a fact about game data rather than
  // about the runtime: file 512 spawns a hud process whose init-particles! dereferences the art
  // group *fuelcell-naked-sg*, and no art group exists until a DGO has been loaded, which
  // jak1-data-boot-test is what does. That dereference is a hard fault in the guard page, not a
  // reportable failure, so the walk stops before it instead of taking the whole probe down. Raise
  // the number to find the next frontier.
  const int limit = goal_aot_boot_file_count < JAK1_AOT_BOOT_FRONTIER ? goal_aot_boot_file_count
                                                                      : JAK1_AOT_BOOT_FRONTIER;

  for (int i = 0; i < limit; i++) {
    const auto& entry = goal_aot_boot_files[i];
    const int holes = untranslated_functions(entry);
    missing_functions += holes;
    say("[%3d/%3d] %s\n", i + 1, limit, entry.source);
    say("  %d statics, %d functions", *entry.static_count, *entry.function_count);
    if (holes) {
      say(", %d of them not translated", holes);
    }
    say("\n");

    goal_aot_object_file file = {entry.tag,      entry.statics,          *entry.static_count,
                                 entry.functions, *entry.function_count, entry.link};
    const auto status = goal_aot_load(&file);
    if (status != GOAL_KERNEL_CORE_OK) {
      say("  LOAD FAILED (%d): %s\n", (int)status, goal_kernel_core_last_error());
      stopped_by = "load";
      break;
    }
    loaded++;

    const uint32_t top_level = goal_aot_top_level_object(entry.tag);
    if (!top_level) {
      say("  NO TOP-LEVEL: the C backend could not translate it\n");
      stopped_by = "top-level was not translated";
      break;
    }
    say("  running top-level #x%x\n", top_level);
    if (goal_aot_run_top_level(entry.tag, nullptr) != GOAL_KERNEL_CORE_OK) {
      say("  TOP-LEVEL FAILED: %s\n", goal_kernel_core_last_error());
      stopped_by = "top-level";
      break;
    }
    ran++;
    drain_goal_print_buffer();
    report_heaps();
  }

  say("\nBOOT: loaded %d/%d files, ran %d top-levels", loaded, limit, ran);
  if (stopped_by) {
    say(", stopped by: %s", stopped_by);
  }
  say("\n");
  if (missing_functions) {
    say("%d functions in those files have no native code and will fail if called\n",
        missing_functions);
  }

  goal_aot_reset();
  goal_kernel_core_shutdown();

  // Without game data the boot cannot get past the documented frontier: file 512 needs an art
  // group that only a DGO load can supply, which is what jak1-data-boot-test does. So this passes
  // when it reaches the frontier and fails if it ever stops earlier than that.
  if (ran < JAK1_AOT_BOOT_FRONTIER) {
    say("REGRESSION: expected to reach at least file %d with no game data loaded\n",
        JAK1_AOT_BOOT_FRONTIER);
    return 1;
  }
  if (ran < goal_aot_boot_file_count) {
    say("At the known frontier: file %d of %d needs game data. jak1-data-boot-test loads it.\n",
        ran + 1, goal_aot_boot_file_count);
  }
  return 0;
}
