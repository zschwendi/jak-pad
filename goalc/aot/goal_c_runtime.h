#pragma once

/*!
 * @file goal_c_runtime.h
 * Runtime model for C source emitted by the GOALPad AOT C backend (goalc/aot/CBackend.cpp).
 *
 * The emitted C reproduces the GOAL machine model that goalc's x86-64 backend targets:
 *  - GOAL pointers are offsets from a single memory base (x86-64 keeps this base in r15).
 *  - The symbol table lives at a GOAL address kept in s7 (x86-64 keeps this in r14).
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

/*! Base of GOAL memory. A GOAL pointer p refers to g_goal_mem + p. */
extern uint8_t* g_goal_mem;

/*! GOAL address of the symbol table (the "#f" symbol). */
extern uint64_t g_goal_s7;

/*!
 * Loader-provided tables. Emitted code indexes these instead of embedding absolute addresses,
 * which keeps every generated translation unit position independent and read-only at runtime.
 */
extern int32_t* goal_symbol_slot(const char* name);
extern uint64_t goal_symbol_ptr(const char* name);
extern uint64_t goal_static_addr(const char* file, int index);
extern uint64_t goal_function_addr(const char* file, int index);

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

/*! PS2 pcpyud: dst = { upper 64 bits of a, upper 64 bits of b }. */
static inline goal_vi goal_pcpyud(goal_vi a, goal_vi b) {
  goal_vi out;
  out[0] = a[2];
  out[1] = a[3];
  out[2] = b[2];
  out[3] = b[3];
  return out;
}

static inline goal_vf goal_vf_splat(goal_vf v, int lane) {
  goal_vf out = {v[lane], v[lane], v[lane], v[lane]};
  return out;
}

#ifdef __cplusplus
}
#endif
