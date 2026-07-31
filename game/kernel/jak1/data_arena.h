#pragma once

#include <cstddef>
#include <cstdint>

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

[[nodiscard]] constexpr std::size_t minimum_data_arena_header_size() {
  return static_cast<std::size_t>(HEAP_START) + static_cast<std::size_t>(SYM_TABLE_MEM_SIZE);
}

/*!
 * Initialize the data-only prefix of Jak 1's GOAL memory layout in caller-owned storage.
 *
 * This establishes the global heap record, symbol table extent, fixed #f/#t value cells, and
 * empty pair representation. It neither binds the storage to g_ee_main_mem nor creates GOAL
 * code, native-function trampolines, symbol metadata, type objects, or linked game data.
 */
[[nodiscard]] DataArenaHeaderResult initialize_data_arena_header(std::byte* storage,
                                                                   std::size_t storage_size);

}  // namespace jak1
