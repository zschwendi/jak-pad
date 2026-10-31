#pragma once

/*!
 * @file metal_present_coalescer.h
 * Portable pending-presentation state for Jak II catch-up batches. A deferred chain still renders
 * and commits in full; only its drawable request is skipped. The newest deferred chain's present
 * inputs are kept by value so one present can be issued after the batch.
 */

namespace metal_renderer {

// The plain values the present pass and schedule_present read, plus the game target size the
// present pass samples. No pointers: nothing here outlives the chain that produced it.
struct PresentSnapshot {
  int game_res_w = 0;
  int game_res_h = 0;
  int draw_region_w = 0;
  int draw_region_h = 0;
  float pmode_alp = 1.f;
  int brightness_contrast_color = 0;
  int brightness_contrast_alpha = 128;
  double min_present_duration = 0.0;
  double presentation_time = 0.0;
};

enum class PresentFlushAction {
  None,     // nothing pending: no command buffer, no drawable, no counter
  Skip,     // pending, but the current game target no longer matches the stored size
  Present,  // present the current game target with the stored inputs
};

class PresentCoalescer {
 public:
  // Called once per committed layer-backed chain. A deferred chain becomes the pending frame (latest
  // wins). Any non-deferred chain clears pending, whether it acquired a drawable or missed one.
  void on_chain_committed(bool deferred, const PresentSnapshot& snapshot) {
    if (deferred) {
      m_pending = snapshot;
      m_has_pending = true;
    } else {
      m_has_pending = false;
    }
  }

  // Consumes the pending frame. Always leaves nothing pending.
  PresentFlushAction take_flush(int color_w, int color_h, PresentSnapshot* out) {
    if (!m_has_pending) {
      return PresentFlushAction::None;
    }
    m_has_pending = false;
    if (color_w != m_pending.game_res_w || color_h != m_pending.game_res_h) {
      return PresentFlushAction::Skip;
    }
    if (out) {
      *out = m_pending;
    }
    return PresentFlushAction::Present;
  }

  void reset() { m_has_pending = false; }
  bool has_pending() const { return m_has_pending; }

 private:
  PresentSnapshot m_pending;
  bool m_has_pending = false;
};

}  // namespace metal_renderer
