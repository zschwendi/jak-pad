file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(INPUT
    "${PROJECT_ROOT}/test/goalc/source_templates/arm64-aot/full-false-func-from-jak1-gcommon.gc")
set(ASSEMBLY_ONLY "${OUTPUT_DIR}/assembly-only.s")
set(PAIRED_ASSEMBLY "${OUTPUT_DIR}/paired.s")
set(EXPORTS "${OUTPUT_DIR}/paired.exports.inc")
set(IDENTITY_INPUT
    "${PROJECT_ROOT}/test/goalc/source_templates/arm64-aot/full-identity-from-jak1-gcommon.gc")
set(IDENTITY_ASSEMBLY_ONLY "${OUTPUT_DIR}/identity-assembly-only.s")
set(IDENTITY_PAIRED_ASSEMBLY "${OUTPUT_DIR}/identity-paired.s")
set(IDENTITY_EXPORTS "${OUTPUT_DIR}/identity-paired.exports.inc")
set(TRUE_INPUT
    "${PROJECT_ROOT}/test/goalc/source_templates/arm64-aot/full-true-func-from-jak1-gcommon.gc")
set(TRUE_ASSEMBLY_ONLY "${OUTPUT_DIR}/true-assembly-only.s")
set(TRUE_PAIRED_ASSEMBLY "${OUTPUT_DIR}/true-paired.s")
set(TRUE_EXPORTS "${OUTPUT_DIR}/true-paired.exports.inc")
set(LOGNOT_INPUT
    "${PROJECT_ROOT}/test/goalc/source_templates/arm64-aot/full-lognot-from-jak1-gcommon.gc")
set(LOGNOT_ASSEMBLY_ONLY "${OUTPUT_DIR}/lognot-assembly-only.s")
set(LOGNOT_PAIRED_ASSEMBLY "${OUTPUT_DIR}/lognot-paired.s")
set(LOGNOT_EXPORTS "${OUTPUT_DIR}/lognot-paired.exports.inc")
set(GLST_NODE_NAME_INPUT
    "${PROJECT_ROOT}/test/goalc/source_templates/arm64-aot/full-glst-node-name-from-jak1-glist-h.gc")
set(GLST_NODE_NAME_ASSEMBLY_ONLY "${OUTPUT_DIR}/glst-node-name-assembly-only.s")
set(GLST_NODE_NAME_PAIRED_ASSEMBLY "${OUTPUT_DIR}/glst-node-name-paired.s")
set(GLST_NODE_NAME_EXPORTS "${OUTPUT_DIR}/glst-node-name-paired.exports.inc")
set(LEVEL_GROUP_LOAD_COMMANDS_SET_INPUT
    "${PROJECT_ROOT}/test/goalc/source_templates/arm64-aot/full-level-group-load-commands-set-from-jak1-level.gc")
set(LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY_ONLY
    "${OUTPUT_DIR}/level-group-load-commands-set-assembly-only.s")
set(LEVEL_GROUP_LOAD_COMMANDS_SET_PAIRED_ASSEMBLY
    "${OUTPUT_DIR}/level-group-load-commands-set-paired.s")
set(LEVEL_GROUP_LOAD_COMMANDS_SET_EXPORTS
    "${OUTPUT_DIR}/level-group-load-commands-set-paired.exports.inc")
set(WANT_VIS_INPUT
    "${PROJECT_ROOT}/test/goalc/source_templates/arm64-aot/full-want-vis-from-jak1-load-boundary.gc")
set(WANT_VIS_ASSEMBLY_ONLY "${OUTPUT_DIR}/want-vis-assembly-only.s")
set(WANT_VIS_PAIRED_ASSEMBLY "${OUTPUT_DIR}/want-vis-paired.s")
set(WANT_VIS_EXPORTS "${OUTPUT_DIR}/want-vis-paired.exports.inc")
set(WANT_LEVELS_INPUT
    "${PROJECT_ROOT}/test/goalc/source_templates/arm64-aot/full-want-levels-from-jak1-load-boundary.gc")
set(WANT_LEVELS_GOLDEN
    "${PROJECT_ROOT}/test/goalc/source_templates/arm64-aot/full-want-levels-from-jak1-load-boundary.s")
set(WANT_LEVELS_ASSEMBLY_ONLY "${OUTPUT_DIR}/want-levels-assembly-only.s")
set(WANT_LEVELS_PAIRED_ASSEMBLY "${OUTPUT_DIR}/want-levels-paired.s")
set(WANT_LEVELS_EXPORTS "${OUTPUT_DIR}/want-levels-paired.exports.inc")

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${INPUT}"
          --function false-func
          --output "${ASSEMBLY_ONLY}"
          --symbol goalpad_aot_false_func
  RESULT_VARIABLE ASSEMBLY_ONLY_RESULT)
if(NOT ASSEMBLY_ONLY_RESULT EQUAL 0)
  message(FATAL_ERROR "Assembly-only goalc-aot invocation failed")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${INPUT}"
          --function false-func
          --output "${PAIRED_ASSEMBLY}"
          --exports-output "${EXPORTS}"
          --symbol goalpad_aot_false_func
  RESULT_VARIABLE PAIRED_RESULT)
if(NOT PAIRED_RESULT EQUAL 0)
  message(FATAL_ERROR "Paired goalc-aot invocation failed")
endif()

file(SHA256 "${ASSEMBLY_ONLY}" ASSEMBLY_ONLY_SHA256)
file(SHA256 "${PAIRED_ASSEMBLY}" PAIRED_ASSEMBLY_SHA256)
if(NOT ASSEMBLY_ONLY_SHA256 STREQUAL PAIRED_ASSEMBLY_SHA256)
  message(FATAL_ERROR "Adding export metadata changed the ARM64 assembly")
endif()

file(READ "${EXPORTS}" ACTUAL_EXPORTS)
set(EXPECTED_EXPORTS
    "#ifndef OPENGOAL_AOT_EXPORT0\n#error \"Define OPENGOAL_AOT_EXPORT0 before including this file.\"\n#endif\nOPENGOAL_AOT_EXPORT0(\"false-func\", goalpad_aot_false_func)\n")
if(NOT ACTUAL_EXPORTS STREQUAL EXPECTED_EXPORTS)
  message(FATAL_ERROR "Generated export metadata did not match the expected record")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${IDENTITY_INPUT}"
          --function identity
          --output "${IDENTITY_ASSEMBLY_ONLY}"
          --symbol goalpad_aot_identity
  RESULT_VARIABLE IDENTITY_ASSEMBLY_ONLY_RESULT)
if(NOT IDENTITY_ASSEMBLY_ONLY_RESULT EQUAL 0)
  message(FATAL_ERROR "Assembly-only identity goalc-aot invocation failed")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${IDENTITY_INPUT}"
          --function identity
          --output "${IDENTITY_PAIRED_ASSEMBLY}"
          --exports-output "${IDENTITY_EXPORTS}"
          --symbol goalpad_aot_identity
  RESULT_VARIABLE IDENTITY_PAIRED_RESULT)
if(NOT IDENTITY_PAIRED_RESULT EQUAL 0)
  message(FATAL_ERROR "Paired identity goalc-aot invocation failed")
endif()

file(SHA256 "${IDENTITY_ASSEMBLY_ONLY}" IDENTITY_ASSEMBLY_ONLY_SHA256)
file(SHA256 "${IDENTITY_PAIRED_ASSEMBLY}" IDENTITY_PAIRED_ASSEMBLY_SHA256)
if(NOT IDENTITY_ASSEMBLY_ONLY_SHA256 STREQUAL IDENTITY_PAIRED_ASSEMBLY_SHA256)
  message(FATAL_ERROR "Adding identity export metadata changed the ARM64 assembly")
endif()

file(READ "${IDENTITY_EXPORTS}" ACTUAL_IDENTITY_EXPORTS)
set(EXPECTED_IDENTITY_EXPORTS
    "#ifndef OPENGOAL_AOT_EXPORT1\n#error \"Define OPENGOAL_AOT_EXPORT1 before including this file.\"\n#endif\nOPENGOAL_AOT_EXPORT1(\"identity\", goalpad_aot_identity)\n")
if(NOT ACTUAL_IDENTITY_EXPORTS STREQUAL EXPECTED_IDENTITY_EXPORTS)
  message(FATAL_ERROR "Generated identity export metadata did not match the expected record")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${TRUE_INPUT}"
          --function true-func
          --output "${TRUE_ASSEMBLY_ONLY}"
          --symbol goalpad_aot_true_func
  RESULT_VARIABLE TRUE_ASSEMBLY_ONLY_RESULT)
if(NOT TRUE_ASSEMBLY_ONLY_RESULT EQUAL 0)
  message(FATAL_ERROR "Assembly-only true-func goalc-aot invocation failed")
endif()

file(READ "${TRUE_ASSEMBLY_ONLY}" ACTUAL_TRUE_ASSEMBLY)
set(EXPECTED_TRUE_ASSEMBLY
    ".section __TEXT,__text,regular,pure_instructions\n.p2align 2\n.globl _goalpad_aot_true_func\n_goalpad_aot_true_func:\n  .long 0xaa1503e0\n  .long 0x91002000\n  .long 0xd65f03c0\n.subsections_via_symbols\n")
if(NOT ACTUAL_TRUE_ASSEMBLY STREQUAL EXPECTED_TRUE_ASSEMBLY)
  message(FATAL_ERROR "Assembly-only true-func artifact did not match the expected ARM64 code")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${LOGNOT_INPUT}"
          --function lognot
          --output "${LOGNOT_ASSEMBLY_ONLY}"
          --symbol goalpad_aot_lognot
  RESULT_VARIABLE LOGNOT_ASSEMBLY_ONLY_RESULT)
if(NOT LOGNOT_ASSEMBLY_ONLY_RESULT EQUAL 0)
  message(FATAL_ERROR "Assembly-only lognot goalc-aot invocation failed")
endif()

file(READ "${LOGNOT_ASSEMBLY_ONLY}" ACTUAL_LOGNOT_ASSEMBLY)
set(EXPECTED_LOGNOT_ASSEMBLY
    ".section __TEXT,__text,regular,pure_instructions\n.p2align 2\n.globl _goalpad_aot_lognot\n_goalpad_aot_lognot:\n  .long 0xaa2003e0\n  .long 0xd65f03c0\n.subsections_via_symbols\n")
if(NOT ACTUAL_LOGNOT_ASSEMBLY STREQUAL EXPECTED_LOGNOT_ASSEMBLY)
  message(FATAL_ERROR "Assembly-only lognot artifact did not match the expected ARM64 code")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${LOGNOT_INPUT}"
          --function lognot
          --output "${LOGNOT_PAIRED_ASSEMBLY}"
          --exports-output "${LOGNOT_EXPORTS}"
          --symbol goalpad_aot_lognot
  RESULT_VARIABLE LOGNOT_PAIRED_RESULT)
if(NOT LOGNOT_PAIRED_RESULT EQUAL 0)
  message(FATAL_ERROR "Paired lognot goalc-aot invocation failed")
endif()

file(SHA256 "${LOGNOT_ASSEMBLY_ONLY}" LOGNOT_ASSEMBLY_ONLY_SHA256)
file(SHA256 "${LOGNOT_PAIRED_ASSEMBLY}" LOGNOT_PAIRED_ASSEMBLY_SHA256)
if(NOT LOGNOT_ASSEMBLY_ONLY_SHA256 STREQUAL LOGNOT_PAIRED_ASSEMBLY_SHA256)
  message(FATAL_ERROR "Adding lognot export metadata changed the ARM64 assembly")
endif()

file(READ "${LOGNOT_PAIRED_ASSEMBLY}" ACTUAL_LOGNOT_PAIRED_ASSEMBLY)
file(READ "${LOGNOT_EXPORTS}" ACTUAL_LOGNOT_EXPORTS)
if(NOT ACTUAL_LOGNOT_PAIRED_ASSEMBLY STREQUAL EXPECTED_LOGNOT_ASSEMBLY)
  message(FATAL_ERROR "Paired lognot artifact did not match the expected ARM64 code")
endif()

set(EXPECTED_LOGNOT_EXPORTS
    "#ifndef OPENGOAL_AOT_EXPORT1\n#error \"Define OPENGOAL_AOT_EXPORT1 before including this file.\"\n#endif\nOPENGOAL_AOT_EXPORT1(\"lognot\", goalpad_aot_lognot)\n")
if(NOT ACTUAL_LOGNOT_EXPORTS STREQUAL EXPECTED_LOGNOT_EXPORTS)
  message(FATAL_ERROR "Generated lognot export metadata did not match the expected record")
endif()

set(LOGNOT_WRONG_ASSEMBLY "${OUTPUT_DIR}/lognot-wrong.s")
set(LOGNOT_WRONG_EXPORTS "${OUTPUT_DIR}/lognot-wrong.exports.inc")
file(WRITE "${LOGNOT_WRONG_ASSEMBLY}" "old lognot wrong assembly\n")
file(WRITE "${LOGNOT_WRONG_EXPORTS}" "old lognot wrong exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${LOGNOT_INPUT}"
          --function lognot
          --output "${LOGNOT_WRONG_ASSEMBLY}"
          --exports-output "${LOGNOT_WRONG_EXPORTS}"
          --symbol goalpad_aot_identity
  RESULT_VARIABLE LOGNOT_WRONG_SYMBOL_RESULT)
if(LOGNOT_WRONG_SYMBOL_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted lognot with the identity symbol")
endif()

file(READ "${LOGNOT_WRONG_ASSEMBLY}" ACTUAL_LOGNOT_WRONG_ASSEMBLY)
file(READ "${LOGNOT_WRONG_EXPORTS}" ACTUAL_LOGNOT_WRONG_EXPORTS)
if(NOT ACTUAL_LOGNOT_WRONG_ASSEMBLY STREQUAL "old lognot wrong assembly\n" OR
   NOT ACTUAL_LOGNOT_WRONG_EXPORTS STREQUAL "old lognot wrong exports\n")
  message(FATAL_ERROR "Failed lognot symbol validation modified an existing artifact")
endif()

set(LOGNOT_CROSS_ASSEMBLY "${OUTPUT_DIR}/lognot-cross.s")
set(LOGNOT_CROSS_EXPORTS "${OUTPUT_DIR}/lognot-cross.exports.inc")
file(WRITE "${LOGNOT_CROSS_ASSEMBLY}" "old lognot cross assembly\n")
file(WRITE "${LOGNOT_CROSS_EXPORTS}" "old lognot cross exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${LOGNOT_INPUT}"
          --function lognot
          --output "${LOGNOT_CROSS_ASSEMBLY}"
          --exports-output "${LOGNOT_CROSS_EXPORTS}"
          --symbol goalpad_aot_false_func
  RESULT_VARIABLE LOGNOT_CROSS_ABI_RESULT)
if(LOGNOT_CROSS_ABI_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted lognot with the Export0 symbol")
endif()

file(READ "${LOGNOT_CROSS_ASSEMBLY}" ACTUAL_LOGNOT_CROSS_ASSEMBLY)
file(READ "${LOGNOT_CROSS_EXPORTS}" ACTUAL_LOGNOT_CROSS_EXPORTS)
if(NOT ACTUAL_LOGNOT_CROSS_ASSEMBLY STREQUAL "old lognot cross assembly\n" OR
   NOT ACTUAL_LOGNOT_CROSS_EXPORTS STREQUAL "old lognot cross exports\n")
  message(FATAL_ERROR "Failed lognot cross-ABI generation modified an existing artifact")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${TRUE_INPUT}"
          --function true-func
          --output "${TRUE_PAIRED_ASSEMBLY}"
          --exports-output "${TRUE_EXPORTS}"
          --symbol goalpad_aot_true_func
  RESULT_VARIABLE TRUE_PAIRED_RESULT)
if(NOT TRUE_PAIRED_RESULT EQUAL 0)
  message(FATAL_ERROR "Paired true-func goalc-aot invocation failed")
endif()

file(SHA256 "${TRUE_ASSEMBLY_ONLY}" TRUE_ASSEMBLY_ONLY_SHA256)
file(SHA256 "${TRUE_PAIRED_ASSEMBLY}" TRUE_PAIRED_ASSEMBLY_SHA256)
if(NOT TRUE_ASSEMBLY_ONLY_SHA256 STREQUAL TRUE_PAIRED_ASSEMBLY_SHA256)
  message(FATAL_ERROR "Adding true-func export metadata changed the ARM64 assembly")
endif()

file(READ "${TRUE_EXPORTS}" ACTUAL_TRUE_EXPORTS)
set(EXPECTED_TRUE_EXPORTS
    "#ifndef OPENGOAL_AOT_EXPORT0\n#error \"Define OPENGOAL_AOT_EXPORT0 before including this file.\"\n#endif\nOPENGOAL_AOT_EXPORT0(\"true-func\", goalpad_aot_true_func)\n")
if(NOT ACTUAL_TRUE_EXPORTS STREQUAL EXPECTED_TRUE_EXPORTS)
  message(FATAL_ERROR "Generated true-func export metadata did not match the expected record")
endif()

set(TRUE_WRONG_ASSEMBLY "${OUTPUT_DIR}/true-wrong.s")
set(TRUE_WRONG_EXPORTS "${OUTPUT_DIR}/true-wrong.exports.inc")
file(WRITE "${TRUE_WRONG_ASSEMBLY}" "old true wrong assembly\n")
file(WRITE "${TRUE_WRONG_EXPORTS}" "old true wrong exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${TRUE_INPUT}"
          --function true-func
          --output "${TRUE_WRONG_ASSEMBLY}"
          --exports-output "${TRUE_WRONG_EXPORTS}"
          --symbol goalpad_aot_false_func
  RESULT_VARIABLE TRUE_WRONG_SYMBOL_RESULT)
if(TRUE_WRONG_SYMBOL_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted true-func with the false-func symbol")
endif()

file(READ "${TRUE_WRONG_ASSEMBLY}" ACTUAL_TRUE_WRONG_ASSEMBLY)
file(READ "${TRUE_WRONG_EXPORTS}" ACTUAL_TRUE_WRONG_EXPORTS)
if(NOT ACTUAL_TRUE_WRONG_ASSEMBLY STREQUAL "old true wrong assembly\n" OR
   NOT ACTUAL_TRUE_WRONG_EXPORTS STREQUAL "old true wrong exports\n")
  message(FATAL_ERROR "Failed true-func symbol validation modified an existing artifact")
endif()

set(TRUE_CROSS_ASSEMBLY "${OUTPUT_DIR}/true-cross.s")
set(TRUE_CROSS_EXPORTS "${OUTPUT_DIR}/true-cross.exports.inc")
file(WRITE "${TRUE_CROSS_ASSEMBLY}" "old true cross assembly\n")
file(WRITE "${TRUE_CROSS_EXPORTS}" "old true cross exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${TRUE_INPUT}"
          --function true-func
          --output "${TRUE_CROSS_ASSEMBLY}"
          --exports-output "${TRUE_CROSS_EXPORTS}"
          --symbol goalpad_aot_identity
  RESULT_VARIABLE TRUE_IDENTITY_SYMBOL_RESULT)
if(TRUE_IDENTITY_SYMBOL_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted true-func with the Export1 symbol")
endif()

file(READ "${TRUE_CROSS_ASSEMBLY}" ACTUAL_TRUE_CROSS_ASSEMBLY)
file(READ "${TRUE_CROSS_EXPORTS}" ACTUAL_TRUE_CROSS_EXPORTS)
if(NOT ACTUAL_TRUE_CROSS_ASSEMBLY STREQUAL "old true cross assembly\n" OR
   NOT ACTUAL_TRUE_CROSS_EXPORTS STREQUAL "old true cross exports\n")
  message(FATAL_ERROR "Failed true-func cross-ABI generation modified an existing artifact")
endif()

set(TRUE_LOGNOT_ASSEMBLY "${OUTPUT_DIR}/true-lognot.s")
set(TRUE_LOGNOT_EXPORTS "${OUTPUT_DIR}/true-lognot.exports.inc")
file(WRITE "${TRUE_LOGNOT_ASSEMBLY}" "old true lognot assembly\n")
file(WRITE "${TRUE_LOGNOT_EXPORTS}" "old true lognot exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${TRUE_INPUT}"
          --function true-func
          --output "${TRUE_LOGNOT_ASSEMBLY}"
          --exports-output "${TRUE_LOGNOT_EXPORTS}"
          --symbol goalpad_aot_lognot
  RESULT_VARIABLE TRUE_LOGNOT_SYMBOL_RESULT)
if(TRUE_LOGNOT_SYMBOL_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted true-func with the lognot symbol")
endif()

file(READ "${TRUE_LOGNOT_ASSEMBLY}" ACTUAL_TRUE_LOGNOT_ASSEMBLY)
file(READ "${TRUE_LOGNOT_EXPORTS}" ACTUAL_TRUE_LOGNOT_EXPORTS)
if(NOT ACTUAL_TRUE_LOGNOT_ASSEMBLY STREQUAL "old true lognot assembly\n" OR
   NOT ACTUAL_TRUE_LOGNOT_EXPORTS STREQUAL "old true lognot exports\n")
  message(FATAL_ERROR "Failed true-func lognot validation modified an existing artifact")
endif()

set(FALSE_WRONG_ASSEMBLY "${OUTPUT_DIR}/false-wrong.s")
set(FALSE_WRONG_EXPORTS "${OUTPUT_DIR}/false-wrong.exports.inc")
file(WRITE "${FALSE_WRONG_ASSEMBLY}" "old false wrong assembly\n")
file(WRITE "${FALSE_WRONG_EXPORTS}" "old false wrong exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${INPUT}"
          --function false-func
          --output "${FALSE_WRONG_ASSEMBLY}"
          --exports-output "${FALSE_WRONG_EXPORTS}"
          --symbol goalpad_aot_true_func
  RESULT_VARIABLE FALSE_TRUE_SYMBOL_RESULT)
if(FALSE_TRUE_SYMBOL_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted false-func with the true-func symbol")
endif()

file(READ "${FALSE_WRONG_ASSEMBLY}" ACTUAL_FALSE_WRONG_ASSEMBLY)
file(READ "${FALSE_WRONG_EXPORTS}" ACTUAL_FALSE_WRONG_EXPORTS)
if(NOT ACTUAL_FALSE_WRONG_ASSEMBLY STREQUAL "old false wrong assembly\n" OR
   NOT ACTUAL_FALSE_WRONG_EXPORTS STREQUAL "old false wrong exports\n")
  message(FATAL_ERROR "Failed false-func symbol validation modified an existing artifact")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${INPUT}"
          --function false-func
          --output alias.s
          --exports-output ./alias.s
          --symbol goalpad_aot_false_func
  WORKING_DIRECTORY "${OUTPUT_DIR}"
  RESULT_VARIABLE ALIAS_RESULT)
if(ALIAS_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted aliased assembly and export paths")
endif()

set(UNCHANGED_ASSEMBLY "${OUTPUT_DIR}/unchanged.s")
set(UNCHANGED_EXPORTS "${OUTPUT_DIR}/unchanged.exports.inc")
file(WRITE "${UNCHANGED_ASSEMBLY}" "old assembly\n")
file(WRITE "${UNCHANGED_EXPORTS}" "old exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${INPUT}"
          --function false-func
          --output "${UNCHANGED_ASSEMBLY}"
          --exports-output "${UNCHANGED_EXPORTS}"
          --symbol __reserved
  RESULT_VARIABLE INVALID_METADATA_RESULT)
if(INVALID_METADATA_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted invalid native export metadata")
endif()

file(READ "${UNCHANGED_ASSEMBLY}" ACTUAL_UNCHANGED_ASSEMBLY)
file(READ "${UNCHANGED_EXPORTS}" ACTUAL_UNCHANGED_EXPORTS)
if(NOT ACTUAL_UNCHANGED_ASSEMBLY STREQUAL "old assembly\n" OR
   NOT ACTUAL_UNCHANGED_EXPORTS STREQUAL "old exports\n")
  message(FATAL_ERROR "Failed paired generation modified an existing artifact")
endif()

set(CROSS_ASSEMBLY "${OUTPUT_DIR}/cross.s")
set(CROSS_EXPORTS "${OUTPUT_DIR}/cross.exports.inc")
file(WRITE "${CROSS_ASSEMBLY}" "old cross assembly\n")
file(WRITE "${CROSS_EXPORTS}" "old cross exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${IDENTITY_INPUT}"
          --function identity
          --output "${CROSS_ASSEMBLY}"
          --exports-output "${CROSS_EXPORTS}"
          --symbol goalpad_aot_false_func
  RESULT_VARIABLE IDENTITY_FALSE_SYMBOL_RESULT)
if(IDENTITY_FALSE_SYMBOL_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted identity with the Export0 symbol")
endif()

file(READ "${CROSS_ASSEMBLY}" ACTUAL_CROSS_ASSEMBLY)
file(READ "${CROSS_EXPORTS}" ACTUAL_CROSS_EXPORTS)
if(NOT ACTUAL_CROSS_ASSEMBLY STREQUAL "old cross assembly\n" OR
   NOT ACTUAL_CROSS_EXPORTS STREQUAL "old cross exports\n")
  message(FATAL_ERROR "Failed identity cross-ABI generation modified an existing artifact")
endif()

set(IDENTITY_LOGNOT_ASSEMBLY "${OUTPUT_DIR}/identity-lognot.s")
set(IDENTITY_LOGNOT_EXPORTS "${OUTPUT_DIR}/identity-lognot.exports.inc")
file(WRITE "${IDENTITY_LOGNOT_ASSEMBLY}" "old identity lognot assembly\n")
file(WRITE "${IDENTITY_LOGNOT_EXPORTS}" "old identity lognot exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${IDENTITY_INPUT}"
          --function identity
          --output "${IDENTITY_LOGNOT_ASSEMBLY}"
          --exports-output "${IDENTITY_LOGNOT_EXPORTS}"
          --symbol goalpad_aot_lognot
  RESULT_VARIABLE IDENTITY_LOGNOT_SYMBOL_RESULT)
if(IDENTITY_LOGNOT_SYMBOL_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted identity with the lognot symbol")
endif()

file(READ "${IDENTITY_LOGNOT_ASSEMBLY}" ACTUAL_IDENTITY_LOGNOT_ASSEMBLY)
file(READ "${IDENTITY_LOGNOT_EXPORTS}" ACTUAL_IDENTITY_LOGNOT_EXPORTS)
if(NOT ACTUAL_IDENTITY_LOGNOT_ASSEMBLY STREQUAL "old identity lognot assembly\n" OR
   NOT ACTUAL_IDENTITY_LOGNOT_EXPORTS STREQUAL "old identity lognot exports\n")
  message(FATAL_ERROR "Failed identity lognot validation modified an existing artifact")
endif()

set(FALSE_CROSS_ASSEMBLY "${OUTPUT_DIR}/false-cross.s")
set(FALSE_CROSS_EXPORTS "${OUTPUT_DIR}/false-cross.exports.inc")
file(WRITE "${FALSE_CROSS_ASSEMBLY}" "old false cross assembly\n")
file(WRITE "${FALSE_CROSS_EXPORTS}" "old false cross exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${INPUT}"
          --function false-func
          --output "${FALSE_CROSS_ASSEMBLY}"
          --exports-output "${FALSE_CROSS_EXPORTS}"
          --symbol goalpad_aot_identity
  RESULT_VARIABLE FALSE_IDENTITY_SYMBOL_RESULT)
if(FALSE_IDENTITY_SYMBOL_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted false-func with the Export1 symbol")
endif()

file(READ "${FALSE_CROSS_ASSEMBLY}" ACTUAL_FALSE_CROSS_ASSEMBLY)
file(READ "${FALSE_CROSS_EXPORTS}" ACTUAL_FALSE_CROSS_EXPORTS)
if(NOT ACTUAL_FALSE_CROSS_ASSEMBLY STREQUAL "old false cross assembly\n" OR
   NOT ACTUAL_FALSE_CROSS_EXPORTS STREQUAL "old false cross exports\n")
  message(FATAL_ERROR "Failed false-func cross-ABI generation modified an existing artifact")
endif()

set(FALSE_LOGNOT_ASSEMBLY "${OUTPUT_DIR}/false-lognot.s")
set(FALSE_LOGNOT_EXPORTS "${OUTPUT_DIR}/false-lognot.exports.inc")
file(WRITE "${FALSE_LOGNOT_ASSEMBLY}" "old false lognot assembly\n")
file(WRITE "${FALSE_LOGNOT_EXPORTS}" "old false lognot exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${INPUT}"
          --function false-func
          --output "${FALSE_LOGNOT_ASSEMBLY}"
          --exports-output "${FALSE_LOGNOT_EXPORTS}"
          --symbol goalpad_aot_lognot
  RESULT_VARIABLE FALSE_LOGNOT_SYMBOL_RESULT)
if(FALSE_LOGNOT_SYMBOL_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted false-func with the lognot symbol")
endif()

file(READ "${FALSE_LOGNOT_ASSEMBLY}" ACTUAL_FALSE_LOGNOT_ASSEMBLY)
file(READ "${FALSE_LOGNOT_EXPORTS}" ACTUAL_FALSE_LOGNOT_EXPORTS)
if(NOT ACTUAL_FALSE_LOGNOT_ASSEMBLY STREQUAL "old false lognot assembly\n" OR
   NOT ACTUAL_FALSE_LOGNOT_EXPORTS STREQUAL "old false lognot exports\n")
  message(FATAL_ERROR "Failed false-func lognot validation modified an existing artifact")
endif()

set(WRONG_IDENTITY_ASSEMBLY "${OUTPUT_DIR}/wrong-identity.s")
set(WRONG_IDENTITY_EXPORTS "${OUTPUT_DIR}/wrong-identity.exports.inc")
file(WRITE "${WRONG_IDENTITY_ASSEMBLY}" "old wrong identity assembly\n")
file(WRITE "${WRONG_IDENTITY_EXPORTS}" "old wrong identity exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${IDENTITY_INPUT}"
          --function identity
          --output "${WRONG_IDENTITY_ASSEMBLY}"
          --exports-output "${WRONG_IDENTITY_EXPORTS}"
          --symbol wrong_identity_symbol
  RESULT_VARIABLE WRONG_IDENTITY_SYMBOL_RESULT)
if(WRONG_IDENTITY_SYMBOL_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted an unsupported identity symbol")
endif()

file(READ "${WRONG_IDENTITY_ASSEMBLY}" ACTUAL_WRONG_IDENTITY_ASSEMBLY)
file(READ "${WRONG_IDENTITY_EXPORTS}" ACTUAL_WRONG_IDENTITY_EXPORTS)
if(NOT ACTUAL_WRONG_IDENTITY_ASSEMBLY STREQUAL "old wrong identity assembly\n" OR
   NOT ACTUAL_WRONG_IDENTITY_EXPORTS STREQUAL "old wrong identity exports\n")
  message(FATAL_ERROR "Failed identity metadata validation modified an existing artifact")
endif()

set(BLOCKED_ASSEMBLY "${OUTPUT_DIR}/blocked.s")
set(BLOCKED_EXPORTS "${OUTPUT_DIR}/blocked.exports.inc")
file(WRITE "${BLOCKED_ASSEMBLY}" "old blocked assembly\n")
file(REMOVE_RECURSE "${BLOCKED_EXPORTS}")
file(MAKE_DIRECTORY "${BLOCKED_EXPORTS}")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${INPUT}"
          --function false-func
          --output "${BLOCKED_ASSEMBLY}"
          --exports-output "${BLOCKED_EXPORTS}"
          --symbol goalpad_aot_false_func
  RESULT_VARIABLE BLOCKED_EXPORT_RESULT)
if(BLOCKED_EXPORT_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted a directory as an export destination")
endif()

file(READ "${BLOCKED_ASSEMBLY}" ACTUAL_BLOCKED_ASSEMBLY)
if(NOT ACTUAL_BLOCKED_ASSEMBLY STREQUAL "old blocked assembly\n" OR
   NOT IS_DIRECTORY "${BLOCKED_EXPORTS}")
  message(FATAL_ERROR "Failed paired publication did not preserve both prior destinations")
endif()

set(COLLISION_EXPORTS "${OUTPUT_DIR}/collision")
set(COLLISION_ASSEMBLY "${COLLISION_EXPORTS}.goalc-aot-new-0.tmp")
file(WRITE "${COLLISION_ASSEMBLY}" "old collision assembly\n")
file(WRITE "${COLLISION_EXPORTS}" "old collision exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${INPUT}"
          --function false-func
          --output "${COLLISION_ASSEMBLY}"
          --exports-output "${COLLISION_EXPORTS}"
          --symbol goalpad_aot_false_func
  RESULT_VARIABLE COLLISION_RESULT)
if(NOT COLLISION_RESULT EQUAL 0)
  message(FATAL_ERROR "Auxiliary-path collision prevented paired generation")
endif()

file(SHA256 "${COLLISION_ASSEMBLY}" COLLISION_ASSEMBLY_SHA256)
file(READ "${COLLISION_EXPORTS}" COLLISION_EXPORTS_CONTENT)
if(NOT COLLISION_ASSEMBLY_SHA256 STREQUAL ASSEMBLY_ONLY_SHA256 OR
   NOT COLLISION_EXPORTS_CONTENT STREQUAL EXPECTED_EXPORTS)
  message(FATAL_ERROR "Auxiliary-path collision corrupted a final artifact")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${GLST_NODE_NAME_INPUT}"
          --function glst-node-name
          --output "${GLST_NODE_NAME_ASSEMBLY_ONLY}"
          --symbol goalpad_aot_glst_node_name
  RESULT_VARIABLE GLST_NODE_NAME_ASSEMBLY_ONLY_RESULT)
if(NOT GLST_NODE_NAME_ASSEMBLY_ONLY_RESULT EQUAL 0)
  message(FATAL_ERROR "Assembly-only glst-node-name goalc-aot invocation failed")
endif()

file(READ "${GLST_NODE_NAME_ASSEMBLY_ONLY}" ACTUAL_GLST_NODE_NAME_ASSEMBLY)
set(EXPECTED_GLST_NODE_NAME_ASSEMBLY
    ".section __TEXT,__text,regular,pure_instructions\n.p2align 2\n.globl _goalpad_aot_glst_node_name\n_goalpad_aot_glst_node_name:\n  .long 0x8b36e010\n  .long 0xb8408200\n  .long 0xd65f03c0\n.subsections_via_symbols\n")
if(NOT ACTUAL_GLST_NODE_NAME_ASSEMBLY STREQUAL EXPECTED_GLST_NODE_NAME_ASSEMBLY)
  message(FATAL_ERROR "Assembly-only glst-node-name artifact did not match the expected ARM64 code")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${GLST_NODE_NAME_INPUT}"
          --function glst-node-name
          --output "${GLST_NODE_NAME_PAIRED_ASSEMBLY}"
          --exports-output "${GLST_NODE_NAME_EXPORTS}"
          --symbol goalpad_aot_glst_node_name
  RESULT_VARIABLE GLST_NODE_NAME_PAIRED_RESULT)
if(NOT GLST_NODE_NAME_PAIRED_RESULT EQUAL 0)
  message(FATAL_ERROR "Paired glst-node-name goalc-aot invocation failed")
endif()

file(SHA256 "${GLST_NODE_NAME_ASSEMBLY_ONLY}" GLST_NODE_NAME_ASSEMBLY_ONLY_SHA256)
file(SHA256 "${GLST_NODE_NAME_PAIRED_ASSEMBLY}" GLST_NODE_NAME_PAIRED_ASSEMBLY_SHA256)
if(NOT GLST_NODE_NAME_ASSEMBLY_ONLY_SHA256 STREQUAL GLST_NODE_NAME_PAIRED_ASSEMBLY_SHA256)
  message(FATAL_ERROR "Adding glst-node-name export metadata changed the ARM64 assembly")
endif()

file(READ "${GLST_NODE_NAME_PAIRED_ASSEMBLY}" ACTUAL_GLST_NODE_NAME_PAIRED_ASSEMBLY)
file(READ "${GLST_NODE_NAME_EXPORTS}" ACTUAL_GLST_NODE_NAME_EXPORTS)
if(NOT ACTUAL_GLST_NODE_NAME_PAIRED_ASSEMBLY STREQUAL EXPECTED_GLST_NODE_NAME_ASSEMBLY)
  message(FATAL_ERROR "Paired glst-node-name artifact did not match the expected ARM64 code")
endif()

set(EXPECTED_GLST_NODE_NAME_EXPORTS
    "#ifndef OPENGOAL_AOT_EXPORT1\n#error \"Define OPENGOAL_AOT_EXPORT1 before including this file.\"\n#endif\nOPENGOAL_AOT_EXPORT1(\"glst-node-name\", goalpad_aot_glst_node_name)\n")
if(NOT ACTUAL_GLST_NODE_NAME_EXPORTS STREQUAL EXPECTED_GLST_NODE_NAME_EXPORTS)
  message(FATAL_ERROR "Generated glst-node-name export metadata did not match the expected record")
endif()

set(GLST_NODE_NAME_WRONG_ASSEMBLY "${OUTPUT_DIR}/glst-node-name-wrong.s")
set(GLST_NODE_NAME_WRONG_EXPORTS "${OUTPUT_DIR}/glst-node-name-wrong.exports.inc")
file(WRITE "${GLST_NODE_NAME_WRONG_ASSEMBLY}" "old glst-node-name wrong assembly\n")
file(WRITE "${GLST_NODE_NAME_WRONG_EXPORTS}" "old glst-node-name wrong exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${GLST_NODE_NAME_INPUT}"
          --function glst-node-name
          --output "${GLST_NODE_NAME_WRONG_ASSEMBLY}"
          --exports-output "${GLST_NODE_NAME_WRONG_EXPORTS}"
          --symbol goalpad_aot_lognot
  RESULT_VARIABLE GLST_NODE_NAME_WRONG_SYMBOL_RESULT)
if(GLST_NODE_NAME_WRONG_SYMBOL_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted glst-node-name with the lognot symbol")
endif()

file(READ "${GLST_NODE_NAME_WRONG_ASSEMBLY}" ACTUAL_GLST_NODE_NAME_WRONG_ASSEMBLY)
file(READ "${GLST_NODE_NAME_WRONG_EXPORTS}" ACTUAL_GLST_NODE_NAME_WRONG_EXPORTS)
if(NOT ACTUAL_GLST_NODE_NAME_WRONG_ASSEMBLY STREQUAL
       "old glst-node-name wrong assembly\n" OR
   NOT ACTUAL_GLST_NODE_NAME_WRONG_EXPORTS STREQUAL "old glst-node-name wrong exports\n")
  message(FATAL_ERROR "Failed glst-node-name symbol validation modified an existing artifact")
endif()

set(GLST_NODE_NAME_CROSS_ASSEMBLY "${OUTPUT_DIR}/glst-node-name-cross.s")
set(GLST_NODE_NAME_CROSS_EXPORTS "${OUTPUT_DIR}/glst-node-name-cross.exports.inc")
file(WRITE "${GLST_NODE_NAME_CROSS_ASSEMBLY}" "old glst-node-name cross assembly\n")
file(WRITE "${GLST_NODE_NAME_CROSS_EXPORTS}" "old glst-node-name cross exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${GLST_NODE_NAME_INPUT}"
          --function glst-node-name
          --output "${GLST_NODE_NAME_CROSS_ASSEMBLY}"
          --exports-output "${GLST_NODE_NAME_CROSS_EXPORTS}"
          --symbol goalpad_aot_false_func
  RESULT_VARIABLE GLST_NODE_NAME_CROSS_ABI_RESULT)
if(GLST_NODE_NAME_CROSS_ABI_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted glst-node-name with the Export0 symbol")
endif()

file(READ "${GLST_NODE_NAME_CROSS_ASSEMBLY}" ACTUAL_GLST_NODE_NAME_CROSS_ASSEMBLY)
file(READ "${GLST_NODE_NAME_CROSS_EXPORTS}" ACTUAL_GLST_NODE_NAME_CROSS_EXPORTS)
if(NOT ACTUAL_GLST_NODE_NAME_CROSS_ASSEMBLY STREQUAL
       "old glst-node-name cross assembly\n" OR
   NOT ACTUAL_GLST_NODE_NAME_CROSS_EXPORTS STREQUAL "old glst-node-name cross exports\n")
  message(FATAL_ERROR "Failed glst-node-name cross-ABI generation modified an existing artifact")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${LEVEL_GROUP_LOAD_COMMANDS_SET_INPUT}"
          --function level-group-load-commands-set!
          --output "${LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY_ONLY}"
          --symbol goalpad_aot_level_group_load_commands_set
  RESULT_VARIABLE LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY_ONLY_RESULT)
if(NOT LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY_ONLY_RESULT EQUAL 0)
  message(FATAL_ERROR "Assembly-only level-group-load-commands-set! goalc-aot invocation failed")
endif()

file(READ "${LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY_ONLY}"
     ACTUAL_LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY)
set(EXPECTED_LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY
    ".section __TEXT,__text,regular,pure_instructions\n.p2align 2\n.globl _goalpad_aot_level_group_load_commands_set\n_goalpad_aot_level_group_load_commands_set:\n  .long 0x8b160010\n  .long 0x91008210\n  .long 0xb9000201\n  .long 0xaa0103e0\n  .long 0xd65f03c0\n.subsections_via_symbols\n")
if(NOT ACTUAL_LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY STREQUAL
       EXPECTED_LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY)
  message(FATAL_ERROR "Assembly-only level-group-load-commands-set! artifact did not match the expected ARM64 code")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${LEVEL_GROUP_LOAD_COMMANDS_SET_INPUT}"
          --function level-group-load-commands-set!
          --output "${LEVEL_GROUP_LOAD_COMMANDS_SET_PAIRED_ASSEMBLY}"
          --exports-output "${LEVEL_GROUP_LOAD_COMMANDS_SET_EXPORTS}"
          --symbol goalpad_aot_level_group_load_commands_set
  RESULT_VARIABLE LEVEL_GROUP_LOAD_COMMANDS_SET_PAIRED_RESULT)
if(NOT LEVEL_GROUP_LOAD_COMMANDS_SET_PAIRED_RESULT EQUAL 0)
  message(FATAL_ERROR "Paired level-group-load-commands-set! goalc-aot invocation failed")
endif()

file(SHA256 "${LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY_ONLY}"
     LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY_ONLY_SHA256)
file(SHA256 "${LEVEL_GROUP_LOAD_COMMANDS_SET_PAIRED_ASSEMBLY}"
     LEVEL_GROUP_LOAD_COMMANDS_SET_PAIRED_ASSEMBLY_SHA256)
if(NOT LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY_ONLY_SHA256 STREQUAL
       LEVEL_GROUP_LOAD_COMMANDS_SET_PAIRED_ASSEMBLY_SHA256)
  message(FATAL_ERROR "Adding level-group-load-commands-set! export metadata changed the ARM64 assembly")
endif()

file(READ "${LEVEL_GROUP_LOAD_COMMANDS_SET_EXPORTS}"
     ACTUAL_LEVEL_GROUP_LOAD_COMMANDS_SET_EXPORTS)
set(EXPECTED_LEVEL_GROUP_LOAD_COMMANDS_SET_EXPORTS
    "#ifndef OPENGOAL_AOT_EXPORT2\n#error \"Define OPENGOAL_AOT_EXPORT2 before including this file.\"\n#endif\nOPENGOAL_AOT_EXPORT2(\"level-group-load-commands-set!\", goalpad_aot_level_group_load_commands_set)\n")
if(NOT ACTUAL_LEVEL_GROUP_LOAD_COMMANDS_SET_EXPORTS STREQUAL
       EXPECTED_LEVEL_GROUP_LOAD_COMMANDS_SET_EXPORTS)
  message(FATAL_ERROR "Generated level-group-load-commands-set! export metadata did not match the expected record")
endif()

set(LEVEL_GROUP_LOAD_COMMANDS_SET_CROSS_ASSEMBLY
    "${OUTPUT_DIR}/level-group-load-commands-set-cross.s")
set(LEVEL_GROUP_LOAD_COMMANDS_SET_CROSS_EXPORTS
    "${OUTPUT_DIR}/level-group-load-commands-set-cross.exports.inc")
file(WRITE "${LEVEL_GROUP_LOAD_COMMANDS_SET_CROSS_ASSEMBLY}" "old level-group cross assembly\n")
file(WRITE "${LEVEL_GROUP_LOAD_COMMANDS_SET_CROSS_EXPORTS}" "old level-group cross exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${LEVEL_GROUP_LOAD_COMMANDS_SET_INPUT}"
          --function level-group-load-commands-set!
          --output "${LEVEL_GROUP_LOAD_COMMANDS_SET_CROSS_ASSEMBLY}"
          --exports-output "${LEVEL_GROUP_LOAD_COMMANDS_SET_CROSS_EXPORTS}"
          --symbol goalpad_aot_glst_node_name
  RESULT_VARIABLE LEVEL_GROUP_LOAD_COMMANDS_SET_CROSS_ABI_RESULT)
if(LEVEL_GROUP_LOAD_COMMANDS_SET_CROSS_ABI_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted level-group-load-commands-set! with the Export1 symbol")
endif()

file(READ "${LEVEL_GROUP_LOAD_COMMANDS_SET_CROSS_ASSEMBLY}"
     ACTUAL_LEVEL_GROUP_LOAD_COMMANDS_SET_CROSS_ASSEMBLY)
file(READ "${LEVEL_GROUP_LOAD_COMMANDS_SET_CROSS_EXPORTS}"
     ACTUAL_LEVEL_GROUP_LOAD_COMMANDS_SET_CROSS_EXPORTS)
if(NOT ACTUAL_LEVEL_GROUP_LOAD_COMMANDS_SET_CROSS_ASSEMBLY STREQUAL
       "old level-group cross assembly\n" OR
   NOT ACTUAL_LEVEL_GROUP_LOAD_COMMANDS_SET_CROSS_EXPORTS STREQUAL
       "old level-group cross exports\n")
  message(FATAL_ERROR "Failed level-group-load-commands-set! cross-ABI generation modified an existing artifact")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${WANT_VIS_INPUT}"
          --function want-vis
          --output "${WANT_VIS_ASSEMBLY_ONLY}"
          --symbol goalpad_aot_load_state_want_vis
  RESULT_VARIABLE WANT_VIS_ASSEMBLY_ONLY_RESULT)
if(NOT WANT_VIS_ASSEMBLY_ONLY_RESULT EQUAL 0)
  message(FATAL_ERROR "Assembly-only want-vis goalc-aot invocation failed")
endif()

file(READ "${WANT_VIS_ASSEMBLY_ONLY}" ACTUAL_WANT_VIS_ASSEMBLY)
set(EXPECTED_WANT_VIS_ASSEMBLY
    ".section __TEXT,__text,regular,pure_instructions\n.p2align 2\n.globl _goalpad_aot_load_state_want_vis\n_goalpad_aot_load_state_want_vis:\n  .long 0x8b160010\n  .long 0x91008210\n  .long 0xb9000201\n  .long 0xca000000\n  .long 0xd65f03c0\n.subsections_via_symbols\n")
if(NOT ACTUAL_WANT_VIS_ASSEMBLY STREQUAL EXPECTED_WANT_VIS_ASSEMBLY)
  message(FATAL_ERROR "Assembly-only want-vis artifact did not match the expected ARM64 code")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${WANT_VIS_INPUT}"
          --function want-vis
          --output "${WANT_VIS_PAIRED_ASSEMBLY}"
          --exports-output "${WANT_VIS_EXPORTS}"
          --symbol goalpad_aot_load_state_want_vis
  RESULT_VARIABLE WANT_VIS_PAIRED_RESULT)
if(NOT WANT_VIS_PAIRED_RESULT EQUAL 0)
  message(FATAL_ERROR "Paired want-vis goalc-aot invocation failed")
endif()

file(SHA256 "${WANT_VIS_ASSEMBLY_ONLY}" WANT_VIS_ASSEMBLY_ONLY_SHA256)
file(SHA256 "${WANT_VIS_PAIRED_ASSEMBLY}" WANT_VIS_PAIRED_ASSEMBLY_SHA256)
if(NOT WANT_VIS_ASSEMBLY_ONLY_SHA256 STREQUAL WANT_VIS_PAIRED_ASSEMBLY_SHA256)
  message(FATAL_ERROR "Adding want-vis export metadata changed the ARM64 assembly")
endif()

file(READ "${WANT_VIS_EXPORTS}" ACTUAL_WANT_VIS_EXPORTS)
set(EXPECTED_WANT_VIS_EXPORTS
    "#ifndef OPENGOAL_AOT_EXPORT2\n#error \"Define OPENGOAL_AOT_EXPORT2 before including this file.\"\n#endif\nOPENGOAL_AOT_EXPORT2(\"want-vis\", goalpad_aot_load_state_want_vis)\n")
if(NOT ACTUAL_WANT_VIS_EXPORTS STREQUAL EXPECTED_WANT_VIS_EXPORTS)
  message(FATAL_ERROR "Generated want-vis export metadata did not match the expected record")
endif()

set(WANT_VIS_WRONG_ASSEMBLY "${OUTPUT_DIR}/want-vis-wrong.s")
set(WANT_VIS_WRONG_EXPORTS "${OUTPUT_DIR}/want-vis-wrong.exports.inc")
file(WRITE "${WANT_VIS_WRONG_ASSEMBLY}" "old want-vis wrong assembly\n")
file(WRITE "${WANT_VIS_WRONG_EXPORTS}" "old want-vis wrong exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${WANT_VIS_INPUT}"
          --function want-vis
          --output "${WANT_VIS_WRONG_ASSEMBLY}"
          --exports-output "${WANT_VIS_WRONG_EXPORTS}"
          --symbol goalpad_aot_level_group_load_commands_set
  RESULT_VARIABLE WANT_VIS_WRONG_SYMBOL_RESULT)
if(WANT_VIS_WRONG_SYMBOL_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted want-vis with the level-group export symbol")
endif()

file(READ "${WANT_VIS_WRONG_ASSEMBLY}" ACTUAL_WANT_VIS_WRONG_ASSEMBLY)
file(READ "${WANT_VIS_WRONG_EXPORTS}" ACTUAL_WANT_VIS_WRONG_EXPORTS)
if(NOT ACTUAL_WANT_VIS_WRONG_ASSEMBLY STREQUAL "old want-vis wrong assembly\n" OR
   NOT ACTUAL_WANT_VIS_WRONG_EXPORTS STREQUAL "old want-vis wrong exports\n")
  message(FATAL_ERROR "Failed want-vis export validation modified an existing artifact")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${WANT_LEVELS_INPUT}"
          --function want-levels
          --output "${WANT_LEVELS_ASSEMBLY_ONLY}"
          --symbol goalpad_aot_load_state_want_levels
  RESULT_VARIABLE WANT_LEVELS_ASSEMBLY_ONLY_RESULT)
if(NOT WANT_LEVELS_ASSEMBLY_ONLY_RESULT EQUAL 0)
  message(FATAL_ERROR "Assembly-only want-levels goalc-aot invocation failed")
endif()

file(READ "${WANT_LEVELS_GOLDEN}" WANT_LEVELS_GENERIC_GOLDEN_ASSEMBLY)
string(REPLACE "_goalpad_aot_want_levels" "_goalpad_aot_load_state_want_levels"
       EXPECTED_WANT_LEVELS_ASSEMBLY "${WANT_LEVELS_GENERIC_GOLDEN_ASSEMBLY}")
file(READ "${WANT_LEVELS_ASSEMBLY_ONLY}" ACTUAL_WANT_LEVELS_ASSEMBLY)
if(NOT ACTUAL_WANT_LEVELS_ASSEMBLY STREQUAL EXPECTED_WANT_LEVELS_ASSEMBLY)
  message(FATAL_ERROR "Assembly-only want-levels artifact did not match the committed ARM64 golden")
endif()

execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${WANT_LEVELS_INPUT}"
          --function want-levels
          --output "${WANT_LEVELS_PAIRED_ASSEMBLY}"
          --exports-output "${WANT_LEVELS_EXPORTS}"
          --symbol goalpad_aot_load_state_want_levels
  RESULT_VARIABLE WANT_LEVELS_PAIRED_RESULT)
if(NOT WANT_LEVELS_PAIRED_RESULT EQUAL 0)
  message(FATAL_ERROR "Paired want-levels goalc-aot invocation failed")
endif()

file(SHA256 "${WANT_LEVELS_ASSEMBLY_ONLY}" WANT_LEVELS_ASSEMBLY_ONLY_SHA256)
file(SHA256 "${WANT_LEVELS_PAIRED_ASSEMBLY}" WANT_LEVELS_PAIRED_ASSEMBLY_SHA256)
if(NOT WANT_LEVELS_ASSEMBLY_ONLY_SHA256 STREQUAL WANT_LEVELS_PAIRED_ASSEMBLY_SHA256)
  message(FATAL_ERROR "Adding want-levels export metadata changed the ARM64 assembly")
endif()

file(READ "${WANT_LEVELS_PAIRED_ASSEMBLY}" ACTUAL_WANT_LEVELS_PAIRED_ASSEMBLY)
file(READ "${WANT_LEVELS_EXPORTS}" ACTUAL_WANT_LEVELS_EXPORTS)
if(NOT ACTUAL_WANT_LEVELS_PAIRED_ASSEMBLY STREQUAL EXPECTED_WANT_LEVELS_ASSEMBLY)
  message(FATAL_ERROR "Paired want-levels artifact did not match the committed ARM64 golden")
endif()

set(EXPECTED_WANT_LEVELS_EXPORTS
    "#ifndef OPENGOAL_AOT_EXPORT3\n#error \"Define OPENGOAL_AOT_EXPORT3 before including this file.\"\n#endif\nOPENGOAL_AOT_EXPORT3(\"want-levels\", goalpad_aot_load_state_want_levels)\n")
if(NOT ACTUAL_WANT_LEVELS_EXPORTS STREQUAL EXPECTED_WANT_LEVELS_EXPORTS)
  message(FATAL_ERROR "Generated want-levels export metadata did not match the expected record")
endif()

set(WANT_LEVELS_WRONG_ASSEMBLY "${OUTPUT_DIR}/want-levels-wrong.s")
set(WANT_LEVELS_WRONG_EXPORTS "${OUTPUT_DIR}/want-levels-wrong.exports.inc")
file(WRITE "${WANT_LEVELS_WRONG_ASSEMBLY}" "old want-levels wrong assembly\n")
file(WRITE "${WANT_LEVELS_WRONG_EXPORTS}" "old want-levels wrong exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${WANT_LEVELS_INPUT}"
          --function want-levels
          --output "${WANT_LEVELS_WRONG_ASSEMBLY}"
          --exports-output "${WANT_LEVELS_WRONG_EXPORTS}"
          --symbol goalpad_aot_want_levels
  RESULT_VARIABLE WANT_LEVELS_WRONG_SYMBOL_RESULT)
if(WANT_LEVELS_WRONG_SYMBOL_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted want-levels with an unsupported symbol")
endif()

file(READ "${WANT_LEVELS_WRONG_ASSEMBLY}" ACTUAL_WANT_LEVELS_WRONG_ASSEMBLY)
file(READ "${WANT_LEVELS_WRONG_EXPORTS}" ACTUAL_WANT_LEVELS_WRONG_EXPORTS)
if(NOT ACTUAL_WANT_LEVELS_WRONG_ASSEMBLY STREQUAL "old want-levels wrong assembly\n" OR
   NOT ACTUAL_WANT_LEVELS_WRONG_EXPORTS STREQUAL "old want-levels wrong exports\n")
  message(FATAL_ERROR "Failed want-levels export validation modified an existing artifact")
endif()

set(WANT_LEVELS_CROSS_ASSEMBLY "${OUTPUT_DIR}/want-levels-cross.s")
set(WANT_LEVELS_CROSS_EXPORTS "${OUTPUT_DIR}/want-levels-cross.exports.inc")
file(WRITE "${WANT_LEVELS_CROSS_ASSEMBLY}" "old want-levels cross assembly\n")
file(WRITE "${WANT_LEVELS_CROSS_EXPORTS}" "old want-levels cross exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${WANT_LEVELS_INPUT}"
          --function want-levels
          --output "${WANT_LEVELS_CROSS_ASSEMBLY}"
          --exports-output "${WANT_LEVELS_CROSS_EXPORTS}"
          --symbol goalpad_aot_load_state_want_vis
  RESULT_VARIABLE WANT_LEVELS_CROSS_ARITY_RESULT)
if(WANT_LEVELS_CROSS_ARITY_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted want-levels with the Export2 symbol")
endif()

file(READ "${WANT_LEVELS_CROSS_ASSEMBLY}" ACTUAL_WANT_LEVELS_CROSS_ASSEMBLY)
file(READ "${WANT_LEVELS_CROSS_EXPORTS}" ACTUAL_WANT_LEVELS_CROSS_EXPORTS)
if(NOT ACTUAL_WANT_LEVELS_CROSS_ASSEMBLY STREQUAL "old want-levels cross assembly\n" OR
   NOT ACTUAL_WANT_LEVELS_CROSS_EXPORTS STREQUAL "old want-levels cross exports\n")
  message(FATAL_ERROR "Failed want-levels cross-arity validation modified an existing artifact")
endif()
