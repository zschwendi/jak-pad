#pragma once

#include <cstddef>
#include <cstring>
#include <limits>

#include "common/dma/dma.h"

enum class DmaChainValidationError {
  None,
  NullMemory,
  MisalignedTag,
  TagHeaderOutOfRange,
  ScratchpadAddress,
  CntAddress,
  MisalignedTarget,
  InlinePayloadOutOfRange,
  ReferencePayloadOutOfRange,
  ControlTargetOutOfRange,
  AddressArithmeticOverflow,
  CallStackOverflow,
  ReturnStackUnderflow,
  Cycle,
  TagBudgetExceeded,
};

struct DmaChainValidationResult {
  DmaChainValidationError error = DmaChainValidationError::None;
  u32 error_offset = 0;
  std::size_t tags_processed = 0;

  explicit operator bool() const { return error == DmaChainValidationError::None; }
};

// A normal graphics frame has hundreds or thousands of tags. This deliberately high limit keeps
// malformed input from spending unbounded CPU time while allowing legal CALL/RET subroutines to
// be revisited with different return stacks.
constexpr std::size_t kDmaChainValidationDefaultTagBudget = 1u << 20;

inline const char* dma_chain_validation_error_message(DmaChainValidationError error) {
  switch (error) {
    case DmaChainValidationError::None:
      return "valid";
    case DmaChainValidationError::NullMemory:
      return "memory is null";
    case DmaChainValidationError::MisalignedTag:
      return "tag is not 16-byte aligned";
    case DmaChainValidationError::TagHeaderOutOfRange:
      return "tag header is outside memory";
    case DmaChainValidationError::ScratchpadAddress:
      return "scratchpad DMA is unsupported";
    case DmaChainValidationError::CntAddress:
      return "CNT tag has a nonzero address";
    case DmaChainValidationError::MisalignedTarget:
      return "DMA target is not 16-byte aligned";
    case DmaChainValidationError::InlinePayloadOutOfRange:
      return "inline payload is outside memory";
    case DmaChainValidationError::ReferencePayloadOutOfRange:
      return "reference payload is outside memory";
    case DmaChainValidationError::ControlTargetOutOfRange:
      return "control-flow target header is outside memory";
    case DmaChainValidationError::AddressArithmeticOverflow:
      return "DMA address arithmetic overflows the follower offset";
    case DmaChainValidationError::CallStackOverflow:
      return "CALL stack exceeds two entries";
    case DmaChainValidationError::ReturnStackUnderflow:
      return "RET has no matching CALL";
    case DmaChainValidationError::Cycle:
      return "DMA control flow contains a cycle";
    case DmaChainValidationError::TagBudgetExceeded:
      return "DMA chain exceeds the validation tag budget";
  }
  return "unknown DMA validation error";
}

namespace dma_chain_validation_detail {

struct State {
  u32 tag_offset = 0;
  u32 return_offsets[2] = {0, 0};
  u8 stack_depth = 0;
};

inline bool states_equal(const State& lhs, const State& rhs) {
  if (lhs.tag_offset != rhs.tag_offset || lhs.stack_depth != rhs.stack_depth) {
    return false;
  }
  for (u8 i = 0; i < lhs.stack_depth; ++i) {
    if (lhs.return_offsets[i] != rhs.return_offsets[i]) {
      return false;
    }
  }
  return true;
}

inline bool range_is_valid(u64 offset, u64 length, std::size_t memory_size) {
  return offset <= memory_size && length <= memory_size - offset;
}

inline bool offset_fits_follower(u64 offset) {
  return offset <= std::numeric_limits<u32>::max();
}

struct StepResult {
  DmaChainValidationError error = DmaChainValidationError::None;
  State next;
  bool ended = false;
};

inline StepResult step(const u8* memory, std::size_t memory_size, const State& current) {
  StepResult result;
  result.next = current;

  if ((current.tag_offset & 15) != 0) {
    result.error = DmaChainValidationError::MisalignedTag;
    return result;
  }
  if (!range_is_valid(current.tag_offset, 16, memory_size)) {
    result.error = DmaChainValidationError::TagHeaderOutOfRange;
    return result;
  }

  u64 raw_tag = 0;
  std::memcpy(&raw_tag, memory + current.tag_offset, sizeof(raw_tag));
  const DmaTag tag(raw_tag);
  if (tag.spr) {
    result.error = DmaChainValidationError::ScratchpadAddress;
    return result;
  }

  const u64 inline_offset = static_cast<u64>(current.tag_offset) + 16;
  const u64 payload_bytes = static_cast<u64>(tag.qwc) * 16;
  if (!offset_fits_follower(inline_offset) ||
      payload_bytes > std::numeric_limits<u32>::max() - inline_offset) {
    result.error = DmaChainValidationError::AddressArithmeticOverflow;
    return result;
  }
  const u64 inline_end = inline_offset + payload_bytes;

  const auto validate_inline_payload = [&]() {
    return range_is_valid(inline_offset, payload_bytes, memory_size);
  };
  const auto validate_target_alignment = [](u64 offset) { return (offset & 15) == 0; };
  const auto validate_control_target = [&](u64 offset) {
    return range_is_valid(offset, 16, memory_size);
  };
  const auto validate_reference_payload = [&]() {
    return range_is_valid(tag.addr, payload_bytes, memory_size);
  };

  switch (tag.kind) {
    case DmaTag::Kind::CNT:
      if (tag.addr != 0) {
        result.error = DmaChainValidationError::CntAddress;
      } else if (!validate_inline_payload()) {
        result.error = DmaChainValidationError::InlinePayloadOutOfRange;
      } else {
        result.next.tag_offset = static_cast<u32>(inline_end);
      }
      break;
    case DmaTag::Kind::NEXT:
      if (!validate_target_alignment(tag.addr)) {
        result.error = DmaChainValidationError::MisalignedTarget;
      } else if (!validate_inline_payload()) {
        result.error = DmaChainValidationError::InlinePayloadOutOfRange;
      } else if (!validate_control_target(tag.addr)) {
        result.error = DmaChainValidationError::ControlTargetOutOfRange;
      } else {
        result.next.tag_offset = tag.addr;
      }
      break;
    case DmaTag::Kind::REF:
    case DmaTag::Kind::REFS:
      if (!validate_target_alignment(tag.addr)) {
        result.error = DmaChainValidationError::MisalignedTarget;
      } else if (!validate_reference_payload()) {
        result.error = DmaChainValidationError::ReferencePayloadOutOfRange;
      } else {
        result.next.tag_offset = static_cast<u32>(inline_offset);
      }
      break;
    case DmaTag::Kind::REFE:
      if (!validate_target_alignment(tag.addr)) {
        result.error = DmaChainValidationError::MisalignedTarget;
      } else if (!validate_reference_payload()) {
        result.error = DmaChainValidationError::ReferencePayloadOutOfRange;
      } else {
        result.ended = true;
      }
      break;
    case DmaTag::Kind::CALL:
      if (!validate_target_alignment(tag.addr)) {
        result.error = DmaChainValidationError::MisalignedTarget;
      } else if (!validate_inline_payload()) {
        result.error = DmaChainValidationError::InlinePayloadOutOfRange;
      } else if (current.stack_depth == 2) {
        result.error = DmaChainValidationError::CallStackOverflow;
      } else if (!validate_control_target(tag.addr)) {
        result.error = DmaChainValidationError::ControlTargetOutOfRange;
      } else {
        result.next.return_offsets[result.next.stack_depth++] = static_cast<u32>(inline_end);
        result.next.tag_offset = tag.addr;
      }
      break;
    case DmaTag::Kind::RET:
      if (!validate_inline_payload()) {
        result.error = DmaChainValidationError::InlinePayloadOutOfRange;
      } else if (current.stack_depth == 0) {
        result.error = DmaChainValidationError::ReturnStackUnderflow;
      } else {
        const u32 return_offset = result.next.return_offsets[--result.next.stack_depth];
        result.next.return_offsets[result.next.stack_depth] = 0;
        if (!validate_target_alignment(return_offset)) {
          result.error = DmaChainValidationError::MisalignedTarget;
        } else if (!validate_control_target(return_offset)) {
          result.error = DmaChainValidationError::ControlTargetOutOfRange;
        } else {
          result.next.tag_offset = return_offset;
        }
      }
      break;
    case DmaTag::Kind::END:
      if (!validate_inline_payload()) {
        result.error = DmaChainValidationError::InlinePayloadOutOfRange;
      } else {
        result.ended = true;
      }
      break;
  }
  return result;
}

}  // namespace dma_chain_validation_detail

inline DmaChainValidationResult validate_dma_chain(
    const void* base,
    std::size_t memory_size,
    u32 start_offset,
    std::size_t max_tags = kDmaChainValidationDefaultTagBudget) noexcept {
  DmaChainValidationResult result;
  result.error_offset = start_offset;
  if (!base) {
    result.error = DmaChainValidationError::NullMemory;
    return result;
  }

  using dma_chain_validation_detail::State;
  State current;
  current.tag_offset = start_offset;
  State cycle_checkpoint = current;
  std::size_t cycle_power = 1;
  std::size_t distance_from_checkpoint = 0;

  while (result.tags_processed < max_tags) {
    result.error_offset = current.tag_offset;
    const auto step = dma_chain_validation_detail::step(
        static_cast<const u8*>(base), memory_size, current);
    if (step.error != DmaChainValidationError::None) {
      result.error = step.error;
      return result;
    }
    result.tags_processed++;
    if (step.ended) {
      return result;
    }

    current = step.next;
    distance_from_checkpoint++;
    if (dma_chain_validation_detail::states_equal(current, cycle_checkpoint)) {
      result.error = DmaChainValidationError::Cycle;
      result.error_offset = current.tag_offset;
      return result;
    }
    if (distance_from_checkpoint == cycle_power) {
      cycle_checkpoint = current;
      distance_from_checkpoint = 0;
      if (cycle_power <= std::numeric_limits<std::size_t>::max() / 2) {
        cycle_power *= 2;
      }
    }
  }

  result.error = DmaChainValidationError::TagBudgetExceeded;
  result.error_offset = current.tag_offset;
  return result;
}
