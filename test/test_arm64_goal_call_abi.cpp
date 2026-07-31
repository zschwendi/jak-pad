#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "game/kernel/jak1/data_arena.h"
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
using Arm64GoalEntry =
    std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t);
using Arm64GoalExport0Entry = std::uint64_t (*)();
using Arm64GoalExport1Entry = std::uint64_t (*)(std::uint64_t);

static_assert(sizeof(Arm64GoalEntry) == sizeof(std::uintptr_t));
static_assert(sizeof(Arm64GoalExport0Entry) == sizeof(std::uintptr_t));
static_assert(sizeof(Arm64GoalExport1Entry) == sizeof(std::uintptr_t));

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
#undef OPENGOAL_AOT_EXPORT0

#define OPENGOAL_AOT_EXPORT1(goal_name, c_symbol) \
  extern "C" std::uint64_t c_symbol(std::uint64_t);
// clang-format off
#include "OpenGOALJak1Identity.exports.h"
#include "OpenGOALJak1Lognot.exports.h"
#include "OpenGOALJak1GlstNodeName.exports.h"
// clang-format on
#undef OPENGOAL_AOT_EXPORT1

struct Arm64GoalExport0 {
  std::string_view goal_name;
  Arm64GoalExport0Entry entry;
};

struct Arm64GoalExport1 {
  std::string_view goal_name;
  Arm64GoalExport1Entry entry;
};

constexpr Arm64GoalExport0 kArm64GoalExports0[] = {
#define OPENGOAL_AOT_EXPORT0(goal_name, c_symbol) {goal_name, &c_symbol},
#include "OpenGOALJak1FalseFunc.exports.h"
#include "OpenGOALJak1TrueFunc.exports.h"
#undef OPENGOAL_AOT_EXPORT0
};

constexpr Arm64GoalExport1 kArm64GoalExports1[] = {
#define OPENGOAL_AOT_EXPORT1(goal_name, c_symbol) {goal_name, &c_symbol},
// clang-format off
#include "OpenGOALJak1Identity.exports.h"
#include "OpenGOALJak1Lognot.exports.h"
#include "OpenGOALJak1GlstNodeName.exports.h"
// clang-format on
#undef OPENGOAL_AOT_EXPORT1
};

static_assert(sizeof(kArm64GoalExports0) == 2 * sizeof(Arm64GoalExport0));
static_assert(sizeof(kArm64GoalExports1) == 3 * sizeof(Arm64GoalExport1));

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

TEST(Arm64GoalCallAbi, executes_generated_jak1_aot_artifacts_against_one_data_arena) {
  std::vector<std::byte> arena(jak1::minimum_data_arena_header_size());
  const auto initialized = jak1::initialize_data_arena_header(arena.data(), arena.size());
  ASSERT_TRUE(initialized.ok());

  ASSERT_EQ(kArm64GoalExports0[0].goal_name, "false-func");
  ASSERT_EQ(kArm64GoalExports0[1].goal_name, "true-func");
  ASSERT_EQ(kArm64GoalExports1[0].goal_name, "identity");
  ASSERT_EQ(kArm64GoalExports1[1].goal_name, "lognot");
  ASSERT_EQ(kArm64GoalExports1[2].goal_name, "glst-node-name");
  ASSERT_NE(kArm64GoalExports0[0].entry, nullptr);
  ASSERT_NE(kArm64GoalExports0[1].entry, nullptr);
  ASSERT_NE(kArm64GoalExports1[0].entry, nullptr);
  ASSERT_NE(kArm64GoalExports1[1].entry, nullptr);
  ASSERT_NE(kArm64GoalExports1[2].entry, nullptr);

  const auto st = static_cast<std::uintptr_t>(initialized.header.s7);
  const auto arena_address = reinterpret_cast<std::uintptr_t>(arena.data());
  auto invoke = [&](auto entry, std::uintptr_t logical_base,
                    std::uint64_t argument0 = 0x1122334455667788) {
    auto probe = make_probe(entry_address(entry), st, logical_base);
    probe.argument0 = argument0;
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
  constexpr std::uint32_t kFirstPrivname = 0x81234567;
  constexpr std::uint32_t kSecondPrivname = 0xf2345678;
  const auto node_offset = static_cast<std::size_t>(initialized.header.global_heap_current);
  ASSERT_NE(node_offset, 0);
  ASSERT_LE(node_offset, arena.size());
  ASSERT_LE(kPrivnameOffset, arena.size() - node_offset);
  const auto first_privname_offset = node_offset + kPrivnameOffset;
  ASSERT_LE(sizeof(kFirstPrivname), arena.size() - first_privname_offset);

  ASSERT_LE(kSecondLogicalBaseOffset, arena.size());
  ASSERT_LE(node_offset, arena.size() - kSecondLogicalBaseOffset);
  const auto second_node_offset = kSecondLogicalBaseOffset + node_offset;
  ASSERT_LE(kPrivnameOffset, arena.size() - second_node_offset);
  const auto second_privname_offset = second_node_offset + kPrivnameOffset;
  ASSERT_LE(sizeof(kSecondPrivname), arena.size() - second_privname_offset);

  std::memcpy(arena.data() + first_privname_offset, &kFirstPrivname, sizeof(kFirstPrivname));
  std::memcpy(arena.data() + second_privname_offset, &kSecondPrivname, sizeof(kSecondPrivname));

  const auto first_glst_node_name_probe =
      invoke(kArm64GoalExports1[2].entry, arena_address, node_offset);
  const auto second_arena_address =
      reinterpret_cast<std::uintptr_t>(arena.data() + kSecondLogicalBaseOffset);
  const auto second_glst_node_name_probe =
      invoke(kArm64GoalExports1[2].entry, second_arena_address, node_offset);

  EXPECT_EQ(first_glst_node_name_probe.result, UINT64_C(0x0000000081234567));
  EXPECT_EQ(second_glst_node_name_probe.result, UINT64_C(0x00000000f2345678));
  EXPECT_EQ(first_glst_node_name_probe.result >> 32, 0);
  EXPECT_EQ(second_glst_node_name_probe.result >> 32, 0);
}

}  // namespace
