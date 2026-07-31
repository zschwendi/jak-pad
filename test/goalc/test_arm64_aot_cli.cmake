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

file(WRITE "${TRUE_PAIRED_ASSEMBLY}" "old true assembly\n")
file(WRITE "${TRUE_EXPORTS}" "old true exports\n")
execute_process(
  COMMAND "${GOALC_AOT}"
          --project-path "${PROJECT_ROOT}"
          --input "${TRUE_INPUT}"
          --function true-func
          --output "${TRUE_PAIRED_ASSEMBLY}"
          --exports-output "${TRUE_EXPORTS}"
          --symbol goalpad_aot_true_func
  RESULT_VARIABLE TRUE_PAIRED_RESULT)
if(TRUE_PAIRED_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-aot accepted true-func native export metadata")
endif()

file(READ "${TRUE_PAIRED_ASSEMBLY}" ACTUAL_TRUE_PAIRED_ASSEMBLY)
file(READ "${TRUE_EXPORTS}" ACTUAL_TRUE_EXPORTS)
if(NOT ACTUAL_TRUE_PAIRED_ASSEMBLY STREQUAL "old true assembly\n" OR
   NOT ACTUAL_TRUE_EXPORTS STREQUAL "old true exports\n")
  message(FATAL_ERROR "Rejected true-func native export modified an existing artifact")
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
