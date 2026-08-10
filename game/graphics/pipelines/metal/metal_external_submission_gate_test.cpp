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

}  // namespace

int main() {
  using metal_renderer::ExternalSubmissionGate;
  using metal_renderer::ExternalSubmissionReservation;

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

  std::cout << "Metal external submission gate check passed\n";
  return 0;
}
