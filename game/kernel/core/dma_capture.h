#pragma once

/*!
 * @file dma_capture.h
 * The seam where a frame's work leaves GOAL: `__send-gfx-dma-chain`. See dma_capture.cpp, which
 * also documents the capture file format.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! What the frames actually built. Nothing is drawn; these are measurements of the chains. */
typedef struct goal_gfx_dma_stats {
  int chains;              /*! chains GOAL handed to __send-gfx-dma-chain */
  uint32_t last_bytes;     /*! chunks the last chain touched, in bytes: what a capture would hold */
  uint32_t largest_bytes;  /*! the largest of those */
  uint32_t captured_bytes; /*! bytes written by captures, or 0 */
  int captures;            /*! capture files written */

  /*! The DMA payload itself - the bytes the chain's tags actually transfer, which is what says
   *  whether a frame drew anything. `last_bytes` above is chunk-granular and says only how much
   *  memory the chain was spread across. */
  uint32_t last_payload_bytes;
  uint32_t largest_payload_bytes;
  int largest_payload_frame;   /*! the 1-based frame that built it */
  int last_texture_uploads;    /*! PC_PORT texture-upload packets in the last chain */
  int largest_payload_uploads; /*! ... and in the largest-payload one */
} goal_gfx_dma_stats;

/*! One frame's chain, measured. Recorded for every frame; see goal_gfx_dma_get_frame. */
typedef struct goal_gfx_dma_frame_summary {
  int frame;              /*! 1-based */
  uint32_t copied_bytes;  /*! chunk-granular size of the chain's memory */
  uint32_t payload_bytes; /*! bytes the chain's tags transfer */
  int tags;
  int texture_uploads; /*! PC_PORT (vif1 == 3) texture-upload packets */
  /*! 16-byte bucket-array segments the walk found, or 0 if the chain was not the bucket chain.
   *  Jak 1 has 70 buckets; the walk cannot know that, so anything past index 69 is the chain's
   *  ending data, which the renderer's bucket dispatch also walks past. */
  int buckets;
} goal_gfx_dma_frame_summary;

/*! One bucket of one captured frame. Only frames that were captured keep this detail. */
typedef struct goal_gfx_dma_bucket_summary {
  int bucket;
  int transfers;
  uint32_t payload_bytes;
  int texture_uploads;
} goal_gfx_dma_bucket_summary;

/*!
 * Install `__send-gfx-dma-chain`, replacing the machine-layer stub. It follows the chain with the
 * same `FixedChunkDmaCopier` the renderer uses, measures it, and drops it. Clears the stats.
 */
void goal_gfx_dma_install(void);

/*!
 * Write frame `frame`'s capture to exactly `path`. `frame` is 1-based and counts calls to
 * `__send-gfx-dma-chain`, so frame 1 is the first chain after boot - which draws almost nothing.
 * Pass NULL or "" to capture nothing.
 */
void goal_gfx_dma_capture_frame_to_file(const char* path, int frame);

/*!
 * Write each of `count` frames' captures into `dir`, named `dma-frame-<frame>.gpdma`. Frames are
 * 1-based. Adds to whatever was already requested.
 */
void goal_gfx_dma_capture_frames_to_dir(const char* dir, const int* frames, int count);

/*!
 * Capture the next `count` frames whose payload reaches `min_payload_bytes`, into `dir`, named the
 * same way. Which frame the game draws a given thing on is not fixed - the level loader runs off
 * the wall clock, so the frame numbers move between runs - so asking by content is how a caller
 * gets the frame it meant rather than the frame it guessed.
 */
void goal_gfx_dma_capture_frames_over(const char* dir, uint32_t min_payload_bytes, int count);

/*! How many frames have been measured. */
int goal_gfx_dma_frame_count(void);

/*! Copy frame `frame`'s summary (1-based) into `out`. Returns 0 if there is no such frame. */
int goal_gfx_dma_get_frame(int frame, goal_gfx_dma_frame_summary* out);

/*!
 * Copy bucket `bucket` of frame `frame` into `out`. Returns 0 unless that frame was captured -
 * per-bucket detail is kept only for captured frames.
 */
int goal_gfx_dma_get_bucket(int frame, int bucket, goal_gfx_dma_bucket_summary* out);

void goal_gfx_dma_get_stats(goal_gfx_dma_stats* out);

#ifdef __cplusplus
}  // extern "C"
#endif
