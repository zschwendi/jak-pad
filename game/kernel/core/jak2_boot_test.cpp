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
 * skipped when there is none. --with-game goes on to attempt GAME.CGO. --play implies
 * --with-game and calls the real Jak 2 `play-boot`, then dispatches the GOAL process it creates
 * only far enough to report the first title-level DGO request and the next missing subsystem.
 * --play-dma preserves that mode and additionally waits for one valid 327-bucket graphics-DMA
 * chain while proving that the same chain crossed the complete graphics-host boundary.
 * --play-gfx-host isolates that host proof without parsing DMA. Both modes count, pace
 * synthetically, and drop graphics work: neither is a renderer.
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
#include "common/symbols.h"

#include "game/kernel/common/kboot.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/common/kernel_types.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/dgo_loader.h"
#include "game/kernel/core/dma_capture.h"
#include "game/kernel/core/gfx_host.h"
#include "game/kernel/core/jak2_runtime.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/pad.h"
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

/*!
 * GAME.CGO contains the release builds of ENGINE.CGO, ART.CGO and COMMON.CGO, so those archives
 * do not exist separately in the prepared data. Mirror the three entries that Jak 2's real
 * InitMachineScheme adds after loading GAME.CGO. This keeps `load-package` from requesting them.
 */
void record_packages_in_game_cgo() {
  using namespace jak2_symbols;
  for (const char* package : {"engine", "art", "common"}) {
    jak2::kernel_packages->value() =
        jak2::new_pair(s7.offset + FIX_SYM_GLOBAL_HEAP,
                       *((s7 + FIX_SYM_PAIR_TYPE - 1).cast<u32>()),
                       jak2::make_string_from_c(package), jak2::kernel_packages->value());
  }
}

struct Jak2DmaWindow {
  int chains = 0;
  int well_formed = 0;
  int malformed = 0;
  bool accounting_complete = false;
  bool found_valid = false;
  goal_gfx_dma_frame_summary valid = {};
};

Jak2DmaWindow audit_jak2_dma_window(const goal_gfx_dma_stats& before,
                                    const goal_gfx_dma_stats& after) {
  Jak2DmaWindow out;
  out.chains = after.chains - before.chains;
  out.well_formed = after.well_formed_chains - before.well_formed_chains;
  out.malformed = after.malformed_chains - before.malformed_chains;
  if (out.chains < 0 || out.well_formed < 0 || out.malformed < 0 ||
      out.chains != out.well_formed + out.malformed) {
    return out;
  }

  int recorded_well_formed = 0;
  int recorded_malformed = 0;
  for (int frame = before.chains + 1; frame <= after.chains; frame++) {
    goal_gfx_dma_frame_summary candidate = {};
    if (!goal_gfx_dma_get_frame(frame, &candidate) || candidate.frame != frame) {
      return out;
    }
    if (candidate.well_formed) {
      recorded_well_formed++;
      if (!out.found_valid && candidate.buckets == 327 && candidate.tags > 0 &&
          candidate.copied_bytes > 0) {
        out.valid = candidate;
        out.found_valid = true;
      }
    } else {
      recorded_malformed++;
    }
  }
  out.accounting_complete = recorded_well_formed == out.well_formed &&
                            recorded_malformed == out.malformed;
  return out;
}

struct HeadlessGfxHostStats {
  int chains = 0;
  int vsyncs = 0;
  int sync_paths = 0;
  int texture_uploads = 0;
  int texture_relocations = 0;
  int desired_level_calls = 0;
  int active_level_calls = 0;
  int pmode_calls = 0;
  int last_desired_level_count = 0;
  int last_active_level_count = 0;
  float last_pmode_alpha = 0.f;
};

HeadlessGfxHostStats g_headless_gfx;

void headless_send_chain(const void* ee_base, uint32_t chain_offset) {
  (void)ee_base;
  (void)chain_offset;
  g_headless_gfx.chains++;
}

void headless_observe_send_chain(const void* ee_base, uint32_t chain_offset) {
  headless_send_chain(ee_base, chain_offset);
  goal_gfx_dma_observe_chain(ee_base, chain_offset);
}

uint32_t headless_vsync() {
  g_headless_gfx.vsyncs++;
  return (g_headless_gfx.vsyncs & 1) ? 1 : 0;
}

uint32_t headless_sync_path() {
  g_headless_gfx.sync_paths++;
  return 0;
}

void headless_texture_upload(const uint8_t* tpage, int mode, uint32_t s7_ptr) {
  (void)tpage;
  (void)mode;
  (void)s7_ptr;
  g_headless_gfx.texture_uploads++;
}

void headless_texture_relocate(uint32_t dst, uint32_t src, uint32_t format) {
  (void)dst;
  (void)src;
  (void)format;
  g_headless_gfx.texture_relocations++;
}

void headless_set_desired_levels(const char* const* names, int count) {
  (void)names;
  g_headless_gfx.desired_level_calls++;
  g_headless_gfx.last_desired_level_count = count;
}

void headless_set_active_levels(const char* const* names, int count) {
  (void)names;
  g_headless_gfx.active_level_calls++;
  g_headless_gfx.last_active_level_count = count;
}

void headless_set_pmode_alpha(float alpha) {
  g_headless_gfx.pmode_calls++;
  g_headless_gfx.last_pmode_alpha = alpha;
}

HeadlessGfxHostStats headless_gfx_delta(const HeadlessGfxHostStats& before,
                                        const HeadlessGfxHostStats& after) {
  HeadlessGfxHostStats out;
  out.chains = after.chains - before.chains;
  out.vsyncs = after.vsyncs - before.vsyncs;
  out.sync_paths = after.sync_paths - before.sync_paths;
  out.texture_uploads = after.texture_uploads - before.texture_uploads;
  out.texture_relocations = after.texture_relocations - before.texture_relocations;
  out.desired_level_calls = after.desired_level_calls - before.desired_level_calls;
  out.active_level_calls = after.active_level_calls - before.active_level_calls;
  out.pmode_calls = after.pmode_calls - before.pmode_calls;
  if (out.desired_level_calls > 0) {
    out.last_desired_level_count = after.last_desired_level_count;
  }
  if (out.active_level_calls > 0) {
    out.last_active_level_count = after.last_active_level_count;
  }
  if (out.pmode_calls > 0) {
    out.last_pmode_alpha = after.last_pmode_alpha;
  }
  return out;
}

bool headless_gfx_frontier_reached(const HeadlessGfxHostStats& window) {
  return window.chains > 0 && window.vsyncs > 0 && window.sync_paths > 0;
}

int run_boot(const std::string& data_dir,
             int dispatch_frames,
             bool with_game,
             bool run_play,
             bool run_play_dma,
             bool run_play_gfx_host) {
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
      goal_pad_install() != GOAL_KERNEL_CORE_OK ||
      goal_jak2_sound_rpc_install() != GOAL_KERNEL_CORE_OK) {
    say("FAILED: could not install the Jak 2 machine seams\n");
    return 1;
  }
  // Sound owns channels 0, 1 and 4. Install the composed DGO router after it so channel 3 is
  // available without replacing those handlers.
  goal_dgo_install_goal_loader();
  if (run_play_dma || run_play_gfx_host) {
    g_headless_gfx = {};
    if (run_play_dma) {
      goal_gfx_dma_reset();
    }
    goal_gfx_host host = {};
    host.send_chain = run_play_dma ? headless_observe_send_chain : headless_send_chain;
    host.vsync = headless_vsync;
    host.sync_path = headless_sync_path;
    host.texture_upload_now = headless_texture_upload;
    host.texture_relocate = headless_texture_relocate;
    host.set_levels = headless_set_desired_levels;
    host.set_pmode_alp = headless_set_pmode_alpha;
    host.set_active_levels = headless_set_active_levels;
    if (goal_gfx_host_install(&host) != GOAL_KERNEL_CORE_OK) {
      say("FAILED: could not install the headless Jak 2 graphics host\n");
      return 1;
    }
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
    record_packages_in_game_cgo();
    say("  recorded engine, art and common in *kernel-packages*\n");

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

  if (run_play) {
    uint32_t play_boot = 0;
    uint32_t dispatcher = 0;
    if (goal_kernel_core_lookup("play-boot", nullptr, &play_boot) != GOAL_KERNEL_CORE_OK ||
        !play_boot ||
        goal_kernel_core_lookup("kernel-dispatcher", nullptr, &dispatcher) != GOAL_KERNEL_CORE_OK ||
        !dispatcher) {
      say("FAILED: Jak 2 play-boot or kernel-dispatcher holds nothing\n");
      return 1;
    }

    say("\n=== play-boot frontier\n");
    goal_gfx_dma_stats dma_before = {};
    if (run_play_dma) {
      goal_gfx_dma_get_stats(&dma_before);
    }
    const bool run_gfx_host = run_play_dma || run_play_gfx_host;
    const HeadlessGfxHostStats gfx_before = g_headless_gfx;
    const u64 play_boot_result = jak2::call_goal_function_by_name("play-boot");
    drain_goal_print_buffer();

    goal_dgo_rpc_stats rpc = {};
    goal_gfx_dma_stats dma = {};
    Jak2DmaWindow dma_window = {};
    HeadlessGfxHostStats gfx_window = {};
    int frames_run = 0;
    while (frames_run < dispatch_frames) {
      call_goal_on_stack(Ptr<Function>(dispatcher), goal_kernel_stack_top(), s7.offset,
                         g_ee_main_mem);
      frames_run++;
      drain_goal_print_buffer();
      goal_dgo_goal_loader_stats(&rpc);
      const bool title_ready = std::strcmp(rpc.first_dgo_name, "TITLE.DGO") == 0 &&
                               rpc.dgo_archives >= 1 && rpc.dgo_objects >= 1 &&
                               rpc.linked_code_objects + rpc.linked_data_objects >= 1;
      if (run_play_dma) {
        goal_gfx_dma_get_stats(&dma);
        dma_window = audit_jak2_dma_window(dma_before, dma);
      }
      if (run_gfx_host) {
        gfx_window = headless_gfx_delta(gfx_before, g_headless_gfx);
      }
      if (run_play_dma && dma_window.malformed > 0) {
        break;
      }
      if (title_ready &&
          (!run_play_dma || (dma_window.accounting_complete && dma_window.found_valid)) &&
          (!run_gfx_host || headless_gfx_frontier_reached(gfx_window))) {
        break;
      }
    }

    say("  play-boot returned #x%llx; dispatched %d frame(s)\n",
        (unsigned long long)play_boot_result, frames_run);
    say("  channel 3 first request: %s; %d archive(s), %d object(s) "
        "(%d code from AOT, %d data linked)\n",
        rpc.first_dgo_name[0] ? rpc.first_dgo_name : "<none>", rpc.dgo_archives,
        rpc.dgo_objects, rpc.linked_code_objects, rpc.linked_data_objects);
    report_heap("after play-boot frontier");

    if (run_play_dma) {
      goal_gfx_dma_get_stats(&dma);
      dma_window = audit_jak2_dma_window(dma_before, dma);
      say("  graphics DMA after play-boot: %d chain(s), %d well formed, %d malformed; measured "
          "and dropped\n",
          dma_window.chains, dma_window.well_formed, dma_window.malformed);
      if (!dma_window.accounting_complete) {
        say("FAILED: --play-dma chain totals and per-frame records do not agree\n");
        return 1;
      }
      if (dma_window.malformed != 0) {
        say("FAILED: --play-dma observed %d malformed graphics-DMA chain(s) after its baseline\n",
            dma_window.malformed);
        return 1;
      }
    }
    if (run_gfx_host) {
      gfx_window = headless_gfx_delta(gfx_before, g_headless_gfx);
      say("  headless graphics host after play-boot: chains=%d syncv=%d sync-path=%d "
          "texture-upload=%d texture-relocate=%d desired-levels=%d (last %d) "
          "active-levels=%d (last %d) pmode=%d (last %.6f)\n",
          gfx_window.chains, gfx_window.vsyncs, gfx_window.sync_paths,
          gfx_window.texture_uploads, gfx_window.texture_relocations,
          gfx_window.desired_level_calls, gfx_window.last_desired_level_count,
          gfx_window.active_level_calls, gfx_window.last_active_level_count,
          gfx_window.pmode_calls, gfx_window.last_pmode_alpha);
      if (!headless_gfx_frontier_reached(gfx_window)) {
        say("FAILED: the headless graphics host requires at least one send-chain, syncv and "
            "sync-path callback after its baseline\n");
        return 1;
      }
      if (run_play_dma && gfx_window.chains != dma_window.chains) {
        say("FAILED: --play-dma host/DMA chain counts disagree: host=%d DMA=%d\n",
            gfx_window.chains, dma_window.chains);
        return 1;
      }
    }

    if (std::strcmp(rpc.first_dgo_name, "TITLE.DGO") != 0 || rpc.dgo_archives < 1 ||
        rpc.dgo_objects < 1 || rpc.linked_code_objects + rpc.linked_data_objects < 1 ||
        rpc.dgo_objects < rpc.linked_code_objects + rpc.linked_data_objects) {
      say("FAILED: play did not link a first title-level channel-3 object\n");
      return 1;
    }
    say("  proved: Jak 2 play reached TITLE.DGO through the composed channel-3 router\n");
    if (run_play_gfx_host) {
      say("  proved: the title frontier crossed the host graphics boundary: %d chain(s), "
          "%d syncv, %d sync-path\n",
          gfx_window.chains, gfx_window.vsyncs, gfx_window.sync_paths);
      say("STOPPED: --play-gfx-host counted and discarded graphics work. It has no renderer, "
          "drawn output or app loop, and level callbacks were observations rather than a "
          "success requirement.\n");
      return 0;
    }
    if (!run_play_dma) {
      say("STOPPED: the host probe ended after the first linked title object; it has no renderer "
          "or app loop, and any missing-machine "
          "reports above are the next unsupported frontier, not successful behavior.\n");
      return 0;
    }

    if (!dma_window.found_valid) {
      say("FAILED: --play-dma did not observe a valid 327-bucket graphics-DMA chain\n");
      return 1;
    }
    say("  proved: graphics DMA frame %d completed 327 buckets (%d tags, %u payload bytes, "
        "%u copied bytes)\n",
        dma_window.valid.frame, dma_window.valid.tags, dma_window.valid.payload_bytes,
        dma_window.valid.copied_bytes);
    say("  proved: the same %d chain(s) crossed send-chain, syncv and sync-path through the "
        "headless graphics host\n",
        gfx_window.chains);
    say("STOPPED: --play-dma synchronously measured and dropped the chain; rendered frames 0, "
        "presented frames 0. This validation host has no renderer or app loop.\n");
    return 0;
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
    if (!sound_stats.bank_requests || !sound_stats.banks_loaded || sound_stats.bank_failures != 0 ||
        sound_stats.bank_requests != sound_stats.banks_loaded + sound_stats.bank_reuses) {
      say("FAILED: Jak 2 checked bank load: requests=%u loaded=%u reused=%u failures=%u\n",
          sound_stats.bank_requests, sound_stats.banks_loaded, sound_stats.bank_reuses,
          sound_stats.bank_failures);
      return 1;
    }
    say("  checked bank loader completed %u load(s), %u reuse(s), zero failures\n",
        sound_stats.banks_loaded, sound_stats.bank_reuses);

    if (sound_stats.str_requests &&
        (sound_stats.str_requests != sound_stats.str_reads || sound_stats.str_failures != 0 ||
         !sound_stats.str_bytes)) {
      say("FAILED: Jak 2 ordinary STR load: requests=%u reads=%u failures=%u bytes=%u\n",
          sound_stats.str_requests, sound_stats.str_reads, sound_stats.str_failures,
          sound_stats.str_bytes);
      return 1;
    }
    say("  sound frontier: language=%u player batches=%u commands=%u failures=%u "
        "PLAY=%u started=%u updated=%u missing=%u STR=%u/%u\n",
        sound_stats.language_requests, sound_stats.player_batches, sound_stats.player_commands,
        sound_stats.player_failures, sound_stats.play_requests, sound_stats.sounds_started,
        sound_stats.sound_updates, sound_stats.sounds_missing, sound_stats.str_reads,
        sound_stats.str_requests);
  }

  say("\nBOOT: KERNEL.CGO is loaded and the Jak 2 GOAL kernel dispatcher ran %d frames.\n",
      dispatch_frames);
  return 0;
}

int run_play_runtime(const std::string& data_dir, int dispatch_frames, bool validate_dma) {
  const std::string saves_dir =
      (std::filesystem::temp_directory_path() / "goalpad-jak2-boot-saves").string();
  std::filesystem::remove_all(saves_dir);

  goal_jak2_runtime_config config = {};
  config.data_directory = data_dir.c_str();
  config.saves_directory = saves_dir.c_str();
  config.graphics = validate_dma ? GOAL_JAK2_RUNTIME_GRAPHICS_DMA_VALIDATION
                                 : GOAL_JAK2_RUNTIME_GRAPHICS_STUBS;

  struct RuntimeShutdown {
    ~RuntimeShutdown() {
      goal_jak2_runtime_shutdown();
    }
  } shutdown;

  say("%d AOT translation units registered by object name\n", goal_aot_boot_file_count);
  say("data directory: %s\n", data_dir.c_str());
  say("\n=== reusable play-boot runtime\n");
  if (goal_jak2_runtime_start(&config) != GOAL_JAK2_RUNTIME_OK) {
    say("FAILED: %s\n", goal_jak2_runtime_last_error());
    return 1;
  }

  goal_jak2_runtime_metrics metrics = {};
  goal_jak2_runtime_get_metrics(&metrics);
  say("  KERNEL.CGO: %d objects; GAME.CGO: %d objects (%d code, %d data)\n",
      metrics.kernel_objects, metrics.game_objects, metrics.game_code_objects,
      metrics.game_data_objects);
  say("  GOAL kernel version %u.%u; sound info #x%x\n", metrics.kernel_version >> 0x13,
      (metrics.kernel_version >> 3) & 0xffff, metrics.sound_info_ee);

  goal_jak2_runtime_status tick_status = GOAL_JAK2_RUNTIME_OK;
  while (metrics.ticks < static_cast<uint64_t>(dispatch_frames)) {
    tick_status = goal_jak2_runtime_tick();
    if (tick_status != GOAL_JAK2_RUNTIME_OK && tick_status != GOAL_JAK2_RUNTIME_EXITED) {
      say("FAILED: %s\n", goal_jak2_runtime_last_error());
      return 1;
    }
    goal_jak2_runtime_get_metrics(&metrics);
    if (validate_dma && metrics.dma_malformed > 0) {
      break;
    }
    if (metrics.title_ready &&
        (!validate_dma || (metrics.dma_accounting_complete && metrics.dma_found_valid))) {
      break;
    }
    if (tick_status == GOAL_JAK2_RUNTIME_EXITED) {
      break;
    }
  }

  say("  play-boot returned #x%llx; dispatched %llu frame(s)\n",
      (unsigned long long)metrics.play_boot_result, (unsigned long long)metrics.ticks);
  say("  channel 3 first request: %s; %d archive(s), %d object(s) "
      "(%d code from AOT, %d data linked)\n",
      metrics.first_dgo_name[0] ? metrics.first_dgo_name : "<none>", metrics.dgo_archives,
      metrics.dgo_objects, metrics.dgo_code_objects, metrics.dgo_data_objects);
  report_heap("after play-boot frontier");

  if (validate_dma) {
    say("  graphics DMA after play-boot: %d chain(s), %d well formed, %d malformed; measured "
        "and dropped\n",
        metrics.dma_chains, metrics.dma_well_formed, metrics.dma_malformed);
    say("  validation graphics host: %d chain(s), %d sync-path call(s), %d syncv call(s); "
        "callback external waits 0, rendered frames 0, presented frames 0\n",
        metrics.host_chains, metrics.host_sync_paths, metrics.host_syncvs);
    if (!metrics.dma_accounting_complete) {
      say("FAILED: --play-dma chain totals and per-frame records do not agree\n");
      return 1;
    }
    if (metrics.host_chains != metrics.dma_chains || metrics.host_sync_paths <= 0 ||
        metrics.host_syncvs <= 0) {
      say("FAILED: --play-dma did not exercise synchronous DMA, sync-path, and syncv through "
          "the validation graphics host\n");
      return 1;
    }
    if (metrics.dma_malformed != 0) {
      say("FAILED: --play-dma observed %d malformed graphics-DMA chain(s) after its baseline\n",
          metrics.dma_malformed);
      return 1;
    }
  }

  if (!metrics.title_ready || metrics.dgo_objects < metrics.dgo_code_objects +
                                                       metrics.dgo_data_objects) {
    say("FAILED: play did not link a first title-level channel-3 object\n");
    return 1;
  }
  say("  proved: Jak 2 play reached TITLE.DGO through the composed channel-3 router\n");
  if (!validate_dma) {
    say("STOPPED: the host probe ended after the first linked title object; it has no renderer "
        "or app loop, and any missing-machine reports above are the next unsupported frontier, "
        "not successful behavior.\n");
    return 0;
  }

  if (!metrics.dma_found_valid) {
    say("FAILED: --play-dma did not observe a valid 327-bucket graphics-DMA chain\n");
    return 1;
  }
  say("  proved: graphics DMA frame %d completed 327 buckets (%d tags, %u payload bytes, "
      "%u copied bytes)\n",
      metrics.dma_valid_frame, metrics.dma_valid_tags, metrics.dma_valid_payload_bytes,
      metrics.dma_valid_copied_bytes);
  say("STOPPED: --play-dma synchronously measured and dropped the chain; rendered frames 0, "
      "presented frames 0. This validation host has no renderer or app loop.\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string data_dir;
  int dispatch_frames = 100;
  bool with_game = false;
  bool run_play = false;
  bool run_play_dma = false;
  bool run_play_gfx_host = false;
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
    } else if (arg == "--play") {
      run_play = true;
      with_game = true;
    } else if (arg == "--play-dma") {
      run_play = true;
      run_play_dma = true;
      with_game = true;
    } else if (arg == "--play-gfx-host") {
      run_play = true;
      run_play_gfx_host = true;
      with_game = true;
    } else {
      std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
      return 2;
    }
  }
  if (run_play_dma && run_play_gfx_host) {
    std::fprintf(stderr, "--play-dma and --play-gfx-host are mutually exclusive\n");
    return 2;
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

  if (run_play && !run_play_gfx_host) {
    return run_play_runtime(data_dir, dispatch_frames, run_play_dma);
  }

  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  clear_print();
  register_aot_objects();
  std::printf("%d AOT translation units registered by object name\n", goal_aot_boot_file_count);

  const int result =
      run_boot(data_dir, dispatch_frames, with_game, run_play, run_play_dma, run_play_gfx_host);

  goal_aot_reset();
  goal_kernel_core_shutdown();
  return result;
}
