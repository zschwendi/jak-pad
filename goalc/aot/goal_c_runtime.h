#pragma once

/*!
 * @file goal_c_runtime.h
 * Runtime model for C source emitted by the GOALPad AOT C backend (goalc/aot/CBackend.cpp).
 *
 * The emitted C reproduces the GOAL machine model that goalc's x86-64 backend targets:
 *  - GOAL pointers are offsets from a single memory base (x86-64 keeps this base in r15).
 *  - The symbol table lives at a GOAL address kept in s7 (x86-64 keeps this in r14).
 *  - The process a thread is running lives in a fixed register too (x86-64 keeps this in r13).
 *  - Symbol values, static objects and function objects are resolved by the loader, not by
 *    patching instructions, so nothing here needs writable executable memory.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t goal_u64;
typedef int64_t goal_s64;

typedef float goal_vf __attribute__((vector_size(16)));
typedef int32_t goal_vi __attribute__((vector_size(16)));

/*!
 * The PS2 128-bit integer instructions read the same register as bytes, halfwords or words, so
 * their C model needs the same register under each of those views. These are only ever used
 * inside the helpers below; emitted code sees goal_vi.
 */
typedef uint32_t goal_vwu __attribute__((vector_size(16)));
typedef int16_t goal_vh __attribute__((vector_size(16)));
typedef uint16_t goal_vhu __attribute__((vector_size(16)));
typedef int8_t goal_vb __attribute__((vector_size(16)));
typedef uint8_t goal_vbu __attribute__((vector_size(16)));

/*! Base of GOAL memory. A GOAL pointer p refers to g_goal_mem + p. */
extern uint8_t* g_goal_mem;

/*! GOAL address of the symbol table (the "#f" symbol). */
extern uint64_t g_goal_s7;

/*!
 * GOAL address of the process that is currently running.
 *
 * x86-64 keeps this in r13 and never lets the register allocator use it, so it is ambient machine
 * state: an ordinary call leaves it alone, and only the kernel's thread dispatch code assigns it.
 * This location reproduces exactly that. A behavior's `self` and every
 * (rlet ((pp :reg r13 ...)) ...) read and write it directly, so they always agree, including
 * inside callees and after a thread switch.
 *
 * Like g_goal_mem and g_goal_s7 this assumes GOAL code runs on one OS thread at a time, which is
 * what the OpenGOAL kernel does.
 */
extern uint64_t g_goal_current_process;

/*!
 * Loader-provided tables. Emitted code indexes these instead of embedding absolute addresses,
 * which keeps every generated translation unit position independent and read-only at runtime.
 */
extern int32_t* goal_symbol_slot(const char* name);
extern uint64_t goal_symbol_ptr(const char* name);
extern uint64_t goal_static_addr(const char* file, int index);
extern uint64_t goal_function_addr(const char* file, int index);

/*!
 * GOAL address of the current stack pointer, for the (suspend) stack-overflow check and with-sp.
 *
 * GOAL cooperative threads run on stacks carved out of GOAL memory, so the machine stack pointer
 * has a GOAL address and `(- sp off)` is a real pointer into the running thread's stack. The C
 * backend can express reading it but not writing it; see docs/aot-stack-model.md.
 *
 * The runtime refuses to answer when the current stack is not a GOAL stack, because there is no
 * correct answer then and a made-up one would silently corrupt GOAL's stack accounting.
 */
extern uint64_t goal_stack_pointer(const void* frame);
#define GOAL_STACK_POINTER() goal_stack_pointer(__builtin_frame_address(0))

/*!
 * Static data description.
 *
 * goalc's x86-64 path writes static objects into the object file and lets the runtime linker
 * place and relocate them. The AOT build keeps the same data but as read-only C arrays plus a
 * relocation list the loader applies once, so no instruction is ever patched.
 */
enum goal_reloc_kind {
  GOAL_RELOC_SYMBOL_PTR,   /*! write the GOAL address of a symbol */
  GOAL_RELOC_TYPE_PTR,     /*! write the value of a type's symbol */
  GOAL_RELOC_STATIC_PTR,   /*! write the GOAL address of another static in this file */
  GOAL_RELOC_FUNCTION_PTR, /*! write the GOAL address of a function object in this file */
};

typedef struct {
  int kind;
  int offset;        /*! byte offset inside the static to patch */
  const char* name;  /*! symbol or type name, for the symbol/type kinds */
  int target_index;  /*! static or function index, for the other kinds */
  int target_offset; /*! extra byte offset inside the target static */
  int method_count;  /*! for GOAL_RELOC_TYPE_PTR, what intern-type needs */
} goal_static_reloc;

typedef struct {
  int align;
  int size;
  int addr_offset; /*! what a reference to this static points at, relative to its start */
  const uint8_t* data;
  int reloc_count;
  const goal_static_reloc* relocs;
} goal_static_desc;

#define GOAL_PTR(base, off) \
  ((void*)((uintptr_t)g_goal_mem + (uintptr_t)(uint64_t)(base) + (intptr_t)(int64_t)(off)))

#define GOAL_ADDR_OF(host_ptr) ((uint64_t)((uintptr_t)(host_ptr) - (uintptr_t)g_goal_mem))

/*!
 * Indirect call through a GOAL function object.
 *
 * The x86-64 backend jumps to g_goal_mem + p, because JIT-compiled GOAL code lives inside GOAL
 * memory. AOT code lives in __TEXT and cannot be reached through a 32-bit GOAL pointer, so the
 * loader stores the 64-bit native entry point at the function object's code address instead.
 * Cost is one load, the same as the x86-64 indirect call.
 */
#define GOAL_FN(p) (*(void**)GOAL_PTR(p, 0))

static inline uint64_t goal_load_u(const void* p, int size) {
  switch (size) {
    case 1:
      return (uint64_t)*(const uint8_t*)p;
    case 2:
      return (uint64_t)*(const uint16_t*)p;
    case 4:
      return (uint64_t)*(const uint32_t*)p;
    default:
      return *(const uint64_t*)p;
  }
}

static inline uint64_t goal_load_s(const void* p, int size) {
  switch (size) {
    case 1:
      return (uint64_t)(int64_t)*(const int8_t*)p;
    case 2:
      return (uint64_t)(int64_t)*(const int16_t*)p;
    case 4:
      return (uint64_t)(int64_t)*(const int32_t*)p;
    default:
      return *(const uint64_t*)p;
  }
}

static inline void goal_store(void* p, uint64_t value, int size) {
  switch (size) {
    case 1:
      *(uint8_t*)p = (uint8_t)value;
      return;
    case 2:
      *(uint16_t*)p = (uint16_t)value;
      return;
    case 4:
      *(uint32_t*)p = (uint32_t)value;
      return;
    default:
      *(uint64_t*)p = value;
      return;
  }
}

static inline goal_vf goal_load_vf(const void* p) {
  goal_vf out;
  __builtin_memcpy(&out, p, 16);
  return out;
}

static inline void goal_store_vf(void* p, goal_vf value) {
  __builtin_memcpy(p, &value, 16);
}

static inline uint32_t goal_f32_bits(float f) {
  uint32_t out;
  __builtin_memcpy(&out, &f, 4);
  return out;
}

static inline float goal_bits_f32(uint32_t u) {
  float out;
  __builtin_memcpy(&out, &u, 4);
  return out;
}

static inline uint64_t goal_vf_low64(goal_vf v) {
  uint64_t out;
  __builtin_memcpy(&out, &v, 8);
  return out;
}

static inline goal_vf goal_vf_from_u64(uint64_t u) {
  goal_vf out = {0.f, 0.f, 0.f, 0.f};
  __builtin_memcpy(&out, &u, 8);
  return out;
}

/*! The PS2 sign-extends 32-bit multiply results; goalc's x86 backend replicates that. */
static inline uint64_t goal_imul32(uint64_t a, uint64_t b) {
  return (uint64_t)(int64_t)(int32_t)((uint32_t)a * (uint32_t)b);
}

static inline uint64_t goal_idiv32(uint64_t a, uint64_t b) {
  return (uint64_t)(int64_t)(int32_t)((int32_t)a / (int32_t)b);
}

static inline uint64_t goal_imod32(uint64_t a, uint64_t b) {
  return (uint64_t)(int64_t)(int32_t)((int32_t)a % (int32_t)b);
}

static inline uint64_t goal_udiv32(uint64_t a, uint64_t b) {
  return (uint64_t)(int64_t)(int32_t)((uint32_t)a / (uint32_t)b);
}

static inline uint64_t goal_umod32(uint64_t a, uint64_t b) {
  return (uint64_t)(int64_t)(int32_t)((uint32_t)a % (uint32_t)b);
}

static inline uint64_t goal_f2i(float f) {
  return (uint64_t)(int64_t)(int32_t)f;
}

static inline float goal_i2f(uint64_t i) {
  return (float)(int32_t)(uint32_t)i;
}

static inline float goal_min_ss(float a, float b) {
  return b < a ? b : a;
}

static inline float goal_max_ss(float a, float b) {
  return b > a ? b : a;
}

static inline float goal_vf_x(goal_vf v) {
  return v[0];
}

static inline uint64_t goal_low64(goal_vf v) {
  uint64_t out;
  __builtin_memcpy(&out, &v, 8);
  return out;
}

static inline goal_vf goal_vf_set_x(goal_vf dst, float value) {
  dst[0] = value;
  return dst;
}

static inline goal_vf goal_vf_min(goal_vf a, goal_vf b) {
  goal_vf out;
  for (int i = 0; i < 4; i++) {
    out[i] = b[i] < a[i] ? b[i] : a[i];
  }
  return out;
}

static inline goal_vf goal_vf_max(goal_vf a, goal_vf b) {
  goal_vf out;
  for (int i = 0; i < 4; i++) {
    out[i] = b[i] > a[i] ? b[i] : a[i];
  }
  return out;
}

static inline goal_vf goal_vf_xor(goal_vf a, goal_vf b) {
  return (goal_vf)((goal_vi)a ^ (goal_vi)b);
}

static inline goal_vf goal_vf_itof(goal_vi v) {
  return __builtin_convertvector(v, goal_vf);
}

static inline goal_vi goal_vf_ftoi(goal_vf v) {
  return __builtin_convertvector(v, goal_vi);
}

static inline goal_vf goal_vf_splat(goal_vf v, int lane) {
  goal_vf out = {v[lane], v[lane], v[lane], v[lane]};
  return out;
}

/*!
 * PS2 128-bit integer operations.
 *
 * Every helper below takes its operands in the order the GOAL asm form writes them:
 * (.pextlb dst src1 src2) is the PS2's `pextlb rd, rs, rt`, so a is rs and b is rt. That is also
 * the order goalc's IR keeps (IR_Int128Math3Asm::source1/source2); the x86-64 backend swaps some
 * of them at the instruction level only because x86's interleave instructions number their
 * operands the other way round.
 *
 * The semantics here are transcribed from the PS2 reference implementations the runtime already
 * carries in game/mips2c/mips2c_private.h, which is where OpenGOAL's hand-translated PS2 assembly
 * gets them.
 */

/*! pcpyud rd, rs, rt: rd = { rs upper 64, rt upper 64 }. */
static inline goal_vi goal_pcpyud(goal_vi a, goal_vi b) {
  goal_vi out;
  out[0] = a[2];
  out[1] = a[3];
  out[2] = b[2];
  out[3] = b[3];
  return out;
}

/*! pcpyld rd, rs, rt: rd = { rt lower 64, rs lower 64 }. */
static inline goal_vi goal_pcpyld(goal_vi a, goal_vi b) {
  goal_vi out;
  out[0] = b[0];
  out[1] = b[1];
  out[2] = a[0];
  out[3] = a[1];
  return out;
}

/*! pextlb rd, rs, rt: interleave the low 8 bytes, rt first. */
static inline goal_vi goal_pextlb(goal_vi a, goal_vi b) {
  const goal_vbu s = (goal_vbu)a;
  const goal_vbu t = (goal_vbu)b;
  goal_vbu out;
  for (int i = 0; i < 8; i++) {
    out[2 * i] = t[i];
    out[2 * i + 1] = s[i];
  }
  return (goal_vi)out;
}

/*! pextub rd, rs, rt: interleave the high 8 bytes, rt first. */
static inline goal_vi goal_pextub(goal_vi a, goal_vi b) {
  const goal_vbu s = (goal_vbu)a;
  const goal_vbu t = (goal_vbu)b;
  goal_vbu out;
  for (int i = 0; i < 8; i++) {
    out[2 * i] = t[8 + i];
    out[2 * i + 1] = s[8 + i];
  }
  return (goal_vi)out;
}

/*! pextlh rd, rs, rt: interleave the low 4 halfwords, rt first. */
static inline goal_vi goal_pextlh(goal_vi a, goal_vi b) {
  const goal_vhu s = (goal_vhu)a;
  const goal_vhu t = (goal_vhu)b;
  goal_vhu out;
  for (int i = 0; i < 4; i++) {
    out[2 * i] = t[i];
    out[2 * i + 1] = s[i];
  }
  return (goal_vi)out;
}

/*! pextuh rd, rs, rt: interleave the high 4 halfwords, rt first. */
static inline goal_vi goal_pextuh(goal_vi a, goal_vi b) {
  const goal_vhu s = (goal_vhu)a;
  const goal_vhu t = (goal_vhu)b;
  goal_vhu out;
  for (int i = 0; i < 4; i++) {
    out[2 * i] = t[4 + i];
    out[2 * i + 1] = s[4 + i];
  }
  return (goal_vi)out;
}

/*! pextlw rd, rs, rt: interleave the low 2 words, rt first. */
static inline goal_vi goal_pextlw(goal_vi a, goal_vi b) {
  goal_vi out;
  out[0] = b[0];
  out[1] = a[0];
  out[2] = b[1];
  out[3] = a[1];
  return out;
}

/*! pextuw rd, rs, rt: interleave the high 2 words, rt first. */
static inline goal_vi goal_pextuw(goal_vi a, goal_vi b) {
  goal_vi out;
  out[0] = b[2];
  out[1] = a[2];
  out[2] = b[3];
  out[3] = a[3];
  return out;
}

/*
 * The parallel compares set a lane to all ones or all zeros. A vector comparison in C already
 * produces exactly that, so the only thing each helper has to get right is the lane width and,
 * for the greater-than compares, the signedness: the PS2's pcgt* are signed.
 */

static inline goal_vi goal_pceqb(goal_vi a, goal_vi b) {
  return (goal_vi)((goal_vb)a == (goal_vb)b);
}

static inline goal_vi goal_pceqh(goal_vi a, goal_vi b) {
  return (goal_vi)((goal_vh)a == (goal_vh)b);
}

static inline goal_vi goal_pceqw(goal_vi a, goal_vi b) {
  return a == b;
}

static inline goal_vi goal_pcgtb(goal_vi a, goal_vi b) {
  return (goal_vi)((goal_vb)a > (goal_vb)b);
}

static inline goal_vi goal_pcgth(goal_vi a, goal_vi b) {
  return (goal_vi)((goal_vh)a > (goal_vh)b);
}

static inline goal_vi goal_pcgtw(goal_vi a, goal_vi b) {
  return a > b;
}

/*! paddb rd, rs, rt: 16 independent byte adds, wrapping. */
static inline goal_vi goal_paddb(goal_vi a, goal_vi b) {
  return (goal_vi)((goal_vbu)a + (goal_vbu)b);
}

/*
 * Shifts. The C backend refuses to emit a shift whose count reaches the lane width, so the counts
 * that get here are always in range and a plain C shift is exact. See CBackend.cpp for why: the
 * PS2 and x86-64 disagree about what an out-of-range count means, and Jak 1 never uses one.
 */

static inline goal_vi goal_pw_sll(goal_vi a, int sa) {
  return (goal_vi)((goal_vwu)a << sa);
}

static inline goal_vi goal_pw_srl(goal_vi a, int sa) {
  return (goal_vi)((goal_vwu)a >> sa);
}

static inline goal_vi goal_pw_sra(goal_vi a, int sa) {
  return a >> sa;
}

static inline goal_vi goal_ph_sll(goal_vi a, int sa) {
  return (goal_vi)((goal_vhu)a << sa);
}

static inline goal_vi goal_ph_srl(goal_vi a, int sa) {
  return (goal_vi)((goal_vhu)a >> sa);
}

/*!
 * Shift the whole 128-bit register right by a whole number of bytes, filling with zeros. This has
 * no PS2 instruction behind it: goalc builds .ppach and .ppacb out of x86 byte shuffles, so the C
 * backend has to reproduce those steps.
 */
static inline goal_vi goal_vsrl_bytes(goal_vi a, int bytes) {
  const goal_vbu s = (goal_vbu)a;
  goal_vbu out;
  for (int i = 0; i < 16; i++) {
    const int j = i + bytes;
    out[i] = j < 16 ? s[j] : 0;
  }
  return (goal_vi)out;
}

/*! Shift the whole 128-bit register left by a whole number of bytes, filling with zeros. */
static inline goal_vi goal_vsll_bytes(goal_vi a, int bytes) {
  const goal_vbu s = (goal_vbu)a;
  goal_vbu out;
  for (int i = 0; i < 16; i++) {
    const int j = i - bytes;
    out[i] = j >= 0 ? s[j] : 0;
  }
  return (goal_vi)out;
}

/*! Permute the low 4 halfwords by a 4x2-bit control, leaving the high 4 alone. */
static inline goal_vi goal_shuffle_low_halfwords(goal_vi a, int control) {
  const goal_vhu s = (goal_vhu)a;
  goal_vhu out;
  for (int i = 0; i < 4; i++) {
    out[i] = s[(control >> (2 * i)) & 3];
    out[4 + i] = s[4 + i];
  }
  return (goal_vi)out;
}

/*! Permute the high 4 halfwords by a 4x2-bit control, leaving the low 4 alone. */
static inline goal_vi goal_shuffle_high_halfwords(goal_vi a, int control) {
  const goal_vhu s = (goal_vhu)a;
  goal_vhu out;
  for (int i = 0; i < 4; i++) {
    out[i] = s[i];
    out[4 + i] = s[4 + ((control >> (2 * i)) & 3)];
  }
  return (goal_vi)out;
}

/*!
 * Pack 8 signed halfwords from a and 8 from b into 16 unsigned bytes, saturating. a supplies the
 * low half of the result.
 */
static inline goal_vi goal_packuswb(goal_vi a, goal_vi b) {
  const goal_vh s = (goal_vh)a;
  const goal_vh t = (goal_vh)b;
  goal_vbu out;
  for (int i = 0; i < 8; i++) {
    out[i] = s[i] < 0 ? 0 : (s[i] > 255 ? 255 : (uint8_t)s[i]);
    out[8 + i] = t[i] < 0 ? 0 : (t[i] > 255 ? 255 : (uint8_t)t[i]);
  }
  return (goal_vi)out;
}

/*! Select each of the 4 float lanes from b where the mask bit is set, from a otherwise. */
static inline goal_vf goal_blend_vf(goal_vf a, goal_vf b, int mask) {
  goal_vf out;
  for (int i = 0; i < 4; i++) {
    out[i] = ((mask >> i) & 1) ? b[i] : a[i];
  }
  return out;
}

/*! Permute the 4 float lanes by a 4x2-bit control. Lane i of the result comes from control[2i]. */
static inline goal_vf goal_vf_shuffle(goal_vf v, int control) {
  goal_vf out;
  for (int i = 0; i < 4; i++) {
    out[i] = v[(control >> (2 * i)) & 3];
  }
  return out;
}

#ifdef __cplusplus
}
#endif
