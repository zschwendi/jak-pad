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
