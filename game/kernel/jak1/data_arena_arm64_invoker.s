.text

; Apple ARM64 ABI:
;   x0: process, w1: s7, x2: data arena, x3: typed native entry, w4: object
; The generated GOAL entry receives X20/X21/X22 and its ordinary arity-one argument in X0.
.global _jak1_data_arena_call_native_method1_arm64
.align 4
_jak1_data_arena_call_native_method1_arm64:
  stp x29, x30, [sp, #-16]!
  mov x29, sp
  stp x19, x20, [sp, #-16]!
  stp x21, x22, [sp, #-16]!

  mov x20, x0
  uxtw x21, w1
  mov x22, x2
  mov x19, x3
  uxtw x0, w4
  blr x19

  ldp x21, x22, [sp], #16
  ldp x19, x20, [sp], #16
  ldp x29, x30, [sp], #16
  ret
