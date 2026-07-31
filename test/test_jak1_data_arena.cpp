#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "common/goal_constants.h"
#include "common/symbols.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/memory_layout.h"
#include "game/kernel/jak1/data_arena.h"
#include "game/kernel/jak1/kscheme.h"
#include "gtest/gtest.h"

namespace {

std::uint32_t read_u32(const std::byte* storage, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, storage + offset, sizeof(value));
  return value;
}

void write_u32(std::byte* storage, std::size_t offset, std::uint32_t value) {
  std::memcpy(storage + offset, &value, sizeof(value));
}

}  // namespace

TEST(Jak1DataArena, InitializesExactDataOnlyHeader) {
  constexpr std::size_t kGuardSize = 64;
  constexpr std::size_t kArenaSize = jak1::minimum_data_arena_header_size() + 0x2000;
  std::vector<std::byte> storage(kGuardSize + kArenaSize + kGuardSize, std::byte{0xa5});
  auto* arena = storage.data() + kGuardSize;

  const auto result = jak1::initialize_data_arena_header(arena, kArenaSize);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.header.global_heap_info, GLOBAL_HEAP_INFO_ADDR);
  EXPECT_EQ(result.header.global_heap_base, HEAP_START);
  EXPECT_EQ(result.header.global_heap_current, 0x17fd20);
  EXPECT_EQ(result.header.global_heap_end, kArenaSize);
  EXPECT_EQ(result.header.symbol_table, 0x13fd20);
  EXPECT_EQ(result.header.symbol_table_end, 0x17fd20);
  EXPECT_EQ(result.header.symbol_table2, 0x13fd24);
  EXPECT_EQ(result.header.last_symbol, 0x15fc20);
  EXPECT_EQ(result.header.s7, 0x14fd24);
  EXPECT_EQ(result.header.empty_pair, 0x14fd1a);
  EXPECT_EQ(result.header.false_value, result.header.s7);
  EXPECT_EQ(result.header.true_value, result.header.s7 + jak1_symbols::FIX_SYM_TRUE);
  EXPECT_EQ(read_u32(arena, GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, base)), HEAP_START);
  EXPECT_EQ(read_u32(arena, GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, top)), kArenaSize);
  EXPECT_EQ(read_u32(arena, GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, current)), 0x17fd20);
  EXPECT_EQ(read_u32(arena, GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, top_base)), kArenaSize);
  EXPECT_EQ(read_u32(arena, result.header.s7 + jak1_symbols::FIX_SYM_EMPTY_CAR),
            result.header.empty_pair);
  EXPECT_EQ(read_u32(arena, result.header.s7 + jak1_symbols::FIX_SYM_EMPTY_CDR),
            result.header.empty_pair);
  EXPECT_EQ(read_u32(arena, result.header.s7 + jak1_symbols::FIX_SYM_FALSE),
            result.header.false_value);
  EXPECT_EQ(read_u32(arena, result.header.s7 + jak1_symbols::FIX_SYM_TRUE),
            result.header.true_value);
  EXPECT_EQ(read_u32(arena, result.header.s7 + jak1_symbols::FIX_SYM_GLOBAL_HEAP),
            result.header.global_heap_info);

  std::vector<std::byte> expected(kArenaSize, std::byte{0});
  write_u32(expected.data(), GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, base), HEAP_START);
  write_u32(expected.data(), GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, top), kArenaSize);
  write_u32(expected.data(), GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, current), 0x17fd20);
  write_u32(expected.data(), GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, top_base), kArenaSize);
  write_u32(expected.data(), result.header.s7 + jak1_symbols::FIX_SYM_EMPTY_CAR,
            result.header.empty_pair);
  write_u32(expected.data(), result.header.s7 + jak1_symbols::FIX_SYM_EMPTY_CDR,
            result.header.empty_pair);
  write_u32(expected.data(), result.header.s7 + jak1_symbols::FIX_SYM_FALSE,
            result.header.false_value);
  write_u32(expected.data(), result.header.s7 + jak1_symbols::FIX_SYM_TRUE,
            result.header.true_value);
  write_u32(expected.data(), result.header.s7 + jak1_symbols::FIX_SYM_GLOBAL_HEAP,
            result.header.global_heap_info);

  EXPECT_EQ(std::memcmp(arena, expected.data(), kArenaSize), 0);
  EXPECT_TRUE(std::all_of(storage.begin(), storage.begin() + kGuardSize,
                          [](std::byte value) { return value == std::byte{0xa5}; }));
  EXPECT_TRUE(std::all_of(storage.end() - kGuardSize, storage.end(),
                          [](std::byte value) { return value == std::byte{0xa5}; }));
}

TEST(Jak1DataArena, PreservesStorageOnRejectedInputs) {
  std::array<std::byte, 64> storage;
  storage.fill(std::byte{0x5a});

  const auto null_result =
      jak1::initialize_data_arena_header(nullptr, jak1::minimum_data_arena_header_size());
  EXPECT_EQ(null_result.error, jak1::DataArenaHeaderError::NullStorage);

  const auto short_result = jak1::initialize_data_arena_header(storage.data(), storage.size());
  EXPECT_EQ(short_result.error, jak1::DataArenaHeaderError::StorageTooSmall);
  EXPECT_TRUE(std::all_of(storage.begin(), storage.end(),
                          [](std::byte value) { return value == std::byte{0x5a}; }));

  if constexpr (std::numeric_limits<std::size_t>::max() >
                std::numeric_limits<std::uint32_t>::max()) {
    const auto large_result = jak1::initialize_data_arena_header(
        storage.data(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1);
    EXPECT_EQ(large_result.error, jak1::DataArenaHeaderError::StorageTooLarge);
    EXPECT_TRUE(std::all_of(storage.begin(), storage.end(),
                            [](std::byte value) { return value == std::byte{0x5a}; }));
  }
}

TEST(Jak1DataArena, UsesOpenGoalDataLayoutRepresentations) {
  EXPECT_EQ(jak1::minimum_data_arena_header_size(), 0x17fd20);
  EXPECT_EQ(sizeof(kheapinfo), 16);
  EXPECT_EQ(sizeof(jak1::Symbol), 4);
  EXPECT_EQ(jak1::SYM_TABLE_MEM_SIZE, 0x40000);
  EXPECT_EQ(jak1::GOAL_MAX_SYMBOLS, 0x4000);
  EXPECT_EQ(jak1_symbols::FIX_SYM_EMPTY_CAR, -0xc);
  EXPECT_EQ(jak1_symbols::FIX_SYM_EMPTY_PAIR, -0xa);
  EXPECT_EQ(jak1_symbols::FIX_SYM_EMPTY_CDR, -0x8);
}
