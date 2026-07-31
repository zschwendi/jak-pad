;; GOALPad: test-only ARM64 helper for game/kernel/core/thread_switch_test.cpp.
;;
;; The whole point of a machine context is that the callee-saved registers survive a non-local
;; transfer, and C gives no way to put a known value in x19-x28 or d8-d15 and read it back. This
;; does: it fills them with sentinels, calls a function that is expected to leave and come back
;; through a GOAL catch frame or a thread suspend, and writes what the registers hold afterwards
;; where the C side can compare them.
;;
;; It is not linked into the runtime library; only the test executable builds it.

.text

;; uint64_t goal_test_saved_registers(uint64_t (*fn)(uint64_t), uint64_t arg, uint64_t* out)
;;
;; out receives 18 values: x19..x28 then d8..d15. The sentinel for register n is
;; #xA5A5000000000000 + n, numbering x19..x28 as 19..28 and d8..d15 as 8..15.
;; Returns whatever fn returned.
.global _goal_test_saved_registers
.align 4
_goal_test_saved_registers:
  stp  x29, x30, [sp, #-16]!
  mov  x29, sp
  ;; these are ours to preserve for our own caller
  stp  x19, x20, [sp, #-16]!
  stp  x21, x22, [sp, #-16]!
  stp  x23, x24, [sp, #-16]!
  stp  x25, x26, [sp, #-16]!
  stp  x27, x28, [sp, #-16]!
  stp  d8,  d9,  [sp, #-16]!
  stp  d10, d11, [sp, #-16]!
  stp  d12, d13, [sp, #-16]!
  stp  d14, d15, [sp, #-16]!
  ;; fn and out go on our own stack, where the callee cannot reach them
  stp  x0, x2, [sp, #-16]!

  movz x9, #0xa5a5, lsl #48
  add  x19, x9, #19
  add  x20, x9, #20
  add  x21, x9, #21
  add  x22, x9, #22
  add  x23, x9, #23
  add  x24, x9, #24
  add  x25, x9, #25
  add  x26, x9, #26
  add  x27, x9, #27
  add  x28, x9, #28
  add  x10, x9, #8
  fmov d8, x10
  add  x10, x9, #9
  fmov d9, x10
  add  x10, x9, #10
  fmov d10, x10
  add  x10, x9, #11
  fmov d11, x10
  add  x10, x9, #12
  fmov d12, x10
  add  x10, x9, #13
  fmov d13, x10
  add  x10, x9, #14
  fmov d14, x10
  add  x10, x9, #15
  fmov d15, x10

  ldr  x9, [sp]        ; fn
  mov  x0, x1          ; arg
  blr  x9

  ldr  x10, [sp, #8]   ; out
  stp  x19, x20, [x10, #0]
  stp  x21, x22, [x10, #16]
  stp  x23, x24, [x10, #32]
  stp  x25, x26, [x10, #48]
  stp  x27, x28, [x10, #64]
  fmov x11, d8
  fmov x12, d9
  stp  x11, x12, [x10, #80]
  fmov x11, d10
  fmov x12, d11
  stp  x11, x12, [x10, #96]
  fmov x11, d12
  fmov x12, d13
  stp  x11, x12, [x10, #112]
  fmov x11, d14
  fmov x12, d15
  stp  x11, x12, [x10, #128]

  add  sp, sp, #16
  ldp  d14, d15, [sp], #16
  ldp  d12, d13, [sp], #16
  ldp  d10, d11, [sp], #16
  ldp  d8,  d9,  [sp], #16
  ldp  x27, x28, [sp], #16
  ldp  x25, x26, [sp], #16
  ldp  x23, x24, [sp], #16
  ldp  x21, x22, [sp], #16
  ldp  x19, x20, [sp], #16
  ldp  x29, x30, [sp], #16
  ret
