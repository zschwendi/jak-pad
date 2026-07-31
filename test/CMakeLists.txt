include(${CMAKE_CURRENT_LIST_DIR}/goalc/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/offline/CMakeLists.txt)

add_executable(goalc-test
        ${CMAKE_CURRENT_LIST_DIR}/emitter_util.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_main.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_reader.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_goos.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_listener_deci2.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_kernel_jak1.cpp
        ${CMAKE_CURRENT_LIST_DIR}/all_jak1_symbols.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_type_system.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_x86.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_avx.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_neon.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_emitter_arm64.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_common_util.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_pretty_print.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_math.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_zstd.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_zydis.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/FormRegressionTest.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_AtomicOpBuilder.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_FormBeforeExpressions.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_FormExpressionBuild.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_FormExpressionBuild2.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_FormExpressionBuild3.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_FormExpressionBuildLong.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_InstructionDecode.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_InstructionParser.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_gkernel_jak1_decomp.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_math_decomp.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_DataParser.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_DisasmVifDecompile.cpp
        ${CMAKE_CURRENT_LIST_DIR}/decompiler/test_VuDisasm.cpp
        ${CMAKE_CURRENT_LIST_DIR}/common/formatter/test_formatter.cpp
        ${GOALC_TEST_FRAMEWORK_SOURCES}
        ${GOALC_TEST_CASES}
        )

target_link_libraries(goalc-test common runtime compiler gtest decomp Zydis libzstd_static tree-sitter)

if(BUILD_TESTING AND GOAL_HOST_TESTS_ENABLED)
  add_executable(jak1-data-arena-test ${CMAKE_CURRENT_LIST_DIR}/test_jak1_data_arena.cpp)
  target_link_libraries(jak1-data-arena-test jak1-data-arena gtest_main)
  gtest_discover_tests(jak1-data-arena-test)
endif()

if(BUILD_TESTING AND GOAL_HOST_TESTS_ENABLED AND APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
  enable_language(ASM)

  set(ARM64_GOAL_CALL_AOT_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/generated-arm64-goal-call")
  set(ARM64_GOAL_CALL_AOT_SOURCE_DIR
      "${CMAKE_SOURCE_DIR}/test/goalc/source_templates/arm64-aot")
  set(ARM64_GOAL_CALL_AOT_FALSE_ASSEMBLY
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1FalseFunc.s")
  set(ARM64_GOAL_CALL_AOT_FALSE_EXPORTS
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1FalseFunc.exports.h")
  set(ARM64_GOAL_CALL_AOT_TRUE_ASSEMBLY
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1TrueFunc.s")
  set(ARM64_GOAL_CALL_AOT_TRUE_EXPORTS
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1TrueFunc.exports.h")
  set(ARM64_GOAL_CALL_AOT_IDENTITY_ASSEMBLY
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1Identity.s")
  set(ARM64_GOAL_CALL_AOT_IDENTITY_EXPORTS
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1Identity.exports.h")
  set(ARM64_GOAL_CALL_AOT_LOGNOT_ASSEMBLY
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1Lognot.s")
  set(ARM64_GOAL_CALL_AOT_LOGNOT_EXPORTS
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1Lognot.exports.h")
  set(ARM64_GOAL_CALL_AOT_GLST_NODE_NAME_ASSEMBLY
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1GlstNodeName.s")
  set(ARM64_GOAL_CALL_AOT_GLST_NODE_NAME_EXPORTS
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1GlstNodeName.exports.h")
  set(ARM64_GOAL_CALL_AOT_LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1LevelGroupLoadCommandsSet.s")
  set(ARM64_GOAL_CALL_AOT_LEVEL_GROUP_LOAD_COMMANDS_SET_EXPORTS
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1LevelGroupLoadCommandsSet.exports.h")
  set(ARM64_GOAL_CALL_AOT_WANT_VIS_ASSEMBLY
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1WantVis.s")
  set(ARM64_GOAL_CALL_AOT_WANT_VIS_EXPORTS
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1WantVis.exports.h")
  set(ARM64_GOAL_CALL_AOT_WANT_LEVELS_ASSEMBLY
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1WantLevels.s")
  set(ARM64_GOAL_CALL_AOT_WANT_LEVELS_EXPORTS
      "${ARM64_GOAL_CALL_AOT_DIR}/OpenGOALJak1WantLevels.exports.h")
  set(ARM64_GOAL_CALL_AOT_ASSEMBLIES
      ${ARM64_GOAL_CALL_AOT_FALSE_ASSEMBLY}
      ${ARM64_GOAL_CALL_AOT_TRUE_ASSEMBLY}
      ${ARM64_GOAL_CALL_AOT_IDENTITY_ASSEMBLY}
      ${ARM64_GOAL_CALL_AOT_LOGNOT_ASSEMBLY}
      ${ARM64_GOAL_CALL_AOT_GLST_NODE_NAME_ASSEMBLY}
      ${ARM64_GOAL_CALL_AOT_LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY}
      ${ARM64_GOAL_CALL_AOT_WANT_VIS_ASSEMBLY}
      ${ARM64_GOAL_CALL_AOT_WANT_LEVELS_ASSEMBLY})
  set(ARM64_GOAL_CALL_AOT_EXPORTS
      ${ARM64_GOAL_CALL_AOT_FALSE_EXPORTS}
      ${ARM64_GOAL_CALL_AOT_TRUE_EXPORTS}
      ${ARM64_GOAL_CALL_AOT_IDENTITY_EXPORTS}
      ${ARM64_GOAL_CALL_AOT_LOGNOT_EXPORTS}
      ${ARM64_GOAL_CALL_AOT_GLST_NODE_NAME_EXPORTS}
      ${ARM64_GOAL_CALL_AOT_LEVEL_GROUP_LOAD_COMMANDS_SET_EXPORTS}
      ${ARM64_GOAL_CALL_AOT_WANT_VIS_EXPORTS}
      ${ARM64_GOAL_CALL_AOT_WANT_LEVELS_EXPORTS})

  add_custom_command(
      OUTPUT ${ARM64_GOAL_CALL_AOT_ASSEMBLIES} ${ARM64_GOAL_CALL_AOT_EXPORTS}
      COMMAND ${CMAKE_COMMAND} -E make_directory "${ARM64_GOAL_CALL_AOT_DIR}"
      COMMAND $<TARGET_FILE:goalc-aot>
              --project-path "${CMAKE_SOURCE_DIR}"
              --input
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-false-func-from-jak1-gcommon.gc"
              --function false-func
              --output "${ARM64_GOAL_CALL_AOT_FALSE_ASSEMBLY}"
              --exports-output "${ARM64_GOAL_CALL_AOT_FALSE_EXPORTS}"
              --symbol goalpad_aot_false_func
      COMMAND $<TARGET_FILE:goalc-aot>
              --project-path "${CMAKE_SOURCE_DIR}"
              --input
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-true-func-from-jak1-gcommon.gc"
              --function true-func
              --output "${ARM64_GOAL_CALL_AOT_TRUE_ASSEMBLY}"
              --exports-output "${ARM64_GOAL_CALL_AOT_TRUE_EXPORTS}"
              --symbol goalpad_aot_true_func
      COMMAND $<TARGET_FILE:goalc-aot>
              --project-path "${CMAKE_SOURCE_DIR}"
              --input
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-identity-from-jak1-gcommon.gc"
              --function identity
              --output "${ARM64_GOAL_CALL_AOT_IDENTITY_ASSEMBLY}"
              --exports-output "${ARM64_GOAL_CALL_AOT_IDENTITY_EXPORTS}"
              --symbol goalpad_aot_identity
      COMMAND $<TARGET_FILE:goalc-aot>
              --project-path "${CMAKE_SOURCE_DIR}"
              --input
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-lognot-from-jak1-gcommon.gc"
              --function lognot
              --output "${ARM64_GOAL_CALL_AOT_LOGNOT_ASSEMBLY}"
              --exports-output "${ARM64_GOAL_CALL_AOT_LOGNOT_EXPORTS}"
              --symbol goalpad_aot_lognot
      COMMAND $<TARGET_FILE:goalc-aot>
              --project-path "${CMAKE_SOURCE_DIR}"
              --input
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-glst-node-name-from-jak1-glist-h.gc"
              --function glst-node-name
              --output "${ARM64_GOAL_CALL_AOT_GLST_NODE_NAME_ASSEMBLY}"
              --exports-output "${ARM64_GOAL_CALL_AOT_GLST_NODE_NAME_EXPORTS}"
              --symbol goalpad_aot_glst_node_name
      COMMAND $<TARGET_FILE:goalc-aot>
              --project-path "${CMAKE_SOURCE_DIR}"
              --input
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-level-group-load-commands-set-from-jak1-level.gc"
              --function level-group-load-commands-set!
              --output "${ARM64_GOAL_CALL_AOT_LEVEL_GROUP_LOAD_COMMANDS_SET_ASSEMBLY}"
              --exports-output "${ARM64_GOAL_CALL_AOT_LEVEL_GROUP_LOAD_COMMANDS_SET_EXPORTS}"
              --symbol goalpad_aot_level_group_load_commands_set
      COMMAND $<TARGET_FILE:goalc-aot>
              --project-path "${CMAKE_SOURCE_DIR}"
              --input
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-want-vis-from-jak1-load-boundary.gc"
              --function want-vis
              --output "${ARM64_GOAL_CALL_AOT_WANT_VIS_ASSEMBLY}"
              --exports-output "${ARM64_GOAL_CALL_AOT_WANT_VIS_EXPORTS}"
              --symbol goalpad_aot_load_state_want_vis
      COMMAND $<TARGET_FILE:goalc-aot>
              --project-path "${CMAKE_SOURCE_DIR}"
              --input
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-want-levels-from-jak1-load-boundary.gc"
              --function want-levels
              --output "${ARM64_GOAL_CALL_AOT_WANT_LEVELS_ASSEMBLY}"
              --exports-output "${ARM64_GOAL_CALL_AOT_WANT_LEVELS_EXPORTS}"
              --symbol goalpad_aot_load_state_want_levels
      DEPENDS
              goalc-aot
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-false-func-from-jak1-gcommon.gc"
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-true-func-from-jak1-gcommon.gc"
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-identity-from-jak1-gcommon.gc"
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-lognot-from-jak1-gcommon.gc"
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-glst-node-name-from-jak1-glist-h.gc"
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-level-group-load-commands-set-from-jak1-level.gc"
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-want-vis-from-jak1-load-boundary.gc"
              "${ARM64_GOAL_CALL_AOT_SOURCE_DIR}/full-want-levels-from-jak1-load-boundary.gc"
      COMMENT "Generating Jak 1 ARM64 AOT call artifacts"
      VERBATIM)

  add_custom_target(
      arm64-goal-call-aot-artifacts
      DEPENDS ${ARM64_GOAL_CALL_AOT_ASSEMBLIES} ${ARM64_GOAL_CALL_AOT_EXPORTS})
  set_source_files_properties(
      ${ARM64_GOAL_CALL_AOT_ASSEMBLIES} ${ARM64_GOAL_CALL_AOT_EXPORTS}
      PROPERTIES GENERATED TRUE)
  set_source_files_properties(
      ${ARM64_GOAL_CALL_AOT_EXPORTS}
      PROPERTIES HEADER_FILE_ONLY TRUE)

  add_executable(arm64-goal-call-abi-test
                 ${CMAKE_CURRENT_LIST_DIR}/test_arm64_goal_call_abi.cpp
                 ${CMAKE_CURRENT_LIST_DIR}/test_arm64_goal_call_abi.s
                 ${CMAKE_SOURCE_DIR}/game/kernel/asm_funcs_arm64.s
                 ${ARM64_GOAL_CALL_AOT_ASSEMBLIES}
                 ${ARM64_GOAL_CALL_AOT_EXPORTS})
  add_dependencies(arm64-goal-call-abi-test arm64-goal-call-aot-artifacts)
  target_include_directories(arm64-goal-call-abi-test
                             PRIVATE "${ARM64_GOAL_CALL_AOT_DIR}")
  target_link_libraries(arm64-goal-call-abi-test jak1-data-arena gtest_main)
  target_compile_features(arm64-goal-call-abi-test PRIVATE cxx_std_20)
  gtest_discover_tests(arm64-goal-call-abi-test PROPERTIES RUN_SERIAL TRUE)
endif()

add_executable(test_image_resize ${CMAKE_CURRENT_LIST_DIR}/common/test_image_resize.cpp)
target_link_libraries(test_image_resize common)

if(WIN32)
  target_link_libraries(goalc-test mman)
endif()

#gtest_discover_tests(goalc-test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

if(UNIX AND CMAKE_COMPILER_IS_GNUCXX AND CODE_COVERAGE)
  set(BUILD_ARGS "-w" "dupbuild=warn")
  include(CodeCoverage)
  append_coverage_compiler_flags()
  setup_target_for_coverage_lcov(NAME goalc-test_coverage
                                 EXECUTABLE goalc-test --gtest_color=yes
                                 DEPENDENCIES goalc-test
                                 EXCLUDE "third-party/*" "/usr/include/*")
endif()
