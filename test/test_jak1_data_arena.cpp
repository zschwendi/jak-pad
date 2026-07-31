#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>
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

void write_u16(std::byte* storage, std::size_t offset, std::uint16_t value) {
  std::memcpy(storage + offset, &value, sizeof(value));
}

bool is_zero(std::byte value) {
  return value == std::byte{0};
}

constexpr std::uint32_t kSymbolTableEntrySize = 8;
constexpr std::uint32_t kSyntheticSymbolTable = 0x1000;
constexpr std::uint32_t kSyntheticSymbolTableEnd = kSyntheticSymbolTable + jak1::SYM_TABLE_MEM_SIZE;
constexpr std::uint32_t kSyntheticS7 =
    kSyntheticSymbolTable + (jak1::GOAL_MAX_SYMBOLS / 2) * kSymbolTableEntrySize + BASIC_OFFSET;
constexpr std::uint32_t kSyntheticLastSymbol =
    kSyntheticSymbolTable + jak1::SYM_TABLE_END * kSymbolTableEntrySize;
constexpr std::uint32_t kSyntheticArenaSize = kSyntheticSymbolTableEnd + 0x2000;
constexpr std::uint32_t kResetMethodId = 9;

static_assert(offsetof(jak1::Type, symbol) == 0);
static_assert(offsetof(jak1::Type, allocated_size) == 0x8);
static_assert(offsetof(jak1::Type, padded_size) == 0xa);
static_assert(offsetof(jak1::Type, num_methods) == 0xe);
static_assert(offsetof(jak1::Type, new_method) == 0x10);
static_assert(offsetof(jak1::Type, new_method) + kResetMethodId * sizeof(std::uint32_t) == 0x34);

std::uint32_t stateless_goal_crc32(std::string_view name) {
  std::uint32_t crc = 0;
  for (const auto character : name) {
    std::uint32_t table_entry = (crc >> 24) << 24;
    for (int bit = 0; bit < 8; ++bit) {
      table_entry =
          (table_entry & 0x80000000U) ? (table_entry << 1) ^ CRC_POLY : (table_entry << 1);
    }
    crc = table_entry ^ ((crc << 8) | static_cast<unsigned char>(character));
  }
  return ~crc;
}

std::int32_t symbol_hash_displacement(std::uint32_t hash) {
  constexpr std::uint32_t kBucketMask = (1U << 14) - 1;
  constexpr std::uint32_t kBucketSignBit = 1U << 13;
  const auto low_bucket = hash & kBucketMask;
  const auto bucket = (low_bucket & kBucketSignBit)
                          ? static_cast<std::int32_t>(low_bucket) - (1 << 14)
                          : static_cast<std::int32_t>(low_bucket);
  return bucket * static_cast<std::int32_t>(kSymbolTableEntrySize);
}

struct SyntheticDataArena {
  std::vector<std::byte> storage = std::vector<std::byte>(kSyntheticArenaSize, std::byte{0});
  jak1::DataArenaHeader header{
      .global_heap_info = 0x100,
      .global_heap_base = kSyntheticSymbolTable,
      .global_heap_current = kSyntheticSymbolTableEnd,
      .global_heap_end = kSyntheticArenaSize,
      .symbol_table = kSyntheticSymbolTable,
      .symbol_table_end = kSyntheticSymbolTableEnd,
      .symbol_table2 = kSyntheticSymbolTable + BASIC_OFFSET,
      .last_symbol = kSyntheticLastSymbol,
      .s7 = kSyntheticS7,
      .empty_pair = kSyntheticS7 + jak1_symbols::FIX_SYM_EMPTY_PAIR,
      .false_value = kSyntheticS7,
      .true_value = kSyntheticS7 + jak1_symbols::FIX_SYM_TRUE,
  };
  std::uint32_t next_string_offset = kSyntheticSymbolTableEnd;
};

std::uint32_t first_probe_offset(const SyntheticDataArena& arena, std::string_view name) {
  return static_cast<std::uint32_t>(static_cast<std::int64_t>(arena.header.s7) +
                                    symbol_hash_displacement(stateless_goal_crc32(name)));
}

std::uint32_t write_string(SyntheticDataArena* arena, std::string_view value) {
  const auto raw_string_offset = arena->next_string_offset;
  const auto string_offset = raw_string_offset + BASIC_OFFSET;
  write_u32(arena->storage.data(), raw_string_offset,
            arena->header.s7 + jak1_symbols::FIX_SYM_STRING_TYPE);
  write_u32(arena->storage.data(), string_offset, static_cast<std::uint32_t>(value.size()));
  if (!value.empty()) {
    std::memcpy(arena->storage.data() + string_offset + sizeof(std::uint32_t), value.data(),
                value.size());
  }
  arena->storage[string_offset + sizeof(std::uint32_t) + value.size()] = std::byte{0};
  arena->next_string_offset = static_cast<std::uint32_t>(
      (string_offset + sizeof(std::uint32_t) + value.size() + 1 + OFFSET_MASK) & ~OFFSET_MASK);
  return string_offset;
}

void install_symbol(SyntheticDataArena* arena,
                    std::uint32_t symbol_offset,
                    std::uint32_t hash,
                    std::string_view stored_name,
                    std::uint32_t value = 0) {
  const auto info_offset = symbol_offset + jak1::SYM_INFO_OFFSET;
  write_u32(arena->storage.data(), symbol_offset, value);
  write_u32(arena->storage.data(), info_offset, hash);
  write_u32(arena->storage.data(), info_offset + sizeof(std::uint32_t),
            write_string(arena, stored_name));
}

void fill_mismatching_hashes(SyntheticDataArena* arena,
                             std::uint32_t start,
                             std::uint32_t end,
                             std::uint32_t requested_hash) {
  for (auto symbol_offset = start; symbol_offset < end; symbol_offset += kSymbolTableEntrySize) {
    write_u32(arena->storage.data(), symbol_offset + jak1::SYM_INFO_OFFSET, requested_hash ^ 1U);
  }
}

struct BasicMethodFixture {
  static constexpr std::uint32_t kTypeType = 0x42004;
  static constexpr std::uint32_t kFunctionType = 0x42044;
  static constexpr std::uint32_t kObjectType = 0x42084;
  static constexpr std::uint32_t kObject = 0x42104;
  static constexpr std::uint32_t kFunction = 0x42a04;
  static constexpr std::uint32_t kDerivedType = 0x42944;
  static constexpr std::uint32_t kDerivedFunction = 0x42a44;
  static constexpr std::uint16_t kAllocatedSize = 0x82c;
  static constexpr std::uint16_t kPaddedSize = 0x830;
  static constexpr std::uint16_t kMethodCount = 21;

  SyntheticDataArena arena;

  BasicMethodFixture() {
    write_u32(arena.storage.data(), arena.header.s7 + jak1_symbols::FIX_SYM_TYPE_TYPE, kTypeType);
    write_u32(arena.storage.data(), kTypeType - BASIC_OFFSET, kTypeType);

    write_u32(arena.storage.data(), arena.header.s7 + jak1_symbols::FIX_SYM_FUNCTION_TYPE,
              kFunctionType);
    write_u32(arena.storage.data(), kFunctionType - BASIC_OFFSET, kTypeType);

    write_u32(arena.storage.data(), kObjectType - BASIC_OFFSET, kTypeType);
    write_u16(arena.storage.data(), kObjectType + offsetof(jak1::Type, allocated_size),
              kAllocatedSize);
    write_u16(arena.storage.data(), kObjectType + offsetof(jak1::Type, padded_size), kPaddedSize);
    write_u16(arena.storage.data(), kObjectType + offsetof(jak1::Type, num_methods), kMethodCount);
    write_u32(arena.storage.data(), method_slot(kObjectType, kResetMethodId), kFunction);

    write_u32(arena.storage.data(), kObject - BASIC_OFFSET, kObjectType);
    write_u32(arena.storage.data(), kFunction - BASIC_OFFSET, kFunctionType);
  }

  static constexpr std::uint32_t method_slot(std::uint32_t type, std::uint32_t method_id) {
    return type + offsetof(jak1::Type, new_method) + method_id * sizeof(std::uint32_t);
  }
};

std::uint64_t synthetic_load_state_reset(std::uint64_t object) {
  return object;
}

std::uint64_t alternate_synthetic_load_state_reset(std::uint64_t object) {
  return object;
}

struct NativeMethodInvocationProbe {
  jak1::DataArenaNativeMethodEntry1 entry = nullptr;
  std::uint32_t object = 0;
  std::uint32_t s7 = 0;
  std::byte* arena = nullptr;
  std::size_t calls = 0;
};

std::uint64_t invoke_synthetic_native_method(const jak1::DataArenaNativeMethodCall1& call,
                                             void* user_context) {
  auto* probe = static_cast<NativeMethodInvocationProbe*>(user_context);
  probe->entry = call.entry;
  probe->object = call.object;
  probe->s7 = call.s7;
  probe->arena = call.arena;
  ++probe->calls;
  return call.entry(call.object);
}

jak1::DataArenaNativeMethodBinding1 load_state_reset_binding() {
  return {
      .object_type = BasicMethodFixture::kObjectType,
      .method_id = kResetMethodId,
      .method_value = BasicMethodFixture::kFunction,
      .allocated_size = BasicMethodFixture::kAllocatedSize,
      .padded_size = BasicMethodFixture::kPaddedSize,
      .arity = 1,
      .entry = &synthetic_load_state_reset,
  };
}

void install_derived_reset(BasicMethodFixture* fixture) {
  write_u32(fixture->arena.storage.data(), BasicMethodFixture::kDerivedType - BASIC_OFFSET,
            BasicMethodFixture::kTypeType);
  write_u32(fixture->arena.storage.data(),
            BasicMethodFixture::kDerivedType + offsetof(jak1::Type, parent),
            BasicMethodFixture::kObjectType);
  write_u16(fixture->arena.storage.data(),
            BasicMethodFixture::kDerivedType + offsetof(jak1::Type, allocated_size),
            BasicMethodFixture::kAllocatedSize);
  write_u16(fixture->arena.storage.data(),
            BasicMethodFixture::kDerivedType + offsetof(jak1::Type, padded_size),
            BasicMethodFixture::kPaddedSize);
  write_u16(fixture->arena.storage.data(),
            BasicMethodFixture::kDerivedType + offsetof(jak1::Type, num_methods),
            BasicMethodFixture::kMethodCount);
  write_u32(fixture->arena.storage.data(),
            BasicMethodFixture::method_slot(BasicMethodFixture::kDerivedType, kResetMethodId),
            BasicMethodFixture::kDerivedFunction);
  write_u32(fixture->arena.storage.data(), BasicMethodFixture::kDerivedFunction - BASIC_OFFSET,
            BasicMethodFixture::kFunctionType);
  write_u32(fixture->arena.storage.data(), BasicMethodFixture::kObject - BASIC_OFFSET,
            BasicMethodFixture::kDerivedType);
}

}  // namespace

TEST(Jak1DataArena, InitializesCanonicalDataOnlyHeader) {
  constexpr std::size_t kGuardSize = 64;
  constexpr std::size_t kUntouchedTailSize = 0x2000;
  constexpr std::size_t kInitializedSize = jak1::minimum_data_arena_header_size();
  constexpr std::size_t kArenaSize = kInitializedSize + kUntouchedTailSize;
  std::vector<std::byte> storage(kGuardSize + kArenaSize + kGuardSize, std::byte{0xa5});
  auto* arena = storage.data() + kGuardSize;

  const auto result = jak1::initialize_data_arena_header(arena, kArenaSize);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.header.global_heap_info, GLOBAL_HEAP_INFO_ADDR);
  EXPECT_EQ(result.header.global_heap_base, HEAP_START);
  EXPECT_EQ(result.header.global_heap_current, 0x17fd20);
  EXPECT_EQ(result.header.global_heap_end, GLOBAL_HEAP_END);
  EXPECT_EQ(result.header.symbol_table, 0x13fd20);
  EXPECT_EQ(result.header.symbol_table_end, 0x17fd20);
  EXPECT_EQ(result.header.symbol_table2, 0x13fd24);
  EXPECT_EQ(result.header.last_symbol, 0x15fc20);
  EXPECT_EQ(result.header.s7, 0x14fd24);
  EXPECT_EQ(result.header.empty_pair, 0x14fd1a);
  EXPECT_EQ(result.header.false_value, result.header.s7);
  EXPECT_EQ(result.header.true_value, result.header.s7 + jak1_symbols::FIX_SYM_TRUE);
  const std::array expected_words{
      std::pair{static_cast<std::size_t>(GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, base)),
                static_cast<std::uint32_t>(HEAP_START)},
      std::pair{static_cast<std::size_t>(GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, top)),
                static_cast<std::uint32_t>(GLOBAL_HEAP_END)},
      std::pair{static_cast<std::size_t>(GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, current)),
                result.header.global_heap_current},
      std::pair{static_cast<std::size_t>(GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, top_base)),
                static_cast<std::uint32_t>(GLOBAL_HEAP_END)},
      std::pair{static_cast<std::size_t>(result.header.s7 + jak1_symbols::FIX_SYM_EMPTY_CAR),
                result.header.empty_pair},
      std::pair{static_cast<std::size_t>(result.header.s7 + jak1_symbols::FIX_SYM_EMPTY_CDR),
                result.header.empty_pair},
      std::pair{static_cast<std::size_t>(result.header.s7 + jak1_symbols::FIX_SYM_FALSE),
                result.header.false_value},
      std::pair{static_cast<std::size_t>(result.header.s7 + jak1_symbols::FIX_SYM_TRUE),
                result.header.true_value},
      std::pair{static_cast<std::size_t>(result.header.s7 + jak1_symbols::FIX_SYM_GLOBAL_HEAP),
                result.header.global_heap_info},
  };

  std::size_t next_offset = 0;
  for (const auto& [offset, value] : expected_words) {
    EXPECT_TRUE(std::all_of(arena + next_offset, arena + offset, is_zero));
    EXPECT_EQ(read_u32(arena, offset), value);
    next_offset = offset + sizeof(std::uint32_t);
  }
  EXPECT_TRUE(std::all_of(arena + next_offset, arena + kInitializedSize, is_zero));
  EXPECT_TRUE(std::all_of(arena + kInitializedSize, arena + kArenaSize,
                          [](std::byte value) { return value == std::byte{0xa5}; }));
  EXPECT_TRUE(std::all_of(storage.begin(), storage.begin() + kGuardSize,
                          [](std::byte value) { return value == std::byte{0xa5}; }));
  EXPECT_TRUE(std::all_of(storage.end() - kGuardSize, storage.end(),
                          [](std::byte value) { return value == std::byte{0xa5}; }));
}

TEST(Jak1DataArena, AcceptsAnUnalignedExactMinimumAfterRejectingOneByteShort) {
  constexpr std::size_t kArenaSize = jak1::minimum_data_arena_header_size();
  std::vector<std::byte> storage(kArenaSize + 1, std::byte{0x5a});
  auto* arena = storage.data() + 1;

  const auto short_result = jak1::initialize_data_arena_header(arena, kArenaSize - 1);
  EXPECT_EQ(short_result.error, jak1::DataArenaHeaderError::StorageTooSmall);
  EXPECT_TRUE(std::all_of(storage.begin(), storage.end(),
                          [](std::byte value) { return value == std::byte{0x5a}; }));

  const auto result = jak1::initialize_data_arena_header(arena, kArenaSize);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(storage.front(), std::byte{0x5a});
  EXPECT_EQ(read_u32(arena, GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, top)), GLOBAL_HEAP_END);
  EXPECT_EQ(read_u32(arena, GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, top_base)),
            GLOBAL_HEAP_END);
}

TEST(Jak1DataArena, PreservesSmallStorageOnRejectedInputs) {
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
  EXPECT_EQ(jak1::minimum_data_arena_header_size(), GLOBAL_HEAP_END);
  EXPECT_EQ(jak1::minimum_data_arena_header_size(), 0x03eb82e0);
  EXPECT_EQ(sizeof(kheapinfo), 16);
  EXPECT_EQ(sizeof(jak1::Symbol), 4);
  EXPECT_EQ(jak1::SYM_TABLE_MEM_SIZE, 0x40000);
  EXPECT_EQ(jak1::GOAL_MAX_SYMBOLS, 0x4000);
  EXPECT_EQ(jak1_symbols::FIX_SYM_EMPTY_CAR, -0xc);
  EXPECT_EQ(jak1_symbols::FIX_SYM_EMPTY_PAIR, -0xa);
  EXPECT_EQ(jak1_symbols::FIX_SYM_EMPTY_CDR, -0x8);
}

TEST(Jak1DataArena, MatchesJak1StatelessCrcAndSignedBucketBoundaries) {
  EXPECT_EQ(stateless_goal_crc32("_empty_"), EMPTY_HASH);
  EXPECT_EQ(stateless_goal_crc32("*load-state*"), 0x5c4a5241);
  EXPECT_EQ(0x14fd24U + 0x9208U, 0x158f2cU);
  EXPECT_EQ(symbol_hash_displacement(0x00000000), 0);
  EXPECT_EQ(symbol_hash_displacement(0x00001fff), 0xfff8);
  EXPECT_EQ(symbol_hash_displacement(0x00002000), -0x10000);
  EXPECT_EQ(symbol_hash_displacement(0x00003fff), -8);
}

TEST(Jak1DataArena, FindsExistingZeroValueLoadStateCell) {
  constexpr std::string_view kName = "*load-state*";
  auto arena = SyntheticDataArena{};
  const auto hash = stateless_goal_crc32(kName);
  const auto symbol_offset = first_probe_offset(arena, kName);
  ASSERT_EQ(symbol_hash_displacement(hash), 0x9208);
  ASSERT_EQ(symbol_offset, 0x1a20cU);
  install_symbol(&arena, symbol_offset, hash, kName);

  const auto result = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), arena.header, kName);

  ASSERT_TRUE(result.found());
  EXPECT_EQ(result.cell.goal_offset, symbol_offset);
  EXPECT_EQ(result.cell.s7_relative_displacement, symbol_hash_displacement(hash));
  EXPECT_EQ(read_u32(arena.storage.data(), result.cell.goal_offset), 0U);
}

TEST(Jak1DataArena, FindsNegativeS7RelativeSymbolCell) {
  constexpr std::string_view kName = "negative";
  auto arena = SyntheticDataArena{};
  const auto hash = stateless_goal_crc32(kName);
  const auto symbol_offset = first_probe_offset(arena, kName);
  ASSERT_LT(symbol_hash_displacement(hash), 0);
  install_symbol(&arena, symbol_offset, hash, kName, 0x12345678);

  const auto result = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), arena.header, kName);

  ASSERT_TRUE(result.found());
  EXPECT_EQ(result.cell.goal_offset, symbol_offset);
  EXPECT_EQ(result.cell.s7_relative_displacement, symbol_hash_displacement(hash));
}

TEST(Jak1DataArena, DistinguishesZeroDisplacementAndZeroValueFromNotFound) {
  constexpr std::string_view kName = "fixed-zero";
  auto arena = SyntheticDataArena{};
  install_symbol(&arena, arena.header.s7, stateless_goal_crc32(kName), kName);

  const auto found = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), arena.header, kName);
  const auto missing = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), arena.header, "fixed-zero-missing");

  ASSERT_TRUE(found.found());
  EXPECT_EQ(found.cell.goal_offset, arena.header.s7);
  EXPECT_EQ(found.cell.s7_relative_displacement, 0);
  EXPECT_EQ(read_u32(arena.storage.data(), found.cell.goal_offset), 0U);
  EXPECT_FALSE(missing.found());
  EXPECT_EQ(missing.error, jak1::DataArenaSymbolValueLookupError::NotFound);
}

TEST(Jak1DataArena, ContinuesAfterHashCollisionAndPrefixNameMismatch) {
  constexpr std::string_view kName = "*load-state*";
  auto arena = SyntheticDataArena{};
  const auto hash = stateless_goal_crc32(kName);
  const auto first_symbol_offset = first_probe_offset(arena, kName);
  const auto second_symbol_offset = first_symbol_offset + kSymbolTableEntrySize;
  install_symbol(&arena, first_symbol_offset, hash, "*load-state*-suffix");
  install_symbol(&arena, second_symbol_offset, hash, kName);

  const auto result = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), arena.header, kName);

  ASSERT_TRUE(result.found());
  EXPECT_EQ(result.cell.goal_offset, second_symbol_offset);
  EXPECT_EQ(result.cell.s7_relative_displacement,
            symbol_hash_displacement(hash) + static_cast<std::int32_t>(kSymbolTableEntrySize));
}

TEST(Jak1DataArena, WrapsPositiveProbeIntoTheLowerSymbolTableArea) {
  constexpr std::string_view kName = "*load-state*";
  auto arena = SyntheticDataArena{};
  const auto hash = stateless_goal_crc32(kName);
  const auto primary_start = first_probe_offset(arena, kName);
  ASSERT_EQ(symbol_hash_displacement(hash), 0x9208);
  fill_mismatching_hashes(&arena, primary_start, arena.header.last_symbol, hash);
  install_symbol(&arena, arena.header.symbol_table2, hash, kName);

  const auto result = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), arena.header, kName);

  ASSERT_TRUE(result.found());
  EXPECT_EQ(result.cell.goal_offset, arena.header.symbol_table2);
  EXPECT_LT(result.cell.s7_relative_displacement, 0);
}

TEST(Jak1DataArena, WrapsNegativeProbeIntoTheUpperSymbolTableArea) {
  constexpr std::string_view kName = "negative";
  auto arena = SyntheticDataArena{};
  const auto hash = stateless_goal_crc32(kName);
  const auto primary_start = first_probe_offset(arena, kName);
  const auto primary_end = arena.header.s7 - 0x10;
  const auto overflow_start = arena.header.s7 + jak1_symbols::FIX_FIXED_SYM_END_OFFSET;
  ASSERT_LT(symbol_hash_displacement(hash), 0);
  fill_mismatching_hashes(&arena, primary_start, primary_end, hash);
  install_symbol(&arena, overflow_start, hash, kName);

  const auto result = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), arena.header, kName);

  ASSERT_TRUE(result.found());
  EXPECT_EQ(result.cell.goal_offset, overflow_start);
  EXPECT_GT(result.cell.s7_relative_displacement, 0);
}

TEST(Jak1DataArena, StopsAtVacancyBeforeALaterMatchingSymbol) {
  constexpr std::string_view kName = "*load-state*";
  auto arena = SyntheticDataArena{};
  const auto hash = stateless_goal_crc32(kName);
  const auto later_symbol_offset = first_probe_offset(arena, kName) + kSymbolTableEntrySize;
  install_symbol(&arena, later_symbol_offset, hash, kName);
  const auto before = arena.storage;

  const auto result = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), arena.header, kName);

  EXPECT_EQ(result.error, jak1::DataArenaSymbolValueLookupError::NotFound);
  EXPECT_EQ(arena.storage, before);
}

TEST(Jak1DataArena, ReturnsNotFoundWithoutMutatingCallerOwnedStorage) {
  auto arena = SyntheticDataArena{};
  const auto before = arena.storage;

  const auto result = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), arena.header, "absent");

  EXPECT_FALSE(result.found());
  EXPECT_EQ(result.error, jak1::DataArenaSymbolValueLookupError::NotFound);
  EXPECT_EQ(arena.storage, before);
}

TEST(Jak1DataArena, RejectsEmbeddedNullAndEmptyPairPseudoSymbolWithoutMutation) {
  auto arena = SyntheticDataArena{};
  const auto before = arena.storage;

  const auto embedded_null =
      jak1::find_data_arena_symbol_value_cell(arena.storage.data(), arena.storage.size(),
                                              arena.header, std::string_view("load\0state", 10));
  const auto empty_pair = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), arena.header, "_empty_");

  EXPECT_EQ(embedded_null.error, jak1::DataArenaSymbolValueLookupError::QueryContainsNull);
  EXPECT_EQ(empty_pair.error, jak1::DataArenaSymbolValueLookupError::UnsupportedPseudoSymbol);
  EXPECT_EQ(arena.storage, before);
}

TEST(Jak1DataArena, RejectsUnrepresentableHashWithoutMutation) {
  constexpr std::string_view kHashZeroName("\xff\xff\xff\xff", 4);
  auto arena = SyntheticDataArena{};
  const auto before = arena.storage;

  EXPECT_EQ(stateless_goal_crc32(kHashZeroName), 0U);
  const auto result = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), arena.header, kHashZeroName);

  EXPECT_EQ(result.error, jak1::DataArenaSymbolValueLookupError::UnrepresentableHash);
  EXPECT_EQ(arena.storage, before);
}

TEST(Jak1DataArena, RejectsMalformedMatchingStringPointersWithoutMutation) {
  constexpr std::string_view kName = "*load-state*";
  const auto hash = stateless_goal_crc32(kName);

  auto null_offset = SyntheticDataArena{};
  const auto null_offset_symbol = first_probe_offset(null_offset, kName);
  const auto null_offset_info = null_offset_symbol + jak1::SYM_INFO_OFFSET;
  write_u32(null_offset.storage.data(), null_offset_info, hash);
  write_u32(null_offset.storage.data(), null_offset_info + sizeof(std::uint32_t), 0);
  const auto null_offset_before = null_offset.storage;

  const auto null_offset_result = jak1::find_data_arena_symbol_value_cell(
      null_offset.storage.data(), null_offset.storage.size(), null_offset.header, kName);

  EXPECT_EQ(null_offset_result.error, jak1::DataArenaSymbolValueLookupError::MalformedString);
  EXPECT_EQ(null_offset.storage, null_offset_before);

  auto near_end = SyntheticDataArena{};
  const auto near_end_symbol = first_probe_offset(near_end, kName);
  const auto near_end_info = near_end_symbol + jak1::SYM_INFO_OFFSET;
  const auto near_end_raw_string = static_cast<std::uint32_t>(near_end.storage.size() - 8);
  const auto near_end_string = near_end_raw_string + BASIC_OFFSET;
  write_u32(near_end.storage.data(), near_end_raw_string,
            near_end.header.s7 + jak1_symbols::FIX_SYM_STRING_TYPE);
  write_u32(near_end.storage.data(), near_end_info, hash);
  write_u32(near_end.storage.data(), near_end_info + sizeof(std::uint32_t), near_end_string);
  const auto near_end_before = near_end.storage;

  const auto near_end_result = jak1::find_data_arena_symbol_value_cell(
      near_end.storage.data(), near_end.storage.size(), near_end.header, kName);

  EXPECT_EQ(near_end_result.error, jak1::DataArenaSymbolValueLookupError::MalformedString);
  EXPECT_EQ(near_end.storage, near_end_before);
}

TEST(Jak1DataArena, ContinuesPastMissingNulNameMismatchWithoutMutation) {
  constexpr std::string_view kName = "*load-state*";
  auto arena = SyntheticDataArena{};
  const auto hash = stateless_goal_crc32(kName);
  const auto symbol_offset = first_probe_offset(arena, kName);
  const auto info_offset = symbol_offset + jak1::SYM_INFO_OFFSET;
  const auto string_offset = write_string(&arena, kName);
  write_u32(arena.storage.data(), info_offset, hash);
  write_u32(arena.storage.data(), info_offset + sizeof(std::uint32_t), string_offset);
  arena.storage[string_offset + sizeof(std::uint32_t) + kName.size()] = std::byte{0xa5};
  const auto before = arena.storage;

  const auto result = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), arena.header, kName);

  EXPECT_EQ(result.error, jak1::DataArenaSymbolValueLookupError::NotFound);
  EXPECT_EQ(arena.storage, before);
}

TEST(Jak1DataArena, DoesNotTrustStringLengthWordForLookupBounds) {
  constexpr std::string_view kName = "*load-state*";
  auto arena = SyntheticDataArena{};
  const auto hash = stateless_goal_crc32(kName);
  const auto symbol_offset = first_probe_offset(arena, kName);
  const auto info_offset = symbol_offset + jak1::SYM_INFO_OFFSET;
  const auto string_offset = write_string(&arena, kName);
  write_u32(arena.storage.data(), string_offset, std::numeric_limits<std::uint32_t>::max());
  write_u32(arena.storage.data(), info_offset, hash);
  write_u32(arena.storage.data(), info_offset + sizeof(std::uint32_t), string_offset);

  const auto result = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), arena.header, kName);

  ASSERT_TRUE(result.found());
  EXPECT_EQ(result.cell.goal_offset, symbol_offset);
}

TEST(Jak1DataArena, AcceptsStringWithSpareCapacityAfterItsTerminator) {
  constexpr std::string_view kName = "*load-state*";
  auto arena = SyntheticDataArena{};
  const auto hash = stateless_goal_crc32(kName);
  const auto symbol_offset = first_probe_offset(arena, kName);
  const auto info_offset = symbol_offset + jak1::SYM_INFO_OFFSET;
  const auto string_offset = write_string(&arena, kName);
  arena.storage[string_offset + sizeof(std::uint32_t) + kName.size() + 1] = std::byte{0xa5};
  write_u32(arena.storage.data(), info_offset, hash);
  write_u32(arena.storage.data(), info_offset + sizeof(std::uint32_t), string_offset);

  const auto result = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), arena.header, kName);

  ASSERT_TRUE(result.found());
  EXPECT_EQ(result.cell.goal_offset, symbol_offset);
}

TEST(Jak1DataArena, RejectsWrappedSymbolTableBoundsWithoutMutation) {
  auto arena = SyntheticDataArena{};
  auto malformed_header = arena.header;
  malformed_header.symbol_table = std::numeric_limits<std::uint32_t>::max() - 0x100;
  const auto before = arena.storage;

  const auto result = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), malformed_header, "*load-state*");

  EXPECT_EQ(result.error, jak1::DataArenaSymbolValueLookupError::InvalidHeader);
  EXPECT_EQ(arena.storage, before);
}

TEST(Jak1DataArena, RejectsSelfConsistentShiftedSymbolTableTagsWithoutMutation) {
  auto arena = SyntheticDataArena{};
  auto shifted_header = arena.header;
  constexpr std::uint32_t kShift = 4;
  shifted_header.global_heap_base += kShift;
  shifted_header.global_heap_current += kShift;
  shifted_header.symbol_table += kShift;
  shifted_header.symbol_table_end += kShift;
  shifted_header.symbol_table2 += kShift;
  shifted_header.last_symbol += kShift;
  shifted_header.s7 += kShift;
  shifted_header.empty_pair += kShift;
  shifted_header.false_value += kShift;
  shifted_header.true_value += kShift;
  const auto before = arena.storage;

  const auto result = jak1::find_data_arena_symbol_value_cell(
      arena.storage.data(), arena.storage.size(), shifted_header, "*load-state*");

  EXPECT_EQ(result.error, jak1::DataArenaSymbolValueLookupError::InvalidHeader);
  EXPECT_EQ(arena.storage, before);
}

TEST(Jak1DataArena, RejectsShortStorageAndMalformedStringPointerWithoutMutation) {
  constexpr std::string_view kName = "*load-state*";
  auto short_storage = SyntheticDataArena{};
  const auto short_storage_before = short_storage.storage;

  const auto short_storage_result = jak1::find_data_arena_symbol_value_cell(
      short_storage.storage.data(), short_storage.storage.size() - 1, short_storage.header, kName);

  EXPECT_EQ(short_storage_result.error, jak1::DataArenaSymbolValueLookupError::InvalidHeader);
  EXPECT_EQ(short_storage.storage, short_storage_before);

  auto malformed_pointer = SyntheticDataArena{};
  const auto hash = stateless_goal_crc32(kName);
  const auto symbol_offset = first_probe_offset(malformed_pointer, kName);
  const auto info_offset = symbol_offset + jak1::SYM_INFO_OFFSET;
  write_u32(malformed_pointer.storage.data(), info_offset, hash);
  write_u32(malformed_pointer.storage.data(), info_offset + sizeof(std::uint32_t),
            malformed_pointer.next_string_offset);
  const auto malformed_pointer_before = malformed_pointer.storage;

  const auto malformed_pointer_result = jak1::find_data_arena_symbol_value_cell(
      malformed_pointer.storage.data(), malformed_pointer.storage.size(), malformed_pointer.header,
      kName);

  EXPECT_EQ(malformed_pointer_result.error, jak1::DataArenaSymbolValueLookupError::MalformedString);
  EXPECT_EQ(malformed_pointer.storage, malformed_pointer_before);
}

TEST(Jak1DataArena, RejectsEarlyLookupInputsWithoutMutation) {
  auto arena = SyntheticDataArena{};
  const auto before = arena.storage;

  const auto null_storage = jak1::find_data_arena_symbol_value_cell(nullptr, arena.storage.size(),
                                                                    arena.header, "*load-state*");
  EXPECT_EQ(null_storage.error, jak1::DataArenaSymbolValueLookupError::NullStorage);

  if constexpr (std::numeric_limits<std::size_t>::max() >
                std::numeric_limits<std::uint32_t>::max()) {
    const auto oversized_storage = jak1::find_data_arena_symbol_value_cell(
        arena.storage.data(),
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1, arena.header,
        "*load-state*");
    EXPECT_EQ(oversized_storage.error, jak1::DataArenaSymbolValueLookupError::StorageTooLarge);
  }

  EXPECT_EQ(arena.storage, before);
}

TEST(Jak1DataArena, ReadsBasicMethodValuesWithoutMutatingStorage) {
  auto fixture = BasicMethodFixture{};
  const auto before = fixture.arena.storage;

  const auto result = jak1::read_data_arena_basic_method_value(
      fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
      BasicMethodFixture::kObject, kResetMethodId);

  ASSERT_TRUE(result.found());
  EXPECT_EQ(result.value.object_type, BasicMethodFixture::kObjectType);
  EXPECT_EQ(result.value.allocated_size, BasicMethodFixture::kAllocatedSize);
  EXPECT_EQ(result.value.padded_size, BasicMethodFixture::kPaddedSize);
  EXPECT_EQ(result.value.method_value, BasicMethodFixture::kFunction);
  EXPECT_EQ(fixture.arena.storage, before);
}

TEST(Jak1DataArena, RejectsInvalidMethodLookupHeadersWithoutMutation) {
  auto fixture = BasicMethodFixture{};
  auto header = fixture.arena.header;
  header.symbol_table += 4;
  const auto before = fixture.arena.storage;

  const auto result = jak1::read_data_arena_basic_method_value(
      fixture.arena.storage.data(), fixture.arena.storage.size(), header,
      BasicMethodFixture::kObject, kResetMethodId);

  EXPECT_EQ(result.error, jak1::DataArenaBasicMethodValueLookupError::InvalidHeader);
  EXPECT_EQ(fixture.arena.storage, before);
}

TEST(Jak1DataArena, RejectsEarlyBasicMethodLookupInputsWithoutMutation) {
  auto fixture = BasicMethodFixture{};
  const auto before = fixture.arena.storage;

  const auto null_storage = jak1::read_data_arena_basic_method_value(
      nullptr, fixture.arena.storage.size(), fixture.arena.header, BasicMethodFixture::kObject,
      kResetMethodId);
  EXPECT_EQ(null_storage.error, jak1::DataArenaBasicMethodValueLookupError::NullStorage);

  if constexpr (std::numeric_limits<std::size_t>::max() >
                std::numeric_limits<std::uint32_t>::max()) {
    const auto oversized_storage = jak1::read_data_arena_basic_method_value(
        fixture.arena.storage.data(),
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1,
        fixture.arena.header, BasicMethodFixture::kObject, kResetMethodId);
    EXPECT_EQ(oversized_storage.error, jak1::DataArenaBasicMethodValueLookupError::StorageTooLarge);
  }

  EXPECT_EQ(fixture.arena.storage, before);
}

TEST(Jak1DataArena, ReadsDerivedBasicMethodOverridesWithoutMutation) {
  auto fixture = BasicMethodFixture{};
  write_u32(fixture.arena.storage.data(), BasicMethodFixture::kDerivedType - BASIC_OFFSET,
            BasicMethodFixture::kTypeType);
  write_u32(fixture.arena.storage.data(),
            BasicMethodFixture::kDerivedType + offsetof(jak1::Type, parent),
            BasicMethodFixture::kObjectType);
  write_u16(fixture.arena.storage.data(),
            BasicMethodFixture::kDerivedType + offsetof(jak1::Type, allocated_size),
            BasicMethodFixture::kAllocatedSize);
  write_u16(fixture.arena.storage.data(),
            BasicMethodFixture::kDerivedType + offsetof(jak1::Type, padded_size),
            BasicMethodFixture::kPaddedSize);
  write_u16(fixture.arena.storage.data(),
            BasicMethodFixture::kDerivedType + offsetof(jak1::Type, num_methods),
            BasicMethodFixture::kMethodCount);
  write_u32(fixture.arena.storage.data(),
            BasicMethodFixture::method_slot(BasicMethodFixture::kDerivedType, kResetMethodId),
            BasicMethodFixture::kDerivedFunction);
  write_u32(fixture.arena.storage.data(), BasicMethodFixture::kDerivedFunction - BASIC_OFFSET,
            BasicMethodFixture::kFunctionType);
  write_u32(fixture.arena.storage.data(), BasicMethodFixture::kObject - BASIC_OFFSET,
            BasicMethodFixture::kDerivedType);
  const auto before = fixture.arena.storage;

  const auto result = jak1::read_data_arena_basic_method_value(
      fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
      BasicMethodFixture::kObject, kResetMethodId);

  ASSERT_TRUE(result.found());
  EXPECT_EQ(result.value.object_type, BasicMethodFixture::kDerivedType);
  EXPECT_EQ(result.value.method_value, BasicMethodFixture::kDerivedFunction);
  EXPECT_EQ(fixture.arena.storage, before);
}

TEST(Jak1DataArena, ReturnsRawBasicMethodValuesWithoutFunctionValidation) {
  auto fixture = BasicMethodFixture{};
  write_u32(fixture.arena.storage.data(),
            BasicMethodFixture::method_slot(BasicMethodFixture::kObjectType, kResetMethodId), 0);
  const auto zero_before = fixture.arena.storage;

  const auto zero = jak1::read_data_arena_basic_method_value(
      fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
      BasicMethodFixture::kObject, kResetMethodId);

  ASSERT_TRUE(zero.found());
  EXPECT_EQ(zero.value.method_value, 0U);
  EXPECT_EQ(fixture.arena.storage, zero_before);

  constexpr std::uint32_t kNonBasicMethodValue = 2;
  write_u32(fixture.arena.storage.data(),
            BasicMethodFixture::method_slot(BasicMethodFixture::kObjectType, kResetMethodId),
            kNonBasicMethodValue);
  const auto non_basic_before = fixture.arena.storage;

  const auto non_basic = jak1::read_data_arena_basic_method_value(
      fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
      BasicMethodFixture::kObject, kResetMethodId);

  ASSERT_TRUE(non_basic.found());
  EXPECT_EQ(non_basic.value.method_value, kNonBasicMethodValue);
  EXPECT_EQ(fixture.arena.storage, non_basic_before);
}

TEST(Jak1DataArena, RejectsMalformedBasicObjectsAndTypesWithoutMutation) {
  {
    auto fixture = BasicMethodFixture{};
    const auto before = fixture.arena.storage;
    const auto result = jak1::read_data_arena_basic_method_value(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header, 0,
        kResetMethodId);
    EXPECT_EQ(result.error, jak1::DataArenaBasicMethodValueLookupError::InvalidObject);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    write_u32(fixture.arena.storage.data(), BasicMethodFixture::kObject - BASIC_OFFSET, 2);
    const auto before = fixture.arena.storage;
    const auto result = jak1::read_data_arena_basic_method_value(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId);
    EXPECT_EQ(result.error, jak1::DataArenaBasicMethodValueLookupError::InvalidObjectType);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    write_u32(fixture.arena.storage.data(),
              fixture.arena.header.s7 + jak1_symbols::FIX_SYM_TYPE_TYPE, 0);
    const auto before = fixture.arena.storage;
    const auto result = jak1::read_data_arena_basic_method_value(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId);
    EXPECT_EQ(result.error, jak1::DataArenaBasicMethodValueLookupError::InvalidTypeType);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    write_u32(fixture.arena.storage.data(), BasicMethodFixture::kTypeType - BASIC_OFFSET, 0);
    const auto before = fixture.arena.storage;
    const auto result = jak1::read_data_arena_basic_method_value(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId);
    EXPECT_EQ(result.error, jak1::DataArenaBasicMethodValueLookupError::InvalidTypeType);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    write_u32(fixture.arena.storage.data(), BasicMethodFixture::kObjectType - BASIC_OFFSET,
              BasicMethodFixture::kFunctionType);
    const auto before = fixture.arena.storage;
    const auto result = jak1::read_data_arena_basic_method_value(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId);
    EXPECT_EQ(result.error, jak1::DataArenaBasicMethodValueLookupError::InvalidTypeTag);
    EXPECT_EQ(fixture.arena.storage, before);
  }
}

TEST(Jak1DataArena, RejectsIncoherentAndUnreadableBasicObjectSizesWithoutMutation) {
  {
    auto fixture = BasicMethodFixture{};
    write_u16(fixture.arena.storage.data(),
              BasicMethodFixture::kObjectType + offsetof(jak1::Type, padded_size), 0x820);
    const auto before = fixture.arena.storage;
    const auto result = jak1::read_data_arena_basic_method_value(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId);
    EXPECT_EQ(result.error, jak1::DataArenaBasicMethodValueLookupError::InvalidObjectSize);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    write_u16(fixture.arena.storage.data(),
              BasicMethodFixture::kObjectType + offsetof(jak1::Type, allocated_size), 0);
    write_u16(fixture.arena.storage.data(),
              BasicMethodFixture::kObjectType + offsetof(jak1::Type, padded_size), 0);
    const auto before = fixture.arena.storage;
    const auto result = jak1::read_data_arena_basic_method_value(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId);
    EXPECT_EQ(result.error, jak1::DataArenaBasicMethodValueLookupError::InvalidObjectSize);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    write_u16(fixture.arena.storage.data(),
              BasicMethodFixture::kObjectType + offsetof(jak1::Type, allocated_size), 0xfff1);
    write_u16(fixture.arena.storage.data(),
              BasicMethodFixture::kObjectType + offsetof(jak1::Type, padded_size), 0);
    const auto before = fixture.arena.storage;
    const auto result = jak1::read_data_arena_basic_method_value(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId);
    EXPECT_EQ(result.error, jak1::DataArenaBasicMethodValueLookupError::InvalidObjectSize);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    constexpr std::uint32_t kNearEndObject = kSyntheticArenaSize - 0x800 + BASIC_OFFSET;
    static_assert((kNearEndObject & OFFSET_MASK) == BASIC_OFFSET);
    write_u32(fixture.arena.storage.data(), kNearEndObject - BASIC_OFFSET,
              BasicMethodFixture::kObjectType);
    const auto before = fixture.arena.storage;
    const auto result = jak1::read_data_arena_basic_method_value(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        kNearEndObject, kResetMethodId);
    EXPECT_EQ(result.error, jak1::DataArenaBasicMethodValueLookupError::InvalidObjectSize);
    EXPECT_EQ(fixture.arena.storage, before);
  }
}

TEST(Jak1DataArena, RejectsOutOfRangeAndOutOfBoundsMethodSlotsWithoutMutation) {
  {
    auto fixture = BasicMethodFixture{};
    const auto before = fixture.arena.storage;
    const auto result = jak1::read_data_arena_basic_method_value(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, BasicMethodFixture::kMethodCount);
    EXPECT_EQ(result.error, jak1::DataArenaBasicMethodValueLookupError::MethodOutOfRange);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    constexpr std::uint32_t kLogicalStorageSize = kSyntheticArenaSize - 4;
    constexpr std::uint32_t kNearEndType = kLogicalStorageSize - 0x10;
    static_assert((kNearEndType & OFFSET_MASK) == BASIC_OFFSET);
    write_u32(fixture.arena.storage.data(), BasicMethodFixture::kObject - BASIC_OFFSET,
              kNearEndType);
    write_u32(fixture.arena.storage.data(), kNearEndType - BASIC_OFFSET,
              BasicMethodFixture::kTypeType);
    write_u16(fixture.arena.storage.data(), kNearEndType + offsetof(jak1::Type, allocated_size),
              BasicMethodFixture::kAllocatedSize);
    write_u16(fixture.arena.storage.data(), kNearEndType + offsetof(jak1::Type, padded_size),
              BasicMethodFixture::kPaddedSize);
    write_u16(fixture.arena.storage.data(), kNearEndType + offsetof(jak1::Type, num_methods), 1);
    auto header = fixture.arena.header;
    header.global_heap_end = kLogicalStorageSize;
    const auto before = fixture.arena.storage;

    const auto result = jak1::read_data_arena_basic_method_value(
        fixture.arena.storage.data(), kLogicalStorageSize, header, BasicMethodFixture::kObject, 0);

    EXPECT_EQ(result.error, jak1::DataArenaBasicMethodValueLookupError::InvalidMethodSlot);
    EXPECT_EQ(fixture.arena.storage, before);
  }
}

TEST(Jak1DataArena, InvokesTrustedLoadStateResetBindingWithoutMutatingStorage) {
  auto fixture = BasicMethodFixture{};
  const auto bindings = std::array{load_state_reset_binding()};
  NativeMethodInvocationProbe probe;
  const auto before = fixture.arena.storage;

  const auto result = jak1::invoke_data_arena_native_basic_method1(
      fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
      BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
      &probe);

  ASSERT_TRUE(result.invoked());
  EXPECT_EQ(result.value, BasicMethodFixture::kObject);
  EXPECT_EQ(result.basic_method.object_type, BasicMethodFixture::kObjectType);
  EXPECT_EQ(result.basic_method.method_value, BasicMethodFixture::kFunction);
  EXPECT_EQ(probe.calls, 1U);
  EXPECT_EQ(probe.entry, &synthetic_load_state_reset);
  EXPECT_EQ(probe.object, BasicMethodFixture::kObject);
  EXPECT_EQ(probe.s7, fixture.arena.header.s7);
  EXPECT_EQ(probe.arena, fixture.arena.storage.data());
  EXPECT_EQ(fixture.arena.storage, before);
}

TEST(Jak1DataArena, RequiresTheExactTrustedMethodBindingTuple) {
  {
    auto fixture = BasicMethodFixture{};
    install_derived_reset(&fixture);
    const auto bindings = std::array{load_state_reset_binding()};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::UnboundMethod);
    EXPECT_EQ(result.basic_method.object_type, BasicMethodFixture::kDerivedType);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    constexpr std::uint32_t kOtherMethodId = kResetMethodId - 1;
    write_u32(fixture.arena.storage.data(),
              BasicMethodFixture::method_slot(BasicMethodFixture::kObjectType, kOtherMethodId),
              BasicMethodFixture::kFunction);
    const auto bindings = std::array{load_state_reset_binding()};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kOtherMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::UnboundMethod);
    EXPECT_EQ(result.basic_method.method_value, BasicMethodFixture::kFunction);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    write_u32(fixture.arena.storage.data(), BasicMethodFixture::kDerivedFunction - BASIC_OFFSET,
              BasicMethodFixture::kFunctionType);
    write_u32(fixture.arena.storage.data(),
              BasicMethodFixture::method_slot(BasicMethodFixture::kObjectType, kResetMethodId),
              BasicMethodFixture::kDerivedFunction);
    const auto bindings = std::array{load_state_reset_binding()};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::UnboundMethod);
    EXPECT_EQ(result.basic_method.method_value, BasicMethodFixture::kDerivedFunction);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    write_u16(fixture.arena.storage.data(),
              BasicMethodFixture::kObjectType + offsetof(jak1::Type, allocated_size), 0x81c);
    write_u16(fixture.arena.storage.data(),
              BasicMethodFixture::kObjectType + offsetof(jak1::Type, padded_size), 0x820);
    const auto bindings = std::array{load_state_reset_binding()};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::UnboundMethod);
    EXPECT_EQ(result.basic_method.allocated_size, 0x81c);
    EXPECT_EQ(result.basic_method.padded_size, 0x820);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    auto binding = load_state_reset_binding();
    binding.allocated_size = BasicMethodFixture::kAllocatedSize - 0x10;
    const auto bindings = std::array{binding};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::UnboundMethod);
    EXPECT_EQ(result.basic_method.allocated_size, BasicMethodFixture::kAllocatedSize);
    EXPECT_EQ(result.basic_method.padded_size, BasicMethodFixture::kPaddedSize);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    auto binding = load_state_reset_binding();
    binding.padded_size = BasicMethodFixture::kPaddedSize - 0x10;
    const auto bindings = std::array{binding};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::UnboundMethod);
    EXPECT_EQ(result.basic_method.allocated_size, BasicMethodFixture::kAllocatedSize);
    EXPECT_EQ(result.basic_method.padded_size, BasicMethodFixture::kPaddedSize);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }
}

TEST(Jak1DataArena, BindsTheActualDerivedTypeOnlyWhenTheTupleMatches) {
  auto fixture = BasicMethodFixture{};
  install_derived_reset(&fixture);
  auto derived_binding = load_state_reset_binding();
  derived_binding.object_type = BasicMethodFixture::kDerivedType;
  derived_binding.method_value = BasicMethodFixture::kDerivedFunction;
  const auto bindings = std::array{derived_binding};
  NativeMethodInvocationProbe probe;
  const auto before = fixture.arena.storage;

  const auto result = jak1::invoke_data_arena_native_basic_method1(
      fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
      BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
      &probe);

  ASSERT_TRUE(result.invoked());
  EXPECT_EQ(result.basic_method.object_type, BasicMethodFixture::kDerivedType);
  EXPECT_EQ(result.basic_method.method_value, BasicMethodFixture::kDerivedFunction);
  EXPECT_EQ(probe.calls, 1U);
  EXPECT_EQ(fixture.arena.storage, before);
}

TEST(Jak1DataArena, RejectsZeroAndIncorrectlyTaggedNativeMethodValues) {
  {
    auto fixture = BasicMethodFixture{};
    write_u32(fixture.arena.storage.data(),
              BasicMethodFixture::method_slot(BasicMethodFixture::kObjectType, kResetMethodId), 0);
    const auto bindings = std::array{load_state_reset_binding()};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::InvalidFunctionValue);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    write_u32(fixture.arena.storage.data(), BasicMethodFixture::kFunction - BASIC_OFFSET,
              BasicMethodFixture::kObjectType);
    const auto bindings = std::array{load_state_reset_binding()};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::InvalidFunctionTag);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    write_u32(fixture.arena.storage.data(), BasicMethodFixture::kFunctionType - BASIC_OFFSET, 0);
    const auto bindings = std::array{load_state_reset_binding()};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::InvalidFunctionType);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    write_u32(fixture.arena.storage.data(),
              fixture.arena.header.s7 + jak1_symbols::FIX_SYM_FUNCTION_TYPE, 0);
    const auto bindings = std::array{load_state_reset_binding()};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::InvalidFunctionType);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    constexpr std::uint32_t kOutOfBoundsFunction = kSyntheticArenaSize + BASIC_OFFSET;
    static_assert((kOutOfBoundsFunction & OFFSET_MASK) == BASIC_OFFSET);
    write_u32(fixture.arena.storage.data(),
              BasicMethodFixture::method_slot(BasicMethodFixture::kObjectType, kResetMethodId),
              kOutOfBoundsFunction);
    const auto bindings = std::array{load_state_reset_binding()};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::InvalidFunctionValue);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }
}

TEST(Jak1DataArena, RejectsInvalidOrUnknownNativeMethodBindingTables) {
  {
    auto fixture = BasicMethodFixture{};
    const auto binding = load_state_reset_binding();
    auto duplicate = binding;
    duplicate.entry = &alternate_synthetic_load_state_reset;
    const auto bindings = std::array{binding, duplicate};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::DuplicateBinding);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    auto binding = load_state_reset_binding();
    binding.entry = nullptr;
    const auto bindings = std::array{binding};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::NullNativeEntry);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    auto binding = load_state_reset_binding();
    binding.arity = 0;
    const auto bindings = std::array{binding};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::InvalidBindingArity);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    const auto binding = load_state_reset_binding();
    auto unrelated_invalid_binding = binding;
    unrelated_invalid_binding.object_type = BasicMethodFixture::kDerivedType;
    unrelated_invalid_binding.arity = 0;
    const auto bindings = std::array{binding, unrelated_invalid_binding};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::InvalidBindingArity);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    const std::array<jak1::DataArenaNativeMethodBinding1, 0> bindings{};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, &invoke_synthetic_native_method,
        &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::UnboundMethod);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }

  {
    auto fixture = BasicMethodFixture{};
    const auto bindings = std::array{load_state_reset_binding()};
    NativeMethodInvocationProbe probe;
    const auto before = fixture.arena.storage;

    const auto result = jak1::invoke_data_arena_native_basic_method1(
        fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
        BasicMethodFixture::kObject, kResetMethodId, bindings, nullptr, &probe);

    EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::NullInvoker);
    EXPECT_EQ(probe.calls, 0U);
    EXPECT_EQ(fixture.arena.storage, before);
  }
}

TEST(Jak1DataArena, PreservesBasicMethodLookupFailuresForNativeBindings) {
  auto fixture = BasicMethodFixture{};
  const auto bindings = std::array{load_state_reset_binding()};
  NativeMethodInvocationProbe probe;
  const auto before = fixture.arena.storage;

  const auto result = jak1::invoke_data_arena_native_basic_method1(
      fixture.arena.storage.data(), fixture.arena.storage.size(), fixture.arena.header,
      BasicMethodFixture::kObject, BasicMethodFixture::kMethodCount, bindings,
      &invoke_synthetic_native_method, &probe);

  EXPECT_EQ(result.error, jak1::DataArenaNativeMethodInvokeError::BasicMethodLookupFailed);
  EXPECT_EQ(result.lookup_error, jak1::DataArenaBasicMethodValueLookupError::MethodOutOfRange);
  EXPECT_EQ(probe.calls, 0U);
  EXPECT_EQ(fixture.arena.storage, before);
}
