/*!
 * @file host_smoke_test.cpp
 * Brings the real Jak 1 kernel core up on the host, prints the real values it produced, exercises
 * the real heap and the real symbol table, and tears it back down.
 *
 * This program contains no staged or synthetic memory: everything it prints is read back out of
 * the GOAL heap that the kernel just built.
 */

#include <cstdio>
#include <cstring>

#include "kernel_core.h"

#include "common/symbols.h"

#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kscheme.h"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    printf("  FAIL: %s (%s)\n", what, goal_kernel_core_last_error());
    g_failures++;
  }
}

void print_type(const char* name) {
  char buffer[64];
  const auto status = goal_kernel_core_type_name_of_symbol(name, buffer, sizeof(buffer));
  if (status != GOAL_KERNEL_CORE_OK) {
    printf("  %-22s <error %d: %s>\n", name, (int)status, goal_kernel_core_last_error());
    g_failures++;
    return;
  }
  uint32_t sym = 0, value = 0;
  goal_kernel_core_lookup(name, &sym, &value);
  auto type = Ptr<jak1::Type>(value);
  printf("  %-22s symbol #x%08x -> type #x%08x  name '%s'  parent '%s'  size %u  methods %u\n",
         name, sym, value, buffer,
         type->parent.offset ? jak1::info(type->parent->symbol)->str->data() : "<none>",
         type->allocated_size, type->num_methods);
}

}  // namespace

int main() {
  printf("== goal_kernel_core_initialize ==\n");
  const auto status = goal_kernel_core_initialize();
  if (status != GOAL_KERNEL_CORE_OK) {
    printf("initialize failed: %d (%s)\n", (int)status, goal_kernel_core_last_error());
    return 1;
  }

  goal_kernel_core_state state;
  check(goal_kernel_core_get_state(&state) == GOAL_KERNEL_CORE_OK, "get_state");

  printf("\n== EE main memory ==\n");
  printf("  base                 %p\n", (void*)(uintptr_t)state.main_memory_address);
  printf("  size                 0x%x (%.1f MiB)\n", state.main_memory_size,
         (double)state.main_memory_size / (1 << 20));
  printf("  PROT_EXEC granted    %s\n", state.main_memory_executable ? "yes" : "NO");

  printf("\n== kernel heaps ==\n");
  printf("  global heap          base #x%08x  current #x%08x  top #x%08x  top_base #x%08x\n",
         state.global_heap_base_offset, state.global_heap_current_offset,
         state.global_heap_top_offset, state.global_heap_top_base_offset);
  printf("  global heap used     %u bytes (%.3f MiB)\n", state.global_heap_used_bytes,
         (double)state.global_heap_used_bytes / (1 << 20));
  printf("  debug heap           base #x%08x  top_base #x%08x\n", state.debug_heap_base_offset,
         state.debug_heap_top_base_offset);

  printf("\n== symbol table ==\n");
  printf("  symbol table         #x%08x\n", state.symbol_table_offset);
  printf("  s7                   #x%08x\n", state.s7_offset);
  printf("  last symbol          #x%08x\n", state.last_symbol_offset);
  printf("  hashed symbol count  %d\n", state.symbol_count);
  printf("  #f                   #x%08x\n", state.false_offset);
  printf("  #t                   #x%08x\n", state.true_offset);
  printf("  empty pair           #x%08x\n", state.empty_pair_offset);

  check(state.s7_offset != 0, "s7 is set");
  check(state.global_heap_used_bytes > 0, "global heap has allocations");
  check(state.symbol_count > 0, "symbols were hashed");
  check(state.false_offset == state.s7_offset, "#f is s7");

  printf("\n== fundamental types (read back out of the heap) ==\n");
  for (const char* name : {"object", "structure", "basic", "symbol", "type", "string", "function",
                           "pair", "array", "kheap", "process", "float", "int32", "uint64"}) {
    print_type(name);
  }

  printf("\n== fixed symbol values ==\n");
  for (const char* name : {"global", "debug", "nothing", "zero-func", "*enable-method-set*",
                           "*debug-segment*", "string->symbol", "kmalloc", "link", "dgo-load"}) {
    uint32_t sym = 0, value = 0;
    if (goal_kernel_core_lookup(name, &sym, &value) == GOAL_KERNEL_CORE_OK) {
      printf("  %-22s symbol #x%08x  value #x%08x\n", name, sym, value);
    } else {
      printf("  %-22s <not found>\n", name);
      g_failures++;
    }
  }

  printf("\n== 'global' symbol points at the real kheapinfo ==\n");
  {
    uint32_t sym = 0, value = 0;
    check(goal_kernel_core_lookup("global", &sym, &value) == GOAL_KERNEL_CORE_OK, "lookup global");
    auto heap = Ptr<kheapinfo>(value);
    printf("  global -> kheapinfo #x%08x  base #x%08x  current #x%08x  top_base #x%08x\n", value,
           heap->base.offset, heap->current.offset, heap->top_base.offset);
    check(heap->base.offset == state.global_heap_base_offset, "'global' matches kglobalheap");
  }

  printf("\n== interning and allocating for real ==\n");
  {
    uint32_t before = 0;
    goal_kernel_core_state s0;
    goal_kernel_core_get_state(&s0);
    before = s0.global_heap_current_offset;

    check(goal_kernel_core_lookup("goalpad-host-smoke-test", nullptr, nullptr) ==
              GOAL_KERNEL_CORE_NOT_FOUND,
          "new symbol is absent before interning");

    uint32_t new_sym = 0;
    check(goal_kernel_core_intern("goalpad-host-smoke-test", &new_sym) == GOAL_KERNEL_CORE_OK,
          "intern new symbol");
    uint32_t found_sym = 0, found_value = 0;
    check(goal_kernel_core_lookup("goalpad-host-smoke-test", &found_sym, &found_value) ==
              GOAL_KERNEL_CORE_OK,
          "look the new symbol back up");
    check(found_sym == new_sym, "lookup returns the interned symbol");
    printf("  interned 'goalpad-host-smoke-test' at #x%08x, name in heap is '%s'\n", new_sym,
           jak1::info(Ptr<jak1::Symbol>(new_sym))->str->data());
    check(strcmp(jak1::info(Ptr<jak1::Symbol>(new_sym))->str->data(),
                 "goalpad-host-smoke-test") == 0,
          "symbol name string round-tripped through the heap");

    uint32_t alloc = 0;
    check(goal_kernel_core_global_alloc(4096, "smoke-test", &alloc) == GOAL_KERNEL_CORE_OK,
          "allocate 4096 bytes from the real global heap");
    printf("  allocated 4096 bytes at #x%08x\n", alloc);

    goal_kernel_core_state s1;
    goal_kernel_core_get_state(&s1);
    printf("  global heap current moved #x%08x -> #x%08x (+%u bytes)\n", before,
           s1.global_heap_current_offset, s1.global_heap_current_offset - before);
    check(s1.global_heap_current_offset > before, "the heap pointer actually moved");
    check(alloc >= before, "the allocation came from the bump region");

    // The allocation must be inside EE main memory and zeroed by KMALLOC_MEMSET.
    auto* bytes = Ptr<u8>(alloc).c();
    bool zeroed = true;
    for (int i = 0; i < 4096; i++) {
      if (bytes[i] != 0) {
        zeroed = false;
      }
    }
    check(zeroed, "allocation was zeroed");
    memset(bytes, 0xab, 4096);
    check(bytes[0] == 0xab && bytes[4095] == 0xab, "allocation is writable");
  }

  // The C kernel builds GOAL function objects by writing machine code into the GOAL heap. On
  // ARM64 those bytes are still x86-64 (see the TODO in make_function_from_c_systemv), and the
  // heap is not executable anyway. Print the bytes so the state of that seam is visible instead
  // of assumed.
  printf("\n== GOAL function objects built by the C kernel ==\n");
  for (const char* name : {"nothing", "zero-func", "string->symbol"}) {
    uint32_t sym = 0, value = 0;
    if (goal_kernel_core_lookup(name, &sym, &value) != GOAL_KERNEL_CORE_OK || !value) {
      continue;
    }
    uint64_t entry = 0;
    memcpy(&entry, Ptr<u8>(value).c(), sizeof(entry));
    printf("  %-16s #x%08x  native entry point %p\n", name, value, (void*)(uintptr_t)entry);
    check(entry != 0, "function object holds a native entry point");
  }
  printf("  EE main memory is %sexecutable. On ARM64 a function object holds the 64-bit native\n"
         "  entry point of its code instead of the code itself, so nothing is executed from the\n"
         "  GOAL heap. See game/kernel/core/aot_loader.h.\n",
         state.main_memory_executable ? "" : "NOT ");

  printf("\n== goal_kernel_core_shutdown ==\n");
  goal_kernel_core_shutdown();
  check(goal_kernel_core_is_initialized() == 0, "kernel reports not initialized");
  check(goal_kernel_core_get_state(&state) == GOAL_KERNEL_CORE_NOT_INITIALIZED,
        "get_state refuses after shutdown");

  printf("\n== re-initialize after shutdown ==\n");
  check(goal_kernel_core_initialize() == GOAL_KERNEL_CORE_OK, "second initialize");
  goal_kernel_core_state state2;
  check(goal_kernel_core_get_state(&state2) == GOAL_KERNEL_CORE_OK, "second get_state");
  printf("  s7 #x%08x, %d symbols, %u heap bytes used\n", state2.s7_offset, state2.symbol_count,
         state2.global_heap_used_bytes);
  check(state2.s7_offset == state.s7_offset, "s7 is deterministic across runs");
  goal_kernel_core_shutdown();

  printf("\n%s (%d failures)\n", g_failures ? "SMOKE TEST FAILED" : "SMOKE TEST PASSED",
         g_failures);
  return g_failures ? 1 : 0;
}
