/*!
 * @file aot_execution_test.cpp
 * Execute real Jak 1 GOAL code, compiled ahead of time by goalc's AOT C backend, inside the real
 * OpenGOAL Jak 1 kernel on ARM64.
 *
 * Everything here goes through the kernel's own machinery: the AOT static data is placed in the
 * real global heap and relocated against the real symbol table, each AOT function gets a real GOAL
 * `function` object on that heap, the object files' `top-level` functions run through `call_goal`,
 * and every GOAL function afterwards is reached by looking its symbol up in the real symbol table
 * and calling `call_goal` on whatever function object it holds.
 *
 * No writable-executable memory is used anywhere.
 */

#include <cinttypes>
#include <cstdio>
#include <cstring>

// The C backend does not put an extern "C" guard in the headers it generates, so the one C++
// consumer adds it here rather than reaching into goalc/aot/CBackend.cpp.
extern "C" {
#include "gcommon_generated.h"
#include "gstring_generated.h"
}

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/symbols.h"

#include "game/kernel/common/klisten.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

namespace {

int g_failures = 0;

void fail(const char* what) {
  std::printf("  FAIL %s\n", what);
  g_failures++;
}

void check_s64(const char* what, u64 got, s64 expected) {
  if ((s64)got != expected) {
    std::printf("  FAIL %s: got %" PRId64 ", expected %" PRId64 "\n", what, (s64)got, expected);
    g_failures++;
  } else {
    std::printf("  ok   %-32s = %" PRId64 "\n", what, (s64)got);
  }
}

void check_u64(const char* what, u64 got, u64 expected) {
  if (got != expected) {
    std::printf("  FAIL %s: got #x%" PRIx64 ", expected #x%" PRIx64 "\n", what, got, expected);
    g_failures++;
  } else {
    std::printf("  ok   %-32s = #x%" PRIx64 "\n", what, got);
  }
}

u32 symbol_value(const char* name) {
  auto sym = jak1::find_symbol_from_c(name);
  return sym.offset ? sym->value : 0;
}

/*! Call a GOAL function by symbol name, through call_goal. Aborts the check on a missing symbol. */
u64 call_symbol(const char* name, u64 a0 = 0, u64 a1 = 0, u64 a2 = 0) {
  u64 result = 0;
  if (goal_aot_call_symbol(name, a0, a1, a2, &result) != GOAL_KERNEL_CORE_OK) {
    fail(name);
    return 0;
  }
  return result;
}

const char* type_name_of(u32 basic) {
  if (!basic) {
    return "<null>";
  }
  auto type = Ptr<jak1::Type>(*Ptr<u32>(basic - BASIC_OFFSET));
  if (!type.offset || !type->symbol.offset) {
    return "<not a basic>";
  }
  return jak1::info(type->symbol)->str->data();
}

/*! Call a GOAL method through the kernel's own method dispatch, which also uses call_goal. */
u64 call_method(u32 obj, u32 type, u32 method_id) {
  return jak1::call_method_of_type(obj, Ptr<jak1::Type>(type), method_id);
}

u32 goal_list_of(const s32* values, int count) {
  u32 list = (s7 + jak1_symbols::FIX_SYM_EMPTY_PAIR).offset;
  const u32 pair_type = *(s7 + jak1_symbols::FIX_SYM_PAIR_TYPE);
  for (int i = count - 1; i >= 0; i--) {
    list = (u32)jak1::new_pair(s7.offset + jak1_symbols::FIX_SYM_GLOBAL_HEAP, pair_type,
                               (u32)values[i], list);
  }
  return list;
}

void print_state(const char* stage) {
  goal_kernel_core_state state;
  if (goal_kernel_core_get_state(&state) != GOAL_KERNEL_CORE_OK) {
    fail("goal_kernel_core_get_state");
    return;
  }
  std::printf("%s: s7 #x%x, %d symbols, global heap current #x%x, EE main memory executable: %s\n",
              stage, state.s7_offset, state.symbol_count, state.global_heap_current_offset,
              state.main_memory_executable ? "YES" : "NO");
}

void load_file(const char* tag,
               const goal_static_desc* statics,
               int static_count,
               const void* const* functions,
               int function_count,
               void (*link)(void)) {
  goal_aot_object_file file = {tag, statics, static_count, functions, function_count, link};
  const auto status = goal_aot_load(&file);
  if (status != GOAL_KERNEL_CORE_OK) {
    std::printf("  FAIL goal_aot_load(%s) = %d\n", tag, (int)status);
    g_failures++;
    return;
  }
  std::printf("  loaded %-8s %3d statics, %3d functions, first function object #x%x, "
              "top-level #x%x\n",
              tag, static_count, function_count, goal_aot_function_object(tag, 0),
              goal_aot_top_level_object(tag));
}

/*!
 * gcommon and gstring only carry LINK_TYPE_PTR relocations, so the other three kinds the loader
 * implements are exercised with a hand-written descriptor of the same shape the C backend emits.
 * This is loader coverage, not GOAL code: nothing here came out of goalc.
 */
u64 loader_check_function() {
  return 0x600d;
}

void check_remaining_relocation_kinds() {
  static const u8 static_0_data[16] = {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  static const u8 static_1_data[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  static const goal_static_reloc static_0_relocs[] = {
      // -1 in the data means "store the symbol's address", anything else means "store its offset
      // from s7" - the rule symlink_v3 uses
      {GOAL_RELOC_SYMBOL_PTR, 0, "global", 0, 0, 0},
      {GOAL_RELOC_SYMBOL_PTR, 4, "global", 0, 0, 0},
      {GOAL_RELOC_TYPE_PTR, 8, "string", 0, 0, 9},
      {GOAL_RELOC_STATIC_PTR, 12, 0, 1, 4, 0},
  };
  static const goal_static_reloc static_1_relocs[] = {
      {GOAL_RELOC_FUNCTION_PTR, 0, 0, 0, 0, 0},
  };
  static const goal_static_desc statics[2] = {
      {16, sizeof(static_0_data), 0, static_0_data, 4, static_0_relocs},
      {16, sizeof(static_1_data), 0, static_1_data, 1, static_1_relocs},
  };
  static const void* const functions[1] = {(const void*)&loader_check_function};

  goal_aot_object_file file = {"loader-check", statics, 2, functions, 1, nullptr};
  if (goal_aot_load(&file) != GOAL_KERNEL_CORE_OK) {
    fail("goal_aot_load(loader-check)");
    return;
  }
  const u32 base = (u32)goal_static_addr("loader-check", 0);
  const u32 other = (u32)goal_static_addr("loader-check", 1);
  const u32 symbol = jak1::find_symbol_from_c("global").offset;

  check_u64("LINK_SYMBOL_OFFSET (address)", *Ptr<u32>(base).c(), symbol);
  check_s64("LINK_SYMBOL_OFFSET (s7 offset)", *Ptr<s32>(base + 4).c(), (s32)(symbol - s7.offset));
  check_u64("LINK_TYPE_PTR", *Ptr<u32>(base + 8).c(), *(s7 + jak1_symbols::FIX_SYM_STRING_TYPE));
  check_u64("LINK_PTR", *Ptr<u32>(base + 12).c(), other + 4);
  check_u64("static -> function object", *Ptr<u32>(other).c(),
            goal_aot_function_object("loader-check", 0));
  check_u64("that function object runs", goal_aot_call(*Ptr<u32>(other).c(), 0, 0, 0), 0x600d);
}

/*!
 * GOAL's cooperative threads run on stacks carved out of GOAL memory, so AOT-compiled GOAL code
 * has to work with the stack pointer inside `g_ee_main_mem`. `call_goal_on_stack` makes that
 * switch, but it passes no GOAL arguments - the first argument register holds the new stack
 * pointer at the call - so the function it runs here is a small native trampoline installed as a
 * real GOAL function object, the same way check_remaining_relocation_kinds installs one. The
 * trampoline records where it is running and then calls the recursive GOAL function `fact` with
 * the ordinary `call_goal`, which runs on whatever stack it is called from: so `fact` and all of
 * its recursion happen on the GOAL-memory stack.
 */
u64 g_trampoline_frame = 0;
u64 g_trampoline_fact = 0;

u64 goal_stack_trampoline() {
  volatile u8 frame = 0;
  g_trampoline_frame = (u64)(uintptr_t)&frame;
  g_trampoline_fact =
      call_goal(Ptr<Function>(symbol_value("fact")), 10, 0, 0, s7.offset, g_ee_main_mem);
  return g_trampoline_fact;
}

/*! Count the non-zero bytes in a region and report how far below its end the lowest one is. */
u32 count_dirty_bytes(const u8* region, s32 size, s32* out_depth) {
  u32 dirty = 0;
  s32 lowest = size;
  for (s32 i = 0; i < size; i++) {
    if (region[i]) {
      dirty++;
      if (i < lowest) {
        lowest = i;
      }
    }
  }
  *out_depth = size - lowest;
  return dirty;
}

}  // namespace

int main() {
  lg::set_stdout_level(lg::level::warn);
  lg::set_flush_level(lg::level::warn);
  lg::initialize();

  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  print_state("kernel up");

  std::printf("\n== loading AOT object files into the real global heap ==\n");
  load_file("gcommon", goal_gcommon_statics, goal_gcommon_static_count, goal_gcommon_functions,
            goal_gcommon_function_count, goal_gcommon_link);
  load_file("gstring", goal_gstring_statics, goal_gstring_static_count, goal_gstring_functions,
            goal_gstring_function_count, goal_gstring_link);
  if (g_failures) {
    return 1;
  }
  print_state("after load");

  std::printf("\n== the placed data is real GOAL data ==\n");
  {
    const u32 first = goal_aot_function_object("gcommon", 0);
    std::printf("  function object #x%x has type '%s'\n", first, type_name_of(first));
    if (std::strcmp(type_name_of(first), "function") != 0) {
      fail("AOT function objects are not of type 'function'");
    }
    // gcommon's statics are all strings, and every one carries a LINK_TYPE_PTR relocation to
    // 'string. Static 2 is the "[~8x] ~A~%" format string used by mem-print.
    const u32 str = (u32)goal_static_addr("gcommon", 2) + BASIC_OFFSET;
    std::printf("  relocated static #x%x has type '%s' and holds \"%s\"\n", str, type_name_of(str),
                Ptr<String>(str)->data());
    if (std::strcmp(type_name_of(str), "string") != 0) {
      fail("LINK_TYPE_PTR relocation did not resolve against the real symbol table");
    }
  }

  std::printf("\n== the remaining V3 relocation kinds, against the real symbol table ==\n");
  check_remaining_relocation_kinds();

  std::printf("\n== running each object file's top-level through call_goal ==\n");
  if (symbol_value("identity")) {
    fail("'identity' already had a value before the top-level ran");
  }
  // goal_aot_run_top_level stands in for a DGO load's EXECUTE step: GOAL's own stack, and
  // *enable-method-set* raised the way InitHeapAndSymbol raises it around the kernel DGO.
  if (goal_aot_run_top_level("gcommon", nullptr) != GOAL_KERNEL_CORE_OK) {
    fail("goal_aot_run_top_level(gcommon)");
  }
  if (goal_aot_run_top_level("gstring", nullptr) != GOAL_KERNEL_CORE_OK) {
    fail("goal_aot_run_top_level(gstring)");
  }
  print_state("after top-level");

  {
    const u32 identity = symbol_value("identity");
    std::printf("  'identity is now #x%x of type '%s'\n", identity, type_name_of(identity));
    if (std::strcmp(type_name_of(identity), "function") != 0) {
      fail("the top-level did not define 'identity' as a function");
    }
    // top-level built the vec4s type by calling the kernel's own (method new type)
    const u32 vec4s = symbol_value("vec4s");
    if (!vec4s) {
      fail("the top-level did not create the 'vec4s type");
    } else {
      auto type = Ptr<jak1::Type>(vec4s);
      std::printf("  'vec4s is now a real type: name '%s', parent '%s', size %d, %d methods\n",
                  jak1::info(type->symbol)->str->data(),
                  jak1::info(type->parent->symbol)->str->data(), type->allocated_size,
                  type->num_methods);
    }
  }

  std::printf("\n== calling real Jak 1 GOAL functions through call_goal ==\n");
  check_s64("(identity 305441741)", call_symbol("identity", 305441741), 305441741);
  check_s64("(+ 3 4)", call_symbol("+", 3, 4), 7);
  check_s64("(- 3 4)", call_symbol("-", 3, 4), -1);
  check_s64("(* 100000 100000)", call_symbol("*", 100000, 100000), 1410065408);
  check_s64("(/ -7 2)", call_symbol("/", (u64)-7, 2), -3);
  check_s64("(mod -7 3)", call_symbol("mod", (u64)-7, 3), -1);
  check_s64("(abs -5)", call_symbol("abs", (u64)-5), 5);
  check_s64("(min -3 4)", call_symbol("min", (u64)-3, 4), -3);
  check_s64("(max -3 4)", call_symbol("max", (u64)-3, 4), 4);
  check_s64("(ash 1 4)", call_symbol("ash", 1, 4), 16);
  check_s64("(ash -16 -2)", call_symbol("ash", (u64)-16, (u64)-2), -4);
  check_u64("(logior #xa #x5)", call_symbol("logior", 0xa, 0x5), 0xf);
  check_u64("(lognot 0)", call_symbol("lognot", 0), 0xffffffffffffffffull);
  check_u64("(false-func)", call_symbol("false-func"), s7.offset);
  check_u64("(true-func)", call_symbol("true-func"), s7.offset + 8);

  // fact recurses through the 'fact symbol, so this is GOAL code calling GOAL code by symbol
  check_s64("(fact 5)", call_symbol("fact", 5), 120);
  check_s64("(fact 10)", call_symbol("fact", 10), 3628800);

  {
    const s32 values[3] = {100, 200, 300};
    const u32 list = goal_list_of(values, 3);
    check_s64("(ref list 0)", call_symbol("ref", list, 0), 100);
    check_s64("(ref list 2)", call_symbol("ref", list, 2), 300);
    const u32 pair_type = *(s7 + jak1_symbols::FIX_SYM_PAIR_TYPE);
    check_s64("(length list)", call_method(list, pair_type, GOAL_LENGTH_METHOD), 3);
    const u32 last = (u32)call_symbol("last", list);
    check_s64("(car (last list))", *Ptr<s32>(last - PAIR_OFFSET), 300);
    check_s64("(length '())",
              call_method((s7 + jak1_symbols::FIX_SYM_EMPTY_PAIR).offset, pair_type,
                          GOAL_LENGTH_METHOD),
              0);
  }

  {
    // basic-type? walks the real type hierarchy the kernel bootstrapped
    const u32 str = (u32)jak1::make_string_from_c("goalpad");
    const u32 string_type = *(s7 + jak1_symbols::FIX_SYM_STRING_TYPE);
    const u32 pair_type = *(s7 + jak1_symbols::FIX_SYM_PAIR_TYPE);
    check_u64("(basic-type? \"goalpad\" string)", call_symbol("basic-type?", str, string_type),
              s7.offset + 8);
    check_u64("(basic-type? \"goalpad\" pair)", call_symbol("basic-type?", str, pair_type),
              s7.offset);
  }

  std::printf("\n== calling GOAL functions from the second object file (cross-file) ==\n");
  {
    const u32 a = (u32)jak1::make_string_from_c("precursor");
    const u32 b = (u32)jak1::make_string_from_c("precursor");
    const u32 c = (u32)jak1::make_string_from_c("orb");
    check_u64("(string= \"precursor\" \"precursor\")", call_symbol("string=", a, b), s7.offset + 8);
    check_u64("(string= \"precursor\" \"orb\")", call_symbol("string=", a, c), s7.offset);
    check_s64("(string->int \"1234\")",
              call_symbol("string->int", (u32)jak1::make_string_from_c("1234")), 1234);
    check_s64("(length \"precursor\")",
              call_method(a, *(s7 + jak1_symbols::FIX_SYM_STRING_TYPE), GOAL_LENGTH_METHOD), 9);

    const u32 upper = (u32)jak1::make_string_from_c("jak and daxter");
    call_symbol("string-upcase", upper, upper);
    std::printf("  ok   (string-upcase ...)              = \"%s\"\n", Ptr<String>(upper)->data());
    if (std::strcmp(Ptr<String>(upper)->data(), "JAK AND DAXTER") != 0) {
      fail("string-upcase");
    }
  }

  std::printf("\n== GOAL printing, through the kernel's own format ==\n");
  clear_print();
  call_symbol("printl", (u32)jak1::make_string_from_c("hello from AOT-compiled GOAL"));
  call_symbol("print", (u32)jak1::make_string_from_c("and from (print ...)"));
  {
    // Nothing drains the GOAL print buffer here: that is the listener's job and the listener
    // transport is not part of this kernel. Read it directly instead.
    const char* printed = Ptr<char>(PrintBufArea.offset + sizeof(ListenerMessageHeader)).c();
    std::printf("  GOAL print buffer: \"%s\"\n", printed);
    // the string type's print method is the kernel's own print_string, which quotes its argument
    if (std::strcmp(printed,
                    "\"hello from AOT-compiled GOAL\"\n\"and from (print ...)\"") != 0) {
      fail("GOAL printing did not produce the expected text");
    }
  }

  std::printf("\n== running AOT-compiled GOAL code on a stack inside GOAL memory ==\n");
  {
    // GOAL's own PROCESS_STACK_SIZE is #x6000 on the PC port; 32 KiB is more than fact needs.
    constexpr s32 kStackSize = 32 * 1024;
    const Ptr<u8> stack =
        kmalloc(kglobalheap, kStackSize, KMALLOC_MEMSET | KMALLOC_ALIGN_16, "goal-thread-stack");
    if (!stack.offset) {
      fail("kmalloc(goal-thread-stack)");
    } else {
      // the stack grows down, so what call_goal_on_stack wants is the top of the region, as a
      // native pointer rather than a GOAL pointer
      const u32 stack_top_goal = stack.offset + (u32)kStackSize;
      const u64 stack_top_native = (u64)(uintptr_t)g_ee_main_mem + stack_top_goal;
      std::printf("  stack region: GOAL #x%x - #x%x (%d bytes), native top #x%" PRIx64 "\n",
                  stack.offset, stack_top_goal, kStackSize, stack_top_native);
      check_u64("stack top 16-byte aligned", stack_top_native & 0xf, 0);

      const u8* region = Ptr<u8>(stack.offset).c();
      s32 depth = 0;
      check_u64("stack region zeroed by kmalloc", count_dirty_bytes(region, kStackSize, &depth), 0);

      static const void* const trampoline_functions[1] = {(const void*)&goal_stack_trampoline};
      goal_aot_object_file file = {"goal-stack-check", nullptr, 0, trampoline_functions, 1,
                                   nullptr};
      if (goal_aot_load(&file) != GOAL_KERNEL_CORE_OK) {
        fail("goal_aot_load(goal-stack-check)");
      } else {
        const u32 func = goal_aot_function_object("goal-stack-check", 0);
        const u64 result =
            call_goal_on_stack(Ptr<Function>(func), stack_top_native, s7.offset, g_ee_main_mem);
        check_s64("(fact 10) on the GOAL stack", result, 3628800);
        check_s64("...and as call_goal saw it", g_trampoline_fact, 3628800);

        // the switch really happened: the trampoline's own frame was inside the region
        std::printf("  trampoline frame at native #x%" PRIx64 ", %" PRId64
                    " bytes below the top\n",
                    g_trampoline_frame, (s64)(stack_top_native - g_trampoline_frame));
        if (g_trampoline_frame <= (u64)(uintptr_t)g_ee_main_mem + stack.offset ||
            g_trampoline_frame > stack_top_native) {
          fail("call_goal_on_stack did not switch to the GOAL-memory stack");
        }

        // and the GOAL code left its frames behind in the region kmalloc had zeroed
        const u32 dirty = count_dirty_bytes(region, kStackSize, &depth);
        std::printf("  %u bytes of the region are non-zero afterwards, deepest write %d bytes "
                    "below the top\n",
                    dirty, depth);
        if (!dirty) {
          fail("nothing was written to the GOAL-memory stack");
        }
      }
    }
  }

  std::printf("\n== the native stack survived the switch ==\n");
  check_s64("(fact 10) on the native stack", call_symbol("fact", 10), 3628800);
  check_s64("(+ 3 4) on the native stack", call_symbol("+", 3, 4), 7);

  goal_aot_reset();
  goal_kernel_core_shutdown();

  if (g_failures) {
    std::printf("AOT EXECUTION TEST FAILED (%d failures)\n", g_failures);
    return 1;
  }
  std::printf("AOT EXECUTION TEST PASSED\n");
  return 0;
}
