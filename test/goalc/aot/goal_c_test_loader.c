/*!
 * @file goal_c_test_loader.c
 * The smallest loader that can run C emitted by goalc's AOT C backend.
 *
 * It stands in for the parts of the GOAL runtime the emitted code depends on: a memory arena,
 * a symbol table, placed static data, and one function object per AOT function. Nothing here
 * writes executable memory, which is the point of the exercise.
 */

#include "test/goalc/aot/goal_c_test_loader.h"

#include <stdlib.h>
#include <string.h>

#define GOAL_MEM_SIZE (16 * 1024 * 1024)
#define SYMBOL_TABLE_GOAL_ADDR 0x100000
#define STATIC_AREA_GOAL_ADDR 0x200000
#define FUNCTION_AREA_GOAL_ADDR 0x300000
#define MAX_SYMBOLS 1024
#define MAX_STATICS 512
#define FUNCTION_OBJECT_SIZE 16

uint8_t* g_goal_mem = 0;
uint64_t g_goal_mem_size = GOAL_MEM_SIZE;
uint64_t g_goal_mem_low_protect = 0;
uint64_t g_goal_s7 = 0;
uint64_t g_goal_current_process = 0;

static char s_symbol_names[MAX_SYMBOLS][64];
static int s_symbol_count = 0;
static uint64_t s_static_addrs[MAX_STATICS];
static int s_static_count = 0;
static uint64_t s_function_area_next = 0;
static uint64_t s_function_addrs[MAX_STATICS];
static int s_function_count = 0;

void goal_test_loader_init(void) {
  if (!g_goal_mem) {
    g_goal_mem = (uint8_t*)calloc(1, GOAL_MEM_SIZE);
  } else {
    memset(g_goal_mem, 0, GOAL_MEM_SIZE);
  }
  g_goal_s7 = SYMBOL_TABLE_GOAL_ADDR;
  g_goal_current_process = 0;
  s_symbol_count = 0;
  s_static_count = 0;
  s_function_count = 0;
  s_function_area_next = FUNCTION_AREA_GOAL_ADDR;

  /* the runtime guarantees these three, and the emitted code assumes their fixed offsets */
  goal_symbol_ptr("#f");
  goal_symbol_ptr("#t");
  *goal_symbol_slot("#f") = (int32_t)g_goal_s7;
  *goal_symbol_slot("#t") = (int32_t)(g_goal_s7 + 8);
}

uint64_t goal_test_empty_pair(void) {
  return g_goal_s7 - 10;
}

static int find_symbol(const char* name) {
  for (int i = 0; i < s_symbol_count; i++) {
    if (strcmp(s_symbol_names[i], name) == 0) {
      return i;
    }
  }
  if (s_symbol_count == MAX_SYMBOLS) {
    abort();
  }
  const int index = s_symbol_count++;
  snprintf(s_symbol_names[index], sizeof(s_symbol_names[index]), "%s", name);
  return index;
}

uint64_t goal_symbol_ptr(const char* name) {
  return g_goal_s7 + 8 * (uint64_t)find_symbol(name);
}

int32_t* goal_symbol_slot(const char* name) {
  return (int32_t*)(g_goal_mem + goal_symbol_ptr(name));
}

uint64_t goal_static_addr(const char* file, int index) {
  (void)file;
  if (index < 0 || index >= s_static_count) {
    abort();
  }
  return s_static_addrs[index];
}

uint64_t goal_function_addr(const char* file, int index) {
  (void)file;
  if (index < 0 || index >= s_function_count) {
    abort();
  }
  return s_function_addrs[index];
}

void goal_test_load_functions(const void* const* natives, int count) {
  if (count > MAX_STATICS) {
    abort();
  }
  s_function_count = count;
  for (int i = 0; i < count; i++) {
    s_function_addrs[i] = s_function_area_next;
    s_function_area_next += FUNCTION_OBJECT_SIZE;
    /* GOAL_FN() reads the 64-bit native entry point from the function object's code address */
    memcpy(g_goal_mem + s_function_addrs[i], &natives[i], sizeof(void*));
  }
}

void goal_test_load_statics(const goal_static_desc* statics, int count) {
  if (count > MAX_STATICS) {
    abort();
  }
  uint64_t next = STATIC_AREA_GOAL_ADDR;
  s_static_count = count;
  for (int i = 0; i < count; i++) {
    const int align = statics[i].align > 0 ? statics[i].align : 4;
    while (next % (uint64_t)align) {
      next++;
    }
    s_static_addrs[i] = next;
    memcpy(g_goal_mem + next, statics[i].data, (size_t)statics[i].size);
    next += (uint64_t)statics[i].size;
  }

  for (int i = 0; i < count; i++) {
    for (int r = 0; r < statics[i].reloc_count; r++) {
      const goal_static_reloc* reloc = &statics[i].relocs[r];
      int32_t value = 0;
      switch (reloc->kind) {
        case GOAL_RELOC_SYMBOL_PTR:
          value = (int32_t)goal_symbol_ptr(reloc->name);
          break;
        case GOAL_RELOC_TYPE_PTR:
          value = *goal_symbol_slot(reloc->name);
          break;
        case GOAL_RELOC_STATIC_PTR:
          value = (int32_t)(s_static_addrs[reloc->target_index] + (uint64_t)reloc->target_offset);
          break;
        case GOAL_RELOC_FUNCTION_PTR:
          value = (int32_t)goal_function_addr("", reloc->target_index);
          break;
        default:
          abort();
      }
      memcpy(g_goal_mem + s_static_addrs[i] + (uint64_t)reloc->offset, &value, 4);
    }
  }
}
