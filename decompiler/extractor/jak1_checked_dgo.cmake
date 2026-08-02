add_library(jak1-checked-dgo STATIC
            "${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_checked_dgo.cpp"
            "${CMAKE_SOURCE_DIR}/third-party/lzokay/lzokay.cpp")
target_include_directories(jak1-checked-dgo PUBLIC "${CMAKE_SOURCE_DIR}")
target_compile_features(jak1-checked-dgo PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(jak1-checked-dgo PRIVATE /W4 /WX)
else()
  target_compile_options(jak1-checked-dgo PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(jak1-checked-dgo-test
                   "${CMAKE_SOURCE_DIR}/test/common/test_jak1_checked_dgo.cpp")
    target_link_libraries(jak1-checked-dgo-test PRIVATE jak1-checked-dgo)
    target_compile_features(jak1-checked-dgo-test PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(jak1-checked-dgo-test PRIVATE /W4 /WX)
    else()
      target_compile_options(jak1-checked-dgo-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME jak1-checked-dgo-test COMMAND jak1-checked-dgo-test)
  endif()
endif()
