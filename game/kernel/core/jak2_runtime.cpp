#include "game/kernel/core/jak2_runtime.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <utility>

extern "C" uint64_t goal_native_thread_suspend(uint64_t,
                                                uint64_t,
                                                uint64_t,
                                                uint64_t,
                                                uint64_t,
                                                uint64_t);

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
#include "game/kernel/core/jak2_progress_menu_reader.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/kernel_game.h"
#include "game/kernel/core/jak2_runtime_metrics_reader.h"
#include "game/kernel/core/pad.h"
#include "game/kernel/core/sound_rpc_jak2.h"
#include "game/kernel/jak2/klisten.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/runtime.h"

namespace {

// The linked Jak II gkernel table places (method thread-suspend cpu-thread) at index 22.
constexpr int kThreadSuspendFunctionIndex = 22;

bool copy_goal_bytes(uint32_t object, int offset, void* out, size_t size) {
  if (!g_ee_main_mem || !object || !out || offset < 0 || size > EE_MAIN_MEM_SIZE) {
    return false;
  }
  const uint64_t address = static_cast<uint64_t>(object) + static_cast<uint64_t>(offset);
  if (address < EE_MAIN_MEM_LOW_PROTECT ||
      address > static_cast<uint64_t>(EE_MAIN_MEM_SIZE) - size) {
    return false;
  }
  std::memcpy(out, g_ee_main_mem + address, size);
  return true;
}

goal_jak2_runtime_metrics g_metrics = {};
jak2_runtime_metrics_reader::SceneActorDiagnostics g_scene_actor_diagnostics = {};
std::string g_error;
std::string g_data_directory;
std::string g_saves_directory;
uint32_t g_dispatcher = 0;
uint64_t g_current_tick = 0;
bool g_owns_kernel = false;

enum class ScenePreviewPhase {
  kAwaitStableTitle,
  kAwaitPadReady,
  kPressStart,
  kReleaseStart,
  kAwaitProgress,
};

constexpr int kScenePreviewNeutralWarmupReads = 4;
constexpr int kScenePreviewStartPressFrames = 2;
char g_pending_scene_preview[GOAL_JAK2_SCENE_PREVIEW_NAME_MAX + 1] = {};
bool g_scene_preview_pending = false;
ScenePreviewPhase g_scene_preview_phase = ScenePreviewPhase::kAwaitStableTitle;
int g_scene_preview_start_press_frames = 0;
int g_scene_preview_pad_read_baseline = 0;
goal_gfx_dma_stats g_dma_before = {};
goal_gfx_host g_external_host = {};

struct HostObservations {
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

HostObservations g_host_observations = {};
HostObservations g_host_before = {};

void validation_send_chain(const void* ee_base, uint32_t chain_offset) {
  g_host_observations.chains++;
  if (g_metrics.graphics == GOAL_JAK2_RUNTIME_GRAPHICS_DMA_VALIDATION) {
    goal_gfx_dma_observe_chain(ee_base, chain_offset);
  }
}

uint32_t validation_vsync() {
  g_host_observations.vsyncs++;
  return static_cast<uint32_t>(g_current_tick & 1);
}

uint32_t validation_sync_path() {
  g_host_observations.sync_paths++;
  return 0;
}

void validation_texture_upload(const uint8_t*, int, uint32_t) {
  g_host_observations.texture_uploads++;
}

void validation_texture_relocate(uint32_t, uint32_t, uint32_t) {
  g_host_observations.texture_relocations++;
}

void validation_set_desired_levels(const char* const*, int count) {
  g_host_observations.desired_level_calls++;
  g_host_observations.last_desired_level_count = count;
}

void validation_set_active_levels(const char* const*, int count) {
  g_host_observations.active_level_calls++;
  g_host_observations.last_active_level_count = count;
}

void validation_set_pmode_alpha(float alpha) {
  g_host_observations.pmode_calls++;
  g_host_observations.last_pmode_alpha = alpha;
}

HostObservations host_delta(const HostObservations& before, const HostObservations& after) {
  HostObservations out;
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
    jak2::kernel_packages->value() = static_cast<u32>(
        jak2::new_pair(s7.offset + FIX_SYM_GLOBAL_HEAP,
                       *((s7 + FIX_SYM_PAIR_TYPE - 1).cast<u32>()),
                       static_cast<u32>(jak2::make_string_from_c(package)),
                       jak2::kernel_packages->value()));
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

uint32_t goal_u32(uint32_t object, int offset = 0) {
  uint32_t value = 0;
  copy_goal_bytes(object, offset, &value, sizeof(value));
  return value;
}

uint32_t symbol_value_if_present(const char* name) {
  uint32_t value = 0;
  return goal_game_find_symbol(name, &value) ? value : 0;
}

uint32_t pointer_symbol_process(const char* name) {
  const uint32_t pointer = symbol_value_if_present(name);
  return pointer && pointer != goal_game_false_offset() ? goal_u32(pointer) : 0;
}

void copy_known_symbol_name(uint32_t symbol, char* out, size_t size) {
  static const char* names[] = {"game",       "menu",    "progress", "pause",
                                "freeze",     "startup", "wait",     "idle",
                                "scrap-book", "release", "play-anim", "come-in",
                                "go-away",    "gone",    "target-title", "pending",
                                "active",     "locked"};
  out[0] = '\0';
  for (const char* name : names) {
    if (goal_game_find_symbol(name, nullptr) == symbol) {
      std::snprintf(out, size, "%s", name);
      return;
    }
  }
}

void copy_process_state(uint32_t process, char* out, size_t size) {
  const uint32_t state =
      process ? goal_u32(process, jak2_runtime_metrics_reader::layout::kProcessState) : 0;
  copy_known_symbol_name(state ? goal_u32(state) : 0, out, size);
}

void update_scene_diagnostic_metrics() {
  using namespace jak2_runtime_metrics_reader;
  const MemoryView memory = {reinterpret_cast<const uint8_t*>(g_ee_main_mem), EE_MAIN_MEM_SIZE,
                             goal_game_false_offset()};
  Inputs inputs;
  inputs.display = symbol_value_if_present("*display*");
  inputs.game_info = symbol_value_if_present("*game-info*");
  inputs.setting_control = symbol_value_if_present("*setting-control*");
  inputs.scene_player = g_metrics.scene_player_process;
  inputs.scene_actor_sequence = symbol_value_if_present("*pc-scene-actor-sequence*");
  inputs.scene_actor_scene_name = symbol_value_if_present("*pc-scene-actor-scene-name*");
  inputs.scene_actor_count = symbol_value_if_present("*pc-scene-actor-count*");
  inputs.scene_actor_total_count = symbol_value_if_present("*pc-scene-actor-total-count*");
  inputs.scene_actor_data = symbol_value_if_present("*pc-scene-actor-data*");
  const Snapshot scene = read(memory, inputs);

  g_metrics.display_timing_valid = scene.display_timing_valid;
  g_metrics.display_base_frame_counter = scene.display_base_frame_counter;
  g_metrics.blackout_time = scene.blackout_time;
  g_metrics.blackout_remaining = scene.blackout_remaining;

  g_metrics.settings_diagnostics_valid = scene.settings_valid;
  g_metrics.background_alpha = scene.background_alpha;
  g_metrics.background_alpha_force = scene.background_alpha_force;
  g_metrics.movie_process = scene.movie_process;
  g_metrics.spooling_process = scene.spooling_process;

  g_metrics.scene_diagnostics_valid = scene.scene_valid;
  g_metrics.scene_identity_valid = scene.scene_identity_valid;
  g_metrics.scene_list = scene.scene_list;
  g_metrics.scene_list_length = scene.scene_list_length;
  g_metrics.scene = scene.scene;
  g_metrics.scene_index = scene.scene_index;
  g_metrics.scene_animation = scene.animation;
  g_metrics.scene_next_animation = scene.next_animation;
  g_metrics.scene_start_time = scene.scene_start_time;
  g_metrics.scene_elapsed = scene.scene_elapsed;
  std::snprintf(g_metrics.scene_entity, sizeof(g_metrics.scene_entity), "%s",
                scene.scene_entity.data());
  std::snprintf(g_metrics.scene_art_group, sizeof(g_metrics.scene_art_group), "%s",
                scene.scene_art_group.data());
  std::snprintf(g_metrics.scene_animation_name, sizeof(g_metrics.scene_animation_name), "%s",
                scene.scene_animation.data());

  g_metrics.skeleton_diagnostics_valid = scene.skeleton_valid;
  g_metrics.skeleton_status = scene.skeleton_status;
  g_metrics.skeleton_active_channels = scene.active_channels;
  g_metrics.skeleton_padding = 0;
  g_metrics.animation_diagnostics_valid = scene.animation_valid;
  g_metrics.animation_frame_group = scene.animation_frame_group;
  g_metrics.animation_frame = scene.animation_frame;
  g_metrics.animation_aframe = scene.animation_aframe;

  retain_scene_actor_diagnostics(&g_scene_actor_diagnostics, scene.scene_actors);
  g_metrics.scene_actor_diagnostics_valid = g_scene_actor_diagnostics.valid;
  g_metrics.scene_actor_sequence = g_scene_actor_diagnostics.sequence;
  g_metrics.scene_actor_scene_name_hash = g_scene_actor_diagnostics.scene_name_hash;
  g_metrics.scene_actor_count = g_scene_actor_diagnostics.count;
  g_metrics.scene_actor_total_count = g_scene_actor_diagnostics.total_count;
  g_metrics.scene_actor_overflow = g_scene_actor_diagnostics.overflow;
  g_metrics.scene_actor_reserved = 0;
  std::memcpy(g_metrics.scene_actors, g_scene_actor_diagnostics.actors.data(),
              sizeof(g_metrics.scene_actors));

  uint32_t wait_gate = 0;
  uint32_t entry_gui_id = 0;
  uint32_t entry_gui_status = 0;
  uint32_t art_file_status = 0;
  uint32_t art_gui_id = 0;
  uint32_t art_gui_channel = 0;
  uint32_t art_gui_action = 0;
  uint32_t art_gui_status = 0;
  g_metrics.scene_wait_diagnostics_valid =
      goal_game_find_symbol("*pc-scene-wait-gate*", &wait_gate) &&
      goal_game_find_symbol("*pc-scene-wait-entry-gui-id*", &entry_gui_id) &&
      goal_game_find_symbol("*pc-scene-wait-entry-gui-status*", &entry_gui_status) &&
      goal_game_find_symbol("*pc-scene-wait-art-file-status*", &art_file_status) &&
      goal_game_find_symbol("*pc-scene-wait-art-gui-id*", &art_gui_id) &&
      goal_game_find_symbol("*pc-scene-wait-art-gui-channel*", &art_gui_channel) &&
      goal_game_find_symbol("*pc-scene-wait-art-gui-action*", &art_gui_action) &&
      goal_game_find_symbol("*pc-scene-wait-art-gui-status*", &art_gui_status);
  g_metrics.scene_wait_gate = static_cast<int32_t>(wait_gate);
  g_metrics.scene_wait_entry_gui_id = entry_gui_id;
  g_metrics.scene_wait_entry_gui_status = static_cast<int32_t>(entry_gui_status);
  g_metrics.scene_wait_art_file_status = art_file_status;
  copy_known_symbol_name(art_file_status, g_metrics.scene_wait_art_file_status_name,
                         sizeof(g_metrics.scene_wait_art_file_status_name));
  g_metrics.scene_wait_art_gui_id = art_gui_id;
  g_metrics.scene_wait_art_gui_channel = static_cast<int32_t>(art_gui_channel);
  g_metrics.scene_wait_art_gui_action = static_cast<int32_t>(art_gui_action);
  g_metrics.scene_wait_art_gui_status = static_cast<int32_t>(art_gui_status);
}

void update_title_state_metrics() {
  copy_known_symbol_name(symbol_value_if_present("*master-mode*"), g_metrics.master_mode,
                         sizeof(g_metrics.master_mode));

  g_metrics.title_control_process = pointer_symbol_process("*title-control*");
  copy_process_state(g_metrics.title_control_process, g_metrics.title_control_state,
                     sizeof(g_metrics.title_control_state));
  g_metrics.title_control_time = 0;
  const uint32_t title_clock = goal_u32(g_metrics.title_control_process, 8);
  // GOAL keeps uint64 fields four-byte aligned; clock::frame-counter is at offset 20.
  copy_goal_bytes(title_clock, 20, &g_metrics.title_control_time,
                  sizeof(g_metrics.title_control_time));

  g_metrics.scene_player_process = pointer_symbol_process("*scene-player*");
  copy_process_state(g_metrics.scene_player_process, g_metrics.scene_player_state,
                     sizeof(g_metrics.scene_player_state));
  g_metrics.progress_process = pointer_symbol_process("*progress-process*");
  copy_process_state(g_metrics.progress_process, g_metrics.progress_state,
                     sizeof(g_metrics.progress_state));
  g_metrics.target_process = symbol_value_if_present("*target*");
  copy_process_state(g_metrics.target_process, g_metrics.target_state,
                     sizeof(g_metrics.target_state));
  update_scene_diagnostic_metrics();
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
  g_metrics.dgo_failures = dgo.dgo_failures;
  g_metrics.dgo_last_result = dgo.last_dgo_result;
  std::snprintf(g_metrics.first_dgo_name, sizeof(g_metrics.first_dgo_name), "%s",
                dgo.first_dgo_name);
  std::snprintf(g_metrics.current_dgo_name, sizeof(g_metrics.current_dgo_name), "%s",
                dgo.current_dgo_name);
  std::snprintf(g_metrics.last_dgo_name, sizeof(g_metrics.last_dgo_name), "%s",
                dgo.last_dgo_name);
  std::snprintf(g_metrics.last_dgo_error, sizeof(g_metrics.last_dgo_error), "%s",
                dgo.last_dgo_error);
  g_metrics.title_ready = std::strcmp(dgo.first_dgo_name, "TITLE.DGO") == 0 &&
                          dgo.dgo_archives >= 1 && dgo.dgo_objects >= 1 &&
                          dgo.linked_code_objects + dgo.linked_data_objects >= 1;
  update_title_state_metrics();

  goal_jak2_sound_rpc_stats sound = {};
  goal_jak2_sound_rpc_stats_get(&sound);
  g_metrics.sound_version_requests = sound.version_requests;
  g_metrics.sound_info_ee = sound.info_ee;
  g_metrics.sound_bank_failures = sound.bank_failures;
  g_metrics.sound_player_failures = sound.player_failures;
  g_metrics.sound_str_failures = sound.str_failures;
  g_metrics.sound_rejected_calls = sound.rejected_calls;
  g_metrics.sound_player_batches = sound.player_batches;
  g_metrics.sound_player_commands = sound.player_commands;
  g_metrics.sound_play_requests = sound.play_requests;
  g_metrics.sound_sounds_started = sound.sounds_started;
  g_metrics.sound_updates = sound.sound_updates;
  g_metrics.sound_str_requests = sound.str_requests;
  g_metrics.sound_str_reads = sound.str_reads;
  g_metrics.sound_str_bytes = sound.str_bytes;

  if (g_metrics.graphics == GOAL_JAK2_RUNTIME_GRAPHICS_STUBS) {
    return;
  }

  goal_gfx_host_stats gfx = {};
  goal_gfx_host_stats_get(&gfx);
  g_metrics.host_desired_level_sets = gfx.level_sets;
  g_metrics.host_active_level_sets = gfx.active_level_sets;
  g_metrics.host_pmode_calls = gfx.pmode_calls;
  g_metrics.host_last_pmode_alpha = gfx.last_pmode_alpha;
  std::snprintf(g_metrics.host_desired_levels, sizeof(g_metrics.host_desired_levels), "%s",
                gfx.last_levels ? gfx.last_levels : "");
  std::snprintf(g_metrics.host_active_levels, sizeof(g_metrics.host_active_levels), "%s",
                gfx.last_active_levels ? gfx.last_active_levels : "");

  const HostObservations host = host_delta(g_host_before, g_host_observations);
  g_metrics.host_chains = host.chains;
  g_metrics.host_sync_paths = host.sync_paths;
  g_metrics.host_syncvs = host.vsyncs;
  g_metrics.host_texture_uploads = host.texture_uploads;
  g_metrics.host_texture_relocations = host.texture_relocations;
  g_metrics.host_desired_level_calls = host.desired_level_calls;
  g_metrics.host_active_level_calls = host.active_level_calls;
  g_metrics.host_last_desired_level_count = host.last_desired_level_count;
  g_metrics.host_last_active_level_count = host.last_active_level_count;

  if (g_metrics.graphics != GOAL_JAK2_RUNTIME_GRAPHICS_DMA_VALIDATION) {
    return;
  }

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

jak2_progress_menu_reader::TypeIdentity progress_type_identity(const char* name,
                                                               uint16_t exact_size) {
  jak2_progress_menu_reader::TypeIdentity identity;
  identity.symbol = goal_game_find_symbol(name, &identity.type);
  identity.exact_size = exact_size;
  return identity;
}

jak2_progress_menu_reader::Inputs progress_menu_inputs() {
  using namespace jak2_progress_menu_reader;
  Inputs inputs;
  inputs.master_mode = symbol_value_if_present("*master-mode*");
  inputs.progress_pointer = symbol_value_if_present("*progress-process*");
  inputs.progress_state = symbol_value_if_present("*progress-state*");
  inputs.title_pc_options = symbol_value_if_present("*title-pc*");
  inputs.load_save_options = symbol_value_if_present("*load-save-options*");
  inputs.save_options_title = symbol_value_if_present("*save-options-title*");
  inputs.insufficient_space_options = symbol_value_if_present("*insufficient-space-options*");
  inputs.create_game_options = symbol_value_if_present("*create-game-options*");
  inputs.already_exists_options = symbol_value_if_present("*already-exists-options*");
  inputs.icon_info_options = symbol_value_if_present("*icon-info-options*");
  inputs.loading_options = symbol_value_if_present("*loading-options*");
  inputs.progress_type =
      progress_type_identity("progress", static_cast<uint16_t>(layout::kProgressSize));
  inputs.progress_global_state_type = progress_type_identity(
      "progress-global-state", static_cast<uint16_t>(layout::kProgressGlobalStateSize));
  inputs.menu_option_list_type = progress_type_identity(
      "menu-option-list", static_cast<uint16_t>(layout::kMenuOptionListSize));
  inputs.state_type = progress_type_identity("state", static_cast<uint16_t>(layout::kStateSize));
  inputs.progress_symbol = inputs.progress_type.symbol;
  inputs.title_symbol = goal_game_find_symbol("title", nullptr);
  inputs.none_symbol = goal_game_find_symbol("none", nullptr);
  inputs.idle_symbol = goal_game_find_symbol("idle", nullptr);
  inputs.select_load_symbol = goal_game_find_symbol("select-load", nullptr);
  inputs.select_save_symbol = goal_game_find_symbol("select-save", nullptr);
  inputs.select_save_title_symbol = goal_game_find_symbol("select-save-title", nullptr);
  inputs.select_save_title_hero_symbol =
      goal_game_find_symbol("select-save-title-hero", nullptr);
  inputs.no_memory_card_symbol = goal_game_find_symbol("no-memory-card", nullptr);
  inputs.create_game_symbol = goal_game_find_symbol("create-game", nullptr);
  inputs.already_exists_symbol = goal_game_find_symbol("already-exists", nullptr);
  inputs.icon_info_symbol = goal_game_find_symbol("icon-info", nullptr);
  inputs.loading_symbol = goal_game_find_symbol("loading", nullptr);
  inputs.creating_symbol = goal_game_find_symbol("creating", nullptr);
  inputs.saving_symbol = goal_game_find_symbol("saving", nullptr);
  inputs.true_object = goal_game_true_offset();
  return inputs;
}

jak2_progress_menu_reader::Snapshot read_progress_menu(
    jak2_progress_menu_reader::Diagnostics* diagnostics) {
  using namespace jak2_progress_menu_reader;
  return read({reinterpret_cast<const uint8_t*>(g_ee_main_mem), EE_MAIN_MEM_SIZE,
               goal_game_false_offset()},
              progress_menu_inputs(), diagnostics);
}

goal_jak2_progress_menu_snapshot unavailable_progress_menu_snapshot() {
  goal_jak2_progress_menu_snapshot out = {};
  out.screen = GOAL_JAK2_PROGRESS_SCREEN_UNAVAILABLE;
  out.option_index = -1;
  out.starting_screen = GOAL_JAK2_PROGRESS_SCREEN_UNAVAILABLE;
  return out;
}

goal_jak2_progress_menu_semantic_snapshot unavailable_progress_menu_semantic_snapshot() {
  goal_jak2_progress_menu_semantic_snapshot out = {};
  out.phase = GOAL_JAK2_PROGRESS_MENU_PHASE_UNAVAILABLE;
  out.option_index = -1;
  return out;
}

void reset_scene_preview_request() {
  g_pending_scene_preview[0] = '\0';
  g_scene_preview_pending = false;
  g_scene_preview_phase = ScenePreviewPhase::kAwaitStableTitle;
  g_scene_preview_start_press_frames = 0;
  g_scene_preview_pad_read_baseline = 0;
}

goal_jak2_runtime_status fail_start(std::string message) {
  g_error = std::move(message);
  if (g_owns_kernel) {
    goal_kernel_core_shutdown();
    g_owns_kernel = false;
  }
  g_dispatcher = 0;
  g_current_tick = 0;
  reset_scene_preview_request();
  g_dma_before = {};
  g_host_observations = {};
  g_host_before = {};
  g_external_host = {};
  g_metrics.state = GOAL_JAK2_RUNTIME_FAILED;
  return GOAL_JAK2_RUNTIME_START_FAILED;
}

bool stable_title_for_scene_preview_start() {
  return g_metrics.title_ready && g_metrics.title_control_process &&
         std::strcmp(g_metrics.master_mode, "game") == 0 &&
         std::strcmp(g_metrics.title_control_state, "wait") == 0 &&
         !g_metrics.progress_process;
}

bool progress_ready_for_scene_preview() {
  if (std::strcmp(g_metrics.master_mode, "progress") != 0 ||
      !g_metrics.progress_process) {
    return false;
  }
  const auto snapshot = read_progress_menu(nullptr);
  return snapshot.available && snapshot.navigation_available;
}

goal_jak2_runtime_status prepare_pending_scene_preview_input() {
  if (!g_scene_preview_pending) {
    return GOAL_JAK2_RUNTIME_OK;
  }

  if (g_scene_preview_phase == ScenePreviewPhase::kAwaitStableTitle) {
    if (!stable_title_for_scene_preview_start()) {
      return GOAL_JAK2_RUNTIME_OK;
    }
    g_scene_preview_pad_read_baseline = goal_pad_read_count(0);
    g_scene_preview_phase = ScenePreviewPhase::kAwaitPadReady;
  } else if (g_scene_preview_phase == ScenePreviewPhase::kAwaitPadReady &&
             !stable_title_for_scene_preview_start()) {
    g_scene_preview_phase = ScenePreviewPhase::kAwaitStableTitle;
    return GOAL_JAK2_RUNTIME_OK;
  }

  goal_pad_state pad = {};
  goal_pad_state_neutral(&pad);
  if (g_scene_preview_phase == ScenePreviewPhase::kAwaitPadReady &&
      goal_pad_read_count(0) - g_scene_preview_pad_read_baseline >=
          kScenePreviewNeutralWarmupReads) {
    g_scene_preview_phase = ScenePreviewPhase::kPressStart;
  }
  if (g_scene_preview_phase == ScenePreviewPhase::kPressStart) {
    pad.buttons = GOAL_PAD_START;
    g_scene_preview_start_press_frames++;
    if (g_scene_preview_start_press_frames == kScenePreviewStartPressFrames) {
      g_scene_preview_phase = ScenePreviewPhase::kReleaseStart;
    }
  } else if (g_scene_preview_phase == ScenePreviewPhase::kReleaseStart) {
    g_scene_preview_phase = ScenePreviewPhase::kAwaitProgress;
  }

  if (goal_pad_set_state(0, &pad) != GOAL_KERNEL_CORE_OK) {
    g_error = "Jak 2 scene preview could not override controller port 0";
    reset_scene_preview_request();
    return GOAL_JAK2_RUNTIME_REQUEST_FAILED;
  }
  return GOAL_JAK2_RUNTIME_OK;
}

goal_jak2_runtime_status run_pending_scene_preview() {
  if (!g_scene_preview_pending ||
      g_scene_preview_phase != ScenePreviewPhase::kAwaitProgress ||
      !progress_ready_for_scene_preview()) {
    return GOAL_JAK2_RUNTIME_OK;
  }

  const std::string requested = g_pending_scene_preview;
  reset_scene_preview_request();

  const uint32_t name = static_cast<uint32_t>(jak2::make_string_from_c(requested.c_str()));
  if (!name || !goal_game_find_symbol("*pc-scene-preview-request*", nullptr)) {
    g_error = "Jak 2 scene preview queue is unavailable";
    return GOAL_JAK2_RUNTIME_REQUEST_FAILED;
  }
  goal_game_set_symbol_value("*pc-scene-preview-request*", name);
  return GOAL_JAK2_RUNTIME_OK;
}

}  // namespace

extern "C" {

goal_jak2_runtime_status goal_jak2_runtime_start(const goal_jak2_runtime_config* config) {
  if (!config || !config->data_directory || !config->data_directory[0] ||
      (config->graphics != GOAL_JAK2_RUNTIME_GRAPHICS_STUBS &&
       config->graphics != GOAL_JAK2_RUNTIME_GRAPHICS_DMA_VALIDATION &&
       config->graphics != GOAL_JAK2_RUNTIME_GRAPHICS_HOST_VALIDATION &&
       config->graphics != GOAL_JAK2_RUNTIME_GRAPHICS_EXTERNAL_HOST)) {
    g_error = "goal_jak2_runtime_start: invalid configuration";
    return GOAL_JAK2_RUNTIME_INVALID_ARGUMENT;
  }
  if (config->graphics == GOAL_JAK2_RUNTIME_GRAPHICS_EXTERNAL_HOST &&
      (!config->external_gfx_host || !config->external_gfx_host->send_chain ||
       !config->external_gfx_host->vsync || !config->external_gfx_host->sync_path)) {
    g_error = "goal_jak2_runtime_start: external graphics host is incomplete";
    return GOAL_JAK2_RUNTIME_INVALID_ARGUMENT;
  }
  if (g_owns_kernel || goal_kernel_core_is_initialized()) {
    g_error = "goal_jak2_runtime_start: a kernel session is already running";
    return GOAL_JAK2_RUNTIME_ALREADY_RUNNING;
  }

  try {
    g_metrics = {};
    g_scene_actor_diagnostics = {};
    g_metrics.state = GOAL_JAK2_RUNTIME_STARTING;
    g_metrics.graphics = config->graphics;
    g_error.clear();
    g_data_directory = config->data_directory;
    g_saves_directory = config->saves_directory ? config->saves_directory : "";
    g_dispatcher = 0;
    g_current_tick = 0;
    reset_scene_preview_request();
    g_dma_before = {};
    g_host_observations = {};
    g_host_before = {};
    g_external_host = config->graphics == GOAL_JAK2_RUNTIME_GRAPHICS_EXTERNAL_HOST
                          ? *config->external_gfx_host
                          : goal_gfx_host{};

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
    if (goal_jak2_dgo_load_boot("KERNEL", kBootFlags, 0x400000, &load) != GOAL_KERNEL_CORE_OK) {
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

    if (config->graphics != GOAL_JAK2_RUNTIME_GRAPHICS_STUBS) {
      if (config->graphics == GOAL_JAK2_RUNTIME_GRAPHICS_DMA_VALIDATION) {
        goal_gfx_dma_reset();
      }
      if (config->graphics == GOAL_JAK2_RUNTIME_GRAPHICS_EXTERNAL_HOST) {
        if (goal_gfx_host_install(&g_external_host) != GOAL_KERNEL_CORE_OK) {
          return fail_start("could not install the copied Jak 2 external graphics host");
        }
      } else {
        goal_gfx_host host = {};
        host.send_chain = validation_send_chain;
        host.vsync = validation_vsync;
        host.sync_path = validation_sync_path;
        host.texture_upload_now = validation_texture_upload;
        host.texture_relocate = validation_texture_relocate;
        host.set_levels = validation_set_desired_levels;
        host.set_pmode_alp = validation_set_pmode_alpha;
        host.set_active_levels = validation_set_active_levels;
        if (goal_gfx_host_install(&host) != GOAL_KERNEL_CORE_OK) {
          return fail_start("could not install the Jak 2 validation graphics host");
        }
      }
    }

    load = {};
    if (goal_jak2_dgo_load_boot("GAME", kBootFlags, 0x400000, &load) != GOAL_KERNEL_CORE_OK) {
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
    }
    if (config->graphics != GOAL_JAK2_RUNTIME_GRAPHICS_STUBS) {
      g_host_before = g_host_observations;
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

goal_jak2_runtime_status goal_jak2_runtime_probe_thread_suspend(
    goal_jak2_thread_suspend_probe* out) {
  if (!out) {
    g_error = "goal_jak2_runtime_probe_thread_suspend: out is null";
    return GOAL_JAK2_RUNTIME_INVALID_ARGUMENT;
  }
  *out = {};
  if (!g_owns_kernel || !goal_kernel_core_is_initialized() ||
      g_metrics.state != GOAL_JAK2_RUNTIME_RUNNING) {
    g_error = "goal_jak2_runtime_probe_thread_suspend: runtime is not running";
    return GOAL_JAK2_RUNTIME_NOT_RUNNING;
  }

  out->function_object =
      goal_aot_function_object("gkernel", kThreadSuspendFunctionIndex);
  out->expected_native_entry =
      reinterpret_cast<uintptr_t>(&goal_native_thread_suspend);
  copy_goal_bytes(out->function_object, 0, &out->native_entry, sizeof(out->native_entry));

  if (goal_kernel_core_lookup("*dproc*", nullptr, &out->display_process) ==
          GOAL_KERNEL_CORE_OK &&
      out->display_process != goal_game_false_offset()) {
    out->hook_available = 1;
    const auto& process_offsets = goal_game_process_offsets();
    copy_goal_bytes(out->display_process, process_offsets.top_thread, &out->top_thread,
                    sizeof(out->top_thread));
    copy_goal_bytes(out->top_thread, kGoalCpuThreadSuspendHookOffset,
                    &out->hook_function_object, sizeof(out->hook_function_object));
    copy_goal_bytes(out->hook_function_object, 0, &out->hook_native_entry,
                    sizeof(out->hook_native_entry));
  }

  const bool source_matches =
      out->native_entry != 0 && out->native_entry == out->expected_native_entry;
  const bool hook_matches =
      !out->hook_available ||
      (out->top_thread != 0 && out->hook_function_object == out->function_object &&
       out->hook_native_entry == out->expected_native_entry);
  out->matches_expected = source_matches && hook_matches;
  if (!out->matches_expected) {
    char message[384];
    std::snprintf(message, sizeof(message),
                  "Jak 2 thread-suspend probe failed: source #x%08x/#x%llx, display "
                  "#x%08x, top thread #x%08x, hook #x%08x/#x%llx; expected #x%llx",
                  out->function_object,
                  static_cast<unsigned long long>(out->native_entry),
                  out->display_process, out->top_thread, out->hook_function_object,
                  static_cast<unsigned long long>(out->hook_native_entry),
                  static_cast<unsigned long long>(out->expected_native_entry));
    g_error = message;
    return GOAL_JAK2_RUNTIME_START_FAILED;
  }
  return GOAL_JAK2_RUNTIME_OK;
}

goal_jak2_runtime_status goal_jak2_runtime_request_scene_preview(const char* scene_name) {
  if (!scene_name) {
    g_error = "goal_jak2_runtime_request_scene_preview: scene_name is null";
    return GOAL_JAK2_RUNTIME_INVALID_ARGUMENT;
  }
  if (!g_owns_kernel || !goal_kernel_core_is_initialized() ||
      g_metrics.state != GOAL_JAK2_RUNTIME_RUNNING) {
    g_error = "goal_jak2_runtime_request_scene_preview: runtime is not running";
    return GOAL_JAK2_RUNTIME_NOT_RUNNING;
  }
  size_t length = 0;
  while (length <= GOAL_JAK2_SCENE_PREVIEW_NAME_MAX && scene_name[length]) {
    length++;
  }
  if (!length || length > GOAL_JAK2_SCENE_PREVIEW_NAME_MAX) {
    g_error = "goal_jak2_runtime_request_scene_preview: scene name is empty or too long";
    return GOAL_JAK2_RUNTIME_INVALID_ARGUMENT;
  }
  if (g_scene_preview_pending) {
    g_error = "goal_jak2_runtime_request_scene_preview: another request is pending";
    return GOAL_JAK2_RUNTIME_INVALID_ARGUMENT;
  }
  std::memcpy(g_pending_scene_preview, scene_name, length + 1);
  g_scene_preview_pending = true;
  g_scene_preview_phase = ScenePreviewPhase::kAwaitStableTitle;
  g_scene_preview_start_press_frames = 0;
  g_scene_preview_pad_read_baseline = 0;
  g_error.clear();
  return GOAL_JAK2_RUNTIME_OK;
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
    if (g_metrics.ticks == 0) {
      goal_jak2_thread_suspend_probe probe = {};
      const auto probe_status = goal_jak2_runtime_probe_thread_suspend(&probe);
      if (probe_status != GOAL_JAK2_RUNTIME_OK) {
        return probe_status;
      }
    }
    g_current_tick = g_metrics.ticks + 1;
    // Jak 2's overlord publishes sound/stream state from its vblank handler. The portable runtime
    // has no IOP vblank, so publish the previous dispatcher frame before GOAL consumes it.
    goal_jak2_sound_frame();
    const auto preview_input_status = prepare_pending_scene_preview_input();
    if (preview_input_status != GOAL_JAK2_RUNTIME_OK) {
      return preview_input_status;
    }
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
    const bool had_pending_preview = g_scene_preview_pending;
    const auto preview_status = run_pending_scene_preview();
    if (preview_status != GOAL_JAK2_RUNTIME_OK) {
      return preview_status;
    }
    if (had_pending_preview && !g_scene_preview_pending) {
      update_metrics();
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

goal_jak2_runtime_status goal_jak2_runtime_get_progress_menu_snapshot(
    goal_jak2_progress_menu_snapshot* out) {
  if (!out) {
    g_error = "goal_jak2_runtime_get_progress_menu_snapshot: out is null";
    return GOAL_JAK2_RUNTIME_INVALID_ARGUMENT;
  }
  *out = unavailable_progress_menu_snapshot();
  if (!g_owns_kernel || !goal_kernel_core_is_initialized() ||
      g_metrics.state != GOAL_JAK2_RUNTIME_RUNNING || !g_ee_main_mem) {
    return GOAL_JAK2_RUNTIME_OK;
  }

  using namespace jak2_progress_menu_reader;
  const Snapshot snapshot = read_progress_menu(nullptr);
  if (!snapshot.available) {
    return GOAL_JAK2_RUNTIME_OK;
  }

  out->available = 1;
  out->screen = snapshot.screen;
  out->option_index = snapshot.option_index;
  out->selected_option = snapshot.selected_option;
  out->in_transition = snapshot.in_transition;
  out->navigation_available = snapshot.navigation_available;
  out->starting_screen = snapshot.starting_screen;
  out->can_exit_with_start = snapshot.can_exit_with_start;
  out->can_go_back = snapshot.can_go_back;
  return GOAL_JAK2_RUNTIME_OK;
}

goal_jak2_runtime_status goal_jak2_runtime_get_progress_menu_semantic_snapshot(
    goal_jak2_progress_menu_semantic_snapshot* out) {
  static_assert(sizeof(goal_jak2_progress_menu_semantic_snapshot) == 16);
  static_assert(static_cast<int32_t>(jak2_progress_menu_reader::SemanticPhase::select_save_title) ==
                GOAL_JAK2_PROGRESS_MENU_PHASE_SELECT_SAVE_TITLE);
  static_assert(static_cast<int32_t>(jak2_progress_menu_reader::SemanticPhase::no_memory_card) ==
                GOAL_JAK2_PROGRESS_MENU_PHASE_NO_MEMORY_CARD);
  static_assert(static_cast<int32_t>(jak2_progress_menu_reader::SemanticPhase::create_game) ==
                GOAL_JAK2_PROGRESS_MENU_PHASE_CREATE_GAME);
  static_assert(static_cast<int32_t>(jak2_progress_menu_reader::SemanticPhase::creating) ==
                GOAL_JAK2_PROGRESS_MENU_PHASE_CREATING);
  static_assert(static_cast<int32_t>(jak2_progress_menu_reader::SemanticPhase::saving) ==
                GOAL_JAK2_PROGRESS_MENU_PHASE_SAVING);
  static_assert(static_cast<int32_t>(jak2_progress_menu_reader::SemanticPhase::already_exists) ==
                GOAL_JAK2_PROGRESS_MENU_PHASE_ALREADY_EXISTS);
  static_assert(static_cast<int32_t>(jak2_progress_menu_reader::SemanticPhase::icon_info) ==
                GOAL_JAK2_PROGRESS_MENU_PHASE_ICON_INFO);
  static_assert(static_cast<int32_t>(jak2_progress_menu_reader::SemanticPhase::select_load) ==
                GOAL_JAK2_PROGRESS_MENU_PHASE_SELECT_LOAD);
  static_assert(static_cast<int32_t>(jak2_progress_menu_reader::SemanticPhase::select_save) ==
                GOAL_JAK2_PROGRESS_MENU_PHASE_SELECT_SAVE);
  static_assert(static_cast<int32_t>(jak2_progress_menu_reader::SemanticPhase::loading) ==
                GOAL_JAK2_PROGRESS_MENU_PHASE_LOADING);
  static_assert(jak2_progress_menu_reader::action_up == GOAL_JAK2_PROGRESS_MENU_ACTION_UP);
  static_assert(jak2_progress_menu_reader::action_down == GOAL_JAK2_PROGRESS_MENU_ACTION_DOWN);
  static_assert(jak2_progress_menu_reader::action_left == GOAL_JAK2_PROGRESS_MENU_ACTION_LEFT);
  static_assert(jak2_progress_menu_reader::action_right == GOAL_JAK2_PROGRESS_MENU_ACTION_RIGHT);
  static_assert(jak2_progress_menu_reader::action_confirm ==
                GOAL_JAK2_PROGRESS_MENU_ACTION_CONFIRM);
  static_assert(jak2_progress_menu_reader::action_back == GOAL_JAK2_PROGRESS_MENU_ACTION_BACK);
  if (!out) {
    g_error = "goal_jak2_runtime_get_progress_menu_semantic_snapshot: out is null";
    return GOAL_JAK2_RUNTIME_INVALID_ARGUMENT;
  }
  *out = unavailable_progress_menu_semantic_snapshot();
  if (!g_owns_kernel || !goal_kernel_core_is_initialized() ||
      g_metrics.state != GOAL_JAK2_RUNTIME_RUNNING || !g_ee_main_mem) {
    return GOAL_JAK2_RUNTIME_OK;
  }

  const auto snapshot = jak2_progress_menu_reader::read_semantic(
      {reinterpret_cast<const uint8_t*>(g_ee_main_mem), EE_MAIN_MEM_SIZE,
       goal_game_false_offset()},
      progress_menu_inputs());
  if (!snapshot.available) {
    return GOAL_JAK2_RUNTIME_OK;
  }

  out->available = 1;
  out->phase = static_cast<int32_t>(snapshot.phase);
  out->option_index = snapshot.option_index;
  out->action_mask = snapshot.action_mask;
  return GOAL_JAK2_RUNTIME_OK;
}

goal_jak2_runtime_status goal_jak2_runtime_get_progress_menu_diagnostics(
    goal_jak2_progress_menu_diagnostics* out) {
  if (!out) {
    g_error = "goal_jak2_runtime_get_progress_menu_diagnostics: out is null";
    return GOAL_JAK2_RUNTIME_INVALID_ARGUMENT;
  }
  *out = {};
  out->option_index = -1;
  if (!g_owns_kernel || !goal_kernel_core_is_initialized() ||
      g_metrics.state != GOAL_JAK2_RUNTIME_RUNNING || !g_ee_main_mem) {
    return GOAL_JAK2_RUNTIME_OK;
  }

  const auto inputs = progress_menu_inputs();
  jak2_progress_menu_reader::Diagnostics detail;
  read_progress_menu(&detail);
  out->rejection = static_cast<int32_t>(detail.rejection);
  out->progress = detail.progress;
  out->process_state = detail.process_state;
  out->process_state_name = detail.process_state_name;
  out->process_next_state = detail.process_next_state;
  out->current_options = detail.current_options;
  out->expected_options = inputs.title_pc_options;
  out->current = detail.current;
  out->expected_current = inputs.title_symbol;
  out->next = detail.next;
  out->expected_next = inputs.none_symbol;
  out->starting_state = detail.starting_state;
  out->option_index = detail.option_index;
  out->selected_option = detail.selected_option;
  out->menu_transition = detail.menu_transition;
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
  reset_scene_preview_request();
  g_dma_before = {};
  g_host_observations = {};
  g_host_before = {};
  g_external_host = {};
  g_data_directory.clear();
  g_saves_directory.clear();
  g_metrics = {};
  g_scene_actor_diagnostics = {};
  g_metrics.state = GOAL_JAK2_RUNTIME_STOPPED;
  g_error.clear();
}

const char* goal_jak2_runtime_last_error(void) {
  return g_error.c_str();
}

}  // extern "C"
