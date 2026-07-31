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
  add_executable(arm64-goal-call-abi-test
                 ${CMAKE_CURRENT_LIST_DIR}/test_arm64_goal_call_abi.cpp
                 ${CMAKE_CURRENT_LIST_DIR}/test_arm64_goal_call_abi.s
                 ${CMAKE_SOURCE_DIR}/game/kernel/asm_funcs_arm64.s)
  target_link_libraries(arm64-goal-call-abi-test gtest_main)
  target_compile_features(arm64-goal-call-abi-test PRIVATE cxx_std_20)
  gtest_discover_tests(arm64-goal-call-abi-test)
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
