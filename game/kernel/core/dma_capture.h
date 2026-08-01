#pragma once

/*!
 * @file dma_capture.h
 * The seam where a frame's work leaves GOAL: `__send-gfx-dma-chain`. See dma_capture.cpp.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! What the frames actually built. Nothing is drawn; these are measurements of the chains. */
typedef struct goal_gfx_dma_stats {
  int chains;              /*! chains GOAL handed to __send-gfx-dma-chain */
  uint32_t last_bytes;     /*! size of the most recent one, after following it */
  uint32_t largest_bytes;  /*! the largest one seen */
  uint32_t captured_bytes; /*! bytes written by a capture, or 0 */
} goal_gfx_dma_stats;

/*!
 * Install `__send-gfx-dma-chain`, replacing the machine-layer stub. It follows the chain with the
 * same `FixedChunkDmaCopier` the renderer uses, records its size, and drops it. Clears the stats.
 */
void goal_gfx_dma_install(void);

/*!
 * Write the next chain to `path`, in `FixedChunkDmaCopier::serialize_last_result` format, so a
 * renderer can replay one real frame of Jak 1 DMA. One chain is captured and then the path is
 * cleared. Pass NULL or "" to capture nothing.
 *
 * A captured chain is derived from the player's own game data. It goes where the caller says and
 * never into the repository.
 */
void goal_gfx_dma_set_capture_path(const char* path);

void goal_gfx_dma_get_stats(goal_gfx_dma_stats* out);

#ifdef __cplusplus
}  // extern "C"
#endif
