#include "common/math/ps2_vu_float.h"

#include "game/mips2c/mips2c_private.h"

#include "gtest/gtest.h"

TEST(PS2VUFloat, DivQNormalizesInputsAndResult) {
  struct Case {
    uint32_t numerator;
    uint32_t denominator;
    uint32_t expected;
  };

  const Case cases[] = {
      {0x3f800000, 0x00000000, 0x7f7fffff},
      {0xbf800000, 0x00000000, 0xff7fffff},
      {0x3f800000, 0x80000000, 0xff7fffff},
      {0xbf800000, 0x80000000, 0x7f7fffff},
      {0x00000000, 0x00000000, 0x7f7fffff},
      {0x80000000, 0x00000000, 0xff7fffff},
      {0x00000001, 0x3f800000, 0x00000000},
      {0x80000001, 0x3f800000, 0x80000000},
      {0x3f800000, 0x00000001, 0x7f7fffff},
      {0x7f800000, 0x3f800000, 0x7f7fffff},
      {0xff800001, 0x3f800000, 0xff7fffff},
      {0x7f7fffff, 0x00800000, 0x7f7fffff},
      {0x00800000, 0x7f7fffff, 0x00000000},
      {0x40c00000, 0x40800000, 0x3fc00000},
  };

  for (const auto& test : cases) {
    const float result = ps2_vu_div_q(ps2_vu_float_from_bits(test.numerator),
                                      ps2_vu_float_from_bits(test.denominator));
    EXPECT_EQ(ps2_vu_float_to_bits(result), test.expected)
        << std::hex << test.numerator << " / " << test.denominator;
  }
}

TEST(PS2VUFloat, Mips2CExecutionContextUsesDivQSemantics) {
  Mips2C::ExecutionContext context{};
  context.vfs[Mips2C::vf1].f[(int)Mips2C::BC::x] = ps2_vu_float_from_bits(0x00000000);
  context.vfs[Mips2C::vf2].f[(int)Mips2C::BC::y] = ps2_vu_float_from_bits(0x80000000);

  context.vdiv(Mips2C::vf1, Mips2C::BC::x, Mips2C::vf2, Mips2C::BC::y);

  EXPECT_EQ(ps2_vu_float_to_bits(context.Q), 0xff7fffff);
}
