#pragma once

#include <stdint.h>

#include "game/kernel/core/gfx_host.h"

#ifdef __OBJC__
@class CAMetalLayer;
typedef CAMetalLayer* goal_jak2_metal_layer_ref;
#else
typedef void* goal_jak2_metal_layer_ref;
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
  uint64_t drawables_acquired;
  uint64_t draws;
  uint64_t triangles;
  uint64_t submissions;
  uint64_t presentations;
  uint64_t completed_command_buffers;
  uint64_t command_buffer_errors;
  uint64_t drawable_misses;
  uint64_t presentation_drops;
  uint64_t presentation_order_mismatches;
  uint64_t last_submission_id;
  uint64_t last_presented_submission_id;
  int64_t last_command_buffer_status;
  int64_t last_command_buffer_error_code;
  uint32_t surface_attached;
  uint32_t presentation_observation_supported;
} goal_jak2_metal_host_metrics;

/*
 * The host and every copied callback are single-owner-thread APIs. The development app keeps
 * creation, runtime start/tick, flushing, metric reads, layer changes, and teardown on main.
 */

/*! Create the process-singleton, nil-layer Jak 2 policy-dispatch host. */
goal_jak2_metal_host* goal_jak2_metal_host_create(void);

/*! Attach or detach the main-thread-owned presentation layer while the runtime is idle. */
int goal_jak2_metal_host_set_layer(goal_jak2_metal_host* host,
                                   goal_jak2_metal_layer_ref layer);

/*! Copy the app-owned callback table into `out`; the runtime copies it again during start. */
int goal_jak2_metal_host_copy_gfx_host(goal_jak2_metal_host* host, goal_gfx_host* out);

/*! Supply the CADisplayLink target timestamp used for the next presentation request. */
void goal_jak2_metal_host_set_presentation_time(goal_jak2_metal_host* host,
                                                double presentation_time);

/*!
 * Consume DMA chains copied by the GOAL-facing callbacks. Call only after the GOAL dispatcher
 * has returned to a native OS stack; Metal framework calls are intentionally deferred here.
 */
int goal_jak2_metal_host_flush_pending(goal_jak2_metal_host* host);

/*! Discard copied operations from a failed runtime start/tick without submitting partial work. */
int goal_jak2_metal_host_discard_pending(goal_jak2_metal_host* host);

/*! Wait for the latest attached-surface command buffer and record its final status. */
int goal_jak2_metal_host_wait_until_idle(goal_jak2_metal_host* host);

int goal_jak2_metal_host_get_metrics(goal_jak2_metal_host* host,
                                     goal_jak2_metal_host_metrics* out);

/*! Call only after goal_jak2_runtime_shutdown, when the copied callbacks are no longer reachable. */
void goal_jak2_metal_host_destroy(goal_jak2_metal_host* host);

const char* goal_jak2_metal_host_last_error(goal_jak2_metal_host* host);

#ifdef __cplusplus
}  // extern "C"
#endif
