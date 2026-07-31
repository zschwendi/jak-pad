#pragma once

#include <cstddef>
#include <cstdint>
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

}  // namespace jak1
