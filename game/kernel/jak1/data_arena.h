#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "common/goal_constants.h"

#include "game/kernel/common/memory_layout.h"

namespace jak1 {

enum class DataArenaHeaderError {
  None,
  NullStorage,
  StorageTooSmall,
  StorageTooLarge,
};

struct DataArenaHeader {
  std::uint32_t global_heap_info;
  std::uint32_t global_heap_base;
  std::uint32_t global_heap_current;
  std::uint32_t global_heap_end;
  std::uint32_t symbol_table;
  std::uint32_t symbol_table_end;
  std::uint32_t symbol_table2;
  std::uint32_t last_symbol;
  std::uint32_t s7;
  std::uint32_t empty_pair;
  std::uint32_t false_value;
  std::uint32_t true_value;
};

struct DataArenaHeaderResult {
  DataArenaHeader header{};
  DataArenaHeaderError error = DataArenaHeaderError::None;

  [[nodiscard]] bool ok() const { return error == DataArenaHeaderError::None; }
};

enum class DataArenaSymbolValueLookupError {
  None,
  NullStorage,
  StorageTooLarge,
  InvalidHeader,
  QueryContainsNull,
  QueryTooLong,
  UnsupportedPseudoSymbol,
  UnrepresentableHash,
  NotFound,
  MalformedSymbol,
  MalformedString,
};

struct DataArenaSymbolValueCell {
  std::uint32_t goal_offset = 0;
  std::int32_t s7_relative_displacement = 0;
};

struct DataArenaSymbolValueLookupResult {
  DataArenaSymbolValueCell cell{};
  DataArenaSymbolValueLookupError error = DataArenaSymbolValueLookupError::NotFound;

  [[nodiscard]] bool found() const { return error == DataArenaSymbolValueLookupError::None; }
};

enum class DataArenaBasicMethodValueLookupError {
  None,
  NullStorage,
  StorageTooLarge,
  InvalidHeader,
  InvalidObject,
  InvalidObjectType,
  InvalidTypeType,
  InvalidTypeTag,
  InvalidObjectSize,
  MethodOutOfRange,
  InvalidMethodSlot,
};

struct DataArenaBasicMethodValue {
  std::uint32_t object_type = 0;
  std::uint16_t allocated_size = 0;
  std::uint16_t padded_size = 0;
  std::uint32_t method_value = 0;
};

struct DataArenaBasicMethodValueLookupResult {
  DataArenaBasicMethodValue value{};
  DataArenaBasicMethodValueLookupError error = DataArenaBasicMethodValueLookupError::InvalidObject;

  [[nodiscard]] bool found() const { return error == DataArenaBasicMethodValueLookupError::None; }
};

// The entry remains a native pointer held outside the GOAL data arena. The arity is part of the
// type so callers cannot invoke a statically linked entry through an incompatible function type.
using DataArenaNativeMethodEntry1 = std::uint64_t (*)(std::uint64_t);

struct DataArenaNativeMethodCall1 {
  DataArenaNativeMethodEntry1 entry = nullptr;
  std::uint32_t object = 0;
  std::uint32_t s7 = 0;
  std::byte* arena = nullptr;
};

// The platform layer owns the ABI bridge. On ARM64 it can use the call context to establish GOAL
// registers before calling entry; this data-only layer never converts a GOAL word into code.
using DataArenaNativeMethodInvoker1 = std::uint64_t (*)(const DataArenaNativeMethodCall1&, void*);

struct DataArenaNativeMethodBinding1 {
  std::uint32_t object_type = 0;
  std::uint32_t method_id = 0;
  std::uint32_t method_value = 0;
  std::uint16_t allocated_size = 0;
  std::uint16_t padded_size = 0;
  std::uint8_t arity = 1;
  DataArenaNativeMethodEntry1 entry = nullptr;
};

enum class DataArenaNativeMethodInvokeError {
  None,
  NullInvoker,
  InvalidBindingArity,
  NullNativeEntry,
  DuplicateBinding,
  BasicMethodLookupFailed,
  InvalidFunctionValue,
  InvalidFunctionType,
  InvalidFunctionTag,
  UnboundMethod,
};

struct DataArenaNativeMethodInvokeResult {
  std::uint64_t value = 0;
  DataArenaBasicMethodValue basic_method{};
  DataArenaBasicMethodValueLookupError lookup_error = DataArenaBasicMethodValueLookupError::None;
  DataArenaNativeMethodInvokeError error = DataArenaNativeMethodInvokeError::UnboundMethod;

  [[nodiscard]] bool invoked() const { return error == DataArenaNativeMethodInvokeError::None; }
};

[[nodiscard]] constexpr std::size_t minimum_data_arena_header_size() {
  return static_cast<std::size_t>(GLOBAL_HEAP_END);
}

/*!
 * Initialize the data-only prefix of Jak 1's GOAL memory layout in caller-owned storage.
 *
 * Storage represents zero-based GOAL offsets and must provide at least GLOBAL_HEAP_END bytes.
 * Successful initialization clears only [storage, storage + GLOBAL_HEAP_END). The global heap
 * record uses the base and bounds configured by InitMachine; the current pointer is the end of
 * the initial symbol-table allocation. It also stages the symbol table extent, fixed #f/#t
 * value cells, and empty pair representation. Callers must treat that range as destructively
 * initialized; bytes beyond it are left untouched.
 *
 * This is a partial data-only bootstrap, not a complete InitHeapAndSymbol snapshot. It neither
 * binds the storage to g_ee_main_mem nor creates GOAL code, native-function trampolines, symbol
 * metadata, type objects, or linked game data.
 */
[[nodiscard]] DataArenaHeaderResult initialize_data_arena_header(std::byte* storage,
                                                                 std::size_t storage_size);

/*!
 * Find an existing, aligned Jak 1 symbol value cell without binding caller-owned storage to the
 * runtime. The supplied header is structurally validated before its table offsets are used.
 *
 * This read-only lookup uses the Jak 1 CRC_POLY hash and bounded symbol-table probe order, but
 * computes the hash without the runtime CRC table. It does not intern symbols, allocate, mutate
 * the arena, execute GOAL, or access g_ee_main_mem or global s7 state. The empty-pair pseudo
 * symbol is deliberately rejected because it is not an aligned symbol value cell.
 */
[[nodiscard]] DataArenaSymbolValueLookupResult find_data_arena_symbol_value_cell(
    const std::byte* storage,
    std::size_t storage_size,
    const DataArenaHeader& header,
    std::string_view name);

/*!
 * Read a BASIC object's virtual method-table value from caller-owned Jak 1 GOAL memory.
 *
 * This is a read-only layout check. It validates the data-only header, the object's BASIC
 * representation, the fixed Type-Type root, the object's runtime Type, and the Type's size and
 * method-table bounds. The returned method value remains a raw 32-bit GOAL word: this helper
 * does not cast it to native code, inspect a function object, mutate storage, or invoke GOAL.
 */
[[nodiscard]] DataArenaBasicMethodValueLookupResult read_data_arena_basic_method_value(
    const std::byte* storage,
    std::size_t storage_size,
    const DataArenaHeader& header,
    std::uint32_t object,
    std::uint32_t method_id);

/*!
 * Resolve and invoke a trusted arity-1 native BASIC method binding in one synchronous operation.
 *
 * Bindings are caller-owned immutable records. Production callers must source entries through
 * normal static linkage in the signed binary and keep binding storage immutable and alive. A
 * binding is accepted only when its complete trusted tuple exactly matches the value read from the
 * caller-owned arena. The tuple contains the actual object Type, requested method id, raw GOAL
 * method value, allocated size, padded size, and arity. The raw
 * method value is verified as a Function-tagged BASIC object but is never interpreted as a host
 * pointer. The corresponding native entry comes only from the binding table and is passed to the
 * caller-supplied ABI invoker together with the object, s7, and arena base.
 *
 * This function does not write the arena. The invoked native method may write it. Callers must
 * keep the arena, header, binding span, and every alias they expose alive; prevent concurrent
 * mutation or reallocation; and serialize exclusive access for the entire operation. This
 * function provides no internal synchronization.
 *
 * It validates the Function tag and the bounded method slot established by the data-arena lookup.
 * It does not validate a Type allocation's full extent, the Function body, native-entry
 * provenance, code signatures, or any platform ABI. Those remain caller and platform-layer
 * responsibilities.
 */
[[nodiscard]] DataArenaNativeMethodInvokeResult invoke_data_arena_native_basic_method1(
    std::byte* storage,
    std::size_t storage_size,
    const DataArenaHeader& header,
    std::uint32_t object,
    std::uint32_t method_id,
    std::span<const DataArenaNativeMethodBinding1> bindings,
    DataArenaNativeMethodInvoker1 invoker,
    void* user_context = nullptr);

}  // namespace jak1
