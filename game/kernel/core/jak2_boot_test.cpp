/*!
 * @file jak2_boot_test.cpp
 * Boot the Jak 2 GOAL kernel the way the game boots it: KERNEL.CGO out of the player's own data,
 * in DGO order, with the code of every object supplied by the AOT path and the data of every
 * object read off the disc, then the GOAL kernel dispatcher frame after frame.
 *
 * This is the jak2 analogue of data_boot_test.cpp's first act and is a progress probe: it reports
 * how far the boot got and what stopped it. Nothing here may skip a step to get further. The
 * machine layer is the loudly-failing stub set - every machine function GOAL touches is named on
 * stdout - so a run that passes is measuring the kernel, not claiming the game works.
 *
 * Needs a data directory, given by --data-dir or GOALPAD_JAK2_DATA_DIR, and reports that it was
 * skipped when there is none. --with-game goes on to attempt GAME.CGO and reports honestly where
 * that stops; it is an exploration flag, not a passing test.
 */

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

extern "C" {
#include "aot_boot_manifest.h"
}

#include "common/goal_constants.h"
#include "common/link_types.h"
#include "common/log/log.h"

#include "game/kernel/common/kboot.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/common/kernel_types.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/dgo_loader.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/sound_rpc_jak2.h"
#include "game/kernel/jak2/klisten.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/mips2c/mips2c_table.h"
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
 * Behavioral check of one registered Jak 2 mips2c function, through the real seam path: the name
 * lookup GOAL's `__pc-get-mips2c` uses, the trampoline function object, the mips2c scratch stack
 * and the ExecutionContext. `adgif-shader<-texture-with-update!` is the first mips2c function
 * GAME.CGO asks for (the `texture` object's top-level), and it is a pure transform: it packs a
 * texture's fields into the shader's GS TEX0/TEX1/MIPTBP1 registers. The expected words below are
 * that packing computed by hand from the GS register layout for one synthetic texture.
 */
int check_adgif_shader_mips2c() {
  const u32 fn = Mips2C::gLinkedFunctionTable.get("adgif-shader<-texture-with-update!");

  // the texture fields the function reads, at the offsets the translated code uses:
  // a 16x16 PSMCT32 texture with one mip, uv-dist 1.0, and distinct vram addresses per level
  auto tex = kmalloc(kglobalheap, 48, KMALLOC_MEMSET, "adgif-check-texture");
  auto shader = kmalloc(kglobalheap, 96, KMALLOC_MEMSET, "adgif-check-shader");
  if (!tex.offset || !shader.offset) {
    say("FAILED: no room for the adgif check buffers\n");
    return 1;
  }
  u8* t = tex.c();
  const u16 w = 16, h = 16;
  const u16 dests[7] = {0x100, 0x120, 0x140, 0x160, 0x180, 0x1a0, 0x1c0};
  const u16 clutdest = 0x1e0;
  const u8 widths[7] = {2, 1, 1, 1, 1, 1, 1};
  const float uv_dist = 1.0f;
  memcpy(t + 0, &w, 2);
  memcpy(t + 2, &h, 2);
  t[4] = 1;  // num-mips
  t[5] = 0;  // tex1-control
  t[6] = 0;  // psm = PSMCT32
  t[7] = 0;  // mip-shift
  memcpy(t + 10, dests, sizeof(dests));
  memcpy(t + 24, &clutdest, 2);
  memcpy(t + 26, widths, sizeof(widths));
  memcpy(t + 44, &uv_dist, 4);

  const u64 result =
      call_goal(Ptr<Function>(fn), shader.offset, tex.offset, 0, s7.offset, g_ee_main_mem);

  // TEX0: CLD=1 | CBP=0x1e0 | TCC=1 | TH=log2(16) | TW=log2(16) | TBW=2 | TBP0=0x100
  // TEX1: K = 16*(log2(256/uv-dist)) - 175 = -47, as a signed 12-bit field at bit 32
  // MIPTBP1: (0x160,1) (0x140,1) (0x120,1)
  struct Expected {
    int offset;
    u64 value;
    const char* what;
  } expected[] = {
      {0, 0x20003C0510008100ull, "TEX0"},
      {16, 0x00000FD100000000ull, "TEX1"},
      {32, 0x0041600414004120ull, "MIPTBP1"},
      {48, 0, "MIPTBP2 (untouched: one mip)"},
      {64, 0, "CLAMP (untouched: one mip)"},
  };
  int failures = 0;
  for (const auto& e : expected) {
    u64 got;
    memcpy(&got, shader.c() + e.offset, 8);
    if (got != e.value) {
      say("FAILED: adgif %s: got #x%016llx, expected #x%016llx\n", e.what,
          (unsigned long long)got, (unsigned long long)e.value);
      failures++;
    }
  }
  if (result != shader.offset) {
    say("FAILED: adgif-shader<-texture-with-update! returned #x%llx, not the shader #x%x\n",
        (unsigned long long)result, shader.offset);
    failures++;
  }
  if (!failures) {
    say("  adgif-shader<-texture-with-update! packed TEX0/TEX1/MIPTBP1 correctly\n");
  }
  return failures;
}

/*! Tell the DGO loader which native translation unit stands in for each object it will meet. */
void register_aot_objects() {
  for (int i = 0; i < goal_aot_boot_file_count; i++) {
    const auto& entry = goal_aot_boot_files[i];
    goal_aot_object_file file = {entry.tag,       entry.statics,         *entry.static_count,
                                 entry.functions, *entry.function_count, entry.link};
    goal_aot_register_object(object_name_of(entry.source).c_str(), &file);
  }
}

int run_boot(const std::string& data_dir, int dispatch_frames, bool with_game) {
  goal_kernel_core_set_data_directory(data_dir.c_str());
  say("data directory: %s\n", data_dir.c_str());

  // keep the harness hermetic: saves go to a scratch directory, never to the player's own
  const std::string saves_dir =
      (std::filesystem::temp_directory_path() / "goalpad-jak2-boot-saves").string();
  std::filesystem::remove_all(saves_dir);
  goal_kernel_core_set_saves_directory(saves_dir.c_str());

  goal_dgo_load_stats stats;
  const u32 boot_flags = LINK_FLAG_OUTPUT_LOAD | LINK_FLAG_EXECUTE | LINK_FLAG_PRINT_LOGIN;

  // InitHeapAndSymbol's kernel load
  say("\n=== KERNEL.CGO\n");
  if (goal_dgo_load("KERNEL", boot_flags, 0x400000, &stats) != GOAL_KERNEL_CORE_OK) {
    say("FAILED: %s\n", goal_dgo_last_error());
    drain_goal_print_buffer();
    say("  got through %d of KERNEL.CGO's objects (%d code, %d data)\n", stats.objects,
        stats.code_objects, stats.data_objects);
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

  say("\n=== mips2c seam\n");
  if (check_adgif_shader_mips2c() != 0) {
    return 1;
  }

  // InitListener, then InitMachineScheme: the machine layer is not in this library, so the stubs
  // stand in for it and name themselves the first time GOAL calls one.
  jak2::InitListener();
  if (goal_kernel_core_stub_machine_layer(0) != GOAL_KERNEL_CORE_OK ||
      goal_jak2_sound_rpc_install() != GOAL_KERNEL_CORE_OK) {
    say("FAILED: could not install the Jak 2 machine seams\n");
    return 1;
  }

  if (with_game) {
    say("\n=== GAME.CGO (exploratory; expected to stop at the first missing subsystem)\n");
    if (goal_dgo_load("GAME", boot_flags, 0x400000, &stats) != GOAL_KERNEL_CORE_OK) {
      say("STOPPED: %s\n", goal_dgo_last_error());
      drain_goal_print_buffer();
      say("  got through %d of GAME.CGO's objects (%d code, %d data)\n", stats.objects,
          stats.code_objects, stats.data_objects);
      return 1;
    }
    drain_goal_print_buffer();
    say("  %d objects: %d code, %d data; heap use %u -> %u bytes\n", stats.objects,
        stats.code_objects, stats.data_objects, stats.heap_used_before, stats.heap_used_after);
    report_heap("after GAME.CGO");

    goal_jak2_sound_rpc_stats sound_stats;
    goal_jak2_sound_rpc_stats_get(&sound_stats);
    uint32_t sound_info = 0;
    goal_kernel_core_lookup("*sound-iop-info*", nullptr, &sound_info);
    if (sound_stats.version_requests != 1 || !sound_stats.info_ee ||
        sound_stats.info_ee != sound_info) {
      say("FAILED: Jak 2 sound handshake: requests=%u info=#x%x symbol=#x%x\n",
          sound_stats.version_requests, sound_stats.info_ee, sound_info);
      return 1;
    }
    say("  sound loader answered IRX 4.0 and retained info block #x%x\n",
        sound_stats.info_ee);
  }

  // KernelCheckAndDispatch's loop body, without the listener half: the GOAL kernel's own frame,
  // running processes and states.
  uint32_t dispatcher = 0;
  if (goal_kernel_core_lookup("kernel-dispatcher", nullptr, &dispatcher) != GOAL_KERNEL_CORE_OK ||
      !dispatcher) {
    say("FAILED: kernel-dispatcher holds nothing\n");
    return 1;
  }
  say("\n=== kernel-dispatcher: %d frames\n", dispatch_frames);
  for (int frame = 1; frame <= dispatch_frames; frame++) {
    call_goal_on_stack(Ptr<Function>(dispatcher), goal_kernel_stack_top(), s7.offset,
                       g_ee_main_mem);
    drain_goal_print_buffer();
  }
  report_heap("after the dispatcher");

  if (with_game) {
    goal_jak2_sound_rpc_stats sound_stats;
    goal_jak2_sound_rpc_stats_get(&sound_stats);
    if (sound_stats.str_requests != 1 || sound_stats.str_reads != 1 ||
        sound_stats.str_failures != 0 || !sound_stats.str_bytes) {
      say("FAILED: Jak 2 ordinary STR load: requests=%u reads=%u failures=%u bytes=%u\n",
          sound_stats.str_requests, sound_stats.str_reads, sound_stats.str_failures,
          sound_stats.str_bytes);
      return 1;
    }
    say("  ordinary STR loader completed one %u-byte file read\n", sound_stats.str_bytes);
  }

  say("\nBOOT: KERNEL.CGO is loaded and the Jak 2 GOAL kernel dispatcher ran %d frames.\n",
      dispatch_frames);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string data_dir;
  int dispatch_frames = 100;
  bool with_game = false;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--data-dir" && i + 1 < argc) {
      data_dir = argv[++i];
    } else if (arg == "--frames" && i + 1 < argc) {
      dispatch_frames = std::atoi(argv[++i]);
    } else if (arg == "--verbose") {
      goal_dgo_set_verbose(1);
    } else if (arg == "--with-game") {
      with_game = true;
    } else {
      std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
      return 2;
    }
  }
  if (data_dir.empty()) {
    const char* env = std::getenv("GOALPAD_JAK2_DATA_DIR");
    data_dir = env ? env : "";
  }
  if (data_dir.empty()) {
    std::printf(
        "SKIPPED: no Jak 2 data directory.\n"
        "This test loads the player's own extracted game data, which is not part of the\n"
        "repository. Set GOALPAD_JAK2_DATA_DIR (or pass --data-dir) to the directory that\n"
        "holds iso/ - what goal_src/jak2/game.gp calls $OUT - to run it.\n");
    return 0;
  }

  lg::set_stdout_level(lg::level::warn);
  lg::set_flush_level(lg::level::warn);
  lg::initialize();

  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  clear_print();
  register_aot_objects();
  std::printf("%d AOT translation units registered by object name\n", goal_aot_boot_file_count);

  const int result = run_boot(data_dir, dispatch_frames, with_game);

  goal_aot_reset();
  goal_kernel_core_shutdown();
  return result;
}
