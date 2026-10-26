#include "game/graphics/pipelines/metal/metal_external_submission_gate.h"

#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

using metal_renderer::ExternalStreamQuarantine;
using metal_renderer::ExternalSubmissionGate;
using metal_renderer::ExternalSubmissionReservation;

// Composes the real gate, quarantine, and completion tracker in MetalRenderer's order. It is a
// model of reserve/submit/fallback, not MetalRenderer itself; metal-proof covers the renderer.
struct ExternalStreamModel {
  ExternalSubmissionGate gate;
  ExternalStreamQuarantine quarantine;
  MetalMonotonicSubmissionCompletion completion;
  std::uint64_t last_external_submission = 0;
  std::uint64_t submission_count = 0;
  int quarantined_page_sets = 0;

  ExternalSubmissionReservation reserve() {
    if (quarantine.still_in_flight(completion)) {
      return ExternalSubmissionReservation::busy;
    }
    quarantined_page_sets = 0;
    const bool prior_stream_busy =
        last_external_submission && !completion.includes(last_external_submission);
    return gate.reserve(prior_stream_busy);
  }

  std::uint64_t submit_stereo() {
    const std::uint64_t submission = ++submission_count;
    check(gate.submit(submission), "a reserved stereo frame submits");
    last_external_submission = submission;
    return submission;
  }

  // The immersive menu plane renders through the external stream without holding the gate.
  std::uint64_t submit_menu_plane() {
    const std::uint64_t submission = ++submission_count;
    last_external_submission = submission;
    return submission;
  }

  void complete_stereo(std::uint64_t submission, bool succeeded) {
    completion.complete(submission);
    gate.complete(submission, succeeded);
  }

  void prepare_fallback() {
    if (!last_external_submission || completion.includes(last_external_submission)) {
      return;
    }
    quarantined_page_sets++;
    quarantine.hold(last_external_submission);
    last_external_submission = 0;
  }
};

void check_reentry_after_exit_with_pending_stereo() {
  ExternalStreamModel model;
  check(model.reserve() == ExternalSubmissionReservation::reserved, "first entry reserves");
  const auto first = model.submit_stereo();
  model.prepare_fallback();
  check(model.quarantined_page_sets == 1, "exit with pending stereo quarantines its pages");
  for (int update = 0; update < 8; ++update) {
    check(model.reserve() == ExternalSubmissionReservation::busy,
          "re-entry while the exit frame is in flight is busy, never a permanent failure");
  }
  check(model.quarantined_page_sets == 1, "pending pages stay quarantined while busy");
  model.complete_stereo(first, true);
  check(model.reserve() == ExternalSubmissionReservation::reserved,
        "second entry reserves once the exit frame completes");
  check(model.quarantined_page_sets == 0, "completed quarantined pages are released");
  model.gate.cancel();
}

void check_repeated_immersive_cycles() {
  ExternalStreamModel model;
  for (int cycle = 0; cycle < 6; ++cycle) {
    check(model.reserve() == ExternalSubmissionReservation::reserved, "each entry reserves");
    const auto rendered = model.submit_stereo();
    model.complete_stereo(rendered, true);
    check(model.reserve() == ExternalSubmissionReservation::reserved, "steady frames reserve");
    const auto exit_frame = model.submit_stereo();
    model.prepare_fallback();
    check(model.reserve() == ExternalSubmissionReservation::busy, "each exit frame holds re-entry");
    model.complete_stereo(exit_frame, true);
    check(model.quarantined_page_sets == 1, "at most one quarantined page set is retained");
  }
  check(model.reserve() == ExternalSubmissionReservation::reserved,
        "the final re-entry reserves after repeated cycles");
  check(model.quarantined_page_sets == 0, "repeated cycles leave no quarantined pages behind");
  model.gate.cancel();
}

void check_normal_fallback_without_pending_work() {
  ExternalStreamModel model;
  check(model.reserve() == ExternalSubmissionReservation::reserved, "entry reserves");
  model.complete_stereo(model.submit_stereo(), true);
  model.prepare_fallback();
  check(model.quarantined_page_sets == 0, "a completed stream needs no quarantine");
  check(model.reserve() == ExternalSubmissionReservation::reserved,
        "re-entry after an idle fallback reserves immediately");
  model.gate.cancel();
  model.prepare_fallback();
  check(model.reserve() == ExternalSubmissionReservation::reserved,
        "fallback after a cancelled reservation keeps the stream available");
  model.gate.cancel();
}

void check_failed_exit_frame_is_reported_once() {
  ExternalStreamModel model;
  check(model.reserve() == ExternalSubmissionReservation::reserved, "entry reserves");
  const auto exit_frame = model.submit_stereo();
  model.prepare_fallback();
  check(model.reserve() == ExternalSubmissionReservation::busy, "pending failed frame is busy");
  model.complete_stereo(exit_frame, false);
  check(model.reserve() == ExternalSubmissionReservation::failed,
        "the exit frame's GPU failure reaches the next entry");
  check(model.reserve() == ExternalSubmissionReservation::reserved,
        "a reported failure does not disable later entries");
  model.gate.cancel();
}

void check_late_and_out_of_order_completion() {
  ExternalStreamModel model;
  check(model.reserve() == ExternalSubmissionReservation::reserved, "entry reserves");
  const auto older = model.submit_stereo();
  model.complete_stereo(older, true);
  check(model.reserve() == ExternalSubmissionReservation::reserved, "next frame reserves");
  const auto exit_frame = model.submit_stereo();
  model.prepare_fallback();
  model.complete_stereo(older, true);
  check(model.reserve() == ExternalSubmissionReservation::busy,
        "a repeated older completion does not release the held submission");
  model.complete_stereo(exit_frame, true);
  check(model.reserve() == ExternalSubmissionReservation::reserved, "held completion releases");
  const auto reentry_frame = model.submit_stereo();
  model.complete_stereo(exit_frame, false);
  check(model.reserve() == ExternalSubmissionReservation::busy,
        "a late quarantined completion cannot complete or fail the new frame");
  model.complete_stereo(reentry_frame, true);
  check(model.reserve() == ExternalSubmissionReservation::reserved,
        "the new frame's own completion frees the stream");
  model.gate.cancel();

  ExternalStreamQuarantine quarantine;
  MetalMonotonicSubmissionCompletion completion;
  quarantine.hold(12);
  quarantine.hold(11);
  completion.complete(11);
  check(quarantine.still_in_flight(completion), "the newest held submission governs release");
  completion.complete(12);
  check(!quarantine.still_in_flight(completion), "the newest held completion releases");
  check(!quarantine.still_in_flight(completion), "a released quarantine stays released");
}

void check_menu_plane_exit_frame_is_held() {
  ExternalStreamModel model;
  check(model.reserve() == ExternalSubmissionReservation::reserved, "menu plane proves idle");
  model.gate.cancel();
  const auto menu_frame = model.submit_menu_plane();
  model.prepare_fallback();
  check(model.quarantined_page_sets == 1, "a pending menu-plane frame is quarantined");
  check(model.reserve() == ExternalSubmissionReservation::busy,
        "the gate-less menu-plane frame still holds the external slot");
  model.completion.complete(menu_frame);
  check(model.reserve() == ExternalSubmissionReservation::reserved,
        "menu-plane completion permits re-entry");
  model.gate.cancel();
}

}  // namespace

int main() {
  ExternalSubmissionGate gate;
  check(gate.reserve(true) == ExternalSubmissionReservation::busy,
        "an in-flight stream returns busy without reserving");
  check(gate.reserve(false) == ExternalSubmissionReservation::reserved,
        "an idle stream reserves");
  check(gate.reserve(false) == ExternalSubmissionReservation::busy,
        "a second reserve is busy");
  gate.cancel();
  check(gate.reserve(false) == ExternalSubmissionReservation::reserved,
        "cancel recovers an unsubmitted reservation");

  gate.cancel();
  check(gate.reserve(false) == ExternalSubmissionReservation::reserved,
        "render-failure setup reserves");
  check(gate.submit(6), "render failure submits the caller's initialized pair once");
  check(!gate.submit(6), "render-failure cleanup cannot commit the pair twice");
  gate.complete(6, true);
  check(gate.reserve(false) == ExternalSubmissionReservation::reserved,
        "render-failure initialization submission cannot wedge the next frame");
  check(gate.submit(7), "a reserved frame submits once");
  check(!gate.submit(8), "a submitted frame cannot submit twice");
  check(gate.reserve(false) == ExternalSubmissionReservation::busy,
        "an incomplete submitted frame is busy");

  gate.complete(7, false);
  check(gate.reserve(false) == ExternalSubmissionReservation::failed,
        "an asynchronous command failure is reported once");
  check(gate.reserve(false) == ExternalSubmissionReservation::reserved,
        "the consumed failure latch permits explicit later recovery");
  check(gate.submit(8), "the recovery reservation submits");
  gate.complete(8, true);
  check(gate.reserve(false) == ExternalSubmissionReservation::reserved,
        "successful completion returns the gate to idle");
  gate.cancel();

  check_reentry_after_exit_with_pending_stereo();
  check_repeated_immersive_cycles();
  check_normal_fallback_without_pending_work();
  check_failed_exit_frame_is_reported_once();
  check_late_and_out_of_order_completion();
  check_menu_plane_exit_frame_is_held();

  std::cout << "Metal external submission gate check passed\n";
  return 0;
}
