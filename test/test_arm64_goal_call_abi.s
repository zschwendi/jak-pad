.text

.global _arm64_goal_call_abi_outer
.align 4
_arm64_goal_call_abi_outer:
  stp x29, x30, [sp, #-16]!
  mov x29, sp
  stp x19, x20, [sp, #-16]!
  stp x21, x22, [sp, #-16]!

  mov x19, x0
  mov x20, #0x14
  mov x21, #0x15
  mov x22, #0x16

  ldr x0, [x19, #0]
  ldr x1, [x19, #8]
  mov x2, x19
  ldr x3, [x19, #32]
  ldr x4, [x19, #16]
  ldr x5, [x19, #24]
  bl _call_goal_asm_arm64

  str x0, [x19, #96]
  str x20, [x19, #104]
  str x21, [x19, #112]
  str x22, [x19, #120]

  ldp x21, x22, [sp], #16
  ldp x19, x20, [sp], #16
  ldp x29, x30, [sp], #16
  ret

.global _arm64_goal_call_abi_entry
.align 4
_arm64_goal_call_abi_entry:
  str x0, [x2, #40]
  str x1, [x2, #48]
  str x2, [x2, #56]
  str x20, [x2, #64]
  str x21, [x2, #72]
  str x22, [x2, #80]
  mov x9, sp
  and x9, x9, #0xf
  str x9, [x2, #88]
  eor x0, x0, x1
  ret

.global _arm64_goal_call_false_like_entry
.align 4
_arm64_goal_call_false_like_entry:
  mov x0, x21
  ret
