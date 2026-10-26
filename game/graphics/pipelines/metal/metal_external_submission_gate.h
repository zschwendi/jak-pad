#pragma once

#include <cstdint>
#include <mutex>

#include "game/graphics/pipelines/metal/metal_frame_resources.h"

namespace metal_renderer {

enum class ExternalSubmissionReservation {
  reserved,
  busy,
  failed,
};

class ExternalSubmissionGate {
 public:
  ExternalSubmissionReservation reserve(bool prior_stream_busy) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_failure_latched) {
      m_failure_latched = false;
      return ExternalSubmissionReservation::failed;
    }
    if (prior_stream_busy || m_state != State::idle) {
      return ExternalSubmissionReservation::busy;
    }
    m_state = State::reserved;
    return ExternalSubmissionReservation::reserved;
  }

  bool is_reserved() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state == State::reserved;
  }

  void cancel() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state == State::reserved) {
      m_state = State::idle;
    }
  }

  bool submit(std::uint64_t submission_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (submission_id == 0 || m_state != State::reserved) {
      return false;
    }
    m_state = State::submitted;
    m_submission_id = submission_id;
    return true;
  }

  void complete(std::uint64_t submission_id, bool succeeded) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != State::submitted || submission_id != m_submission_id) {
      return;
    }
    m_state = State::idle;
    m_submission_id = 0;
    m_failure_latched = !succeeded;
  }

 private:
  enum class State {
    idle,
    reserved,
    submitted,
  };

  mutable std::mutex m_mutex;
  State m_state = State::idle;
  std::uint64_t m_submission_id = 0;
  bool m_failure_latched = false;
};

// Holds back external frames after presentation fell back while an external submission was still
// in flight. That submission may still read the quarantined stream pages and the external
// frame-resource slot, so reservations stay busy until its completion is observed. Nothing waits
// on the GPU. Callers serialize access.
class ExternalStreamQuarantine {
 public:
  void hold(std::uint64_t submission) {
    if (submission > m_submission) {
      m_submission = submission;
    }
  }

  // Ends the hold once the held submission has completed; returns whether it is still in flight.
  bool still_in_flight(const MetalMonotonicSubmissionCompletion& completion) {
    if (m_submission && completion.includes(m_submission)) {
      m_submission = 0;
    }
    return m_submission != 0;
  }

 private:
  std::uint64_t m_submission = 0;
};

}  // namespace metal_renderer
