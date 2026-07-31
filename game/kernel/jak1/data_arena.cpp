#include "data_arena.h"

#include <cstddef>
#include <cstring>
#include <limits>

#include "common/goal_constants.h"
#include "common/symbols.h"

#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/memory_layout.h"
#include "game/kernel/jak1/kscheme.h"

namespace jak1 {
namespace {

constexpr std::uint32_t kSymbolTableEntrySize = 8;
constexpr std::uint32_t kSymbolTableOffset = HEAP_START;
constexpr std::uint32_t kSymbolTableEnd = kSymbolTableOffset + SYM_TABLE_MEM_SIZE;
constexpr std::uint32_t kGlobalHeapEnd = GLOBAL_HEAP_END;
constexpr std::uint32_t kS7 =
    kSymbolTableOffset + (GOAL_MAX_SYMBOLS / 2) * kSymbolTableEntrySize + BASIC_OFFSET;
constexpr std::uint32_t kSymbolTable2 = kSymbolTableOffset + BASIC_OFFSET;
constexpr std::uint32_t kLastSymbol = kSymbolTableOffset + SYM_TABLE_END * kSymbolTableEntrySize;
constexpr std::uint32_t kEmptyPair = kS7 + jak1_symbols::FIX_SYM_EMPTY_PAIR;

static_assert(sizeof(kheapinfo) == 4 * sizeof(std::uint32_t));
static_assert(offsetof(kheapinfo, base) == 0);
static_assert(offsetof(kheapinfo, top) == sizeof(std::uint32_t));
static_assert(offsetof(kheapinfo, current) == 2 * sizeof(std::uint32_t));
static_assert(offsetof(kheapinfo, top_base) == 3 * sizeof(std::uint32_t));
static_assert(sizeof(Symbol) == sizeof(std::uint32_t));
static_assert(offsetof(Symbol, value) == 0);
static_assert(kGlobalHeapEnd == minimum_data_arena_header_size());
static_assert(kSymbolTableEnd <= kGlobalHeapEnd);
static_assert(kSymbolTableOffset >= GLOBAL_HEAP_INFO_ADDR + sizeof(kheapinfo));

void write_u32(std::byte* storage, std::size_t offset, std::uint32_t value) {
  std::memcpy(storage + offset, &value, sizeof(value));
}

}  // namespace

DataArenaHeaderResult initialize_data_arena_header(std::byte* storage, std::size_t storage_size) {
  if (!storage) {
    return {.error = DataArenaHeaderError::NullStorage};
  }
  if (storage_size < minimum_data_arena_header_size()) {
    return {.error = DataArenaHeaderError::StorageTooSmall};
  }
  if (storage_size > std::numeric_limits<std::uint32_t>::max()) {
    return {.error = DataArenaHeaderError::StorageTooLarge};
  }

  std::memset(storage, 0, kGlobalHeapEnd);

  write_u32(storage, GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, base), HEAP_START);
  write_u32(storage, GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, top), kGlobalHeapEnd);
  write_u32(storage, GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, current), kSymbolTableEnd);
  write_u32(storage, GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, top_base), kGlobalHeapEnd);

  write_u32(storage, kS7 + jak1_symbols::FIX_SYM_EMPTY_CAR, kEmptyPair);
  write_u32(storage, kS7 + jak1_symbols::FIX_SYM_EMPTY_CDR, kEmptyPair);
  write_u32(storage, kS7 + jak1_symbols::FIX_SYM_FALSE, kS7);
  write_u32(storage, kS7 + jak1_symbols::FIX_SYM_TRUE, kS7 + jak1_symbols::FIX_SYM_TRUE);
  write_u32(storage, kS7 + jak1_symbols::FIX_SYM_GLOBAL_HEAP, GLOBAL_HEAP_INFO_ADDR);

  return {
      .header =
          {
              .global_heap_info = GLOBAL_HEAP_INFO_ADDR,
              .global_heap_base = HEAP_START,
              .global_heap_current = kSymbolTableEnd,
              .global_heap_end = kGlobalHeapEnd,
              .symbol_table = kSymbolTableOffset,
              .symbol_table_end = kSymbolTableEnd,
              .symbol_table2 = kSymbolTable2,
              .last_symbol = kLastSymbol,
              .s7 = kS7,
              .empty_pair = kEmptyPair,
              .false_value = kS7,
              .true_value = kS7 + jak1_symbols::FIX_SYM_TRUE,
          },
      .error = DataArenaHeaderError::None,
  };
}

}  // namespace jak1
