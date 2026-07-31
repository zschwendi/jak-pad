file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(GENERATED_C "${OUTPUT_DIR}/gcommon.c")
set(GENERATED_H "${OUTPUT_DIR}/gcommon_generated.h")
set(REPORT "${OUTPUT_DIR}/gcommon.report.txt")
set(TEST_BINARY "${OUTPUT_DIR}/gcommon_c_backend_test")

execute_process(
  COMMAND "${GOALC_CBACKEND}"
          --project-path "${PROJECT_ROOT}"
          --tag gcommon
          --output "${GENERATED_C}"
          --header "${GENERATED_H}"
          --report "${REPORT}"
          "${PROJECT_ROOT}/goal_src/jak1/compiler-setup.gc"
          "${PROJECT_ROOT}/goal_src/jak1/kernel-defs.gc"
          "${PROJECT_ROOT}/goal_src/jak1/kernel/gcommon.gc"
  RESULT_VARIABLE EMIT_RESULT)
if(NOT EMIT_RESULT EQUAL 0)
  message(FATAL_ERROR "goalc-cbackend failed on jak1 gcommon.gc")
endif()

file(READ "${REPORT}" REPORT_TEXT)
if(REPORT_TEXT MATCHES "FAILED")
  message(FATAL_ERROR "goalc-cbackend could not translate every gcommon.gc function:\n${REPORT_TEXT}")
endif()

# -fno-strict-aliasing: GOAL reads and writes the same memory at several widths, exactly like the
# x86-64 backend does, so the generated C must not be optimized under strict aliasing rules.
execute_process(
  COMMAND "${C_COMPILER}"
          -O2 -fno-strict-aliasing -std=c11
          -I "${PROJECT_ROOT}" -I "${OUTPUT_DIR}"
          -o "${TEST_BINARY}"
          "${GENERATED_C}"
          "${PROJECT_ROOT}/test/goalc/aot/goal_c_test_loader.c"
          "${PROJECT_ROOT}/test/goalc/aot/gcommon_c_backend_test.c"
  RESULT_VARIABLE HOST_BUILD_RESULT)
if(NOT HOST_BUILD_RESULT EQUAL 0)
  message(FATAL_ERROR "Host compilation of the generated C failed")
endif()

execute_process(COMMAND "${TEST_BINARY}" RESULT_VARIABLE HOST_RUN_RESULT)
if(NOT HOST_RUN_RESULT EQUAL 0)
  message(FATAL_ERROR "Generated gcommon.gc functions produced wrong results on the host")
endif()

if(APPLE)
  execute_process(
    COMMAND xcrun --sdk iphoneos --show-sdk-path
    OUTPUT_VARIABLE IOS_SDK
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE IOS_SDK_RESULT
    ERROR_QUIET)
  if(IOS_SDK_RESULT EQUAL 0 AND IOS_SDK)
    execute_process(
      COMMAND "${C_COMPILER}"
              -c -O2 -fno-strict-aliasing -std=c11
              -target arm64-apple-ios15.0 -isysroot "${IOS_SDK}"
              -I "${PROJECT_ROOT}" -I "${OUTPUT_DIR}"
              -o "${OUTPUT_DIR}/gcommon_ios.o"
              "${GENERATED_C}"
      RESULT_VARIABLE IOS_BUILD_RESULT)
    if(NOT IOS_BUILD_RESULT EQUAL 0)
      message(FATAL_ERROR "arm64-apple-ios compilation of the generated C failed")
    endif()
  else()
    message(STATUS "No iphoneos SDK available; skipped the arm64-apple-ios compile check")
  endif()
endif()
