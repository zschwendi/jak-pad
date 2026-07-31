#pragma once

#include <cstdint>

#include "game/kernel/jak1/data_arena.h"

namespace jak1 {

// The caller owns this context and keeps it valid for the whole synchronous invocation.
// The data-arena layer does not retain it or assign a process value itself.
struct DataArenaNativeMethodArm64Context {
  std::uint64_t process = 0;
};

// Apple ARM64 platform callback for a statically linked arity-one native entry. user_context must
// point to a non-null DataArenaNativeMethodArm64Context that remains valid for the entire call.
// The call's entry remains a typed native pointer outside the GOAL data arena; the raw method-table
// word is validated by the data-arena layer but is never converted into a code address.
std::uint64_t invoke_data_arena_native_method1_arm64(const DataArenaNativeMethodCall1& call,
                                                     void* user_context);

}  // namespace jak1
