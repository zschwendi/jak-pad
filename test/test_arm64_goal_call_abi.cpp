#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "common/symbols.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/memory_layout.h"
#include "game/kernel/jak1/data_arena.h"
#include "game/kernel/jak1/kscheme.h"
#include "gtest/gtest.h"

namespace {

struct Arm64GoalCallProbe {
  std::uint64_t argument0;
  std::uint64_t argument1;
  std::uintptr_t st;
  std::uintptr_t arena;
  std::uintptr_t entry;
  std::uint64_t entry_x0;
  std::uint64_t entry_x1;
  std::uintptr_t entry_x2;
  std::uintptr_t entry_x20;
  std::uintptr_t entry_x21;
  std::uintptr_t entry_x22;
  std::uint64_t entry_sp_mod16;
  std::uint64_t result;
  std::uint64_t caller_x20_after;
  std::uint64_t caller_x21_after;
  std::uint64_t caller_x22_after;
};

static_assert(sizeof(Arm64GoalCallProbe) == 128);
static_assert(offsetof(Arm64GoalCallProbe, argument0) == 0);
static_assert(offsetof(Arm64GoalCallProbe, argument1) == 8);
static_assert(offsetof(Arm64GoalCallProbe, st) == 16);
static_assert(offsetof(Arm64GoalCallProbe, arena) == 24);
static_assert(offsetof(Arm64GoalCallProbe, entry) == 32);
static_assert(offsetof(Arm64GoalCallProbe, entry_x0) == 40);
static_assert(offsetof(Arm64GoalCallProbe, entry_x1) == 48);
static_assert(offsetof(Arm64GoalCallProbe, entry_x2) == 56);
static_assert(offsetof(Arm64GoalCallProbe, entry_x20) == 64);
static_assert(offsetof(Arm64GoalCallProbe, entry_x21) == 72);
static_assert(offsetof(Arm64GoalCallProbe, entry_x22) == 80);
static_assert(offsetof(Arm64GoalCallProbe, entry_sp_mod16) == 88);
static_assert(offsetof(Arm64GoalCallProbe, result) == 96);
static_assert(offsetof(Arm64GoalCallProbe, caller_x20_after) == 104);
static_assert(offsetof(Arm64GoalCallProbe, caller_x21_after) == 112);
static_assert(offsetof(Arm64GoalCallProbe, caller_x22_after) == 120);

extern "C" void arm64_goal_call_abi_outer(Arm64GoalCallProbe* probe);
extern "C" std::uint64_t call_goal_asm_arm64(std::uint64_t,
                                             std::uint64_t,
                                             std::uint64_t,
                                             void*,
                                             void*,
                                             void*);
using Arm64GoalEntry =
    std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t);
using Arm64GoalExport0Entry = std::uint64_t (*)();
using Arm64GoalExport1Entry = std::uint64_t (*)(std::uint64_t);
using Arm64GoalExport2Entry = std::uint64_t (*)(std::uint64_t, std::uint64_t);
using Arm64GoalExport3Entry = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t);

static_assert(sizeof(Arm64GoalEntry) == sizeof(std::uintptr_t));
static_assert(sizeof(Arm64GoalExport0Entry) == sizeof(std::uintptr_t));
static_assert(sizeof(Arm64GoalExport1Entry) == sizeof(std::uintptr_t));
static_assert(sizeof(Arm64GoalExport2Entry) == sizeof(std::uintptr_t));
static_assert(sizeof(Arm64GoalExport3Entry) == sizeof(std::uintptr_t));

extern "C" std::uint64_t arm64_goal_call_abi_entry(std::uint64_t,
                                                    std::uint64_t,
                                                    std::uint64_t);
extern "C" std::uint64_t arm64_goal_call_false_like_entry(std::uint64_t,
                                                           std::uint64_t,
                                                           std::uint64_t);

#define OPENGOAL_AOT_EXPORT0(goal_name, c_symbol) \
  extern "C" std::uint64_t c_symbol();
#include "OpenGOALJak1FalseFunc.exports.h"
#include "OpenGOALJak1TrueFunc.exports.h"
#include "OpenGOALJak1LoadStateValue.exports.h"
#undef OPENGOAL_AOT_EXPORT0

#define OPENGOAL_AOT_SYMBOL_VALUE_OFFSET32(game_name, goal_name, value_type, c_symbol) \
  extern "C" std::int32_t c_symbol;
#include "OpenGOALJak1LoadStateValue.symbol-value-offset.h"
#undef OPENGOAL_AOT_SYMBOL_VALUE_OFFSET32

extern "C" {
std::int32_t goalpad_aot_jak1_load_state_symbol_offset = 0;
}

#define OPENGOAL_AOT_EXPORT1(goal_name, c_symbol) \
  extern "C" std::uint64_t c_symbol(std::uint64_t);
// clang-format off
#include "OpenGOALJak1Identity.exports.h"
#include "OpenGOALJak1Lognot.exports.h"
#include "OpenGOALJak1GlstNodeName.exports.h"
#include "OpenGOALJak1LoadStateReset.exports.h"
// clang-format on
#undef OPENGOAL_AOT_EXPORT1

#define OPENGOAL_AOT_EXPORT2(goal_name, c_symbol) \
  extern "C" std::uint64_t c_symbol(std::uint64_t, std::uint64_t);
#include "OpenGOALJak1LevelGroupLoadCommandsSet.exports.h"
#include "OpenGOALJak1WantVis.exports.h"
#undef OPENGOAL_AOT_EXPORT2

#define OPENGOAL_AOT_EXPORT3(goal_name, c_symbol) \
  extern "C" std::uint64_t c_symbol(std::uint64_t, std::uint64_t, std::uint64_t);
#include "OpenGOALJak1WantLevels.exports.h"
#undef OPENGOAL_AOT_EXPORT3

struct Arm64GoalExport0 {
  std::string_view goal_name;
  Arm64GoalExport0Entry entry;
};

struct Arm64GoalExport1 {
  std::string_view goal_name;
  Arm64GoalExport1Entry entry;
};

struct Arm64GoalExport2 {
  std::string_view goal_name;
  Arm64GoalExport2Entry entry;
};

struct Arm64GoalExport3 {
  std::string_view goal_name;
  Arm64GoalExport3Entry entry;
};

struct Arm64GoalSymbolValueOffset32 {
  std::string_view game_name;
  std::string_view goal_name;
  std::string_view value_type;
  std::int32_t* offset;
};

constexpr Arm64GoalExport0 kArm64GoalExports0[] = {
#define OPENGOAL_AOT_EXPORT0(goal_name, c_symbol) {goal_name, &c_symbol},
#include "OpenGOALJak1FalseFunc.exports.h"
#include "OpenGOALJak1TrueFunc.exports.h"
#include "OpenGOALJak1LoadStateValue.exports.h"
#undef OPENGOAL_AOT_EXPORT0
};

constexpr Arm64GoalSymbolValueOffset32 kArm64GoalSymbolValueOffsets32[] = {
#define OPENGOAL_AOT_SYMBOL_VALUE_OFFSET32(game_name, goal_name, value_type, c_symbol) \
  {game_name, goal_name, value_type, &c_symbol},
#include "OpenGOALJak1LoadStateValue.symbol-value-offset.h"
#undef OPENGOAL_AOT_SYMBOL_VALUE_OFFSET32
};

constexpr Arm64GoalExport1 kArm64GoalExports1[] = {
#define OPENGOAL_AOT_EXPORT1(goal_name, c_symbol) {goal_name, &c_symbol},
// clang-format off
#include "OpenGOALJak1Identity.exports.h"
#include "OpenGOALJak1Lognot.exports.h"
#include "OpenGOALJak1GlstNodeName.exports.h"
#include "OpenGOALJak1LoadStateReset.exports.h"
// clang-format on
#undef OPENGOAL_AOT_EXPORT1
};

constexpr Arm64GoalExport2 kArm64GoalExports2[] = {
#define OPENGOAL_AOT_EXPORT2(goal_name, c_symbol) {goal_name, &c_symbol},
#include "OpenGOALJak1LevelGroupLoadCommandsSet.exports.h"
#include "OpenGOALJak1WantVis.exports.h"
#undef OPENGOAL_AOT_EXPORT2
};

constexpr Arm64GoalExport3 kArm64GoalExports3[] = {
#define OPENGOAL_AOT_EXPORT3(goal_name, c_symbol) {goal_name, &c_symbol},
#include "OpenGOALJak1WantLevels.exports.h"
#undef OPENGOAL_AOT_EXPORT3
};

static_assert(sizeof(kArm64GoalExports0) == 3 * sizeof(Arm64GoalExport0));
static_assert(sizeof(kArm64GoalExports1) == 4 * sizeof(Arm64GoalExport1));
static_assert(sizeof(kArm64GoalExports2) == 2 * sizeof(Arm64GoalExport2));
static_assert(sizeof(kArm64GoalExports3) == sizeof(Arm64GoalExport3));
static_assert(sizeof(kArm64GoalSymbolValueOffsets32) == sizeof(Arm64GoalSymbolValueOffset32));

template <typename Entry>
std::uintptr_t entry_address(Entry entry) {
  static_assert(sizeof(Entry) == sizeof(std::uintptr_t));
  return reinterpret_cast<std::uintptr_t>(entry);
}

Arm64GoalCallProbe make_probe(std::uintptr_t entry, std::uintptr_t st, std::uintptr_t arena) {
  return {
      .argument0 = 0x1122334455667788,
      .argument1 = 0x8877665544332211,
      .st = st,
      .arena = arena,
      .entry = entry,
  };
}

void expect_caller_registers_restored(const Arm64GoalCallProbe& probe) {
  EXPECT_EQ(probe.caller_x20_after, 0x14);
  EXPECT_EQ(probe.caller_x21_after, 0x15);
  EXPECT_EQ(probe.caller_x22_after, 0x16);
}

std::uint32_t read_u32(const std::byte* storage, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, storage + offset, sizeof(value));
  return value;
}

void write_u32(std::byte* storage, std::size_t offset, std::uint32_t value) {
  std::memcpy(storage + offset, &value, sizeof(value));
}

bool is_zero(std::byte value) {
  return value == std::byte{0};
}

struct ExpectedArenaWord {
  std::size_t offset;
  std::uint32_t value;
};

template <typename Arena, std::size_t N>
void expect_sparse_arena(const Arena& arena,
                         const std::array<ExpectedArenaWord, N>& expected_words) {
  std::size_t next_offset = 0;
  for (const auto& [offset, value] : expected_words) {
    ASSERT_LE(next_offset, offset);
    ASSERT_LE(offset, arena.size());
    ASSERT_LE(sizeof(value), arena.size() - offset);
    EXPECT_TRUE(std::all_of(arena.begin() + static_cast<std::ptrdiff_t>(next_offset),
                            arena.begin() + static_cast<std::ptrdiff_t>(offset), is_zero));
    EXPECT_EQ(read_u32(arena.data(), offset), value);
    next_offset = offset + sizeof(value);
  }
  EXPECT_TRUE(
      std::all_of(arena.begin() + static_cast<std::ptrdiff_t>(next_offset), arena.end(), is_zero));
}

TEST(Arm64GoalCallAbi, forwards_goal_register_context_and_restores_caller_registers) {
  std::array<std::byte, 64> arena{};
  constexpr std::uintptr_t kSt = 0x14fd24;
  auto probe = make_probe(entry_address(arm64_goal_call_abi_entry), kSt,
                          reinterpret_cast<std::uintptr_t>(arena.data()));

  arm64_goal_call_abi_outer(&probe);

  EXPECT_EQ(probe.entry_x0, probe.argument0);
  EXPECT_EQ(probe.entry_x1, probe.argument1);
  EXPECT_EQ(probe.entry_x2, reinterpret_cast<std::uintptr_t>(&probe));
  EXPECT_EQ(probe.entry_x20, kSt);
  EXPECT_EQ(probe.entry_x21, kSt);
  EXPECT_EQ(probe.entry_x22, reinterpret_cast<std::uintptr_t>(arena.data()));
  EXPECT_EQ(probe.entry_sp_mod16, 0);
  EXPECT_EQ(probe.result, probe.argument0 ^ probe.argument1);
  expect_caller_registers_restored(probe);
}

TEST(Arm64GoalCallAbi, calls_a_static_false_like_entry_with_the_symbol_table_register) {
  std::array<std::byte, 64> arena{};
  constexpr std::uintptr_t kSt = 0x14fd24;
  auto probe = make_probe(entry_address(arm64_goal_call_false_like_entry), kSt,
                          reinterpret_cast<std::uintptr_t>(arena.data()));

  arm64_goal_call_abi_outer(&probe);

  EXPECT_EQ(probe.result, kSt);
  expect_caller_registers_restored(probe);
}

TEST(Arm64GoalCallAbi, reads_live_load_state_symbol_values_through_a_signed_s7_offset_binding) {
  std::vector<std::byte> arena(jak1::minimum_data_arena_header_size());
  const auto initialized = jak1::initialize_data_arena_header(arena.data(), arena.size());
  ASSERT_TRUE(initialized.ok());

  ASSERT_EQ(kArm64GoalExports0[2].goal_name, "load-state-value");
  ASSERT_NE(kArm64GoalExports0[2].entry, nullptr);
  ASSERT_EQ(kArm64GoalSymbolValueOffsets32[0].game_name, "jak1");
  ASSERT_EQ(kArm64GoalSymbolValueOffsets32[0].goal_name, "*load-state*");
  ASSERT_EQ(kArm64GoalSymbolValueOffsets32[0].value_type, "load-state");
  ASSERT_EQ(kArm64GoalSymbolValueOffsets32[0].offset,
            &goalpad_aot_jak1_load_state_symbol_offset);

  const auto st = static_cast<std::uintptr_t>(initialized.header.s7);
  const auto arena_address = reinterpret_cast<std::uintptr_t>(arena.data());
  constexpr std::int32_t kFirstSymbolOffset = 0x400;
  constexpr std::int32_t kSecondSymbolOffset = -0x400;
  static_assert(kFirstSymbolOffset >= jak1_symbols::FIX_FIXED_SYM_END_OFFSET);
  static_assert(-kSecondSymbolOffset >= jak1_symbols::FIX_FIXED_SYM_END_OFFSET);
  ASSERT_GE(st, static_cast<std::uintptr_t>(-kSecondSymbolOffset));
  ASSERT_LE(st + static_cast<std::uintptr_t>(kFirstSymbolOffset) + sizeof(std::uint32_t),
            arena.size());
  const auto first_slot = st + static_cast<std::uintptr_t>(kFirstSymbolOffset);
  const auto second_slot = st - static_cast<std::uintptr_t>(-kSecondSymbolOffset);
  ASSERT_EQ(read_u32(arena.data(), first_slot), 0u);
  ASSERT_EQ(read_u32(arena.data(), second_slot), 0u);

  const auto invoke = [&](std::int32_t symbol_offset) {
    goalpad_aot_jak1_load_state_symbol_offset = symbol_offset;
    auto probe = make_probe(entry_address(kArm64GoalExports0[2].entry), st, arena_address);
    arm64_goal_call_abi_outer(&probe);
    expect_caller_registers_restored(probe);
    return probe;
  };

  write_u32(arena.data(), first_slot, 0u);
  const auto zero_value = invoke(kFirstSymbolOffset);
  EXPECT_EQ(zero_value.result, 0u);

  constexpr std::uint32_t kReboundFirstValue = 0x00012345;
  write_u32(arena.data(), first_slot, kReboundFirstValue);
  const auto rebound_first_value = invoke(kFirstSymbolOffset);
  EXPECT_EQ(rebound_first_value.result, kReboundFirstValue);
  EXPECT_EQ(rebound_first_value.result >> 32, 0u);

  constexpr std::uint32_t kSecondValue = 0x81234567;
  write_u32(arena.data(), second_slot, kSecondValue);
  const auto rebound_second_slot = invoke(kSecondSymbolOffset);
  EXPECT_EQ(rebound_second_slot.result, UINT64_C(0x0000000081234567));
  EXPECT_EQ(rebound_second_slot.result >> 32, 0u);
  EXPECT_EQ(read_u32(arena.data(), first_slot), kReboundFirstValue);
  EXPECT_EQ(read_u32(arena.data(), second_slot), kSecondValue);
}

TEST(Arm64GoalCallAbi, executes_generated_jak1_aot_artifacts_against_one_data_arena) {
  std::vector<std::byte> arena(jak1::minimum_data_arena_header_size());
  const auto initialized = jak1::initialize_data_arena_header(arena.data(), arena.size());
  ASSERT_TRUE(initialized.ok());

  ASSERT_EQ(kArm64GoalExports0[0].goal_name, "false-func");
  ASSERT_EQ(kArm64GoalExports0[1].goal_name, "true-func");
  ASSERT_EQ(kArm64GoalExports1[0].goal_name, "identity");
  ASSERT_EQ(kArm64GoalExports1[1].goal_name, "lognot");
  ASSERT_EQ(kArm64GoalExports1[2].goal_name, "glst-node-name");
  ASSERT_EQ(kArm64GoalExports2[0].goal_name, "level-group-load-commands-set!");
  ASSERT_NE(kArm64GoalExports0[0].entry, nullptr);
  ASSERT_NE(kArm64GoalExports0[1].entry, nullptr);
  ASSERT_NE(kArm64GoalExports1[0].entry, nullptr);
  ASSERT_NE(kArm64GoalExports1[1].entry, nullptr);
  ASSERT_NE(kArm64GoalExports1[2].entry, nullptr);
  ASSERT_NE(kArm64GoalExports2[0].entry, nullptr);

  const auto st = static_cast<std::uintptr_t>(initialized.header.s7);
  const auto arena_address = reinterpret_cast<std::uintptr_t>(arena.data());
  auto invoke = [&](auto entry, std::uintptr_t logical_base,
                    std::uint64_t argument0 = 0x1122334455667788,
                    std::uint64_t argument1 = 0x8877665544332211) {
    auto probe = make_probe(entry_address(entry), st, logical_base);
    probe.argument0 = argument0;
    probe.argument1 = argument1;
    arm64_goal_call_abi_outer(&probe);
    expect_caller_registers_restored(probe);
    return probe;
  };

  const auto false_probe = invoke(kArm64GoalExports0[0].entry, arena_address);
  const auto true_probe = invoke(kArm64GoalExports0[1].entry, arena_address);
  const auto identity_probe = invoke(kArm64GoalExports1[0].entry, arena_address);
  const auto lognot_probe = invoke(kArm64GoalExports1[1].entry, arena_address);

  EXPECT_EQ(false_probe.result, initialized.header.false_value);
  EXPECT_EQ(true_probe.result, initialized.header.true_value);
  EXPECT_NE(identity_probe.argument0, 0);
  EXPECT_EQ(identity_probe.result, identity_probe.argument0);
  EXPECT_EQ(lognot_probe.result, ~lognot_probe.argument0);

  constexpr std::size_t kSecondLogicalBaseOffset = 0x40;
  constexpr std::size_t kPrivnameOffset = 8;
  constexpr std::size_t kBasicPointerBias = 4;
  constexpr std::size_t kEmittedStoreOffset = 0x20;
  constexpr std::size_t kRawLoadCommandsOffset = 0x24;
  constexpr std::uint32_t kFirstPair = 0x81234567;
  constexpr std::uint32_t kSecondPair = 0xf2345678;
  static_assert(kBasicPointerBias + kEmittedStoreOffset == kRawLoadCommandsOffset);
  const auto raw_object_offset = static_cast<std::size_t>(initialized.header.global_heap_current);
  ASSERT_NE(raw_object_offset, 0);
  ASSERT_LE(raw_object_offset, arena.size());
  ASSERT_LE(kPrivnameOffset, arena.size() - raw_object_offset);
  const auto first_privname_offset = raw_object_offset + kPrivnameOffset;
  ASSERT_LE(sizeof(kFirstPair), arena.size() - first_privname_offset);

  ASSERT_LE(kBasicPointerBias, arena.size() - raw_object_offset);
  const auto first_object_pointer = raw_object_offset + kBasicPointerBias;
  ASSERT_LE(kEmittedStoreOffset, arena.size() - first_object_pointer);
  const auto first_load_commands_offset = first_object_pointer + kEmittedStoreOffset;
  ASSERT_EQ(first_load_commands_offset, raw_object_offset + kRawLoadCommandsOffset);
  ASSERT_LE(sizeof(kFirstPair), arena.size() - first_load_commands_offset);
  ASSERT_LE(kRawLoadCommandsOffset + 2 * sizeof(std::uint32_t), arena.size() - raw_object_offset);

  ASSERT_LE(kSecondLogicalBaseOffset, arena.size());
  ASSERT_LE(raw_object_offset, arena.size() - kSecondLogicalBaseOffset);
  const auto second_node_offset = kSecondLogicalBaseOffset + raw_object_offset;
  ASSERT_LE(kPrivnameOffset, arena.size() - second_node_offset);
  const auto second_privname_offset = second_node_offset + kPrivnameOffset;
  ASSERT_LE(sizeof(kSecondPair), arena.size() - second_privname_offset);

  ASSERT_LE(kBasicPointerBias, arena.size() - second_node_offset);
  const auto second_object_pointer = second_node_offset + kBasicPointerBias;
  ASSERT_LE(kEmittedStoreOffset, arena.size() - second_object_pointer);
  const auto second_load_commands_offset = second_object_pointer + kEmittedStoreOffset;
  ASSERT_EQ(second_load_commands_offset, second_node_offset + kRawLoadCommandsOffset);
  ASSERT_LE(sizeof(kSecondPair), arena.size() - second_load_commands_offset);
  ASSERT_LE(kRawLoadCommandsOffset + 2 * sizeof(std::uint32_t), arena.size() - second_node_offset);

  std::memcpy(arena.data() + first_privname_offset, &kFirstPair, sizeof(kFirstPair));
  std::memcpy(arena.data() + second_privname_offset, &kSecondPair, sizeof(kSecondPair));

  const auto first_glst_node_name_probe =
      invoke(kArm64GoalExports1[2].entry, arena_address, raw_object_offset);
  const auto second_arena_address =
      reinterpret_cast<std::uintptr_t>(arena.data() + kSecondLogicalBaseOffset);
  const auto second_glst_node_name_probe =
      invoke(kArm64GoalExports1[2].entry, second_arena_address, raw_object_offset);

  EXPECT_EQ(first_glst_node_name_probe.result, UINT64_C(0x0000000081234567));
  EXPECT_EQ(second_glst_node_name_probe.result, UINT64_C(0x00000000f2345678));
  EXPECT_EQ(first_glst_node_name_probe.result >> 32, 0);
  EXPECT_EQ(second_glst_node_name_probe.result >> 32, 0);

  const auto first_level_group_load_commands_set_probe =
      invoke(kArm64GoalExports2[0].entry, arena_address, first_object_pointer, kFirstPair);
  const auto second_level_group_load_commands_set_probe =
      invoke(kArm64GoalExports2[0].entry, second_arena_address,
             raw_object_offset + kBasicPointerBias, kSecondPair);

  EXPECT_EQ(first_level_group_load_commands_set_probe.result, kFirstPair);
  EXPECT_EQ(second_level_group_load_commands_set_probe.result, kSecondPair);
  EXPECT_EQ(read_u32(arena.data(), raw_object_offset + 0x20), 0u);
  EXPECT_EQ(read_u32(arena.data(), first_load_commands_offset), kFirstPair);
  EXPECT_EQ(read_u32(arena.data(), raw_object_offset + 0x28), 0u);
  EXPECT_EQ(read_u32(arena.data(), second_node_offset + 0x20), 0u);
  EXPECT_EQ(read_u32(arena.data(), second_load_commands_offset), kSecondPair);
  EXPECT_EQ(read_u32(arena.data(), second_node_offset + 0x28), 0u);

  const std::array expected_words{
      ExpectedArenaWord{static_cast<std::size_t>(GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, base)),
                        initialized.header.global_heap_base},
      ExpectedArenaWord{static_cast<std::size_t>(GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, top)),
                        initialized.header.global_heap_end},
      ExpectedArenaWord{
          static_cast<std::size_t>(GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, current)),
          initialized.header.global_heap_current},
      ExpectedArenaWord{
          static_cast<std::size_t>(GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, top_base)),
          initialized.header.global_heap_end},
      ExpectedArenaWord{
          static_cast<std::size_t>(initialized.header.s7 + jak1_symbols::FIX_SYM_EMPTY_CAR),
          initialized.header.empty_pair},
      ExpectedArenaWord{
          static_cast<std::size_t>(initialized.header.s7 + jak1_symbols::FIX_SYM_EMPTY_CDR),
          initialized.header.empty_pair},
      ExpectedArenaWord{
          static_cast<std::size_t>(initialized.header.s7 + jak1_symbols::FIX_SYM_FALSE),
          initialized.header.false_value},
      ExpectedArenaWord{
          static_cast<std::size_t>(initialized.header.s7 + jak1_symbols::FIX_SYM_TRUE),
          initialized.header.true_value},
      ExpectedArenaWord{
          static_cast<std::size_t>(initialized.header.s7 + jak1_symbols::FIX_SYM_GLOBAL_HEAP),
          initialized.header.global_heap_info},
      ExpectedArenaWord{first_privname_offset, kFirstPair},
      ExpectedArenaWord{first_load_commands_offset, kFirstPair},
      ExpectedArenaWord{second_privname_offset, kSecondPair},
      ExpectedArenaWord{second_load_commands_offset, kSecondPair},
  };
  expect_sparse_arena(arena, expected_words);
}

TEST(Arm64GoalCallAbi, executes_generated_jak1_want_vis_against_two_guarded_arena_bases) {
  struct alignas(16) GuardedWantVisArena {
    std::array<std::byte, 16> leading_guard;
    std::array<std::byte, 0x80> storage;
    std::array<std::byte, 16> trailing_guard;
  } arena{};

  constexpr auto kGuardValue = std::byte{0xa5};
  std::fill(arena.leading_guard.begin(), arena.leading_guard.end(), kGuardValue);
  std::fill(arena.trailing_guard.begin(), arena.trailing_guard.end(), kGuardValue);

  ASSERT_EQ(kArm64GoalExports2[1].goal_name, "want-vis");
  ASSERT_NE(kArm64GoalExports2[1].entry, nullptr);

  constexpr std::uintptr_t kSt = 0x14fd24;
  constexpr std::size_t kSecondArenaBaseOffset = 0x40;
  constexpr std::size_t kBasicPointerBias = 4;
  constexpr std::size_t kEmittedStoreOffset = 0x20;
  constexpr std::size_t kRawVisNickOffset = 0x24;
  constexpr std::uint64_t kFirstSymbol = UINT64_C(0xaabbccdd81234567);
  constexpr std::uint64_t kSecondSymbol = UINT64_C(0x11223344f2345678);
  static_assert(kBasicPointerBias + kEmittedStoreOffset == kRawVisNickOffset);
  static_assert(kSecondArenaBaseOffset + kRawVisNickOffset + sizeof(std::uint32_t) <=
                sizeof(arena.storage));

  const auto first_arena_base = reinterpret_cast<std::uintptr_t>(arena.storage.data());
  const auto second_arena_base = first_arena_base + kSecondArenaBaseOffset;
  const auto invoke = [&](std::uintptr_t arena_base, std::uint64_t symbol) {
    auto probe = make_probe(entry_address(kArm64GoalExports2[1].entry), kSt, arena_base);
    probe.argument0 = kBasicPointerBias;
    probe.argument1 = symbol;
    arm64_goal_call_abi_outer(&probe);
    expect_caller_registers_restored(probe);
    return probe;
  };

  const auto first_probe = invoke(first_arena_base, kFirstSymbol);
  EXPECT_EQ(first_probe.result, 0u);
  EXPECT_EQ(read_u32(arena.storage.data(), kRawVisNickOffset),
            static_cast<std::uint32_t>(kFirstSymbol));
  EXPECT_EQ(read_u32(arena.storage.data(), kSecondArenaBaseOffset + kRawVisNickOffset), 0u);
  expect_sparse_arena(
      arena.storage,
      std::array{ExpectedArenaWord{kRawVisNickOffset, static_cast<std::uint32_t>(kFirstSymbol)}});

  const auto second_probe = invoke(second_arena_base, kSecondSymbol);
  EXPECT_EQ(second_probe.result, 0u);
  EXPECT_EQ(read_u32(arena.storage.data(), kRawVisNickOffset),
            static_cast<std::uint32_t>(kFirstSymbol));
  EXPECT_EQ(read_u32(arena.storage.data(), kSecondArenaBaseOffset + kRawVisNickOffset),
            static_cast<std::uint32_t>(kSecondSymbol));
  expect_sparse_arena(
      arena.storage,
      std::array{ExpectedArenaWord{kRawVisNickOffset, static_cast<std::uint32_t>(kFirstSymbol)},
                 ExpectedArenaWord{kSecondArenaBaseOffset + kRawVisNickOffset,
                                   static_cast<std::uint32_t>(kSecondSymbol)}});
  EXPECT_TRUE(std::all_of(arena.leading_guard.begin(), arena.leading_guard.end(),
                          [=](std::byte value) { return value == kGuardValue; }));
  EXPECT_TRUE(std::all_of(arena.trailing_guard.begin(), arena.trailing_guard.end(),
                          [=](std::byte value) { return value == kGuardValue; }));
}

TEST(Arm64GoalCallAbi, executes_generated_jak1_want_levels_branch_matrix_against_guarded_arenas) {
  struct WantLevelsSlot {
    std::uint32_t name;
    std::uint32_t display;
    std::uint32_t force_vis;
    std::uint32_t force_inside;
  };
  struct WantLevelsCase {
    std::string_view name;
    std::array<WantLevelsSlot, 2> initial;
    std::uint32_t argument0;
    std::uint32_t argument1;
    std::array<WantLevelsSlot, 2> expected;
  };

  constexpr std::size_t kGuardSize = 16;
  constexpr std::size_t kStorageSize = 0x80;
  constexpr std::size_t kThis = 4;
  constexpr std::size_t kSlotSize = 0x10;
  constexpr std::uint32_t kFalse = 0x14fd24;
  constexpr std::uint32_t kLevelA = 0x81234567;
  constexpr std::uint32_t kLevelB = 0xf2345678;
  constexpr std::uint32_t kLevelC = 0x89abcdef;
  constexpr std::uint32_t kLevelD = 0xcafebabe;
  constexpr std::uint32_t kFirstDisplay = 0x91a2b3c4;
  constexpr std::uint32_t kFirstForceVis = 0xa1b2c3d4;
  constexpr std::uint32_t kFirstForceInside = 0xb1c2d3e4;
  constexpr std::uint32_t kSecondDisplay = 0xc1d2e3f4;
  constexpr std::uint32_t kSecondForceVis = 0xd1e2f304;
  constexpr std::uint32_t kSecondForceInside = 0xe1f20314;
  constexpr std::byte kCanary = std::byte{0xa5};
  constexpr WantLevelsSlot kSlotA{kLevelA, kFirstDisplay, kFirstForceVis, kFirstForceInside};
  constexpr WantLevelsSlot kSlotB{kLevelB, kSecondDisplay, kSecondForceVis, kSecondForceInside};
  constexpr WantLevelsSlot kSlotC{kLevelC, kFirstDisplay, kFirstForceVis, kFirstForceInside};
  constexpr WantLevelsSlot kSlotD{kLevelD, kSecondDisplay, kSecondForceVis, kSecondForceInside};
  constexpr WantLevelsSlot kSlotAReset{kLevelA, kFalse, kFalse, kFalse};
  constexpr WantLevelsSlot kSlotBReset{kLevelB, kFalse, kFalse, kFalse};

  static_assert(kThis + 2 * kSlotSize <= kStorageSize);
  static_assert(kThis + 0 * kSlotSize == 0x4);
  static_assert(kThis + 1 * kSlotSize == 0x14);
  ASSERT_EQ(kArm64GoalExports3[0].goal_name, "want-levels");
  ASSERT_NE(kArm64GoalExports3[0].entry, nullptr);

  constexpr std::array cases{
      WantLevelsCase{"both retained", {kSlotA, kSlotB}, kLevelA, kLevelB, {kSlotA, kSlotB}},
      WantLevelsCase{
          "both replaced", {kSlotC, kSlotD}, kLevelA, kLevelB, {kSlotAReset, kSlotBReset}},
      WantLevelsCase{
          "duplicate arguments", {kSlotA, kSlotC}, kLevelA, kLevelA, {kSlotA, kSlotAReset}},
      WantLevelsCase{"argument zero false",
                     {kSlotC, kSlotD},
                     kFalse,
                     kLevelB,
                     {kSlotBReset,
                      WantLevelsSlot{kFalse, kSecondDisplay, kSecondForceVis, kSecondForceInside}}},
      WantLevelsCase{"argument one false",
                     {kSlotC, kSlotD},
                     kLevelA,
                     kFalse,
                     {kSlotAReset,
                      WantLevelsSlot{kFalse, kSecondDisplay, kSecondForceVis, kSecondForceInside}}},
      WantLevelsCase{"both false",
                     {kSlotC, kSlotD},
                     kFalse,
                     kFalse,
                     {WantLevelsSlot{kFalse, kFirstDisplay, kFirstForceVis, kFirstForceInside},
                      WantLevelsSlot{kFalse, kSecondDisplay, kSecondForceVis, kSecondForceInside}}},
  };

  const auto write_slot = [](std::byte* storage, std::size_t index, const WantLevelsSlot& slot) {
    const auto offset = kThis + index * kSlotSize;
    write_u32(storage, offset + 0, slot.name);
    write_u32(storage, offset + 4, slot.display);
    write_u32(storage, offset + 8, slot.force_vis);
    write_u32(storage, offset + 12, slot.force_inside);
  };

  for (const auto& test_case : cases) {
    std::array<std::byte, kGuardSize + kStorageSize + kGuardSize> actual;
    actual.fill(kCanary);
    auto expected = actual;
    auto* actual_storage = actual.data() + kGuardSize;
    auto* expected_storage = expected.data() + kGuardSize;
    for (std::size_t index = 0; index < test_case.initial.size(); ++index) {
      write_slot(actual_storage, index, test_case.initial[index]);
      write_slot(expected_storage, index, test_case.expected[index]);
    }

    const auto result = call_goal_asm_arm64(
        kThis, test_case.argument0, test_case.argument1,
        reinterpret_cast<void*>(entry_address(kArm64GoalExports3[0].entry)),
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(kFalse)), actual_storage);

    SCOPED_TRACE(test_case.name);
    EXPECT_EQ(result, 0u);
    EXPECT_EQ(actual, expected);
    EXPECT_TRUE(std::all_of(actual.begin(), actual.begin() + kGuardSize,
                            [=](std::byte value) { return value == kCanary; }));
    EXPECT_TRUE(std::all_of(actual.end() - kGuardSize, actual.end(),
                            [=](std::byte value) { return value == kCanary; }));
  }
}

TEST(Arm64GoalCallAbi, executes_generated_jak1_load_state_reset_against_two_guarded_arena_bases) {
  constexpr std::size_t kGuardSize = 16;
  constexpr std::size_t kStorageSize = 0x112c;
  struct alignas(16) GuardedLoadStateArena {
    std::array<std::byte, kGuardSize> leading_guard;
    std::array<std::byte, kStorageSize> storage;
    std::array<std::byte, kGuardSize> trailing_guard;
  };

  constexpr std::size_t kThis = 4;
  constexpr std::size_t kSecondArenaBaseOffset = 0x900;
  constexpr std::size_t kLoadStateSize = 0x82c;
  constexpr std::size_t kWantOffset = 0x4;
  constexpr std::size_t kWantWordCount = 8;
  constexpr std::size_t kVisNickOffset = 0x24;
  constexpr std::size_t kCommandListOffset = 0x28;
  constexpr std::size_t kObjectNameOffset = 0x2c;
  constexpr std::size_t kObjectStatusOffset = 0x42c;
  constexpr std::size_t kObjectCount = 256;
  constexpr std::uint32_t kFalse = 0x0014fd24;
  constexpr std::uint32_t kEmpty = kFalse - 0x0a;
  constexpr auto seeded_byte = [](std::size_t index) {
    return static_cast<std::byte>(1 + (index * 37) % 255);
  };

  static_assert(offsetof(GuardedLoadStateArena, storage) == kGuardSize);
  static_assert(kThis == kWantOffset);
  static_assert(kWantOffset + kWantWordCount * sizeof(std::uint32_t) == kVisNickOffset);
  static_assert(kSecondArenaBaseOffset >= kLoadStateSize);
  static_assert(kSecondArenaBaseOffset + kLoadStateSize == kStorageSize);

  ASSERT_EQ(kArm64GoalExports1[3].goal_name, "reset!");
  ASSERT_NE(kArm64GoalExports1[3].entry, nullptr);

  GuardedLoadStateArena actual;
  std::size_t seed_index = 0;
  const auto seed_range = [&](auto& range) {
    for (auto& byte : range) {
      byte = seeded_byte(seed_index++);
    }
  };
  seed_range(actual.leading_guard);
  seed_range(actual.storage);
  seed_range(actual.trailing_guard);
  auto expected = actual;

  const auto reset_expected = [&](std::size_t base_offset) {
    for (std::size_t index = 0; index < kWantWordCount; ++index) {
      write_u32(expected.storage.data(),
                base_offset + kWantOffset + index * sizeof(std::uint32_t), kFalse);
    }
    write_u32(expected.storage.data(), base_offset + kCommandListOffset, kEmpty);
    for (std::size_t index = 0; index < kObjectCount; ++index) {
      write_u32(expected.storage.data(),
                base_offset + kObjectNameOffset + index * sizeof(std::uint32_t), kFalse);
      write_u32(expected.storage.data(),
                base_offset + kObjectStatusOffset + index * sizeof(std::uint32_t), 0);
    }
  };
  reset_expected(0);
  reset_expected(kSecondArenaBaseOffset);

  const auto first_arena_base = reinterpret_cast<std::uintptr_t>(actual.storage.data());
  const auto invoke = [&](std::size_t base_offset) {
    auto probe = make_probe(entry_address(kArm64GoalExports1[3].entry), kFalse,
                            first_arena_base + base_offset);
    probe.argument0 = kThis;
    arm64_goal_call_abi_outer(&probe);
    expect_caller_registers_restored(probe);
    return probe;
  };

  EXPECT_EQ(invoke(0).result, kThis);
  EXPECT_EQ(invoke(kSecondArenaBaseOffset).result, kThis);

  const auto expect_reset_arena = [&](std::size_t base_offset) {
    EXPECT_EQ(read_u32(actual.storage.data(), base_offset),
              read_u32(expected.storage.data(), base_offset));
    for (std::size_t index = 0; index < kWantWordCount; ++index) {
      EXPECT_EQ(read_u32(actual.storage.data(),
                         base_offset + kWantOffset + index * sizeof(std::uint32_t)),
                kFalse);
    }
    EXPECT_EQ(read_u32(actual.storage.data(), base_offset + kVisNickOffset),
              read_u32(expected.storage.data(), base_offset + kVisNickOffset));
    EXPECT_EQ(read_u32(actual.storage.data(), base_offset + kCommandListOffset), kEmpty);
    for (std::size_t index = 0; index < kObjectCount; ++index) {
      EXPECT_EQ(read_u32(actual.storage.data(),
                         base_offset + kObjectNameOffset + index * sizeof(std::uint32_t)),
                kFalse);
      EXPECT_EQ(read_u32(actual.storage.data(),
                         base_offset + kObjectStatusOffset + index * sizeof(std::uint32_t)),
                0u);
    }
  };
  expect_reset_arena(0);
  expect_reset_arena(kSecondArenaBaseOffset);

  EXPECT_TRUE(std::equal(actual.storage.begin() + static_cast<std::ptrdiff_t>(kLoadStateSize),
                         actual.storage.begin() + static_cast<std::ptrdiff_t>(kSecondArenaBaseOffset),
                         expected.storage.begin() + static_cast<std::ptrdiff_t>(kLoadStateSize)));
  EXPECT_EQ(actual.leading_guard, expected.leading_guard);
  EXPECT_EQ(actual.storage, expected.storage);
  EXPECT_EQ(actual.trailing_guard, expected.trailing_guard);
}

}  // namespace
