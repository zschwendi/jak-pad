#pragma once

#include <stdint.h>
#include <string.h>

#define PS2_VU_FLOAT_SIGN_MASK UINT32_C(0x80000000)
#define PS2_VU_FLOAT_EXP_MASK UINT32_C(0x7f800000)
#define PS2_VU_FLOAT_MAX_BITS UINT32_C(0x7f7fffff)

static inline uint32_t ps2_vu_float_to_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static inline float ps2_vu_float_from_bits(uint32_t bits) {
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static inline uint32_t ps2_vu_normalize_float_bits(uint32_t bits) {
  const uint32_t exponent = bits & PS2_VU_FLOAT_EXP_MASK;
  if (exponent == 0) {
    return bits & PS2_VU_FLOAT_SIGN_MASK;
  }
  if (exponent == PS2_VU_FLOAT_EXP_MASK) {
    return (bits & PS2_VU_FLOAT_SIGN_MASK) | PS2_VU_FLOAT_MAX_BITS;
  }
  return bits;
}

static inline float ps2_vu_div_q(float numerator, float denominator) {
  const uint32_t numerator_bits = ps2_vu_normalize_float_bits(ps2_vu_float_to_bits(numerator));
  const uint32_t denominator_bits =
      ps2_vu_normalize_float_bits(ps2_vu_float_to_bits(denominator));

  if ((denominator_bits & ~PS2_VU_FLOAT_SIGN_MASK) == 0) {
    return ps2_vu_float_from_bits(((numerator_bits ^ denominator_bits) & PS2_VU_FLOAT_SIGN_MASK) |
                                  PS2_VU_FLOAT_MAX_BITS);
  }

  const float result =
      ps2_vu_float_from_bits(numerator_bits) / ps2_vu_float_from_bits(denominator_bits);
  return ps2_vu_float_from_bits(ps2_vu_normalize_float_bits(ps2_vu_float_to_bits(result)));
}
