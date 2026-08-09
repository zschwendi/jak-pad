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

typedef struct goal_jak2_runtime_config {
  /*! The player's prepared Jak 2 directory, containing `iso/`. Required and copied at start. */
  const char* data_directory;
  /*! Local gameplay-save directory. Empty selects the portable kernel's existing fallback. */
  const char* saves_directory;
  goal_jak2_runtime_graphics graphics;
  /*! Required only for EXTERNAL_HOST. Read and copied synchronously by runtime_start. */
  const struct goal_gfx_host* external_gfx_host;
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

/*! Run exactly one `kernel-dispatcher` call. This function has no loop, sleep, or clock input. */
goal_jak2_runtime_status goal_jak2_runtime_tick(void);

/*! Copy the latest runtime snapshot into `out`. */
goal_jak2_runtime_status goal_jak2_runtime_get_metrics(goal_jak2_runtime_metrics* out);

int goal_jak2_runtime_is_running(void);

/*! Stop and release the owned kernel session. Safe to call when no session is live. */
void goal_jak2_runtime_shutdown(void);

/*! Owned static storage, valid until the next runtime call that changes the error. */
const char* goal_jak2_runtime_last_error(void);

#ifdef __cplusplus
}  // extern "C"
#endif
