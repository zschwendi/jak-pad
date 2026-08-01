/*!
 * @file goal_native_kernel.cpp
 * Native ARM64 implementations of the GOAL kernel routines that manipulate the machine stack.
 *
 * `kernel/gkernel.gc` and `kernel/gstate.gc` write GOAL's cooperative threads, its catch/throw and
 * its state entry as `(declare (asm-func ...))` functions that bind `rsp` and the callee-saved
 * registers with `rlet` and assign to them. The AOT C backend refuses to lower those, because C
 * owns the stack pointer and a lowering that wrote to a local instead would compile and be
 * silently wrong (goalc/aot/CBackend.cpp, check_stack_pointer_use). They get native code here
 * instead, and goalc's native-implementation table puts these entry points in the file's function
 * table so the loader builds ordinary GOAL `function` objects for them - the same objects the
 * symbol definitions, the method tables and the static relocations all end up pointing at.
 *
 * The model is the one docs/aot-stack-model.md settles on: GOAL runs on stacks inside GOAL memory,
 * exactly as it does upstream. What changes on ARM64 is only *how* a machine context is captured:
 *
 *  - x86-64 GOAL pushes five saved registers and remembers `rsp`. A resumable ARM64 context is
 *    x19-x28, x29, x30, sp and d8-d15, which does not fit in a `cpu-thread`'s seven `rreg` slots
 *    or a `catch-frame`'s five, so the context is written to the running GOAL stack instead and
 *    the object records its GOAL address. That address is what those objects' `sp` fields already
 *    mean, and it is inside the region `thread-suspend` copies, so suspend/resume carry it along.
 *
 *  - x86-64 GOAL forges return addresses: it pushes `return-from-thread` and jumps to the user
 *    function. AOT GOAL functions are ordinary native functions reached with `bl`, so there is no
 *    return address to forge. The trampolines below call the user function and then do what the
 *    forged return address would have done. The observable behaviour - including which value comes
 *    back to the kernel - is the same.
 *
 * Nothing here needs writable-executable memory.
 */

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/goal_constants.h"

#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/kernel_game.h"
#include "game/runtime.h"
#include "goalc/aot/goal_c_runtime.h"

namespace {

/*!
 * The machine context goal_thread_arm64.s saves. Every offset is fixed by that file.
 */
struct GoalContext {
  uint64_t saved_gpr[10];  // x19 - x28
  uint64_t fp;             // x29
  uint64_t lr;             // x30
  uint64_t sp;
  uint64_t magic;
  double saved_fpr[8];  // d8 - d15
};

constexpr uint64_t kContextMagic = 0x474f414c43545800ull;  // "GOALCTX"

static_assert(sizeof(GoalContext) == 176, "goal_thread_arm64.s reserves 176 bytes");
static_assert(offsetof(GoalContext, fp) == 80, "goal_thread_arm64.s stores x29 at 80");
static_assert(offsetof(GoalContext, lr) == 88, "goal_thread_arm64.s stores x30 at 88");
static_assert(offsetof(GoalContext, sp) == 96, "goal_thread_arm64.s stores sp at 96");
static_assert(offsetof(GoalContext, magic) == 104, "goal_thread_arm64.s stores the magic at 104");
static_assert(offsetof(GoalContext, saved_fpr) == 112, "goal_thread_arm64.s stores d8 at 112");

/*!
 * GOAL field offsets, relative to a basic's object pointer. A GOAL basic's declared offsets count
 * from its allocation and the object pointer is four bytes into it, so these are the `deftype`
 * offsets in kernel/gkernel-h.gc minus four. `type` therefore sits at -4.
 */
namespace field {
// process (kernel/gkernel-h.gc): the process grew between the games, so these come from the
// per-game seam (kernel_game.h)
inline int process_status() {
  return goal_game_process_offsets().status;
}
inline int process_main_thread() {
  return goal_game_process_offsets().main_thread;
}
inline int process_top_thread() {
  return goal_game_process_offsets().top_thread;
}
inline int process_stack_frame_top() {
  return goal_game_process_offsets().stack_frame_top;
}
// thread / cpu-thread
constexpr int kThreadName = 0;
constexpr int kThreadProcess = 4;
constexpr int kThreadPc = 20;
constexpr int kThreadSp = 24;
constexpr int kThreadStackTop = 28;
constexpr int kThreadStackSize = 32;
constexpr int kCpuThreadStack = 124;  // uint8, dynamic: the backup stack
// stack-frame / catch-frame
constexpr int kStackFrameName = 0;
constexpr int kStackFrameNext = 4;
constexpr int kCatchFrameSp = 8;
constexpr int kCatchFrameRa = 12;
}  // namespace field

/*!
 * How much of the backup stacks the run actually used. `thread-suspend` still aborts when a live
 * stack does not fit - this only records what did fit, so a run can say how close the
 * process-stack-save-size conversion in gkernel-h.gc came to being wrong.
 */
struct StackWatermark {
  int suspends;
  int deepest_used;   // the largest live stack any suspend had to copy
  int deepest_size;
  uint32_t deepest_name;
  int fullest_used;   // the suspend that came closest to filling its buffer
  int fullest_size;
  uint32_t fullest_name;
};

StackWatermark g_stack_watermark = {0, 0, 0, 0, 0, 1, 0};

[[noreturn]] void fail(const char* format, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void fail(const char* format, ...) {
  std::fputs("\nGOAL native kernel: ", stderr);
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fputs("\n", stderr);
  std::fflush(stderr);
  std::abort();
}

uint8_t* mem() {
  if (!g_ee_main_mem) {
    fail("GOAL memory is not mapped");
  }
  return g_ee_main_mem;
}

uint32_t load32(uint32_t goal_addr, int offset) {
  uint32_t out;
  std::memcpy(&out, mem() + goal_addr + offset, 4);
  return out;
}

void store32(uint32_t goal_addr, int offset, uint32_t value) {
  std::memcpy(mem() + goal_addr + offset, &value, 4);
}

uint64_t load64(uint32_t goal_addr, int offset) {
  uint64_t out;
  std::memcpy(&out, mem() + goal_addr + offset, 8);
  return out;
}

/*! GOAL address of a native pointer, checked: it has to be inside EE main memory to be one. */
uint32_t goal_address_of(const void* native, const char* what) {
  const uintptr_t base = (uintptr_t)mem();
  const uintptr_t addr = (uintptr_t)native;
  if (addr < base || addr - base >= EE_MAIN_MEM_SIZE) {
    fail("%s is at native #%lx, which is not inside GOAL memory. GOAL threads and catch frames "
         "only work on stacks inside GOAL memory; see docs/aot-stack-model.md.",
         what, (unsigned long)addr);
  }
  return (uint32_t)(addr - base);
}

/*!
 * These intern rather than look up, because *kernel-sp* has no other reader left: GOAL only ever
 * touched it from the routines that now live here, so nothing else puts it in the symbol table.
 */
uint32_t symbol_value(const char* name) {
  return goal_game_symbol_value(name);
}

void set_symbol_value(const char* name, uint32_t value) {
  goal_game_set_symbol_value(name, value);
}

/*! GOAL address of a quoted symbol, which is what a symbol-valued field holds. */
uint32_t symbol_object(const char* name) {
  return goal_game_intern(name);
}

using GoalFunction = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/*!
 * Call a GOAL function object. On ARM64 the object holds the 64-bit native entry point rather than
 * machine code, the same representation call_goal and every AOT call site use.
 */
GoalFunction goal_function(uint32_t object, const char* what) {
  if (!object) {
    fail("%s is 0: no function object", what);
  }
  void* entry = nullptr;
  std::memcpy(&entry, mem() + object, sizeof(entry));
  if (!entry) {
    fail("%s (GOAL #x%x) has no native entry point", what, object);
  }
  return (GoalFunction)entry;
}

/*! The name of a GOAL symbol, or "" if the address does not look like one. */
const char* goal_thread_stack_watermark_name(uint32_t symbol) {
  return goal_game_symbol_name(symbol);
}

/*! The name of the GOAL type of a basic, for a diagnostic. "" when it does not look like one. */
const char* type_name_of(uint32_t basic) {
  if (!basic || basic < 4 || basic >= EE_MAIN_MEM_SIZE) {
    return "";
  }
  const uint32_t type = load32(basic, -4);
  if (!type || type >= EE_MAIN_MEM_SIZE) {
    return "";
  }
  return goal_thread_stack_watermark_name(load32(type, 0));
}

/*! What a GOAL object calls itself: a symbol's or string's text, otherwise its type's name. */
const char* goal_name_of(uint32_t basic) {
  const char* type = type_name_of(basic);
  if (!std::strcmp(type, "string")) {
    return (const char*)(mem() + basic + 4);
  }
  if (!std::strcmp(type, "symbol")) {
    return goal_thread_stack_watermark_name(basic);
  }
  return type;
}

/*!
 * Who a thread is, for the failure messages below: its type, its name, and the process it belongs
 * to. A thread whose type is not `cpu-thread` is the usual reason one of these fires.
 */
const char* describe_thread(uint32_t thread) {
  static char buffer[256];
  const uint32_t process = load32(thread, field::kThreadProcess);
  std::snprintf(buffer, sizeof(buffer), "the %s '%s' at GOAL #x%x, of the %s '%s' at #x%x",
                type_name_of(thread), goal_name_of(load32(thread, 0)), thread,
                type_name_of(process), goal_name_of(load32(process, 0)), process);
  return buffer;
}

const GoalContext* checked_context(uint32_t goal_addr, const char* what) {
  if (!goal_addr || goal_addr + sizeof(GoalContext) > EE_MAIN_MEM_SIZE) {
    fail("%s points at GOAL #x%x, which is not a stack address", what, goal_addr);
  }
  const auto* ctx = (const GoalContext*)(mem() + goal_addr);
  if (ctx->magic != kContextMagic) {
    fail("%s points at GOAL #x%x, which does not hold a saved machine context (magic #%llx)", what,
         goal_addr, (unsigned long long)ctx->magic);
  }
  return ctx;
}

}  // namespace

extern "C" {

// goal_thread_arm64.s
uint64_t goal_context_save_and_call(void (*fn)(GoalContext*, uint64_t), uint64_t arg);
[[noreturn]] void goal_context_restore(const GoalContext* ctx, uint64_t value);
uint64_t goal_call_on_stack_arm64(void* new_sp,
                                  void* fn,
                                  uint64_t a0,
                                  uint64_t a1,
                                  uint64_t a2,
                                  uint64_t a3,
                                  uint64_t a4,
                                  uint64_t a5);
uint64_t goal_read_stack_pointer(void);

[[noreturn]] void goal_context_fn_returned(void) {
  fail("a machine-context body returned; it is supposed to transfer control elsewhere");
}

}  // extern "C"

namespace {

/*!
 * Go back to the kernel, the way `return-from-thread` does: `*kernel-sp*` names the context the
 * kernel saved before it handed control to a thread, and `value` is what the kernel's call returns.
 */
[[noreturn]] void return_to_kernel(uint64_t value) {
  const uint32_t kernel_sp = symbol_value("*kernel-sp*");
  if (!kernel_sp) {
    fail("*kernel-sp* is 0: GOAL tried to return to the kernel without the kernel having entered a "
         "thread. reset-and-call or thread-resume has to run first.");
  }
  goal_context_restore(checked_context(kernel_sp, "*kernel-sp*"), value);
}

uint32_t goal_native_stack_top(uint32_t thread) {
  return load32(thread, field::kThreadStackTop);
}

void* native_stack_pointer(uint32_t goal_addr) {
  if (goal_addr & 0xf) {
    fail("GOAL stack address #x%x is not 16-byte aligned, which ARM64 requires", goal_addr);
  }
  return mem() + goal_addr;
}

// --------------------------------------------------------------------------------------------
// reset-and-call
// --------------------------------------------------------------------------------------------

struct ResetAndCallArgs {
  uint32_t thread;
  uint32_t func;
};

/*!
 * Runs at the top of the thread's stack. Standing in for the `return-from-thread` address that the
 * x86-64 version pushes before jumping to the user function: when the function returns, go back to
 * the kernel with its value.
 */
[[noreturn]] void reset_and_call_trampoline(uint64_t func) {
  const uint64_t result = goal_function((uint32_t)func, "reset-and-call's function")(0, 0, 0, 0, 0, 0);
  return_to_kernel(result);
}

void reset_and_call_body(GoalContext* ctx, uint64_t argp) {
  const auto* args = (const ResetAndCallArgs*)(uintptr_t)argp;
  set_symbol_value("*kernel-sp*", goal_address_of(ctx, "the kernel's saved context"));
  goal_call_on_stack_arm64(native_stack_pointer(goal_native_stack_top(args->thread)),
                           (void*)&reset_and_call_trampoline, args->func, 0, 0, 0, 0, 0);
  fail("reset-and-call's trampoline returned");
}

// --------------------------------------------------------------------------------------------
// thread-suspend / thread-resume
// --------------------------------------------------------------------------------------------

void thread_suspend_body(GoalContext* ctx, uint64_t thread_u) {
  const uint32_t thread = (uint32_t)thread_u;
  const uint32_t sp = goal_address_of(ctx, "a suspending thread's stack pointer");
  const uint32_t stack_top = load32(thread, field::kThreadStackTop);
  const int32_t used = (int32_t)(stack_top - sp);
  const int32_t backup_size = (int32_t)load32(thread, field::kThreadStackSize);

  g_stack_watermark.suspends++;
  if (used > g_stack_watermark.deepest_used) {
    g_stack_watermark.deepest_used = used;
    g_stack_watermark.deepest_size = backup_size;
    g_stack_watermark.deepest_name = load32(thread, field::kThreadName);
  }
  if (backup_size > 0 && used * g_stack_watermark.fullest_size > backup_size * g_stack_watermark.fullest_used) {
    g_stack_watermark.fullest_used = used;
    g_stack_watermark.fullest_size = backup_size;
    g_stack_watermark.fullest_name = load32(thread, field::kThreadName);
  }

  // GOAL's own check, which is a (break) there. Copying more than the backup buffer holds would
  // walk off the end of the thread object and into the process heap behind it.
  if (used > backup_size) {
    fail("%s used %d bytes of stack but its backup buffer is only %d. Raise the thread's "
         "stack-size; see docs/aot-stack-model.md.",
         describe_thread(thread), used, backup_size);
  }
  if (used < 0) {
    fail("a suspending thread's stack pointer (GOAL #x%x) is above its stack top (#x%x)", sp,
         stack_top);
  }

  // The context is the resume point, and the thread's `pc` is where GOAL records that. A native
  // return address does not fit in the 32-bit field, so it names the context instead; the address
  // is the same as `sp` and thread-resume checks that.
  store32(thread, field::kThreadPc, sp);
  store32(thread, field::kThreadSp, sp);

  const uint32_t process = load32(thread, field::kThreadProcess);
  store32(process, field::process_status(), symbol_object("suspended"));

  const uint32_t backup_end = thread + (uint32_t)field::kCpuThreadStack + (uint32_t)backup_size;
  std::memcpy(mem() + backup_end - used, mem() + sp, (size_t)used);

  g_goal_current_process = 0;
  return_to_kernel(0);
}

[[noreturn]] void bootstrap_trampoline(uint64_t bootstrap) {
  goal_function((uint32_t)bootstrap, "set-to-run-bootstrap")(0, 0, 0, 0, 0, 0);
  fail("set-to-run-bootstrap returned; it ends in return-from-thread-dead and must not");
}

void thread_resume_body(GoalContext* ctx, uint64_t thread_u) {
  const uint32_t thread = (uint32_t)thread_u;
  set_symbol_value("*kernel-sp*", goal_address_of(ctx, "the kernel's saved context"));

  const uint32_t sp = load32(thread, field::kThreadSp);
  const uint32_t stack_top = load32(thread, field::kThreadStackTop);
  const int32_t used = (int32_t)(stack_top - sp);
  const int32_t backup_size = (int32_t)load32(thread, field::kThreadStackSize);
  if (used < 0 || used > backup_size) {
    fail("thread-resume was given %s, whose stack pointer (GOAL #x%x) does not fit under its "
         "stack top (#x%x) and backup size (%d)",
         describe_thread(thread), sp, stack_top, backup_size);
  }
  const uint32_t backup_end = thread + (uint32_t)field::kCpuThreadStack + (uint32_t)backup_size;
  std::memcpy(mem() + sp, mem() + backup_end - used, (size_t)used);

  const uint32_t process = load32(thread, field::kThreadProcess);
  store32(process, field::process_top_thread(), thread);
  store32(process, field::process_status(), symbol_object("running"));
  g_goal_current_process = process;

  const uint32_t pc = load32(thread, field::kThreadPc);
  if (pc == symbol_value("set-to-run-bootstrap")) {
    // set-to-run left the thread with an empty stack and the function plus its arguments in the
    // rreg slots. It has never run, so there is no context to restore.
    goal_call_on_stack_arm64(native_stack_pointer(sp), (void*)&bootstrap_trampoline, pc, 0, 0, 0, 0,
                             0);
    fail("thread-resume's bootstrap trampoline returned");
  }
  if (pc != sp) {
    fail("thread-resume was given a thread whose pc (GOAL #x%x) is neither set-to-run-bootstrap nor "
         "its own saved context at #x%x",
         pc, sp);
  }
  goal_context_restore(checked_context(sp, "a suspended thread's context"), 0);
}

// --------------------------------------------------------------------------------------------
// catch-frame
// --------------------------------------------------------------------------------------------

struct CatchFrameArgs {
  uint32_t allocation;
  uint32_t type;
  uint32_t name;
  uint32_t func;
  uint32_t param_block;
};

void catch_frame_body(GoalContext* ctx, uint64_t argp) {
  const auto* args = (const CatchFrameArgs*)(uintptr_t)argp;
  const uint32_t frame = args->allocation + BASIC_OFFSET;

  store32(frame, -BASIC_OFFSET, args->type);
  store32(frame, field::kStackFrameName, args->name);
  // Where a throw puts the machine back. On x86-64 this is a stack pointer plus a return address;
  // on ARM64 the return address is a __TEXT address that no 32-bit GOAL field can hold, so the
  // whole context lives on the stack and `sp` names it. `ra` has nothing left to say.
  store32(frame, field::kCatchFrameSp, goal_address_of(ctx, "a catch frame's saved context"));
  store32(frame, field::kCatchFrameRa, 0);

  const uint32_t process = (uint32_t)g_goal_current_process;
  store32(frame, field::kStackFrameNext, load32(process, field::process_stack_frame_top()));
  store32(process, field::process_stack_frame_top(), frame);

  const uint32_t block = args->param_block;
  const uint64_t result = goal_function(args->func, "a catch frame's function")(
      load64(block, 0), load64(block, 8), load64(block, 16), load64(block, 24), load64(block, 32),
      load64(block, 40));

  // Returned rather than thrown: pop the frame off whatever process is current now, exactly as the
  // GOAL version does, and hand the result back through the same path a throw would use.
  const uint32_t current = (uint32_t)g_goal_current_process;
  const uint32_t top = load32(current, field::process_stack_frame_top());
  store32(current, field::process_stack_frame_top(), load32(top, field::kStackFrameNext));
  goal_context_restore(ctx, result);
}

// --------------------------------------------------------------------------------------------
// enter-state's tail
// --------------------------------------------------------------------------------------------

/*!
 * Runs at the top of the main thread's stack, after enter-state has reset it. The state's code
 * function returning means the process is done, which is what `return-from-thread-dead` handles.
 *
 * This has to be a trampoline rather than a plain call-and-return: resetting the stack throws away
 * the frame of whatever called it, so there is nothing on the old stack left to return to.
 */
[[noreturn]] void state_code_trampoline(uint64_t code,
                                        uint64_t a0,
                                        uint64_t a1,
                                        uint64_t a2,
                                        uint64_t a3) {
  goal_function((uint32_t)code, "a state's code")(a0, a1, a2, a3, 0, 0);
  goal_function(symbol_value("return-from-thread-dead"), "return-from-thread-dead")(0, 0, 0, 0, 0,
                                                                                    0);
  fail("return-from-thread-dead returned");
}

}  // namespace

// ----------------------------------------------------------------------------------------------
// The GOAL-visible entry points. goalc's native-implementation table (goalc/aot/CBackend.cpp)
// names these, so the loader gives each one a real GOAL function object.
// ----------------------------------------------------------------------------------------------

extern "C" {

/*! (defun return-from-thread () ...) - context switch back to the saved kernel context. */
uint64_t goal_native_return_from_thread(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
  return_to_kernel(0);
}

/*! (defun reset-and-call ((this thread) (func function)) ...) */
uint64_t goal_native_reset_and_call(uint64_t this_thread,
                                    uint64_t func,
                                    uint64_t,
                                    uint64_t,
                                    uint64_t,
                                    uint64_t) {
  const uint32_t thread = (uint32_t)this_thread;
  const uint32_t process = load32(thread, field::kThreadProcess);
  store32(process, field::process_status(), symbol_object("running"));
  store32(process, field::process_top_thread(), thread);
  g_goal_current_process = process;

  ResetAndCallArgs args = {thread, (uint32_t)func};
  return goal_context_save_and_call(&reset_and_call_body, (uint64_t)(uintptr_t)&args);
}

/*!
 * (defmethod thread-suspend ((unused cpu-thread)) ...)
 *
 * The (suspend) macro puts the thread in the process register and calls the thread's suspend hook
 * with no useful argument, so the thread comes from there and not from the argument.
 */
uint64_t goal_native_thread_suspend(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
  const uint64_t thread = g_goal_current_process;
  if (!thread) {
    fail("thread-suspend ran with no thread in the process register");
  }
  goal_context_save_and_call(&thread_suspend_body, thread);
  // resumed: the process register holds our process again, set by thread-resume
  return 0;
}

/*! (defmethod thread-resume ((thread-to-resume cpu-thread)) ...) */
uint64_t goal_native_thread_resume(uint64_t thread,
                                   uint64_t,
                                   uint64_t,
                                   uint64_t,
                                   uint64_t,
                                   uint64_t) {
  if (!(uint32_t)thread) {
    fail("thread-resume was given no thread");
  }
  goal_context_save_and_call(&thread_resume_body, thread);
  return 0;
}

/*!
 * (defmethod new catch-frame ((allocation symbol) (type-to-make type) (name symbol)
 *                             (func function) (param-block (pointer uint64))) ...)
 */
uint64_t goal_native_catch_frame_new(uint64_t allocation,
                                     uint64_t type,
                                     uint64_t name,
                                     uint64_t func,
                                     uint64_t param_block,
                                     uint64_t) {
  CatchFrameArgs args = {(uint32_t)allocation, (uint32_t)type, (uint32_t)name, (uint32_t)func,
                         (uint32_t)param_block};
  return goal_context_save_and_call(&catch_frame_body, (uint64_t)(uintptr_t)&args);
}

/*! (defun throw-dispatch ((this catch-frame) value) ...) */
uint64_t goal_native_throw_dispatch(uint64_t this_frame,
                                    uint64_t value,
                                    uint64_t,
                                    uint64_t,
                                    uint64_t,
                                    uint64_t) {
  const uint32_t frame = (uint32_t)this_frame;
  const uint32_t process = (uint32_t)g_goal_current_process;
  store32(process, field::process_stack_frame_top(), load32(frame, field::kStackFrameNext));
  goal_context_restore(checked_context(load32(frame, field::kCatchFrameSp), "a catch frame's sp"),
                       value);
}

/*!
 * (defun enter-state-run-code ((code function) (arg0 object) ... (arg3 object)) ...)
 *
 * Reset the current process's main thread stack to its top and run a state's code function there,
 * with return-from-thread-dead standing in for the return trampoline. Never returns.
 */
uint64_t goal_native_enter_state_run_code(uint64_t code,
                                          uint64_t a0,
                                          uint64_t a1,
                                          uint64_t a2,
                                          uint64_t a3,
                                          uint64_t) {
  const uint32_t process = (uint32_t)g_goal_current_process;
  const uint32_t main_thread = load32(process, field::process_main_thread());
  const uint32_t stack_top = goal_native_stack_top(main_thread);
  goal_call_on_stack_arm64(native_stack_pointer(stack_top), (void*)&state_code_trampoline, code, a0,
                           a1, a2, a3, 0);
  fail("enter-state's code trampoline returned");
}

/*!
 * What the run did to its backup stacks. `thread-suspend` still aborts when a live stack does not
 * fit; this reports what did fit, so a run can say how close the process-stack-save-size
 * conversion in gkernel-h.gc came to being wrong. The names are GOAL symbol addresses.
 */
void goal_thread_stack_watermark(goal_thread_stack_watermark_report* out) {
  out->suspends = g_stack_watermark.suspends;
  out->deepest_used = g_stack_watermark.deepest_used;
  out->deepest_size = g_stack_watermark.deepest_size;
  out->deepest_name = goal_thread_stack_watermark_name(g_stack_watermark.deepest_name);
  out->fullest_used = g_stack_watermark.fullest_used;
  out->fullest_size = g_stack_watermark.fullest_size;
  out->fullest_name = goal_thread_stack_watermark_name(g_stack_watermark.fullest_name);
}

}  // extern "C"
