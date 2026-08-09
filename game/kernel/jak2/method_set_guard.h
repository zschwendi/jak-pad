#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"
#include "common/goal_constants.h"

namespace jak2 {

constexpr std::size_t METHOD_SET_TYPE_PARENT_LIMIT = 256;

enum class MethodSetTypeChainStatus {
  SUBTYPE,
  UNRELATED,
  INVALID_RANGE,
  INVALID_BASIC_ALIGNMENT,
  INVALID_TYPE_TAG,
  CYCLE,
  DEPTH_LIMIT,
};

struct MethodSetTypeChainResult {
  MethodSetTypeChainStatus status = MethodSetTypeChainStatus::UNRELATED;
  u32 bad_parent = 0;
  u32 depth = 0;
};

inline const char* method_set_type_chain_reason(MethodSetTypeChainStatus status) {
  switch (status) {
    case MethodSetTypeChainStatus::INVALID_RANGE:
      return "invalid-range";
    case MethodSetTypeChainStatus::INVALID_BASIC_ALIGNMENT:
      return "invalid-basic-alignment";
    case MethodSetTypeChainStatus::INVALID_TYPE_TAG:
      return "invalid-type-tag";
    case MethodSetTypeChainStatus::CYCLE:
      return "cycle";
    case MethodSetTypeChainStatus::DEPTH_LIMIT:
      return "depth-limit";
    case MethodSetTypeChainStatus::SUBTYPE:
      return "subtype";
    case MethodSetTypeChainStatus::UNRELATED:
      return "unrelated";
  }
  return "unknown";
}

template <typename TypeTagReader, typename ParentReader>
MethodSetTypeChainResult check_method_set_type_chain(u32 candidate,
                                                     u32 target,
                                                     u32 object_type,
                                                     u32 symbol_table2,
                                                     u32 type_type,
                                                     TypeTagReader&& read_type_tag,
                                                     ParentReader&& read_parent) {
  std::array<u32, METHOD_SET_TYPE_PARENT_LIMIT> visited{};
  u32 current = candidate;

  for (std::size_t depth = 0; depth < METHOD_SET_TYPE_PARENT_LIMIT; ++depth) {
    const bool in_ee_memory = symbol_table2 <= current && current < EE_MAIN_MEM_SIZE;
    const bool in_kernel_memory = 0x84000 <= current && current < 0x100000;
    if (!in_ee_memory && !in_kernel_memory) {
      return {MethodSetTypeChainStatus::INVALID_RANGE, current, (u32)depth};
    }
    if ((current & 7) != BASIC_OFFSET) {
      return {MethodSetTypeChainStatus::INVALID_BASIC_ALIGNMENT, current, (u32)depth};
    }
    if (read_type_tag(current - BASIC_OFFSET) != type_type) {
      return {MethodSetTypeChainStatus::INVALID_TYPE_TAG, current, (u32)depth};
    }
    if (current == target) {
      return {MethodSetTypeChainStatus::SUBTYPE, 0, (u32)depth};
    }

    for (std::size_t i = 0; i < depth; ++i) {
      if (visited[i] == current) {
        return {MethodSetTypeChainStatus::CYCLE, current, (u32)depth};
      }
    }
    visited[depth] = current;

    const u32 parent = read_parent(current);
    if (parent == target) {
      return {MethodSetTypeChainStatus::SUBTYPE, 0, (u32)depth + 1};
    }
    if (!parent || parent == object_type) {
      return {MethodSetTypeChainStatus::UNRELATED, 0, (u32)depth + 1};
    }
    current = parent;
  }

  return {MethodSetTypeChainStatus::DEPTH_LIMIT, current, (u32)METHOD_SET_TYPE_PARENT_LIMIT};
}

}  // namespace jak2
