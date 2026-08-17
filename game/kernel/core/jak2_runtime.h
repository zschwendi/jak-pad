#pragma once

/*!
 * @file jak2_runtime.h
 * Process-singleton Jak 2 title runtime over the portable AOT kernel.
 *
 * This boundary owns no application loop, clock, renderer, input device, or platform object. A
 * host starts it once, pushes controller state through pad.h, and calls `goal_jak2_runtime_tick`
 * exactly once for each frame it wants the GOAL kernel to dispatch.
 */

#include <stdint.h>

struct goal_gfx_host;

#ifdef __cplusplus
extern "C" {
#endif

typedef enum goal_jak2_runtime_status {
  GOAL_JAK2_RUNTIME_OK = 0,
  GOAL_JAK2_RUNTIME_ALREADY_RUNNING = 1,
  GOAL_JAK2_RUNTIME_NOT_RUNNING = 2,
  GOAL_JAK2_RUNTIME_INVALID_ARGUMENT = 3,
  GOAL_JAK2_RUNTIME_START_FAILED = 4,
  GOAL_JAK2_RUNTIME_EXITED = 5,
  GOAL_JAK2_RUNTIME_REQUEST_FAILED = 6,
} goal_jak2_runtime_status;

typedef enum goal_jak2_runtime_state {
  GOAL_JAK2_RUNTIME_STOPPED = 0,
  GOAL_JAK2_RUNTIME_STARTING = 1,
  GOAL_JAK2_RUNTIME_RUNNING = 2,
  GOAL_JAK2_RUNTIME_FAILED = 3,
  GOAL_JAK2_RUNTIME_STOPPED_BY_GAME = 4,
} goal_jak2_runtime_state;

typedef enum goal_jak2_runtime_graphics {
  /*! Preserve the portable machine stubs used by the plain `--play` diagnostic. */
  GOAL_JAK2_RUNTIME_GRAPHICS_STUBS = 0,
  /*! Synchronously validate and drop DMA; `sync-path` and `syncv` never wait externally. */
  GOAL_JAK2_RUNTIME_GRAPHICS_DMA_VALIDATION = 1,
  /*! Count and drop the complete graphics-host boundary without parsing the DMA chain. */
  GOAL_JAK2_RUNTIME_GRAPHICS_HOST_VALIDATION = 2,
  /*! Install a copied host supplied by the app. The app retains any state its callbacks use. */
  GOAL_JAK2_RUNTIME_GRAPHICS_EXTERNAL_HOST = 3,
} goal_jak2_runtime_graphics;

typedef enum goal_jak2_player_traversal_state {
  GOAL_JAK2_PLAYER_TRAVERSAL_UNKNOWN = 0,
  GOAL_JAK2_PLAYER_TRAVERSAL_ON_FOOT = 1,
  GOAL_JAK2_PLAYER_TRAVERSAL_JETBOARD = 2,
  GOAL_JAK2_PLAYER_TRAVERSAL_VEHICLE_TRANSITION = 3,
  GOAL_JAK2_PLAYER_TRAVERSAL_VEHICLE_RIDING = 4,
} goal_jak2_player_traversal_state;

typedef enum goal_jak2_player_look_state {
  GOAL_JAK2_PLAYER_LOOK_UNKNOWN = 0,
  GOAL_JAK2_PLAYER_LOOK_NORMAL = 1,
  GOAL_JAK2_PLAYER_LOOK_AROUND = 2,
} goal_jak2_player_look_state;

/*! Exact-type-checked target traversal and camera state. Unknown values fail closed independently. */
typedef struct goal_jak2_player_context_snapshot {
  goal_jak2_player_traversal_state traversal;
  goal_jak2_player_look_state look_state;
} goal_jak2_player_context_snapshot;

/*! Fresh, exact-type-checked Jak II face-button prompt state. */
typedef struct goal_jak2_face_prompt_touch_snapshot {
  int32_t available;
  uint32_t requested_buttons;
  uint32_t sequence;
} goal_jak2_face_prompt_touch_snapshot;

typedef enum goal_jak2_progress_screen {
  GOAL_JAK2_PROGRESS_SCREEN_UNAVAILABLE = -1,
  GOAL_JAK2_PROGRESS_SCREEN_TITLE = 27,
} goal_jak2_progress_screen;

/*! Fail-closed, copied fields for the one source-proven Jak II progress screen. */
typedef struct goal_jak2_progress_menu_snapshot {
  int32_t available;
  int32_t screen;
  int32_t option_index;
  int32_t selected_option;
  int32_t in_transition;
  int32_t navigation_available;
  int32_t starting_screen;
  int32_t can_exit_with_start;
  int32_t can_go_back;
} goal_jak2_progress_menu_snapshot;

typedef enum goal_jak2_progress_menu_semantic_phase {
  GOAL_JAK2_PROGRESS_MENU_PHASE_UNAVAILABLE = 0,
  GOAL_JAK2_PROGRESS_MENU_PHASE_SELECT_SAVE_TITLE = 1,
  GOAL_JAK2_PROGRESS_MENU_PHASE_NO_MEMORY_CARD = 2,
  GOAL_JAK2_PROGRESS_MENU_PHASE_CREATE_GAME = 3,
  GOAL_JAK2_PROGRESS_MENU_PHASE_CREATING = 4,
  GOAL_JAK2_PROGRESS_MENU_PHASE_SAVING = 5,
  GOAL_JAK2_PROGRESS_MENU_PHASE_ALREADY_EXISTS = 6,
  GOAL_JAK2_PROGRESS_MENU_PHASE_ICON_INFO = 7,
  GOAL_JAK2_PROGRESS_MENU_PHASE_SELECT_LOAD = 8,
  GOAL_JAK2_PROGRESS_MENU_PHASE_SELECT_SAVE = 9,
  GOAL_JAK2_PROGRESS_MENU_PHASE_LOADING = 10,
} goal_jak2_progress_menu_semantic_phase;

typedef enum goal_jak2_progress_menu_semantic_action {
  GOAL_JAK2_PROGRESS_MENU_ACTION_NONE = 0,
  GOAL_JAK2_PROGRESS_MENU_ACTION_UP = 1u << 0,
  GOAL_JAK2_PROGRESS_MENU_ACTION_DOWN = 1u << 1,
  GOAL_JAK2_PROGRESS_MENU_ACTION_LEFT = 1u << 2,
  GOAL_JAK2_PROGRESS_MENU_ACTION_RIGHT = 1u << 3,
  GOAL_JAK2_PROGRESS_MENU_ACTION_CONFIRM = 1u << 4,
  GOAL_JAK2_PROGRESS_MENU_ACTION_BACK = 1u << 5,
} goal_jak2_progress_menu_semantic_action;

/*! Fixed-width additive ABI for source-proven Jak II menu meanings, independent of Jak 1 IDs. */
typedef struct goal_jak2_progress_menu_semantic_snapshot {
  int32_t available;
  int32_t phase;
  int32_t option_index;
  uint32_t action_mask;
} goal_jak2_progress_menu_semantic_snapshot;

/*! Raw, copied values explaining why a live progress snapshot failed closed. */
typedef struct goal_jak2_progress_menu_diagnostics {
  int32_t rejection;
  uint32_t progress;
  uint32_t process_state;
  uint32_t process_state_name;
  uint32_t process_next_state;
  uint32_t current_options;
  uint32_t expected_options;
  uint32_t current;
  uint32_t expected_current;
  uint32_t next;
  uint32_t expected_next;
  uint32_t starting_state;
  int32_t option_index;
  uint32_t selected_option;
  float menu_transition;
} goal_jak2_progress_menu_diagnostics;

typedef struct goal_jak2_runtime_config {
  /*! The player's prepared Jak 2 directory, containing `iso/`. Required and copied at start. */
  const char* data_directory;
  /*! Local gameplay-save directory. Empty selects the portable kernel's existing fallback. */
  const char* saves_directory;
  goal_jak2_runtime_graphics graphics;
  /*! Required only for EXTERNAL_HOST. Read and copied synchronously by runtime_start. */
  const struct goal_gfx_host* external_gfx_host;
  /*! Optional host display domain. Zero preserves the established 60 Hz default; 120 is supported. */
  int32_t target_frame_rate;
} goal_jak2_runtime_config;

/*! Snapshot of the linked gkernel thread-suspend function object before the first display tick. */
typedef struct goal_jak2_thread_suspend_probe {
  uint32_t function_object;
  uintptr_t native_entry;
  uint32_t display_process;
  uint32_t top_thread;
  uint32_t hook_function_object;
  uintptr_t hook_native_entry;
  uintptr_t expected_native_entry;
  int32_t hook_available;
  int32_t matches_expected;
} goal_jak2_thread_suspend_probe;

typedef enum goal_jak2_scene_wait_gate {
  GOAL_JAK2_SCENE_WAIT_NONE = 0,
  GOAL_JAK2_SCENE_WAIT_PROGRESS = 1,
  GOAL_JAK2_SCENE_WAIT_TARGET_GRAB = 2,
  GOAL_JAK2_SCENE_WAIT_SETTING_OR_ENTRY_GUI = 3,
  GOAL_JAK2_SCENE_WAIT_GROUND_TIME = 4,
  GOAL_JAK2_SCENE_WAIT_LEVELS = 5,
  GOAL_JAK2_SCENE_WAIT_ART_FILE = 6,
  GOAL_JAK2_SCENE_WAIT_ART_GUI = 7,
  GOAL_JAK2_SCENE_WAIT_READY = 8,
} goal_jak2_scene_wait_gate;

#define GOAL_JAK2_SCENE_PREVIEW_NAME_MAX 63

#define GOAL_JAK2_SCENE_ACTOR_DIAGNOSTIC_MAX 8

typedef enum goal_jak2_scene_actor_diagnostic_flag {
  GOAL_JAK2_SCENE_ACTOR_SPAWN_ATTEMPTED = 1u << 0,
  GOAL_JAK2_SCENE_ACTOR_POOL_ALLOCATED = 1u << 1,
  GOAL_JAK2_SCENE_ACTOR_DRAW_CONTROL = 1u << 2,
  GOAL_JAK2_SCENE_ACTOR_JOINT_CONTROL = 1u << 3,
  GOAL_JAK2_SCENE_ACTOR_MERC_GEOMETRY = 1u << 4,
} goal_jak2_scene_actor_diagnostic_flag;

/*! Sticky numeric facts for one bounded actor in the most recently initialized scene. */
typedef struct goal_jak2_scene_actor_diagnostic {
  uint32_t flags;
  uint32_t level_index;
  uint32_t merc_pris_bucket;
  uint32_t skeleton_status;
  uint32_t merc_joint_count;
} goal_jak2_scene_actor_diagnostic;

/*! A copied snapshot. It owns no pointers into the GOAL heap or graphics host. */
typedef struct goal_jak2_runtime_metrics {
  goal_jak2_runtime_state state;
  goal_jak2_runtime_graphics graphics;
  uint64_t ticks;
  uint64_t play_boot_result;
  uint64_t last_dispatch_result;
  int32_t master_exit;

  uint32_t kernel_version;
  int32_t kernel_objects;
  int32_t game_objects;
  int32_t game_code_objects;
  int32_t game_data_objects;
  uint32_t global_heap_used_bytes;
  int32_t symbol_count;

  int32_t dgo_archives;
  int32_t dgo_objects;
  int32_t dgo_code_objects;
  int32_t dgo_data_objects;
  int32_t dgo_failures;
  int32_t dgo_last_result;
  char first_dgo_name[17];
  char current_dgo_name[17];
  char last_dgo_name[17];
  char last_dgo_error[256];
  int32_t title_ready;

  char master_mode[24];
  uint32_t title_control_process;
  char title_control_state[24];
  uint64_t title_control_time;
  uint32_t scene_player_process;
  char scene_player_state[24];
  uint32_t progress_process;
  char progress_state[24];
  uint32_t target_process;
  char target_state[24];

  int32_t host_chains;
  int32_t host_sync_paths;
  int32_t host_syncvs;
  int32_t host_texture_uploads;
  int32_t host_texture_relocations;
  int32_t host_desired_level_calls;
  int32_t host_active_level_calls;
  int32_t host_pmode_calls;
  int32_t host_last_desired_level_count;
  int32_t host_last_active_level_count;
  float host_last_pmode_alpha;
  int32_t host_desired_level_sets;
  int32_t host_active_level_sets;
  char host_desired_levels[128];
  char host_active_levels[128];

  int32_t dma_chains;
  int32_t dma_well_formed;
  int32_t dma_malformed;
  int32_t dma_accounting_complete;
  int32_t dma_found_valid;
  int32_t dma_valid_frame;
  int32_t dma_valid_buckets;
  int32_t dma_valid_tags;
  uint32_t dma_valid_payload_bytes;
  uint32_t dma_valid_copied_bytes;

  uint32_t sound_version_requests;
  uint32_t sound_info_ee;
  uint32_t sound_bank_failures;
  uint32_t sound_player_failures;
  uint32_t sound_str_failures;
  uint32_t sound_rejected_calls;

  int32_t display_timing_valid;
  int64_t display_base_frame_counter;
  int64_t blackout_time;
  int64_t blackout_remaining;

  int32_t settings_diagnostics_valid;
  float background_alpha;
  float background_alpha_force;
  uint32_t movie_process;
  uint32_t spooling_process;

  int32_t scene_diagnostics_valid;
  int32_t scene_identity_valid;
  uint32_t scene_list;
  int32_t scene_list_length;
  uint32_t scene;
  int32_t scene_index;
  uint32_t scene_animation;
  uint32_t scene_next_animation;
  int64_t scene_start_time;
  int64_t scene_elapsed;
  char scene_entity[48];
  char scene_art_group[48];
  char scene_animation_name[64];

  int32_t skeleton_diagnostics_valid;
  uint16_t skeleton_status;
  uint8_t skeleton_active_channels;
  uint8_t skeleton_padding;
  int32_t animation_diagnostics_valid;
  uint32_t animation_frame_group;
  float animation_frame;
  float animation_aframe;

  uint32_t sound_player_batches;
  uint32_t sound_player_commands;
  uint32_t sound_play_requests;
  uint32_t sound_sounds_started;
  uint32_t sound_updates;
  uint32_t sound_str_requests;
  uint32_t sound_str_reads;
  uint32_t sound_str_bytes;

  int32_t scene_wait_diagnostics_valid;
  int32_t scene_wait_gate;
  uint32_t scene_wait_entry_gui_id;
  int32_t scene_wait_entry_gui_status;
  uint32_t scene_wait_art_file_status;
  char scene_wait_art_file_status_name[16];
  uint32_t scene_wait_art_gui_id;
  int32_t scene_wait_art_gui_channel;
  int32_t scene_wait_art_gui_action;
  int32_t scene_wait_art_gui_status;

  int32_t ocean_diagnostics_valid;
  uint32_t ocean_map;
  uint32_t ocean_map_city;
  uint32_t ocean_object;
  uint32_t ocean_false_object;
  uint32_t ocean_blit_displays_work;
  uint32_t ocean_blit_menu_mode;
  uint64_t ocean_renderer_mask;
  uint64_t ocean_renderer_menu_mask;
  uint32_t ocean_off;
  uint32_t ocean_near_off;
  uint32_t ocean_mid_off;
  uint32_t ocean_far_on;
  uint32_t ocean_heights;
  uint32_t ocean_verts;

  int32_t scene_actor_diagnostics_valid;
  uint32_t scene_actor_sequence;
  uint64_t scene_actor_scene_name_hash;
  int32_t scene_actor_count;
  int32_t scene_actor_total_count;
  int32_t scene_actor_overflow;
  uint32_t scene_actor_reserved;
  goal_jak2_scene_actor_diagnostic
      scene_actors[GOAL_JAK2_SCENE_ACTOR_DIAGNOSTIC_MAX];
} goal_jak2_runtime_metrics;

/*!
 * Initialize the kernel, register the linked AOT manifest, load KERNEL.CGO and GAME.CGO, install
 * the portable machine seams, call `play-boot`, and retain the kernel dispatcher. All startup
 * work is synchronous. A failure tears the kernel down completely before returning.
 */
goal_jak2_runtime_status goal_jak2_runtime_start(const goal_jak2_runtime_config* config);

/*!
 * Copy and validate the linked gkernel thread-suspend function object. The runtime must already be
 * running. This reads the complete native pointer with memcpy; it does not call or replace it.
 */
goal_jak2_runtime_status goal_jak2_runtime_probe_thread_suspend(
    goal_jak2_thread_suspend_probe* out);

/*
 * Copy and queue one authored scene name for the debug-only Scene Player preview path. At the
 * source-validated title menu, the pending request transiently drives the normal port-zero START
 * path into the authored progress process. The helper is called only after progress owns the
 * master mode. This does not persist the temporary in-memory Scene Player unlock, select a save,
 * or evaluate arbitrary GOAL text.
 */
goal_jak2_runtime_status goal_jak2_runtime_request_scene_preview(const char* scene_name);

/*! Run one 60 Hz-compatible dispatcher frame when the host has no presentation timestamp. */
goal_jak2_runtime_status goal_jak2_runtime_tick(void);

/*!
 * Drive the bounded number of logical GOAL frames represented by one host presentation callback.
 * The host owns the timestamp and presentation; this runtime owns no display object or run loop.
 */
goal_jak2_runtime_status goal_jak2_runtime_tick_at(double target_presentation_time);

/*! Apply a verified 60 or 120 Hz PC-settings domain to a running Jak II AOT session. */
goal_jak2_runtime_status goal_jak2_runtime_set_target_frame_rate(int32_t target_frame_rate);

/*! Drop only stale presentation timestamp history after lifecycle interruption. */
void goal_jak2_runtime_reset_frame_timing(void);

/*! Copy the latest runtime snapshot into `out`. */
goal_jak2_runtime_status goal_jak2_runtime_get_metrics(goal_jak2_runtime_metrics* out);

/*! Copy the exact-type-checked live target traversal and camera context. */
goal_jak2_runtime_status goal_jak2_runtime_get_player_context_snapshot(
    goal_jak2_player_context_snapshot* out);

/*! Copy the fresh face-button prompt state without exposing its GOAL owner or heartbeat. */
goal_jak2_runtime_status goal_jak2_runtime_get_face_prompt_touch_snapshot(
    goal_jak2_face_prompt_touch_snapshot* out);

/*!
 * Copy the live Jak II title progress-menu state. Every other progress screen, malformed pointer,
 * type mismatch, unsupported symbol, and out-of-range field returns an unavailable snapshot.
 */
goal_jak2_runtime_status goal_jak2_runtime_get_progress_menu_snapshot(
    goal_jak2_progress_menu_snapshot* out);

/* Copy only stable title-origin save-flow phases and their exact supported input actions. */
goal_jak2_runtime_status goal_jak2_runtime_get_progress_menu_semantic_snapshot(
    goal_jak2_progress_menu_semantic_snapshot* out);

/*! Copy bounded raw fields used by the fail-closed progress snapshot reader. */
goal_jak2_runtime_status goal_jak2_runtime_get_progress_menu_diagnostics(
    goal_jak2_progress_menu_diagnostics* out);

int goal_jak2_runtime_is_running(void);

/*! Stop and release the owned kernel session. Safe to call when no session is live. */
void goal_jak2_runtime_shutdown(void);

/*! Owned static storage, valid until the next runtime call that changes the error. */
const char* goal_jak2_runtime_last_error(void);

#ifdef __cplusplus
}  // extern "C"
#endif
