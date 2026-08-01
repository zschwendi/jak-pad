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
#include "game/kernel/core/dma_capture.h"
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

/*!
 * `(method update-vis! level)` in engine/load/decomp.gc checks its own decompressed visibility
 * against the BSP's all-visible list and prints this when a bit names a drawable that does not
 * exist. Nothing stops when it happens - the game only reports it - so the report is counted here
 * and failed on at the end. It is the one signal that says the visibility a frame computed is
 * wrong.
 */
int g_illegal_vis_reports = 0;

/*! Anything GOAL printed since the last call, so a failing top-level's own message is visible. */
void drain_goal_print_buffer() {
  const char* printed = Ptr<char>(PrintBufArea.offset + sizeof(ListenerMessageHeader)).c();
  if (printed[0]) {
    for (const char* at = printed; (at = std::strstr(at, "illegal vis bits set")) != nullptr; at++) {
      g_illegal_vis_reports++;
    }
    say("GOAL said: %s\n", printed);
    clear_print();
  }
}

/*!
 * How close the run came to overflowing a GOAL thread's backup stack. The sizes come from
 * `process-stack-save-size` in kernel/gkernel-h.gc, which converts the game's PS2 measurements for
 * this build's code generator; this says whether that conversion is generous or nearly wrong.
 */
void report_stack_watermark() {
  goal_thread_stack_watermark_report w;
  goal_thread_stack_watermark(&w);
  if (!w.suspends) {
    return;
  }
  say("  backup stacks: %d suspends, deepest %d bytes of %d ('%s), fullest %d of %d (%d%%, '%s)\n",
      w.suspends, w.deepest_used, w.deepest_size, w.deepest_name, w.fullest_used, w.fullest_size,
      w.fullest_size ? 100 * w.fullest_used / w.fullest_size : 0, w.fullest_name);
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

/*!
 * GAME.CGO is the release build's union of ENGINE.CGO, ART.CGO and COMMON.CGO, which do not exist
 * as separate files on the disc. The C kernel records that by putting their names in
 * `*kernel-packages*` after loading GAME.CGO (jak1::InitMachineScheme in
 * game/kernel/jak1/kmachine.cpp), so `alloc-levels!`'s `(load-package "art" global)` finds "art"
 * already loaded and asks for nothing. Without this the level system tries to load ART.CGO and
 * fails, because there is no such file to load.
 */
void record_packages_in_game_cgo() {
  using namespace jak1_symbols;
  for (const char* package : {"engine", "art", "common"}) {
    jak1::kernel_packages->value =
        jak1::new_pair(s7.offset + FIX_SYM_GLOBAL_HEAP, *((s7 + FIX_SYM_PAIR_TYPE).cast<u32>()),
                       jak1::make_string_from_c(package), jak1::kernel_packages->value);
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
int run_real_boot(const std::string& data_dir,
                  int dispatch_frames,
                  bool run_play,
                  const std::string& dma_capture_path) {
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
  // stand in for it and name themselves the first time GOAL calls one. The DGO RPC and the two
  // linker entry points GOAL's own level loader drives are real, and replace the stubs.
  jak1::InitListener();
  goal_kernel_core_stub_machine_layer(0);
  goal_dgo_install_goal_loader();
  goal_gfx_dma_install();
  goal_gfx_dma_set_capture_path(dma_capture_path.c_str());
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
  record_packages_in_game_cgo();

  // The last thing InitMachineScheme does. `play` allocates the level heaps and drives the whole
  // title-level load itself: with no display process running yet its `while` loop calls
  // `load-continue` until the level reaches 'active, which is what needs the DGO RPC and
  // `link-begin` above.
  if (run_play) {
    say("\n=== (play)\n");
    const u64 play_result = jak1::call_goal_function_by_name("play");
    drain_goal_print_buffer();
    say("  play returned #x%" PRIx64 "\n", play_result);
    report_heap("after play");
  } else {
    say("\n=== (play) skipped; pass --play to run it.\n");
  }

  // KernelCheckAndDispatch's loop body, without the listener half: this is the GOAL kernel's own
  // frame, running processes and states.
  auto dispatcher = jak1::find_symbol_from_c("kernel-dispatcher");
  if (!dispatcher.offset || !dispatcher->value) {
    say("FAILED: kernel-dispatcher holds nothing\n");
    return 1;
  }
  say("\n=== kernel-dispatcher: %d frames\n", dispatch_frames);
  for (int frame = 0; frame < dispatch_frames; frame++) {
    call_goal_on_stack(Ptr<Function>(dispatcher->value), goal_kernel_stack_top(), s7.offset,
                       g_ee_main_mem);
    drain_goal_print_buffer();
  }
  report_heap("after the dispatcher");
  report_stack_watermark();

  goal_gfx_dma_stats dma;
  goal_gfx_dma_get_stats(&dma);
  say("  DMA: %d chains built, largest %u bytes, last %u bytes%s. Nothing was drawn: there is no\n"
      "  renderer here, so the chains are followed, measured and dropped.\n",
      dma.chains, dma.largest_bytes, dma.last_bytes,
      dma.captured_bytes ? " (one captured)" : "");

  goal_dgo_rpc_stats rpc;
  goal_dgo_goal_loader_stats(&rpc);
  say("  GOAL's own loader: %d DGO loads, %d objects (%d code from the AOT path, %d data linked),"
      " %d STR reads, %d STR misses\n",
      rpc.dgo_archives, rpc.dgo_objects, rpc.linked_code_objects, rpc.linked_data_objects,
      rpc.str_reads, rpc.str_failures);
  say("  visibility: %d .VIS files in the ramdisk, %d vis strings read, %d misses,"
      " %d illegal-vis reports\n",
      rpc.ramdisk_files, rpc.ramdisk_reads, rpc.ramdisk_misses, g_illegal_vis_reports);

  int failures = 0;
  auto expect = [&](bool ok, const char* what) {
    say("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
      failures++;
    }
  };
  if (run_play) {
    // `play` cannot finish without GOAL's own loader having read a level out of a DGO through the
    // RPC and linked both halves of it, so these are the shape of what it did, not a restatement
    // of "it did not crash".
    expect(rpc.dgo_archives > 0, "GOAL started a level DGO load through the RPC");
    expect(rpc.dgo_objects >= rpc.linked_code_objects + rpc.linked_data_objects,
           "every object GOAL linked came from the RPC");
    expect(rpc.linked_code_objects > 0, "GOAL linked level code through the AOT path");
    expect(rpc.linked_data_objects > 0, "GOAL linked level data through the real linker");
  }
  if (dispatch_frames > 0) {
    goal_thread_stack_watermark_report w;
    goal_thread_stack_watermark(&w);
    expect(w.suspends > 0, "processes suspended and resumed across the frames");
    expect(w.fullest_used <= w.fullest_size, "no backup stack was overrun");
    if (run_play) {
      // A level's own visibility comes out of its .VIS file through the ramdisk RPC, and
      // `update-vis!` checks its own decompressed output. Both halves have to hold: no read may
      // fail, and no swap may produce a bit for a drawable the BSP does not have.
      expect(rpc.ramdisk_reads > 0 && rpc.ramdisk_misses == 0,
             "every vis string a level asked for was read");
      expect(g_illegal_vis_reports == 0, "every visibility swap decompressed to legal bits");
    }
    if (run_play) {
      // Following a chain means reading every tag in it, so a chain that came back with a size is
      // a chain that was well-formed.
      expect(dma.chains > 0 && dma.largest_bytes > 0, "the frames built real DMA chains");
    }
    if (!dma_capture_path.empty()) {
      expect(dma.captured_bytes > 0, "a DMA chain was written to the capture file");
    }
  }

  say("\nBOOT: KERNEL.CGO and GAME.CGO are loaded and the GOAL kernel dispatcher ran %d frames.\n",
      dispatch_frames);
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool synthetic = false;
  bool run_play = false;
  std::string data_dir;
  std::string dma_capture_path;
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
    } else if (arg == "--capture-dma" && i + 1 < argc) {
      dma_capture_path = argv[++i];
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

  const int result = synthetic ? run_synthetic()
                               : run_real_boot(data_dir, dispatch_frames, run_play,
                                               dma_capture_path);

  goal_aot_reset();
  goal_kernel_core_shutdown();
  return result;
}
