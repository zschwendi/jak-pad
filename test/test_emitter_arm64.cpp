#include "emitter_util.h"

#include <array>

#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "gtest/gtest.h"

using namespace emitter;

TEST(ARM64EmitterLoads, mov_gpr64_u64_encodes_immediate_and_shift) {
  std::array<u8, 4> code{};

  auto literal = IGen::ARM64::mov_gpr64_u64(X0, 42);
  EXPECT_EQ(literal.emit(code.data()), code.size());
  EXPECT_EQ(code, (std::array<u8, 4>{0x40, 0x05, 0x80, 0xd2}));

  auto shifted = IGen::ARM64::mov_gpr64_u64(X0, 0x10000);
  EXPECT_EQ(shifted.emit(code.data()), code.size());
  EXPECT_EQ(code, (std::array<u8, 4>{0x20, 0x00, 0xa0, 0xd2}));
}

TEST(ARM64EmitterIntegerMath, add_gpr64_imm8s) {
  CodeTester tester(InstructionSet::ARM64);
  tester.init_code_buffer(256);

  std::vector<s64> vals = {0, 1, -1, INT32_MIN, INT32_MAX, INT64_MIN, INT64_MAX};
  std::vector<s64> imms = {0, 1, -1, INT8_MIN, INT8_MAX};

  // test the ones that aren't sp
  for (int i = 0; i < 16; i++) {
    if (i == SP) {
      continue;
    }

    for (auto val : vals) {
      for (auto imm : imms) {
        auto expected = val + imm;

        tester.clear();
        tester.emit_push_all_gprs(true);

        // move initial value to register
        tester.emit(IGen::mov_gpr64_gpr64(tester.generator(), i, X0));
        // do the add
        tester.emit(IGen::add_gpr64_imm8s(tester.generator(), i, imm));
        // move for return
        tester.emit(IGen::mov_gpr64_gpr64(tester.generator(), X0, i));

        tester.emit_pop_all_gprs(true);
        tester.emit_return();

        execute_ret_tester(tester, val, expected);
      }
    }
  }
  tester.clear();
}

TEST(ARM64EmitterExactEncodings, cmp_and_register_add) {
  std::array<u8, 4> code{};

  const auto compare = IGen::ARM64::cmp_gpr64_gpr64(X0, X1);
  EXPECT_EQ(compare.emit(code.data()), code.size());
  EXPECT_EQ(code, (std::array<u8, 4>{0x1f, 0x00, 0x01, 0xeb}));

  const auto add = IGen::ARM64::add_gpr64_gpr64(X3, X4);
  EXPECT_EQ(add.emit(code.data()), code.size());
  EXPECT_EQ(code, (std::array<u8, 4>{0x63, 0x00, 0x04, 0x8b}));
}

TEST(ARM64EmitterExactEncodings, lsl_and_condition_branches) {
  std::array<u8, 4> code{};

  const auto shift = IGen::ARM64::shl_gpr64_u8(X3, 4);
  EXPECT_EQ(shift.emit(code.data()), code.size());
  EXPECT_EQ(code, (std::array<u8, 4>{0x63, 0xec, 0x7c, 0xd3}));

  const auto geq = IGen::ARM64::jge_imm();
  EXPECT_EQ(geq.emit(code.data()), code.size());
  EXPECT_EQ(code, (std::array<u8, 4>{0x0a, 0x00, 0x00, 0x54}));

  const auto less_than = IGen::ARM64::jl_imm();
  EXPECT_EQ(less_than.emit(code.data()), code.size());
  EXPECT_EQ(code, (std::array<u8, 4>{0x0b, 0x00, 0x00, 0x54}));

  const auto signed_negative_conditional_offset =
      InstructionARM64(ARM64::Base(0b01010100, 8), ARM64::Imm19(0x7ffff), ARM64::Cond(0xa));
  EXPECT_EQ(signed_negative_conditional_offset.emit(code.data()), code.size());
  EXPECT_EQ(code, (std::array<u8, 4>{0xea, 0xff, 0xff, 0x54}));
}

TEST(ARM64EmitterExactEncodings, dynamic_word_load) {
  std::array<u8, 4> code{};

  const auto load = IGen::ARM64::load32u_gpr64_gpr64_plus_gpr64(X0, X1, X2);
  EXPECT_EQ(load.emit(code.data()), code.size());
  EXPECT_EQ(code, (std::array<u8, 4>{0x20, 0x68, 0x62, 0xb8}));
}
