#pragma once

#include <cstdint>
#include <mutex>

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

}  // namespace metal_renderer
