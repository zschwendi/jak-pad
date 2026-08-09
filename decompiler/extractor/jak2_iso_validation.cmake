add_library(jak2-iso-validation STATIC
            "${CMAKE_SOURCE_DIR}/common/versions/jak2_iso_revisions.cpp"
            "${CMAKE_SOURCE_DIR}/decompiler/extractor/jak2_iso_validation.cpp")
target_include_directories(jak2-iso-validation PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(jak2-iso-validation PUBLIC safe-iso-reader)
target_compile_features(jak2-iso-validation PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(jak2-iso-validation PRIVATE /W4 /WX)
else()
  target_compile_options(jak2-iso-validation PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(jak2-iso-validation-test
                   "${CMAKE_SOURCE_DIR}/test/common/test_jak2_iso_validation.cpp")
    target_link_libraries(jak2-iso-validation-test PRIVATE jak2-iso-validation)
    target_compile_features(jak2-iso-validation-test PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(jak2-iso-validation-test PRIVATE /W4 /WX)
    else()
      target_compile_options(jak2-iso-validation-test
                             PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME jak2-iso-validation-test COMMAND jak2-iso-validation-test)
  endif()
endif()
