/*!
 * @file gcommon_c_backend_test.c
 * Calls real Jak 1 gcommon.gc functions that goalc's AOT C backend translated to C, and checks
 * the results against what the GOAL source says they should be.
 */

#include <string.h>

#include "gcommon_generated.h"

#include "test/goalc/aot/goal_c_test_loader.h"

static int g_failures = 0;

static void check_u64(const char* what, uint64_t got, uint64_t expected) {
  if (got != expected) {
    printf("FAIL %s: got %llu (0x%llx), expected %llu (0x%llx)\n", what, (unsigned long long)got,
           (unsigned long long)got, (unsigned long long)expected, (unsigned long long)expected);
    g_failures++;
  }
}

static void check_s64(const char* what, uint64_t got, int64_t expected) {
  if ((int64_t)got != expected) {
    printf("FAIL %s: got %lld, expected %lld\n", what, (long long)(int64_t)got,
           (long long)expected);
    g_failures++;
  }
}

static uint64_t float_bits(float f) {
  uint32_t bits;
  memcpy(&bits, &f, 4);
  return (uint64_t)(int64_t)(int32_t)bits;
}

static float bits_float(uint64_t v) {
  uint32_t bits = (uint32_t)v;
  float f;
  memcpy(&f, &bits, 4);
  return f;
}

/*! Build a proper list of three integers. GOAL pairs keep car at ptr-2 and cdr at ptr+2. */
static uint64_t build_list(uint64_t base, const int32_t* values, int count) {
  for (int i = 0; i < count; i++) {
    const uint64_t pair = base + 16 * (uint64_t)i + 2;
    const int32_t cdr = (i + 1 < count) ? (int32_t)(base + 16 * (uint64_t)(i + 1) + 2)
                                        : (int32_t)goal_test_empty_pair();
    memcpy(g_goal_mem + pair - 2, &values[i], 4);
    memcpy(g_goal_mem + pair + 2, &cdr, 4);
  }
  return base + 2;
}

int main(void) {
  goal_test_loader_init();
  goal_test_load_functions(goal_gcommon_functions, goal_gcommon_function_count);
  goal_test_load_statics(goal_gcommon_statics, goal_gcommon_static_count);
  goal_gcommon_link();

  /* identity */
  check_u64("identity", goal_gcommon_identity(0x1234abcdull), 0x1234abcdull);

  /* integer arithmetic */
  check_s64("+", goal_gcommon__plus(3, 4), 7);
  check_s64("-", goal_gcommon__((uint64_t)3, (uint64_t)4), -1);
  /* (* x y) is a 32-bit multiply whose result the PS2 sign-extends */
  check_s64("*", goal_gcommon__star(100000, 100000), 1410065408);
  check_s64("/", goal_gcommon__slash((uint64_t)-7, 2), -3);
  check_s64("mod", goal_gcommon_mod((uint64_t)-7, 3), -1);
  check_s64("rem", goal_gcommon_rem(7, 3), 1);
  check_s64("abs +", goal_gcommon_abs(5), 5);
  check_s64("abs -", goal_gcommon_abs((uint64_t)-5), 5);
  check_s64("min", goal_gcommon_min((uint64_t)-3, 4), -3);
  check_s64("max", goal_gcommon_max((uint64_t)-3, 4), 4);
  check_s64("ash left", goal_gcommon_ash(1, 4), 16);
  check_s64("ash right", goal_gcommon_ash((uint64_t)-16, (uint64_t)-2), -4);
  check_u64("logior", goal_gcommon_logior(0xa, 0x5), 0xf);
  check_u64("logand", goal_gcommon_logand(0xc, 0xa), 0x8);
  check_u64("logxor", goal_gcommon_logxor(0xc, 0xa), 0x6);
  check_u64("lognot", goal_gcommon_lognot(0), 0xffffffffffffffffull);
  check_u64("lognor", goal_gcommon_lognor(0xa, 0x5), ~(uint64_t)0xf);

  /* symbol pointers */
  check_u64("false-func", goal_gcommon_false_func(), g_goal_s7);
  check_u64("true-func", goal_gcommon_true_func(), g_goal_s7 + 8);

  /* float math through a static float constant */
  if (bits_float(goal_gcommon_1_slash(float_bits(2.f))) != 0.5f) {
    printf("FAIL 1/: got %f, expected 0.5\n",
           (double)bits_float(goal_gcommon_1_slash(float_bits(2.f))));
    g_failures++;
  }

  /* recursion through the symbol table and an indirect GOAL function call */
  int fact_index = -1;
  for (int i = 0; i < goal_gcommon_function_count; i++) {
    if (goal_gcommon_functions[i] == (const void*)&goal_gcommon_fact) {
      fact_index = i;
    }
  }
  if (fact_index < 0) {
    printf("FAIL fact: not in the emitted function table\n");
    g_failures++;
  } else {
    *goal_symbol_slot("fact") = (int32_t)goal_function_addr("gcommon", fact_index);
    check_s64("fact", goal_gcommon_fact(5), 120);
    check_s64("fact 1", goal_gcommon_fact(1), 1);
  }

  /* list walking: loads through GOAL pointers, the empty-pair symbol, and tag arithmetic */
  const int32_t values[3] = {100, 200, 300};
  const uint64_t list = build_list(0x400000, values, 3);
  check_s64("ref 0", goal_gcommon_ref(list, 0), 100);
  check_s64("ref 2", goal_gcommon_ref(list, 2), 300);
  check_u64("last", goal_gcommon_last(list), 0x400000 + 32 + 2);
  check_s64("length pair", goal_gcommon__method_length_pair_(list), 3);
  check_s64("length empty", goal_gcommon__method_length_pair_(goal_test_empty_pair()), 0);

  /* byte-level memory access through GOAL pointers */
  const uint64_t src = 0x500000;
  const uint64_t dst = 0x500100;
  for (int i = 0; i < 32; i++) {
    g_goal_mem[src + i] = (uint8_t)(i * 7 + 1);
  }
  check_u64("mem-copy! result", goal_gcommon_mem_copy_bang(dst, src, 32), dst);
  if (memcmp(g_goal_mem + dst, g_goal_mem + src, 32) != 0) {
    printf("FAIL mem-copy!: bytes differ\n");
    g_failures++;
  }

  /* mem-set32! writes 32-bit words */
  goal_gcommon_mem_set32_bang(dst, 4, 0x11223344);
  for (int i = 0; i < 4; i++) {
    uint32_t word;
    memcpy(&word, g_goal_mem + dst + 4 * i, 4);
    check_u64("mem-set32!", word, 0x11223344u);
  }

  if (g_failures) {
    printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("all AOT C backend checks passed\n");
  return 0;
}
