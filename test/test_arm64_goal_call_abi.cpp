#include <array>
#include <cstddef>
#include <cstdint>
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
#include "OpenGOALJak1Identity.exports.h"
#include "OpenGOALJak1Lognot.exports.h"
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
#include "OpenGOALJak1Identity.exports.h"
#include "OpenGOALJak1Lognot.exports.h"
#undef OPENGOAL_AOT_EXPORT1
};

static_assert(sizeof(kArm64GoalExports0) == 2 * sizeof(Arm64GoalExport0));
static_assert(sizeof(kArm64GoalExports1) == 2 * sizeof(Arm64GoalExport1));

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
  ASSERT_NE(kArm64GoalExports0[0].entry, nullptr);
  ASSERT_NE(kArm64GoalExports0[1].entry, nullptr);
  ASSERT_NE(kArm64GoalExports1[0].entry, nullptr);
  ASSERT_NE(kArm64GoalExports1[1].entry, nullptr);

  const auto st = static_cast<std::uintptr_t>(initialized.header.s7);
  const auto arena_address = reinterpret_cast<std::uintptr_t>(arena.data());
  auto invoke = [&](auto entry) {
    auto probe = make_probe(entry_address(entry), st, arena_address);
    arm64_goal_call_abi_outer(&probe);
    expect_caller_registers_restored(probe);
    return probe;
  };

  const auto false_probe = invoke(kArm64GoalExports0[0].entry);
  const auto true_probe = invoke(kArm64GoalExports0[1].entry);
  const auto identity_probe = invoke(kArm64GoalExports1[0].entry);
  const auto lognot_probe = invoke(kArm64GoalExports1[1].entry);

  EXPECT_EQ(false_probe.result, initialized.header.false_value);
  EXPECT_EQ(true_probe.result, initialized.header.true_value);
  EXPECT_NE(identity_probe.argument0, 0);
  EXPECT_EQ(identity_probe.result, identity_probe.argument0);
  EXPECT_EQ(lognot_probe.result, ~lognot_probe.argument0);
}

}  // namespace
