#include "game/graphics/pipelines/metal/metal_present_coalescer.h"

#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

metal_renderer::PresentSnapshot snapshot(int w, int h, float alp, double duration) {
  metal_renderer::PresentSnapshot out;
  out.game_res_w = w;
  out.game_res_h = h;
  out.draw_region_w = w * 2;
  out.draw_region_h = h * 2;
  out.pmode_alp = alp;
  out.brightness_contrast_color = 3;
  out.brightness_contrast_alpha = 99;
  out.min_present_duration = duration;
  out.presentation_time = 0.0;
  return out;
}

}  // namespace

int main() {
  using metal_renderer::PresentCoalescer;
  using metal_renderer::PresentFlushAction;
  using metal_renderer::PresentSnapshot;

  {
    // N <= 1 and zero-chain callbacks: nothing pending, the flush is a no-op.
    PresentCoalescer c;
    PresentSnapshot out = snapshot(1, 1, 0.5f, 1.0);
    check(c.take_flush(640, 480, &out) == PresentFlushAction::None, "empty flush is a no-op");
    check(out.game_res_w == 1, "empty flush does not write the snapshot");
    c.on_chain_committed(false, snapshot(640, 480, 1.f, 1.0 / 60.0));
    check(!c.has_pending(), "a single presenting chain leaves nothing pending");
    check(c.take_flush(640, 480, &out) == PresentFlushAction::None,
          "flush after a single presenting chain is a no-op");
  }
  {
    // Latest wins across deferred chains, and the flush consumes it exactly once.
    PresentCoalescer c;
    c.on_chain_committed(true, snapshot(640, 480, 0.25f, 1.0 / 120.0));
    c.on_chain_committed(true, snapshot(640, 480, 0.75f, 1.0 / 60.0));
    PresentSnapshot out;
    check(c.take_flush(640, 480, &out) == PresentFlushAction::Present,
          "a deferred final chain is presented by the flush");
    check(out.pmode_alp == 0.75f && out.min_present_duration == 1.0 / 60.0 &&
              out.draw_region_w == 1280 && out.draw_region_h == 960 &&
              out.brightness_contrast_color == 3 && out.brightness_contrast_alpha == 99,
          "the newest deferred chain's present inputs win");
    check(!c.has_pending(), "flush clears pending");
    check(c.take_flush(640, 480, &out) == PresentFlushAction::None, "a second flush is a no-op");
  }
  {
    // A final non-deferred chain clears pending whether it acquired or missed its drawable: the
    // renderer reports both the same way, so the flush never retries nextDrawable.
    PresentCoalescer c;
    c.on_chain_committed(true, snapshot(640, 480, 1.f, 0.0));
    c.on_chain_committed(false, snapshot(640, 480, 1.f, 0.0));
    check(c.take_flush(640, 480, nullptr) == PresentFlushAction::None,
          "a final acquired-or-missed chain leaves nothing for the flush");
  }
  {
    // The current game target no longer matches the deferred frame: skip and clear.
    PresentCoalescer c;
    c.on_chain_committed(true, snapshot(640, 480, 1.f, 0.0));
    check(c.take_flush(800, 600, nullptr) == PresentFlushAction::Skip, "size mismatch skips");
    check(!c.has_pending(), "size mismatch clears pending");
    check(c.take_flush(640, 480, nullptr) == PresentFlushAction::None,
          "a skipped frame is not presented later");
  }
  {
    // finish(0) (cancelled batch), detach, or destroy resets without presenting.
    PresentCoalescer c;
    c.on_chain_committed(true, snapshot(640, 480, 1.f, 0.0));
    c.reset();
    check(c.take_flush(640, 480, nullptr) == PresentFlushAction::None, "reset clears pending");
  }
  {
    // A cancelled batch cannot leak into the next one: the next batch presents only its own frame.
    PresentCoalescer c;
    c.on_chain_committed(true, snapshot(640, 480, 0.1f, 0.0));
    c.reset();
    c.on_chain_committed(true, snapshot(640, 480, 0.9f, 0.0));
    PresentSnapshot out;
    check(c.take_flush(640, 480, &out) == PresentFlushAction::Present && out.pmode_alp == 0.9f,
          "after a cancel, the next batch presents its own newest frame");
  }
  {
    // A normal batch whose final tick sent no presenting chain: finish(1) presents the newest frame.
    PresentCoalescer c;
    for (int i = 0; i < 4; ++i) {
      c.on_chain_committed(true, snapshot(640, 480, 1.f, 0.0));
    }
    check(c.take_flush(640, 480, nullptr) == PresentFlushAction::Present,
          "a normal finish presents the newest deferred chain once");
  }

  std::cout << "metal_present_coalescer_test: all checks passed\n";
  return 0;
}
