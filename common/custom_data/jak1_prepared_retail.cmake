add_library(jak1-prepared-retail STATIC
            "${CMAKE_SOURCE_DIR}/common/custom_data/Jak1PreparedRetail.cpp"
            "${CMAKE_SOURCE_DIR}/common/versions/jak1_iso_revisions.cpp")
target_include_directories(jak1-prepared-retail PUBLIC "${CMAKE_SOURCE_DIR}")
target_compile_features(jak1-prepared-retail PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(jak1-prepared-retail PRIVATE /W4 /WX)
else()
  target_compile_options(jak1-prepared-retail PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(jak1-prepared-retail-test
                   "${CMAKE_SOURCE_DIR}/test/common/test_jak1_prepared_retail.cpp")
    target_link_libraries(jak1-prepared-retail-test PRIVATE jak1-prepared-retail)
    target_compile_features(jak1-prepared-retail-test PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(jak1-prepared-retail-test PRIVATE /W4 /WX)
    else()
      target_compile_options(jak1-prepared-retail-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME jak1-prepared-retail-test COMMAND jak1-prepared-retail-test)
  endif()
endif()
