.section __TEXT,__text,regular,pure_instructions
.p2align 2
.globl _goalpad_aot_load_state_value
.extern _goalpad_aot_jak1_load_state_symbol_offset
_goalpad_aot_load_state_value:
  adrp x16, _goalpad_aot_jak1_load_state_symbol_offset@PAGE
  ldrsw x16, [x16, _goalpad_aot_jak1_load_state_symbol_offset@PAGEOFF]
  add x16, x21, x16
  ldr w0, [x16, x22]
  ret
.subsections_via_symbols
