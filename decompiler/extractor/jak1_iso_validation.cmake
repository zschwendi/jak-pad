add_library(jak1-iso-validation STATIC
            "${CMAKE_SOURCE_DIR}/common/versions/jak1_iso_revisions.cpp"
            "${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_iso_validation.cpp")
target_include_directories(jak1-iso-validation PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(jak1-iso-validation PUBLIC safe-iso-reader)
target_compile_features(jak1-iso-validation PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(jak1-iso-validation PRIVATE /W4 /WX)
else()
  target_compile_options(jak1-iso-validation PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(jak1-iso-validation-test
                   "${CMAKE_SOURCE_DIR}/test/common/test_jak1_iso_validation.cpp")
    target_link_libraries(jak1-iso-validation-test PRIVATE jak1-iso-validation)
    target_compile_features(jak1-iso-validation-test PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(jak1-iso-validation-test PRIVATE /W4 /WX)
    else()
      target_compile_options(jak1-iso-validation-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME jak1-iso-validation-test COMMAND jak1-iso-validation-test)
  endif()
endif()
