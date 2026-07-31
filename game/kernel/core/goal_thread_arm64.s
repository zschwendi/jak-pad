;; GOALPad: the ARM64 machine-level primitives GOAL's cooperative threads and catch frames are
;; built out of.
;;
;; goalc's x86-64 backend lets GOAL write these by hand, with rlet bindings of rsp and the
;; callee-saved registers (kernel/gkernel.gc, kernel/gstate.gc). The AOT C backend cannot express
;; assigning to the stack pointer, so those functions get native implementations instead; see
;; docs/aot-stack-model.md. game/kernel/core/goal_native_kernel.cpp holds the GOAL-visible half of
;; them and this file holds the three things C cannot do:
;;
;;   goal_context_save_and_call   capture a resumable machine context on the current stack
;;   goal_context_restore         go back to one, with a value, from anywhere
;;   goal_call_on_stack_arm64     run a function on a different stack
;;
;; The saved set is exactly what the Apple ARM64 ABI makes a function's own business to preserve:
;; x19-x28, x29, x30, sp and d8-d15. That is the ARM64 equivalent of the rbx/rbp/r10/r11/r12 plus
;; xmm8-xmm15 set the x86-64 GOAL routines save.
;;
;; - https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms
;; - s16-s31 (d8-d15, q4-q7) must be preserved by a callee; s0-s15 and d16-d31 need not be.

.text

;; struct goal_arm64_context, 176 bytes, 16-byte aligned. Kept in sync with
;; game/kernel/core/goal_native_kernel.cpp, which static_asserts every offset below.
;;
;;    0  x19 x20
;;   16  x21 x22
;;   32  x23 x24
;;   48  x25 x26
;;   64  x27 x28
;;   80  x29 (frame pointer)   x30 (return address into our caller)
;;   96  sp (our caller's stack pointer)   magic
;;  112  d8  d9
;;  128  d10 d11
;;  144  d12 d13
;;  160  d14 d15

;; uint64_t goal_context_save_and_call(void (*fn)(goal_arm64_context*, uint64_t), uint64_t arg)
;;
;; Pushes a context onto the current stack and calls fn with its address. fn is not expected to
;; return: it hands control somewhere else (a thread's stack, the kernel, a GOAL function). When
;; someone later calls goal_context_restore on that context, execution resumes here, the context is
;; popped, and the value they passed is returned to *this function's caller*.
;;
;; This is setjmp/longjmp with the buffer placed at a known address - the lowest live address of
;; the current stack - so that thread-suspend can record it as the thread's GOAL stack pointer and
;; copy everything above it into the thread's backup buffer.
.global _goal_context_save_and_call
.align 4
_goal_context_save_and_call:
  mov  x9, sp
  sub  sp, sp, #176
  stp  x19, x20, [sp, #0]
  stp  x21, x22, [sp, #16]
  stp  x23, x24, [sp, #32]
  stp  x25, x26, [sp, #48]
  stp  x27, x28, [sp, #64]
  stp  x29, x30, [sp, #80]
  ;; magic 0x474f414c43545800, "GOALCTX" - a resumable context and not whatever else was on the
  ;; stack. Every restore path checks it.
  mov  x10, #0x5800
  movk x10, #0x4354, lsl #16
  movk x10, #0x414c, lsl #32
  movk x10, #0x474f, lsl #48
  stp  x9, x10, [sp, #96]
  stp  d8,  d9,  [sp, #112]
  stp  d10, d11, [sp, #128]
  stp  d12, d13, [sp, #144]
  stp  d14, d15, [sp, #160]

  mov  x9, x0        ; fn
  mov  x0, sp        ; the context
                     ; x1 is already arg
  blr  x9
  ;; fn returned. It was documented not to, so something transferred control wrongly rather than
  ;; not at all: say so instead of running on with a stack that no longer means anything.
  bl   _goal_context_fn_returned
  brk  #0

;; void goal_context_restore(goal_arm64_context* ctx, uint64_t value)
;;
;; Makes the goal_context_save_and_call that produced ctx return `value` to its caller. Never
;; returns. Everything is loaded out of ctx before x0 and sp are touched, because ctx normally
;; points into the stack we are switching to.
.global _goal_context_restore
.align 4
_goal_context_restore:
  ldp  x19, x20, [x0, #0]
  ldp  x21, x22, [x0, #16]
  ldp  x23, x24, [x0, #32]
  ldp  x25, x26, [x0, #48]
  ldp  x27, x28, [x0, #64]
  ldp  x29, x30, [x0, #80]
  ldp  d8,  d9,  [x0, #112]
  ldp  d10, d11, [x0, #128]
  ldp  d12, d13, [x0, #144]
  ldp  d14, d15, [x0, #160]
  ldr  x9, [x0, #96]
  mov  x0, x1
  mov  sp, x9
  ret

;; uint64_t goal_call_on_stack_arm64(void* new_sp, void* fn, uint64_t a0, uint64_t a1, uint64_t a2,
;;                                   uint64_t a3, uint64_t a4, uint64_t a5)
;;
;; Run fn(a0..a5) with the stack pointer set to new_sp, then come back. new_sp must be 16-byte
;; aligned.
;;
;; The old stack pointer is stashed on the NEW stack, after the switch. Saving it on the old stack
;; and reading it back after the call would read whatever the callee left at the top of the new
;; stack instead - the bug that was fixed in _call_goal_on_stack_asm_arm64.
.global _goal_call_on_stack_arm64
.align 4
_goal_call_on_stack_arm64:
  stp  x29, x30, [sp, #-16]!
  mov  x29, sp
  mov  x9, sp
  mov  sp, x0
  str  x9, [sp, #-16]!
  mov  x10, x1
  mov  x0, x2
  mov  x1, x3
  mov  x2, x4
  mov  x3, x5
  mov  x4, x6
  mov  x5, x7
  blr  x10
  ldr  x9, [sp], #16
  mov  sp, x9
  ldp  x29, x30, [sp], #16
  ret

;; uint64_t goal_read_stack_pointer(void)
;; The current machine stack pointer, for checks that GOAL code is running on a GOAL-memory stack.
.global _goal_read_stack_pointer
.align 4
_goal_read_stack_pointer:
  mov  x0, sp
  ret
