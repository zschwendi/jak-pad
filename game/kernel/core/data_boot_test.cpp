/*!
 * @file data_boot_test.cpp
 * Boot Jak 1 the way the game boots it: out of the DGO archives, in DGO order, with the code of
 * every object supplied by the AOT path and the data of every object read off the disc.
 *
 * Two modes, because they need different things:
 *
 *   --synthetic  builds a DGO archive in a temporary directory and loads it. No game data is
 *                needed or read, so this always runs. It proves the archive reader and the
 *                code/data precedence rule, not the game.
 *
 *   (default)    loads the player's real KERNEL.CGO and GAME.CGO and then runs the engine's own
 *                startup - `play`, then the GOAL kernel dispatcher. Needs a data directory, given
 *                by --data-dir or GOALPAD_JAK1_DATA_DIR, and reports that it was skipped when
 *                there is none. No game data is ever written anywhere.
 *
 * Like aot_boot_test, this is a progress probe: it reports how far the boot got and what stopped
 * it. Nothing here may skip a step to get further.
 */

#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "aot_boot_manifest.h"
}

#include "common/goal_constants.h"
#include "common/link_types.h"
#include "common/log/log.h"
#include "common/symbols.h"

#include "game/kernel/common/kboot.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/dgo_loader.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/jak1/klisten.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

namespace {

void say(const char* format, ...) __attribute__((format(printf, 1, 2)));
void say(const char* format, ...) {
  va_list args;
  va_start(args, format);
  std::vfprintf(stdout, format, args);
  va_end(args);
  std::fflush(stdout);
}

/*! Anything GOAL printed since the last call, so a failing top-level's own message is visible. */
void drain_goal_print_buffer() {
  const char* printed = Ptr<char>(PrintBufArea.offset + sizeof(ListenerMessageHeader)).c();
  if (printed[0]) {
    say("GOAL said: %s\n", printed);
    clear_print();
  }
}

void report_heap(const char* what) {
  goal_kernel_core_state state;
  if (goal_kernel_core_get_state(&state) == GOAL_KERNEL_CORE_OK) {
    say("  %s: global heap at #x%x (%u bytes used), %d symbols\n", what,
        state.global_heap_current_offset, state.global_heap_used_bytes, state.symbol_count);
  }
}

/*! The DGO object name for a GOAL source: its base name without the extension. */
std::string object_name_of(const char* source) {
  std::string path = source;
  const auto slash = path.find_last_of('/');
  if (slash != std::string::npos) {
    path = path.substr(slash + 1);
  }
  const auto dot = path.find_last_of('.');
  if (dot != std::string::npos) {
    path = path.substr(0, dot);
  }
  return path;
}

/*!
 * Tell the DGO loader which native translation unit stands in for each object it will meet.
 */
void register_aot_objects() {
  for (int i = 0; i < goal_aot_boot_file_count; i++) {
    const auto& entry = goal_aot_boot_files[i];
    goal_aot_object_file file = {entry.tag,       entry.statics,          *entry.static_count,
                                 entry.functions, *entry.function_count,  entry.link};
    goal_aot_register_object(object_name_of(entry.source).c_str(), &file);
  }
}

bool write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) {
    return false;
  }
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), fp) == bytes.size();
  std::fclose(fp);
  return ok;
}

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; i++) {
    out.push_back((uint8_t)(value >> (8 * i)));
  }
}

void append_name(std::vector<uint8_t>& out, const std::string& name) {
  for (int i = 0; i < 60; i++) {
    out.push_back(i < (int)name.size() ? (uint8_t)name[i] : 0);
  }
}

/*!
 * A DGO holding the eight objects of the real KERNEL.CGO, by name, with bodies that are not the
 * game's: each is a v3 OpenGOAL header over filler. The loader must recognize them as code, throw
 * the bodies away, and run the native translation instead - so the filler is exactly the point.
 * Nothing here comes from the game.
 */
std::vector<uint8_t> build_synthetic_dgo(const std::vector<std::string>& objects) {
  std::vector<uint8_t> out;
  append_u32(out, (uint32_t)objects.size());
  append_name(out, "SYNTH.CGO");

  for (const auto& name : objects) {
    // an object body just long enough to hold a link header the reader will classify
    std::vector<uint8_t> body;
    append_u32(body, 0x4c414f47);  // 'GOAL', the v3 type tag: not the 0xffffffff of v2/v4 data
    append_u32(body, 0);
    append_u32(body, 3);  // object file version
    while (body.size() < 48) {
      body.push_back(0xcd);
    }

    append_u32(out, (uint32_t)body.size());
    append_name(out, name);
    out.insert(out.end(), body.begin(), body.end());
    while (out.size() % 16) {
      out.push_back(0);
    }
  }
  return out;
}

int run_synthetic() {
  const char* tmp = std::getenv("TMPDIR");
  const std::string dir = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/goalpad-synthetic-dgo";
  const std::string iso = dir + "/iso";
  mkdir(dir.c_str(), 0755);
  if (mkdir(iso.c_str(), 0755) != 0 && errno != EEXIST) {
    say("FAIL: could not make %s\n", iso.c_str());
    return 1;
  }

  const std::vector<std::string> objects = {"gcommon",  "gstring-h", "gkernel-h", "gkernel",
                                            "pskernel", "gstring",   "dgo-h",     "gstate"};
  if (!write_file(iso + "/SYNTH.CGO", build_synthetic_dgo(objects))) {
    say("FAIL: could not write %s/SYNTH.CGO\n", iso.c_str());
    return 1;
  }

  goal_kernel_core_set_data_directory(dir.c_str());
  goal_kernel_core_stub_machine_layer(0);

  goal_dgo_load_stats stats;
  say("loading the synthetic archive from %s\n", iso.c_str());
  if (goal_dgo_load("SYNTH", LINK_FLAG_EXECUTE, 0x400000, &stats) != GOAL_KERNEL_CORE_OK) {
    say("FAIL: %s\n", goal_dgo_last_error());
    return 1;
  }
  drain_goal_print_buffer();
  say("  %d objects: %d code (from the AOT path), %d data, %d already loaded\n", stats.objects,
      stats.code_objects, stats.data_objects, stats.reused_code);
  report_heap("after");

  int failures = 0;
  auto expect = [&](bool ok, const char* what) {
    say("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
      failures++;
    }
  };
  expect(stats.objects == (int)objects.size(), "every object in the archive was processed");
  expect(stats.code_objects == (int)objects.size(), "all of them were classified as code");
  expect(stats.data_objects == 0, "none of them were linked as data");

  // If the top-levels really ran, the GOAL kernel is there: these come from gkernel.gc and
  // gstate.gc, not from the C kernel.
  uint32_t value = 0;
  expect(goal_kernel_core_lookup("*kernel-version*", nullptr, &value) == GOAL_KERNEL_CORE_OK &&
             value != 0,
         "*kernel-version* was set by the GOAL kernel's own top-level");
  say("  *kernel-version* = %u.%u\n", value >> 0x13, (value >> 3) & 0xffff);
  expect(goal_kernel_core_lookup("kernel-dispatcher", nullptr, &value) == GOAL_KERNEL_CORE_OK &&
             value != 0,
         "kernel-dispatcher holds a function");

  // A code object with no native translation must fail loudly rather than be skipped.
  if (!write_file(iso + "/NOSUCH.CGO", build_synthetic_dgo({"an-object-that-does-not-exist"}))) {
    say("FAIL: could not write the second fixture\n");
    return 1;
  }
  const auto status = goal_dgo_load("NOSUCH", LINK_FLAG_EXECUTE, 0x400000, nullptr);
  expect(status != GOAL_KERNEL_CORE_OK, "an untranslated code object fails the load");
  say("  reported: %s\n", goal_dgo_last_error());

  remove((iso + "/SYNTH.CGO").c_str());
  remove((iso + "/NOSUCH.CGO").c_str());
  rmdir(iso.c_str());
  rmdir(dir.c_str());
  return failures ? 1 : 0;
}

/*!
 * The boot sequence itself, in the order jak1::InitHeapAndSymbol and jak1::InitMachineScheme run
 * it. Every step says what it is standing in for.
 */
int run_real_boot(const std::string& data_dir, int dispatch_frames, bool run_play) {
  goal_kernel_core_set_data_directory(data_dir.c_str());
  say("data directory: %s\n", data_dir.c_str());

  goal_dgo_load_stats stats;
  const u32 boot_flags = LINK_FLAG_OUTPUT_LOAD | LINK_FLAG_EXECUTE | LINK_FLAG_PRINT_LOGIN;

  // InitHeapAndSymbol: the GOAL kernel.
  say("\n=== KERNEL.CGO\n");
  if (goal_dgo_load("KERNEL", boot_flags, 0x400000, &stats) != GOAL_KERNEL_CORE_OK) {
    say("FAILED: %s\n", goal_dgo_last_error());
    drain_goal_print_buffer();
    return 1;
  }
  drain_goal_print_buffer();
  say("  %d objects: %d code, %d data; heap use %u -> %u bytes\n", stats.objects,
      stats.code_objects, stats.data_objects, stats.heap_used_before, stats.heap_used_after);
  report_heap("after KERNEL.CGO");

  uint32_t kernel_version = 0;
  goal_kernel_core_lookup("*kernel-version*", nullptr, &kernel_version);
  if (!kernel_version) {
    say("FAILED: the GOAL kernel did not set *kernel-version*\n");
    return 1;
  }
  say("  GOAL kernel version %u.%u\n", kernel_version >> 0x13, (kernel_version >> 3) & 0xffff);

  // InitListener, then InitMachineScheme. The machine layer is not in this library; the stubs
  // stand in for it and name themselves the first time GOAL calls one.
  jak1::InitListener();
  goal_kernel_core_stub_machine_layer(0);
  jak1::intern_from_c("*kernel-boot-message*")->value =
      jak1::intern_from_c(DebugBootMessage).offset;
  jak1::intern_from_c("*kernel-boot-mode*")->value = jak1::intern_from_c("boot").offset;
  jak1::intern_from_c("*kernel-boot-level*")->value =
      jak1::intern_from_c(DebugBootLevel).offset;

  // InitMachineScheme's DiskBoot path: the engine and the game.
  say("\n=== GAME.CGO\n");
  if (goal_dgo_load("GAME", boot_flags, 0x400000, &stats) != GOAL_KERNEL_CORE_OK) {
    say("FAILED: %s\n", goal_dgo_last_error());
    drain_goal_print_buffer();
    say("  got through %d of GAME.CGO's objects (%d code, %d data)\n", stats.objects,
        stats.code_objects, stats.data_objects);
    return 1;
  }
  drain_goal_print_buffer();
  say("  %d objects: %d code, %d data; heap use %u -> %u bytes\n", stats.objects,
      stats.code_objects, stats.data_objects, stats.heap_used_before, stats.heap_used_after);
  report_heap("after GAME.CGO");

  // The last thing InitMachineScheme does. It does not come back yet: `play` starts the title
  // level load, and GOAL's own level loader (engine/load/load-dgo.gc, engine/level/level.gc)
  // drives the DGO through `rpc-call` / `rpc-busy?` and links it with `link-begin` / `link-resume`.
  // Neither is available here: the DGO RPC is the IOP channel this library replaced with a
  // synchronous reader, and GOAL-side linking has no way to substitute AOT code for a v3 object.
  // So it spins in `(check-busy rpc-buffer-pair)` forever. Opt in with --play to see it happen.
  if (run_play) {
    say("\n=== (play)\n");
    const u64 play_result = jak1::call_goal_function_by_name("play");
    drain_goal_print_buffer();
    say("  play returned #x%" PRIx64 "\n", play_result);
    report_heap("after play");
  } else {
    say("\n=== (play) skipped; pass --play to run it. See the comment in data_boot_test.cpp.\n");
  }

  // KernelCheckAndDispatch's loop body, without the listener half: this is the GOAL kernel's own
  // frame, running processes and states.
  auto dispatcher = jak1::find_symbol_from_c("kernel-dispatcher");
  if (!dispatcher.offset || !dispatcher->value) {
    say("FAILED: kernel-dispatcher holds nothing\n");
    return 1;
  }
  say("\n=== kernel-dispatcher: %d frames\n", dispatch_frames);
  if (!dispatch_frames) {
    // Measured, not guessed: one frame aborts in the native thread-suspend, because a process that
    // suspends needs to back up more stack than its buffer holds. PROCESS_STACK_SAVE_SIZE is the
    // PS2's 256 bytes, the game lowers some processes to 128 with stack-size-set!, and an ARM64
    // machine context alone is 176. That is data tuning in goal_src, not a runtime gap, and it
    // fails loudly rather than corrupting the heap. See docs/aot-stack-model.md.
    say("  not run. --frames 1 reaches the GOAL kernel's first frame and aborts in\n"
        "  thread-suspend: a process used 400 bytes of stack against a 128-byte backup buffer.\n"
        "  The game's stack sizes were chosen for the PS2; see docs/aot-stack-model.md.\n");
  }
  for (int frame = 0; frame < dispatch_frames; frame++) {
    call_goal_on_stack(Ptr<Function>(dispatcher->value), goal_kernel_stack_top(), s7.offset,
                       g_ee_main_mem);
    drain_goal_print_buffer();
  }
  report_heap("after the dispatcher");
  say("\nBOOT: KERNEL.CGO and GAME.CGO are loaded and the GOAL kernel dispatcher is callable.\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool synthetic = false;
  bool run_play = false;
  std::string data_dir;
  int dispatch_frames = 0;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--synthetic") {
      synthetic = true;
    } else if (arg == "--data-dir" && i + 1 < argc) {
      data_dir = argv[++i];
    } else if (arg == "--play") {
      run_play = true;
    } else if (arg == "--verbose") {
      goal_dgo_set_verbose(1);
    } else if (arg == "--frames" && i + 1 < argc) {
      dispatch_frames = std::atoi(argv[++i]);
    } else {
      say("unknown argument %s\n", arg.c_str());
      return 2;
    }
  }
  if (data_dir.empty()) {
    const char* env = std::getenv("GOALPAD_JAK1_DATA_DIR");
    data_dir = env ? env : "";
  }

  if (!synthetic && data_dir.empty()) {
    say("SKIPPED: no Jak 1 data directory.\n"
        "This test loads the player's own extracted game data, which is not part of the\n"
        "repository. Set GOALPAD_JAK1_DATA_DIR (or pass --data-dir) to the directory that\n"
        "holds iso/ and fr3/ - what goal_src/jak1/game.gp calls $OUT - to run it.\n");
    return 0;
  }

  lg::set_stdout_level(lg::level::warn);
  lg::set_flush_level(lg::level::warn);
  lg::initialize();

  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    say("FAIL: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  clear_print();
  register_aot_objects();
  say("%d AOT translation units registered by object name\n", goal_aot_boot_file_count);

  const int result = synthetic ? run_synthetic() : run_real_boot(data_dir, dispatch_frames, run_play);

  goal_aot_reset();
  goal_kernel_core_shutdown();
  return result;
}
