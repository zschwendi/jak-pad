/*!
 * @file vector_ops_c_backend_test.c
 * Runs the C that goalc's AOT C backend emitted for test/goalc/aot/vector_ops_test.gc and checks
 * that the PS2 vector and 128-bit integer operations produce the right bytes.
 *
 * These operations are the ones where "it compiled" proves nothing. A swapped lane order, a
 * logical shift where the PS2 does an arithmetic one, or an unsigned compare where the PS2 does a
 * signed one all compile and run and only show up much later as corrupted geometry or physics. So
 * every input below is chosen so that each lane, each byte and each sign is distinguishable, and
 * every expected result is written out in full.
 *
 * The expected values are the PS2 semantics recorded in game/mips2c/mips2c_private.h, which is
 * where OpenGOAL's hand-translated PS2 assembly gets them.
 */

#include <math.h>
#include <string.h>

#include "vec_generated.h"

#include "test/goalc/aot/goal_c_test_loader.h"

/* three 16-byte scratch quadwords in GOAL memory */
#define DST 0x600000ull
#define SRC_A 0x600010ull
#define SRC_B 0x600020ull
#define MERC_JOINT 0x600100ull
#define MERC_BONE 0x600140ull
#define MERC_CAMERA 0x600180ull

static int g_failures = 0;

static void* host(uint64_t goal_addr) {
  return g_goal_mem + goal_addr;
}

static void put_words(uint64_t at, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
  const uint32_t v[4] = {w0, w1, w2, w3};
  memcpy(host(at), v, 16);
}

static void put_halfwords(uint64_t at, const uint16_t h[8]) {
  memcpy(host(at), h, 16);
}

static void put_bytes(uint64_t at, const uint8_t b[16]) {
  memcpy(host(at), b, 16);
}

static void put_floats(uint64_t at, float f0, float f1, float f2, float f3) {
  const float v[4] = {f0, f1, f2, f3};
  memcpy(host(at), v, 16);
}

static void report(const char* what) {
  const uint8_t* got = (const uint8_t*)host(DST);
  printf("FAIL %s: got", what);
  for (int i = 0; i < 16; i++) {
    printf(" %02x", got[i]);
  }
  printf("\n");
  g_failures++;
}

static void expect_words(const char* what, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
  const uint32_t want[4] = {w0, w1, w2, w3};
  if (memcmp(host(DST), want, 16) != 0) {
    report(what);
    printf("     wanted %08x %08x %08x %08x\n", w0, w1, w2, w3);
  }
}

static void expect_words_at(
    const char* what, uint64_t at, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
  const uint32_t want[4] = {w0, w1, w2, w3};
  if (memcmp(host(at), want, 16) != 0) {
    const uint32_t* got = (const uint32_t*)host(at);
    printf("FAIL %s: got %08x %08x %08x %08x\n", what, got[0], got[1], got[2], got[3]);
    printf("     wanted %08x %08x %08x %08x\n", w0, w1, w2, w3);
    g_failures++;
  }
}

static void expect_halfwords(const char* what, const uint16_t want[8]) {
  if (memcmp(host(DST), want, 16) != 0) {
    report(what);
    printf("     wanted");
    for (int i = 0; i < 8; i++) {
      printf(" %04x", want[i]);
    }
    printf("\n");
  }
}

static void expect_bytes(const char* what, const uint8_t want[16]) {
  if (memcmp(host(DST), want, 16) != 0) {
    report(what);
    printf("     wanted");
    for (int i = 0; i < 16; i++) {
      printf(" %02x", want[i]);
    }
    printf("\n");
  }
}

static void expect_floats_at(const char* what,
                             uint64_t at,
                             float f0,
                             float f1,
                             float f2,
                             float f3) {
  const float want[4] = {f0, f1, f2, f3};
  float got[4];
  memcpy(got, host(at), 16);
  for (int i = 0; i < 4; i++) {
    if (got[i] != want[i]) {
      printf("FAIL %s: got %f %f %f %f, wanted %f %f %f %f\n", what, (double)got[0], (double)got[1],
             (double)got[2], (double)got[3], (double)want[0], (double)want[1], (double)want[2],
             (double)want[3]);
      g_failures++;
      return;
    }
  }
}

static void expect_floats(const char* what, float f0, float f1, float f2, float f3) {
  expect_floats_at(what, DST, f0, f1, f2, f3);
}

static void test_quadword_copies(void) {
  put_words(SRC_A, 0x0a0a0a0a, 0x0b0b0b0b, 0x0c0c0c0c, 0x0d0d0d0d);
  put_words(SRC_B, 0x11111111, 0x22222222, 0x33333333, 0x44444444);

  /* pcpyld rd, rs, rt: rd = { rt low 64, rs low 64 } */
  goal_vec_aot_test_pcpyld(DST, SRC_A, SRC_B);
  expect_words("pcpyld", 0x11111111, 0x22222222, 0x0a0a0a0a, 0x0b0b0b0b);

  /* pcpyud rd, rs, rt: rd = { rs high 64, rt high 64 } */
  goal_vec_aot_test_pcpyud(DST, SRC_A, SRC_B);
  expect_words("pcpyud", 0x0c0c0c0c, 0x0d0d0d0d, 0x33333333, 0x44444444);
}

static void test_byte_interleave(void) {
  uint8_t a[16], b[16], want[16];
  for (int i = 0; i < 16; i++) {
    a[i] = (uint8_t)i;
    b[i] = (uint8_t)(0x80 + i);
  }
  put_bytes(SRC_A, a);
  put_bytes(SRC_B, b);

  /* pextlb rd, rs, rt: the low 8 bytes of each, rt supplying the even destination bytes */
  for (int i = 0; i < 8; i++) {
    want[2 * i] = b[i];
    want[2 * i + 1] = a[i];
  }
  goal_vec_aot_test_pextlb(DST, SRC_A, SRC_B);
  expect_bytes("pextlb", want);

  /* pextub rd, rs, rt: the same, from the high 8 bytes */
  for (int i = 0; i < 8; i++) {
    want[2 * i] = b[8 + i];
    want[2 * i + 1] = a[8 + i];
  }
  goal_vec_aot_test_pextub(DST, SRC_A, SRC_B);
  expect_bytes("pextub", want);
}

static void test_halfword_interleave(void) {
  const uint16_t a[8] = {0xa000, 0xa001, 0xa002, 0xa003, 0xa004, 0xa005, 0xa006, 0xa007};
  const uint16_t b[8] = {0xb000, 0xb001, 0xb002, 0xb003, 0xb004, 0xb005, 0xb006, 0xb007};
  uint16_t want[8];
  put_halfwords(SRC_A, a);
  put_halfwords(SRC_B, b);

  for (int i = 0; i < 4; i++) {
    want[2 * i] = b[i];
    want[2 * i + 1] = a[i];
  }
  goal_vec_aot_test_pextlh(DST, SRC_A, SRC_B);
  expect_halfwords("pextlh", want);

  for (int i = 0; i < 4; i++) {
    want[2 * i] = b[4 + i];
    want[2 * i + 1] = a[4 + i];
  }
  goal_vec_aot_test_pextuh(DST, SRC_A, SRC_B);
  expect_halfwords("pextuh", want);
}

static void test_word_interleave(void) {
  put_words(SRC_A, 0xa0000000, 0xa0000001, 0xa0000002, 0xa0000003);
  put_words(SRC_B, 0xb0000000, 0xb0000001, 0xb0000002, 0xb0000003);

  goal_vec_aot_test_pextlw(DST, SRC_A, SRC_B);
  expect_words("pextlw", 0xb0000000, 0xa0000000, 0xb0000001, 0xa0000001);

  goal_vec_aot_test_pextuw(DST, SRC_A, SRC_B);
  expect_words("pextuw", 0xb0000002, 0xa0000002, 0xb0000003, 0xa0000003);
}

static void test_compares(void) {
  /*
   * pcgtw is a *signed* compare. Lanes 0 and 2 both flip if it is done unsigned:
   *   lane 0: -1 > 0 is false signed, but 0xffffffff > 0 is true unsigned
   *   lane 2: 0x7fffffff > -1 is true signed, but false unsigned
   */
  put_words(SRC_A, 0xffffffff, 5, 0x7fffffff, 0x80000000);
  put_words(SRC_B, 0, 5, 0xffffffff, 1);
  goal_vec_aot_test_pcgtw(DST, SRC_A, SRC_B);
  expect_words("pcgtw", 0, 0, 0xffffffff, 0);

  /* and it is not commutative: swapping the operands must change the answer */
  goal_vec_aot_test_pcgtw(DST, SRC_B, SRC_A);
  expect_words("pcgtw with operands swapped", 0xffffffff, 0, 0, 0xffffffff);

  put_words(SRC_A, 1, 0xffffffff, 0, 0x80000000);
  put_words(SRC_B, 1, 0xffffffff, 5, 0x80000000);
  goal_vec_aot_test_pceqw(DST, SRC_A, SRC_B);
  expect_words("pceqw", 0xffffffff, 0xffffffff, 0, 0xffffffff);
}

static void test_subtract(void) {
  /* per-word, wrapping, and not a single 128-bit subtract: lane 2 borrows if the lanes are joined */
  put_words(SRC_A, 10, 0xfffffff6 /* -10 */, 0x7fffffff, 0);
  put_words(SRC_B, 3, 3, 0xffffffff /* -1 */, 1);
  goal_vec_aot_test_psubw(DST, SRC_A, SRC_B);
  expect_words("psubw", 7, 0xfffffff3 /* -13 */, 0x80000000, 0xffffffff);
}

static void test_shifts(void) {
  put_words(SRC_A, 0x00001234, 0xffff8765, 0x00000001, 0x00010000);
  goal_vec_aot_test_pw_sll(DST, SRC_A);
  expect_words("pw.sll 16", 0x12340000, 0x87650000, 0x00010000, 0x00000000);

  /* arithmetic: lane 1 is negative, so it must come back sign-extended, not zero-filled */
  put_words(SRC_A, 0x12340000, 0x87650000, 0x00010000, 0x00000000);
  goal_vec_aot_test_pw_sra(DST, SRC_A);
  expect_words("pw.sra 16", 0x00001234, 0xffff8765, 0x00000001, 0x00000000);

  /* logical: the same input, and lane 1 must now be zero-filled */
  goal_vec_aot_test_pw_srl(DST, SRC_A);
  expect_words("pw.srl 16", 0x00001234, 0x00008765, 0x00000001, 0x00000000);
}

static void test_ppach(void) {
  /*
   * ppach rd, rs, rt takes every other halfword: rd = { rt 0,2,4,6, rs 0,2,4,6 }. goalc builds it
   * out of the halfword shuffles, the 128-bit byte shift and pcpyld, so this covers all of those.
   */
  const uint16_t a[8] = {0xa000, 0xa001, 0xa002, 0xa003, 0xa004, 0xa005, 0xa006, 0xa007};
  const uint16_t b[8] = {0xb000, 0xb001, 0xb002, 0xb003, 0xb004, 0xb005, 0xb006, 0xb007};
  const uint16_t want[8] = {b[0], b[2], b[4], b[6], a[0], a[2], a[4], a[6]};
  put_halfwords(SRC_A, a);
  put_halfwords(SRC_B, b);
  goal_vec_aot_test_ppach(DST, SRC_A, SRC_B);
  expect_halfwords("ppach", want);
}

static void test_blend(void) {
  /* lane i of the result comes from the second source exactly when bit i of the mask is set */
  put_floats(SRC_A, 1.f, 2.f, 3.f, 4.f);
  put_floats(SRC_B, 10.f, 20.f, 30.f, 40.f);

  goal_vec_aot_test_blend(DST, SRC_A, SRC_B, 1);
  expect_floats("blend.vf.x", 10.f, 2.f, 3.f, 4.f);
  goal_vec_aot_test_blend(DST, SRC_A, SRC_B, 2);
  expect_floats("blend.vf.y", 1.f, 20.f, 3.f, 4.f);
  goal_vec_aot_test_blend(DST, SRC_A, SRC_B, 4);
  expect_floats("blend.vf.z", 1.f, 2.f, 30.f, 4.f);
  goal_vec_aot_test_blend(DST, SRC_A, SRC_B, 8);
  expect_floats("blend.vf.w", 1.f, 2.f, 3.f, 40.f);
}

static void test_outer_product(void) {
  /*
   * The cross product goalc assembles from .swizzle.vf and .blend.vf. This is the worked example
   * in Compiler::compile_asm_outer_product_vf: <1,2,3,4> x <5,6,7,8> is <-4, 8, -4>, with w left
   * alone. Any wrong swizzle control ordering gives a different vector here.
   */
  put_floats(DST, 0.f, 0.f, 0.f, 999.f);
  put_floats(SRC_A, 1.f, 2.f, 3.f, 4.f);
  put_floats(SRC_B, 5.f, 6.f, 7.f, 8.f);
  goal_vec_aot_test_outer_product(DST, SRC_A, SRC_B);
  expect_floats("outer.product", -4.f, 8.f, -4.f, 999.f);

  /* a second, asymmetric pair, checked against the cross product computed here */
  const float a[3] = {2.f, -3.f, 5.f};
  const float b[3] = {-7.f, 11.f, 0.5f};
  put_floats(DST, 0.f, 0.f, 0.f, 1.f);
  put_floats(SRC_A, a[0], a[1], a[2], 0.f);
  put_floats(SRC_B, b[0], b[1], b[2], 0.f);
  goal_vec_aot_test_outer_product(DST, SRC_A, SRC_B);
  expect_floats("outer.product of an asymmetric pair", a[1] * b[2] - a[2] * b[1],
                a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0], 1.f);
}

static void test_div_q_exceptional_values(void) {
  put_words(SRC_A, 0x3f800000, 0, 0, 0);
  put_words(SRC_B, 0x80000000, 0, 0, 0);
  goal_vec_aot_test_div_q(DST, SRC_A, SRC_B);
  expect_words("div Q +1 / -0", 0xff7fffff, 0xff7fffff, 0xff7fffff, 0xff7fffff);

  put_words(SRC_A, 0x80000000, 0, 0, 0);
  put_words(SRC_B, 0x00000000, 0, 0, 0);
  goal_vec_aot_test_div_q(DST, SRC_A, SRC_B);
  expect_words("div Q -0 / +0", 0xff7fffff, 0xff7fffff, 0xff7fffff, 0xff7fffff);

  put_words(SRC_A, 0x00000001, 0, 0, 0);
  put_words(SRC_B, 0x3f800000, 0, 0, 0);
  goal_vec_aot_test_div_q(DST, SRC_A, SRC_B);
  expect_words("div Q denormal numerator flush", 0, 0, 0, 0);
}

static void test_merc_matrix_contract(void) {
  /* Identity bind pose and camera, with a 90-degree Z rotation and nonuniform 2x3x4 scale. */
  put_floats(MERC_JOINT, 1.f, 0.f, 0.f, 0.f);
  put_floats(MERC_JOINT + 16, 0.f, 1.f, 0.f, 0.f);
  put_floats(MERC_JOINT + 32, 0.f, 0.f, 1.f, 0.f);
  put_floats(MERC_JOINT + 48, 0.f, 0.f, 0.f, 1.f);
  put_floats(MERC_BONE, 0.f, 2.f, 0.f, 0.f);
  put_floats(MERC_BONE + 16, -3.f, 0.f, 0.f, 0.f);
  put_floats(MERC_BONE + 32, 0.f, 0.f, 4.f, 0.f);
  put_floats(MERC_BONE + 48, 10.f, 20.f, 30.f, 1.f);
  put_floats(MERC_CAMERA, 1.f, 0.f, 0.f, 0.f);
  put_floats(MERC_CAMERA + 16, 0.f, 1.f, 0.f, 0.f);
  put_floats(MERC_CAMERA + 32, 0.f, 0.f, 1.f, 0.f);
  put_floats(MERC_CAMERA + 48, 0.f, 0.f, 0.f, 1.f);

  goal_vec_aot_test_merc_matrix(DST, MERC_JOINT, MERC_BONE, MERC_CAMERA);

  int all_finite = 1;
  const float* lanes = (const float*)host(DST);
  for (int lane = 0; lane < 28; lane++) {
    all_finite &= isfinite(lanes[lane]);
  }
  if (!all_finite) {
    printf("FAIL merc matrix: a required transform or normal lane is non-finite\n");
    g_failures++;
  }

  expect_floats_at("merc tmat column 0", DST, 0.f, 2.f, 0.f, 0.f);
  expect_floats_at("merc tmat column 1", DST + 16, -3.f, 0.f, 0.f, 0.f);
  expect_floats_at("merc tmat column 2", DST + 32, 0.f, 0.f, 4.f, 0.f);
  expect_floats_at("merc tmat translation", DST + 48, 10.f, 20.f, 30.f, 1.f);
  expect_floats_at("merc nmat column 0", DST + 64, 0.f, .5f, 0.f, 0.f);
  expect_floats_at("merc nmat column 1", DST + 80, -1.f / 3.f, 0.f, 0.f, 0.f);
  expect_floats_at("merc nmat column 2", DST + 96, 0.f, 0.f, .25f, 0.f);

  /* A zero X scale is singular. The -0.25 Y and 0.5 Z columns have one +0.125 Y cofactor. PS2 DIV
     clamps 1/0 to MAX; multiplying that exact power-of-two cofactor gives MAX/8 (0x7dffffff),
     while the other two normal columns remain zero. */
  put_floats(MERC_BONE, 0.f, 0.f, 0.f, 0.f);
  put_floats(MERC_BONE + 16, -.25f, 0.f, 0.f, 0.f);
  put_floats(MERC_BONE + 32, 0.f, 0.f, .5f, 0.f);
  goal_vec_aot_test_merc_matrix(DST, MERC_JOINT, MERC_BONE, MERC_CAMERA);
  expect_words_at("singular merc nmat column 0", DST + 64, 0, 0x7dffffff, 0, 0);
  expect_words_at("singular merc nmat column 1", DST + 80, 0, 0, 0, 0);
  expect_words_at("singular merc nmat column 2", DST + 96, 0, 0, 0, 0);

  /* A fully collapsed hide transform has no surviving cofactors, so finite MAX Q still produces a
     zero normal matrix instead of IEEE 0 * infinity NaNs. */
  put_floats(MERC_BONE + 16, 0.f, 0.f, 0.f, 0.f);
  put_floats(MERC_BONE + 32, 0.f, 0.f, 0.f, 0.f);
  goal_vec_aot_test_merc_matrix(DST, MERC_JOINT, MERC_BONE, MERC_CAMERA);
  expect_words_at("collapsed merc nmat column 0", DST + 64, 0, 0, 0, 0);
  expect_words_at("collapsed merc nmat column 1", DST + 80, 0, 0, 0, 0);
  expect_words_at("collapsed merc nmat column 2", DST + 96, 0, 0, 0, 0);
}

static void test_vu_sync_barriers(void) {
  /* .nop.vf and .wait.vf emit nothing, so the add around them must still happen */
  put_floats(SRC_A, 1.f, 2.f, 3.f, 4.f);
  put_floats(SRC_B, 0.5f, 0.25f, -1.f, 100.f);
  goal_vec_aot_test_wait_nop(DST, SRC_A, SRC_B);
  expect_floats("add between VU sync barriers", 1.5f, 2.25f, 2.f, 104.f);
}

static void test_scalar_vu_sqrt_results_are_broadcast(void) {
  put_floats(SRC_A, 1.f, 4.f, 16.f, 64.f);
  goal_vec_aot_test_sqrt(DST, SRC_A);
  expect_floats("sqrt.vf selected lane broadcast", 4.f, 4.f, 4.f, 4.f);

  put_floats(SRC_A, 1.f, 8.f, 27.f, 64.f);
  put_floats(SRC_B, 1.f, 4.f, 16.f, 64.f);
  goal_vec_aot_test_inverse_sqrt(DST, SRC_A, SRC_B);
  expect_floats("isqrt.vf selected lanes broadcast", 2.f, 2.f, 2.f, 2.f);
}

int main(void) {
  goal_test_loader_init();
  goal_test_load_functions(goal_vec_functions, goal_vec_function_count);
  goal_test_load_statics(goal_vec_statics, goal_vec_static_count);
  goal_vec_link();

  test_quadword_copies();
  test_byte_interleave();
  test_halfword_interleave();
  test_word_interleave();
  test_compares();
  test_subtract();
  test_shifts();
  test_ppach();
  test_blend();
  test_outer_product();
  test_div_q_exceptional_values();
  test_merc_matrix_contract();
  test_vu_sync_barriers();
  test_scalar_vu_sqrt_results_are_broadcast();

  if (g_failures) {
    printf("%d vector operation check(s) failed\n", g_failures);
    return 1;
  }
  printf("all AOT C backend vector operation checks passed\n");
  return 0;
}
