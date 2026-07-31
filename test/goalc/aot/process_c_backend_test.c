/*!
 * @file process_c_backend_test.c
 * Runs the C that goalc's AOT C backend emitted for test/goalc/aot/process_test.gc and checks that
 * the current-process register behaves the way it does on x86-64.
 *
 * What this has to prove, beyond "it compiled":
 *  - a behavior's `self` is the current process, not an argument the caller passes,
 *  - a callee that binds r13 with rlet sees the same process the behavior does,
 *  - an ordinary call in between does not disturb it,
 *  - assigning it, the way the kernel does when it dispatches a thread, is visible immediately,
 *    in callees, and after the assigning function returns (x86-64 never restores r13),
 *  - GOAL's own save/restore idiom still unwinds correctly, including nested,
 *  - and reading `self` is a load, so a callee that switches process changes what `self` means in
 *    the frame that called it.
 */

#include "proc_generated.h"

#include "test/goalc/aot/goal_c_test_loader.h"

#define PROCESS_A 0x600000ull
#define PROCESS_B 0x600100ull
#define PROCESS_C 0x600200ull

#define TAG_A 7
#define TAG_B 9
#define TAG_C 5

static int g_failures = 0;

static void check(const char* what, uint64_t got, uint64_t expected) {
  if (got != expected) {
    printf("FAIL %s: got %llu (0x%llx), expected %llu (0x%llx)\n", what, (unsigned long long)got,
           (unsigned long long)got, (unsigned long long)expected, (unsigned long long)expected);
    g_failures++;
  }
}

/*!
 * A behavior takes only the arguments its GOAL source declares. If `self` had become a C
 * parameter these initializers would not compile.
 */
static uint64_t (*const s_self_tag)(void) = &goal_proc_aot_test_self_tag;
static uint64_t (*const s_self_and_callee)(void) = &goal_proc_aot_test_self_and_callee;
static uint64_t (*const s_args)(uint64_t, uint64_t, uint64_t) = &goal_proc_aot_test_args;

/*! Point a GOAL symbol at one of the functions we generated, so calls through it work. */
static void bind_function(const char* goal_name, const void* native) {
  for (int i = 0; i < goal_proc_function_count; i++) {
    if (goal_proc_functions[i] == native) {
      *goal_symbol_slot(goal_name) = (int32_t)goal_function_addr("proc", i);
      return;
    }
  }
  printf("FAIL %s is not in the emitted function table\n", goal_name);
  g_failures++;
}

static void reset_processes(void) {
  goal_proc_aot_test_init(PROCESS_A, TAG_A);
  goal_proc_aot_test_init(PROCESS_B, TAG_B);
  goal_proc_aot_test_init(PROCESS_C, TAG_C);
}

int main(void) {
  goal_test_loader_init();
  goal_test_load_functions(goal_proc_functions, goal_proc_function_count);
  goal_test_load_statics(goal_proc_statics, goal_proc_static_count);
  goal_proc_link();

  bind_function("aot-test-leaf", (const void*)&goal_proc_aot_test_leaf);
  bind_function("aot-test-middle", (const void*)&goal_proc_aot_test_middle);
  bind_function("aot-test-switch", (const void*)&goal_proc_aot_test_switch);
  bind_function("aot-test-call-with", (const void*)&goal_proc_aot_test_call_with);

  /* an ordinary function reading the current process with (rlet ((pp :reg r13 ...))) */
  reset_processes();
  g_goal_current_process = PROCESS_A;
  check("leaf reads the current process", goal_proc_aot_test_leaf(), TAG_A);
  check("leaf wrote through the current process",
        goal_proc_aot_test_visits(PROCESS_A), 1);
  check("leaf left the current process alone", g_goal_current_process, PROCESS_A);

  /* a behavior reading self */
  reset_processes();
  g_goal_current_process = PROCESS_A;
  check("self is the current process", s_self_tag(), TAG_A);
  g_goal_current_process = PROCESS_B;
  check("self follows the current process", s_self_tag(), TAG_B);

  /* self and a callee two frames down must be the same process */
  reset_processes();
  g_goal_current_process = PROCESS_A;
  check("self agrees with a nested callee", s_self_and_callee(), 1000 * TAG_A + TAG_A);
  check("the nested callee ran against the same process",
        goal_proc_aot_test_visits(PROCESS_A), 1);
  check("nesting left the current process alone", g_goal_current_process, PROCESS_A);

  /* self does not occupy an argument slot */
  reset_processes();
  g_goal_current_process = PROCESS_A;
  check("behavior arguments are not shifted by self", s_args(1, 2, 3),
        1000000 * TAG_A + 10203);

  /* a kernel-style dispatch: the new process is current in the callee and stays current */
  reset_processes();
  g_goal_current_process = PROCESS_A;
  check("a switched-to process is current in the callee", goal_proc_aot_test_switch(PROCESS_B),
        TAG_B);
  check("the switch is not undone on return", g_goal_current_process, PROCESS_B);
  check("only the switched-to process was visited", goal_proc_aot_test_visits(PROCESS_B), 1);
  check("the previous process was not visited", goal_proc_aot_test_visits(PROCESS_A), 0);

  /* GOAL's own save/restore, which is what enter-state does */
  reset_processes();
  g_goal_current_process = PROCESS_A;
  check("a saved/restored call still runs in the new process",
        goal_proc_aot_test_call_with(PROCESS_B), TAG_B);
  check("the caller's process is restored", g_goal_current_process, PROCESS_A);

  /* nested save/restore unwinds in order */
  reset_processes();
  g_goal_current_process = PROCESS_C;
  check("nested switches unwind in order", goal_proc_aot_test_nested(PROCESS_A, PROCESS_B),
        100 * TAG_B + TAG_A);
  check("the outermost process is restored", g_goal_current_process, PROCESS_C);
  check("the inner process was visited once", goal_proc_aot_test_visits(PROCESS_B), 1);
  check("the middle process was visited once", goal_proc_aot_test_visits(PROCESS_A), 1);

  /* self is a load: a callee that switches process changes what self means in its caller */
  reset_processes();
  g_goal_current_process = PROCESS_A;
  check("self is re-read after a callee switches process",
        goal_proc_aot_test_self_after_callee_switch(PROCESS_B), 1000 * TAG_A + TAG_B);
  check("the callee's switch is still in effect", g_goal_current_process, PROCESS_B);

  if (g_failures) {
    printf("%d process-register check(s) failed\n", g_failures);
    return 1;
  }
  printf("all AOT C backend process-register checks passed\n");
  return 0;
}
