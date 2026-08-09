include("${CMAKE_SOURCE_DIR}/common/custom_data/jak2_output_recipe_core.cmake")
include("${CMAKE_SOURCE_DIR}/common/custom_data/jak1_output_materializer.cmake")

if(NOT TARGET jak2-output-materializer)
  add_library(jak2-output-materializer STATIC
              "${CMAKE_SOURCE_DIR}/common/custom_data/Jak2OutputMaterializer.cpp")
  target_include_directories(jak2-output-materializer PUBLIC "${CMAKE_SOURCE_DIR}")
  target_compile_features(jak2-output-materializer PUBLIC cxx_std_20)
  target_link_libraries(jak2-output-materializer
                        PUBLIC jak1-output-materializer jak2-output-recipe-core)

  if(MSVC)
    target_compile_options(jak2-output-materializer PRIVATE /W4 /WX)
  else()
    target_compile_options(jak2-output-materializer PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()

  if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
      add_executable(jak2-output-materializer-test
                     "${CMAKE_SOURCE_DIR}/test/common/test_jak2_output_materializer.cpp")
      target_link_libraries(jak2-output-materializer-test PRIVATE jak2-output-materializer)
      target_compile_features(jak2-output-materializer-test PRIVATE cxx_std_20)
      if(MSVC)
        target_compile_options(jak2-output-materializer-test PRIVATE /W4 /WX)
      else()
        target_compile_options(jak2-output-materializer-test
                               PRIVATE -Wall -Wextra -Wpedantic -Werror)
      endif()
      add_test(NAME jak2-output-materializer-test COMMAND jak2-output-materializer-test)
    endif()
  endif()
endif()
