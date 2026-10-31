#pragma once

/*!
 * @file jak2_presentation_batch.h
 * Coalesces the presentation of a catch-up batch of Jak II dispatcher frames.
 *
 * Every frame still runs and every chain it sends is still rendered and committed; only the
 * drawable request and present of chains before the last frame are deferred to the host, through
 * the optional gfx host hooks. A batch that completes asks the host to present the newest frame
 * once if it is still pending. A batch that stops early or throws only cancels the pending
 * presentation: that cleanup never requests a drawable or GPU work, although chains already sent
 * by the last frame may have presented inline before the failure.
 */

#include <cstdint>

#include "game/kernel/core/gfx_host.h"

namespace jak2_presentation_batch {

/*!
 * Run `frames` dispatcher frames as `run_frame(frame)`, which returns false to stop the batch.
 * A batch of zero or one frame makes no presentation hook calls. Returns whether every frame ran.
 */
template <typename RunFrame>
bool run(uint32_t frames, RunFrame&& run_frame) {
  if (frames < 2) {
    return frames == 0 || run_frame(0u);
  }

  struct CancelUnlessFinished {
    bool finished = false;
    ~CancelUnlessFinished() noexcept {
      if (!finished) {
        goal_gfx_host_finish_presentation_batch(0);
      }
    }
  } batch;

  for (uint32_t frame = 0; frame < frames; frame++) {
    goal_gfx_host_set_presentation_deferred(frame + 1 < frames ? 1 : 0);
    if (!run_frame(frame)) {
      return false;
    }
  }
  batch.finished = true;
  goal_gfx_host_finish_presentation_batch(1);
  return true;
}

}  // namespace jak2_presentation_batch
