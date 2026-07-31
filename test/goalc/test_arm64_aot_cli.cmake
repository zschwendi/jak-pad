file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(INPUT
    "${PROJECT_ROOT}/test/goalc/source_templates/arm64-aot/full-false-func-from-jak1-gcommon.gc")
set(ASSEMBLY_ONLY "${OUTPUT_DIR}/assembly-only.s")
set(PAIRED_ASSEMBLY "${OUTPUT_DIR}/paired.s")
set(EXPORTS "${OUTPUT_DIR}/paired.exports.inc")

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
