#include "game/kernel/core/jak2_runtime.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <utility>

extern "C" {
#include "aot_boot_manifest.h"
}

#include "common/goal_constants.h"
#include "common/link_types.h"
#include "common/log/log.h"
#include "common/symbols.h"

#include "game/kernel/common/klink.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/dgo_loader.h"
#include "game/kernel/core/dma_capture.h"
#include "game/kernel/core/gfx_host.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/pad.h"
#include "game/kernel/core/sound_rpc_jak2.h"
#include "game/kernel/jak2/klisten.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/runtime.h"

namespace {

goal_jak2_runtime_metrics g_metrics = {};
std::string g_error;
std::string g_data_directory;
std::string g_saves_directory;
uint32_t g_dispatcher = 0;
uint64_t g_current_tick = 0;
bool g_owns_kernel = false;
goal_gfx_dma_stats g_dma_before = {};
goal_gfx_host_stats g_gfx_before = {};

uint32_t validation_vsync() {
  return static_cast<uint32_t>(g_current_tick & 1);
}

uint32_t validation_sync_path() {
  return 0;
}

void drain_goal_print_buffer() {
  const char* printed = Ptr<char>(PrintBufArea.offset + sizeof(ListenerMessageHeader)).c();
  if (printed[0]) {
    lg::warn("[jak2-runtime] GOAL: {}", printed);
    clear_print();
  }
}

std::string object_name_of(const char* source) {
  std::string path = source ? source : "";
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

bool register_aot_objects() {
  for (int i = 0; i < goal_aot_boot_file_count; i++) {
    const auto& entry = goal_aot_boot_files[i];
    goal_aot_object_file file = {entry.tag,       entry.statics,         *entry.static_count,
                                 entry.functions, *entry.function_count, entry.link};
    const auto status = goal_aot_register_object(object_name_of(entry.source).c_str(), &file);
    if (status != GOAL_KERNEL_CORE_OK) {
      g_error = std::string("AOT manifest registration failed: ") +
                goal_kernel_core_last_error();
      return false;
    }
  }
  return true;
}

void record_packages_in_game_cgo() {
  using namespace jak2_symbols;
  for (const char* package : {"engine", "art", "common"}) {
    jak2::kernel_packages->value() =
        jak2::new_pair(s7.offset + FIX_SYM_GLOBAL_HEAP,
                       *((s7 + FIX_SYM_PAIR_TYPE - 1).cast<u32>()),
                       jak2::make_string_from_c(package), jak2::kernel_packages->value());
  }
}

struct DmaWindow {
  int chains = 0;
  int well_formed = 0;
  int malformed = 0;
  bool accounting_complete = false;
  bool found_valid = false;
  goal_gfx_dma_frame_summary valid = {};
};

DmaWindow audit_dma_window(const goal_gfx_dma_stats& before,
                           const goal_gfx_dma_stats& after) {
  DmaWindow out;
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

void update_metrics() {
  g_metrics.master_exit = static_cast<int32_t>(MasterExit);
  if (!g_owns_kernel || !goal_kernel_core_is_initialized()) {
    return;
  }

  goal_kernel_core_state kernel = {};
  if (goal_kernel_core_get_state(&kernel) == GOAL_KERNEL_CORE_OK) {
    g_metrics.global_heap_used_bytes = kernel.global_heap_used_bytes;
    g_metrics.symbol_count = kernel.symbol_count;
  }

  goal_dgo_rpc_stats dgo = {};
  goal_dgo_goal_loader_stats(&dgo);
  g_metrics.dgo_archives = dgo.dgo_archives;
  g_metrics.dgo_objects = dgo.dgo_objects;
  g_metrics.dgo_code_objects = dgo.linked_code_objects;
  g_metrics.dgo_data_objects = dgo.linked_data_objects;
  std::snprintf(g_metrics.first_dgo_name, sizeof(g_metrics.first_dgo_name), "%s",
                dgo.first_dgo_name);
  g_metrics.title_ready = std::strcmp(dgo.first_dgo_name, "TITLE.DGO") == 0 &&
                          dgo.dgo_archives >= 1 && dgo.dgo_objects >= 1 &&
                          dgo.linked_code_objects + dgo.linked_data_objects >= 1;

  goal_jak2_sound_rpc_stats sound = {};
  goal_jak2_sound_rpc_stats_get(&sound);
  g_metrics.sound_version_requests = sound.version_requests;
  g_metrics.sound_info_ee = sound.info_ee;
  g_metrics.sound_bank_failures = sound.bank_failures;
  g_metrics.sound_player_failures = sound.player_failures;
  g_metrics.sound_str_failures = sound.str_failures;
  g_metrics.sound_rejected_calls = sound.rejected_calls;

  if (g_metrics.graphics != GOAL_JAK2_RUNTIME_GRAPHICS_DMA_VALIDATION) {
    return;
  }

  goal_gfx_host_stats gfx = {};
  goal_gfx_host_stats_get(&gfx);
  g_metrics.host_chains = gfx.chains - g_gfx_before.chains;
  g_metrics.host_sync_paths = gfx.sync_paths - g_gfx_before.sync_paths;
  g_metrics.host_syncvs = gfx.vsyncs - g_gfx_before.vsyncs;

  goal_gfx_dma_stats dma = {};
  goal_gfx_dma_get_stats(&dma);
  const DmaWindow window = audit_dma_window(g_dma_before, dma);
  g_metrics.dma_chains = window.chains;
  g_metrics.dma_well_formed = window.well_formed;
  g_metrics.dma_malformed = window.malformed;
  g_metrics.dma_accounting_complete = window.accounting_complete;
  g_metrics.dma_found_valid = window.found_valid;
  g_metrics.dma_valid_frame = window.valid.frame;
  g_metrics.dma_valid_buckets = window.valid.buckets;
  g_metrics.dma_valid_tags = window.valid.tags;
  g_metrics.dma_valid_payload_bytes = window.valid.payload_bytes;
  g_metrics.dma_valid_copied_bytes = window.valid.copied_bytes;
}

goal_jak2_runtime_status fail_start(std::string message) {
  g_error = std::move(message);
  if (g_owns_kernel) {
    goal_kernel_core_shutdown();
    g_owns_kernel = false;
  }
  g_dispatcher = 0;
  g_current_tick = 0;
  g_metrics.state = GOAL_JAK2_RUNTIME_FAILED;
  return GOAL_JAK2_RUNTIME_START_FAILED;
}

}  // namespace

extern "C" {

goal_jak2_runtime_status goal_jak2_runtime_start(const goal_jak2_runtime_config* config) {
  if (!config || !config->data_directory || !config->data_directory[0] ||
      (config->graphics != GOAL_JAK2_RUNTIME_GRAPHICS_STUBS &&
       config->graphics != GOAL_JAK2_RUNTIME_GRAPHICS_DMA_VALIDATION)) {
    g_error = "goal_jak2_runtime_start: invalid configuration";
    return GOAL_JAK2_RUNTIME_INVALID_ARGUMENT;
  }
  if (g_owns_kernel || goal_kernel_core_is_initialized()) {
    g_error = "goal_jak2_runtime_start: a kernel session is already running";
    return GOAL_JAK2_RUNTIME_ALREADY_RUNNING;
  }

  try {
    g_metrics = {};
    g_metrics.state = GOAL_JAK2_RUNTIME_STARTING;
    g_metrics.graphics = config->graphics;
    g_error.clear();
    g_data_directory = config->data_directory;
    g_saves_directory = config->saves_directory ? config->saves_directory : "";
    g_dispatcher = 0;
    g_current_tick = 0;
    g_dma_before = {};
    g_gfx_before = {};

    if (goal_kernel_core_set_data_directory(g_data_directory.c_str()) != GOAL_KERNEL_CORE_OK ||
        goal_kernel_core_set_saves_directory(g_saves_directory.c_str()) !=
            GOAL_KERNEL_CORE_OK) {
      return fail_start(std::string("runtime directory setup failed: ") +
                        goal_kernel_core_last_error());
    }

    if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
      return fail_start(std::string("kernel initialization failed: ") +
                        goal_kernel_core_last_error());
    }
    g_owns_kernel = true;

    clear_print();
    if (!register_aot_objects()) {
      return fail_start(g_error);
    }

    constexpr u32 kBootFlags =
        LINK_FLAG_OUTPUT_LOAD | LINK_FLAG_EXECUTE | LINK_FLAG_PRINT_LOGIN;
    goal_dgo_load_stats load = {};
    if (goal_dgo_load("KERNEL", kBootFlags, 0x400000, &load) != GOAL_KERNEL_CORE_OK) {
      const std::string error = std::string("KERNEL.CGO: ") + goal_dgo_last_error();
      drain_goal_print_buffer();
      return fail_start(error);
    }
    drain_goal_print_buffer();
    g_metrics.kernel_objects = load.objects;

    goal_kernel_core_lookup("*kernel-version*", nullptr, &g_metrics.kernel_version);
    if (!g_metrics.kernel_version) {
      return fail_start("KERNEL.CGO did not set *kernel-version*");
    }

    jak2::InitListener();
    if (goal_kernel_core_stub_machine_layer(0) != GOAL_KERNEL_CORE_OK ||
        goal_pad_install() != GOAL_KERNEL_CORE_OK ||
        goal_jak2_sound_rpc_install() != GOAL_KERNEL_CORE_OK) {
      return fail_start("could not install the Jak 2 portable machine seams");
    }
    goal_dgo_install_goal_loader();

    if (config->graphics == GOAL_JAK2_RUNTIME_GRAPHICS_DMA_VALIDATION) {
      goal_gfx_dma_reset();
      goal_gfx_host host = {};
      host.send_chain = goal_gfx_dma_observe_chain;
      host.vsync = validation_vsync;
      host.sync_path = validation_sync_path;
      if (goal_gfx_host_install(&host) != GOAL_KERNEL_CORE_OK) {
        return fail_start("could not install the Jak 2 DMA-validation graphics host");
      }
    }

    load = {};
    if (goal_dgo_load("GAME", kBootFlags, 0x400000, &load) != GOAL_KERNEL_CORE_OK) {
      const std::string error = std::string("GAME.CGO: ") + goal_dgo_last_error();
      drain_goal_print_buffer();
      return fail_start(error);
    }
    drain_goal_print_buffer();
    g_metrics.game_objects = load.objects;
    g_metrics.game_code_objects = load.code_objects;
    g_metrics.game_data_objects = load.data_objects;
    record_packages_in_game_cgo();

    goal_jak2_sound_rpc_stats sound = {};
    goal_jak2_sound_rpc_stats_get(&sound);
    uint32_t sound_info = 0;
    goal_kernel_core_lookup("*sound-iop-info*", nullptr, &sound_info);
    if (sound.version_requests != 1 || !sound.info_ee || sound.info_ee != sound_info) {
      return fail_start("GAME.CGO did not complete the Jak 2 sound handshake");
    }

    uint32_t play_boot = 0;
    if (goal_kernel_core_lookup("play-boot", nullptr, &play_boot) != GOAL_KERNEL_CORE_OK ||
        !play_boot ||
        goal_kernel_core_lookup("kernel-dispatcher", nullptr, &g_dispatcher) !=
            GOAL_KERNEL_CORE_OK ||
        !g_dispatcher) {
      return fail_start("Jak 2 play-boot or kernel-dispatcher holds nothing");
    }

    if (config->graphics == GOAL_JAK2_RUNTIME_GRAPHICS_DMA_VALIDATION) {
      goal_gfx_dma_get_stats(&g_dma_before);
      goal_gfx_host_stats_get(&g_gfx_before);
    }
    g_metrics.play_boot_result = jak2::call_goal_function_by_name("play-boot");
    drain_goal_print_buffer();

    if (MasterExit != RuntimeExitStatus::RUNNING) {
      return fail_start("Jak 2 play-boot requested exit before the runtime started");
    }

    g_metrics.state = GOAL_JAK2_RUNTIME_RUNNING;
    update_metrics();
    return GOAL_JAK2_RUNTIME_OK;
  } catch (const std::exception& e) {
    return fail_start(std::string("Jak 2 runtime startup threw: ") + e.what());
  } catch (...) {
    return fail_start("Jak 2 runtime startup threw an unknown exception");
  }
}

goal_jak2_runtime_status goal_jak2_runtime_tick(void) {
  if (!g_owns_kernel || !goal_kernel_core_is_initialized() ||
      g_metrics.state != GOAL_JAK2_RUNTIME_RUNNING || !g_dispatcher) {
    g_error = "goal_jak2_runtime_tick: no runtime is running";
    return GOAL_JAK2_RUNTIME_NOT_RUNNING;
  }
  if (MasterExit != RuntimeExitStatus::RUNNING) {
    g_metrics.state = GOAL_JAK2_RUNTIME_STOPPED_BY_GAME;
    update_metrics();
    return GOAL_JAK2_RUNTIME_EXITED;
  }

  try {
    g_current_tick = g_metrics.ticks + 1;
    g_metrics.last_dispatch_result =
        call_goal_on_stack(Ptr<Function>(g_dispatcher), goal_kernel_stack_top(), s7.offset,
                           g_ee_main_mem);
    drain_goal_print_buffer();
    g_metrics.ticks++;
    update_metrics();
    if (MasterExit != RuntimeExitStatus::RUNNING) {
      g_metrics.state = GOAL_JAK2_RUNTIME_STOPPED_BY_GAME;
      return GOAL_JAK2_RUNTIME_EXITED;
    }
    return GOAL_JAK2_RUNTIME_OK;
  } catch (const std::exception& e) {
    return fail_start(std::string("Jak 2 runtime tick threw: ") + e.what());
  } catch (...) {
    return fail_start("Jak 2 runtime tick threw an unknown exception");
  }
}

goal_jak2_runtime_status goal_jak2_runtime_get_metrics(goal_jak2_runtime_metrics* out) {
  if (!out) {
    g_error = "goal_jak2_runtime_get_metrics: out is null";
    return GOAL_JAK2_RUNTIME_INVALID_ARGUMENT;
  }
  update_metrics();
  *out = g_metrics;
  return GOAL_JAK2_RUNTIME_OK;
}

int goal_jak2_runtime_is_running(void) {
  return g_owns_kernel && goal_kernel_core_is_initialized() &&
         g_metrics.state == GOAL_JAK2_RUNTIME_RUNNING;
}

void goal_jak2_runtime_shutdown(void) {
  if (g_owns_kernel) {
    MasterExit = RuntimeExitStatus::EXIT;
    goal_kernel_core_shutdown();
    g_owns_kernel = false;
  }
  g_dispatcher = 0;
  g_current_tick = 0;
  g_dma_before = {};
  g_gfx_before = {};
  g_data_directory.clear();
  g_saves_directory.clear();
  g_metrics = {};
  g_metrics.state = GOAL_JAK2_RUNTIME_STOPPED;
  g_error.clear();
}

const char* goal_jak2_runtime_last_error(void) {
  return g_error.c_str();
}

}  // extern "C"
