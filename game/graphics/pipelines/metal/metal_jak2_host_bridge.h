#pragma once

#include <stdint.h>

#include "game/kernel/core/gfx_host.h"

#ifdef __OBJC__
@class CAMetalLayer;
typedef CAMetalLayer* goal_jak2_metal_host_layer;
#else
typedef void* goal_jak2_metal_host_layer;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct goal_jak2_metal_host goal_jak2_metal_host;

typedef struct goal_jak2_metal_host_metrics {
  uint64_t chains;
  uint64_t completed_chains;
  uint64_t failed_chains;
  uint64_t sync_paths;
  uint64_t vsyncs;
  uint64_t texture_uploads;
  uint64_t texture_relocations;
  uint32_t last_copied_bytes;
  uint64_t last_buckets_dispatched;
  uint64_t command_buffers_committed;
  uint64_t command_buffers_completed;
  uint64_t command_buffer_errors;
  uint64_t drawables_acquired;
  uint64_t drawable_misses;
  uint64_t late_present_submissions;
  uint64_t draws;
  uint64_t triangles;
  uint64_t submissions;
  uint64_t presentations;
  uint64_t presentation_drops;
  uint64_t presentation_order_mismatches;
  uint64_t skipped_bucket_bytes;
  uint64_t unsupported_blends;
  int32_t last_command_buffer_status;
  int64_t last_command_buffer_error_code;
} goal_jak2_metal_host_metrics;

/*! Create the process-singleton, nil-layer Jak 2 policy-dispatch host. */
goal_jak2_metal_host* goal_jak2_metal_host_create(void);

/*! Create the same external host with an app-owned layer for bounded real-DMA presentation. */
goal_jak2_metal_host* goal_jak2_metal_host_create_presenting(
    goal_jak2_metal_host_layer layer);

/*! Copy the app-owned callback table into `out`; the runtime copies it again during start. */
int goal_jak2_metal_host_copy_gfx_host(goal_jak2_metal_host* host, goal_gfx_host* out);

int goal_jak2_metal_host_get_metrics(goal_jak2_metal_host* host,
                                     goal_jak2_metal_host_metrics* out);

/*!
 * Wait for every layer-backed command buffer submitted so far. When `require_presentation` is
 * nonzero, also require every physical drawable callback. The iOS Simulator passes zero because
 * its SDK does not expose that callback.
 */
int goal_jak2_metal_host_wait_for_last_frame(goal_jak2_metal_host* host,
                                             double timeout_seconds,
                                             int require_presentation);

/*! Call only after goal_jak2_runtime_shutdown, when the copied callbacks are no longer reachable. */
void goal_jak2_metal_host_destroy(goal_jak2_metal_host* host);

const char* goal_jak2_metal_host_last_error(goal_jak2_metal_host* host);

#ifdef __cplusplus
}  // extern "C"
#endif
