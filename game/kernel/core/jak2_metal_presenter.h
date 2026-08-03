#pragma once

#include <stdint.h>

#ifdef __OBJC__
@class CAMetalLayer;
typedef CAMetalLayer* goal_jak2_metal_layer;
#else
typedef void* goal_jak2_metal_layer;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct goal_jak2_metal_stats {
  uint64_t render_attempts;
  uint64_t chains_rendered;
  uint64_t buckets_dispatched;
  uint64_t drawables_acquired;
  uint64_t drawable_misses;
  uint64_t command_buffers_committed;
  uint64_t command_buffers_completed;
  uint64_t command_buffer_errors;
  uint64_t late_present_submissions;
  uint64_t submissions;
  uint64_t presentations_completed;
  uint64_t presentation_drops;
  uint64_t presentation_order_mismatches;
  uint64_t skipped_bucket_bytes;
  int32_t draw_calls;
  int32_t triangles;
  int32_t last_command_buffer_status;
  int64_t last_command_buffer_error_code;
} goal_jak2_metal_stats;

/*! Initialize the development presenter around the app-owned CAMetalLayer. */
int goal_jak2_metal_presenter_start(goal_jak2_metal_layer layer);

/*! Submit the public synthetic 327-bucket Jak II chain for one display tick. */
int goal_jak2_metal_presenter_render(double target_presentation_time);

/*! Wait up to five seconds for the one submitted proof frame to finish on the GPU. */
int goal_jak2_metal_presenter_wait_for_completion(void);

/*! Copy the latest bounded renderer counters. */
int goal_jak2_metal_presenter_get_stats(goal_jak2_metal_stats* out);

const char* goal_jak2_metal_presenter_last_error(void);
void goal_jak2_metal_presenter_shutdown(void);

#ifdef __cplusplus
}  // extern "C"
#endif
