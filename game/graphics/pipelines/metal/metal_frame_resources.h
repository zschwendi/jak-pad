#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

inline constexpr std::size_t kMetalOrdinaryFrameResourceSlotCount = 3;
inline constexpr std::size_t kMetalExternalFrameResourceSlot =
    kMetalOrdinaryFrameResourceSlotCount;
inline constexpr std::size_t kMetalFrameResourceSlotCount =
    kMetalOrdinaryFrameResourceSlotCount + 1;

class MetalOrdinaryFrameSlotRing {
 public:
  struct Acquisition {
    std::size_t slot = 0;
    std::uint64_t previous_submission = 0;
  };

  Acquisition acquire() {
    const std::size_t slot = m_next_slot;
    m_next_slot = (m_next_slot + 1) % kMetalOrdinaryFrameResourceSlotCount;
    return {slot, m_submissions[slot]};
  }

  void record_submission(std::size_t slot, std::uint64_t submission) {
    m_submissions.at(slot) = submission;
  }

 private:
  std::array<std::uint64_t, kMetalOrdinaryFrameResourceSlotCount> m_submissions = {};
  std::size_t m_next_slot = 0;
};

class MetalMonotonicSubmissionCompletion {
 public:
  void complete(std::uint64_t submission) {
    if (submission > m_completed_submission) {
      m_completed_submission = submission;
    }
  }

  bool includes(std::uint64_t submission) const {
    return m_completed_submission >= submission;
  }

 private:
  std::uint64_t m_completed_submission = 0;
};
