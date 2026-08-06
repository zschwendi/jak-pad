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
  add_executable(dma-chain-validation-test
                 ${CMAKE_CURRENT_LIST_DIR}/common/test_dma_chain_validation.cpp)
  target_compile_definitions(dma-chain-validation-test PRIVATE NO_ASSERT=1)
  target_link_libraries(dma-chain-validation-test PRIVATE common)
  add_test(NAME dma-chain-validation COMMAND dma-chain-validation-test)

  add_executable(jak1-data-arena-test ${CMAKE_CURRENT_LIST_DIR}/test_jak1_data_arena.cpp)
  target_link_libraries(jak1-data-arena-test jak1-data-arena gtest_main)
  gtest_discover_tests(jak1-data-arena-test)

  add_executable(jak1-output-recipe-test
                 ${CMAKE_CURRENT_LIST_DIR}/common/test_jak1_output_recipe.cpp)
  target_link_libraries(jak1-output-recipe-test
                        PRIVATE jak1-output-recipe-generator)
  target_compile_features(jak1-output-recipe-test PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(jak1-output-recipe-test PRIVATE /W4 /WX)
  else()
    target_compile_options(jak1-output-recipe-test
                           PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME jak1-output-recipe COMMAND jak1-output-recipe-test)

  add_executable(jak1-output-recipe-generator-test
                 ${CMAKE_CURRENT_LIST_DIR}/goalc/test_jak1_output_recipe_generator.cpp)
  target_link_libraries(jak1-output-recipe-generator-test
                        PRIVATE jak1-output-recipe-generator)
  target_compile_features(jak1-output-recipe-generator-test PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(jak1-output-recipe-generator-test PRIVATE /W4 /WX)
  else()
    target_compile_options(jak1-output-recipe-generator-test
                           PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME jak1-output-recipe-generator COMMAND jak1-output-recipe-generator-test)

  add_executable(jak1-output-recipe-graph-test
                 ${CMAKE_CURRENT_LIST_DIR}/goalc/test_jak1_output_recipe_graph.cpp)
  target_link_libraries(jak1-output-recipe-graph-test
                        PRIVATE jak1-output-recipe-generator)
  target_compile_features(jak1-output-recipe-graph-test PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(jak1-output-recipe-graph-test PRIVATE /W4 /WX)
  else()
    target_compile_options(jak1-output-recipe-graph-test
                           PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME jak1-output-recipe-graph COMMAND jak1-output-recipe-graph-test)
  set_tests_properties(jak1-output-recipe-graph
                       PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  # Maintainer-only proof requiring caller-supplied retail data; deliberately not a CTest entry.
  add_executable(jak1-output-recipe-real-catalog-test
                 ${CMAKE_CURRENT_LIST_DIR}/goalc/test_jak1_output_recipe_real_catalog.cpp
                 ${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_checked_dgo.cpp
                 ${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_retail_object_catalog.cpp
                 ${CMAKE_SOURCE_DIR}/third-party/lzokay/lzokay.cpp
                 ${CMAKE_SOURCE_DIR}/third-party/zstd/lib/common/xxhash.c)
  target_link_libraries(jak1-output-recipe-real-catalog-test
                        PRIVATE jak1-output-recipe-core)
  target_compile_features(jak1-output-recipe-real-catalog-test PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(jak1-output-recipe-real-catalog-test PRIVATE /W4 /WX)
  else()
    target_compile_options(jak1-output-recipe-real-catalog-test
                           PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()

if(BUILD_TESTING AND GOAL_HOST_TESTS_ENABLED AND TARGET jak1-data-arena-arm64-invoker)
  enable_language(ASM)

  # Jak 1 GOAL functions, frozen as ARM64 assembly. They are payloads for the runtime GOAL call
  # ABI and its native-method dispatcher, not compiler output under test: the compiler that once
  # generated them was removed when the AOT C backend replaced it.
  set(ARM64_GOAL_CALL_FIXTURE_DIR
      "${CMAKE_CURRENT_LIST_DIR}/fixtures/arm64-goal-call")
  set(ARM64_GOAL_CALL_FIXTURE_NAMES
      OpenGOALJak1FalseFunc
      OpenGOALJak1TrueFunc
      OpenGOALJak1Identity
      OpenGOALJak1Lognot
      OpenGOALJak1GlstNodeName
      OpenGOALJak1LevelGroupLoadCommandsSet
      OpenGOALJak1WantVis
      OpenGOALJak1WantLevels
      OpenGOALJak1LoadStateReset
      OpenGOALJak1LoadStateValue)
  set(ARM64_GOAL_CALL_ASSEMBLIES "")
  foreach(fixture ${ARM64_GOAL_CALL_FIXTURE_NAMES})
    list(APPEND ARM64_GOAL_CALL_ASSEMBLIES
         "${ARM64_GOAL_CALL_FIXTURE_DIR}/${fixture}.s")
  endforeach()

  add_executable(arm64-goal-call-abi-test
                 ${CMAKE_CURRENT_LIST_DIR}/test_arm64_goal_call_abi.cpp
                 ${CMAKE_CURRENT_LIST_DIR}/test_arm64_goal_call_abi.s
                 ${CMAKE_SOURCE_DIR}/game/kernel/asm_funcs_arm64.s
                 ${ARM64_GOAL_CALL_ASSEMBLIES})
  target_include_directories(arm64-goal-call-abi-test
                             PRIVATE "${ARM64_GOAL_CALL_FIXTURE_DIR}")
  target_link_libraries(arm64-goal-call-abi-test
                        PRIVATE jak1-data-arena-arm64-invoker gtest_main)
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
