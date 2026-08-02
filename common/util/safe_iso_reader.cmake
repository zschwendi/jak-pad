add_library(safe-iso-reader STATIC
            "${CMAKE_SOURCE_DIR}/common/util/read_iso_file.cpp")
target_include_directories(safe-iso-reader PUBLIC "${CMAKE_SOURCE_DIR}")
target_compile_features(safe-iso-reader PUBLIC cxx_std_20)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(safe-iso-reader-test
                   "${CMAKE_SOURCE_DIR}/test/common/test_safe_iso_reader.cpp")
    target_link_libraries(safe-iso-reader-test PRIVATE safe-iso-reader)
    target_compile_features(safe-iso-reader-test PRIVATE cxx_std_20)
    add_test(NAME safe-iso-reader-test COMMAND safe-iso-reader-test)
  endif()
endif()
