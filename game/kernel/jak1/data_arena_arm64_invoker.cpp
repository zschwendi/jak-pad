#include "data_arena_arm64_invoker.h"

#include <cassert>

namespace jak1 {
namespace {

extern "C" std::uint64_t jak1_data_arena_call_native_method1_arm64(
    std::uint64_t process,
    std::uint32_t s7,
    std::byte* arena,
    DataArenaNativeMethodEntry1 entry,
    std::uint32_t object);

}  // namespace

std::uint64_t invoke_data_arena_native_method1_arm64(const DataArenaNativeMethodCall1& call,
                                                     void* user_context) {
  assert(user_context);
  assert(call.entry);
  assert(call.arena);

  const auto* context = static_cast<const DataArenaNativeMethodArm64Context*>(user_context);
  return jak1_data_arena_call_native_method1_arm64(context->process, call.s7, call.arena,
                                                   call.entry, call.object);
}

}  // namespace jak1
