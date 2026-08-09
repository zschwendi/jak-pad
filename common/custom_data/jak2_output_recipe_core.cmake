include("${CMAKE_SOURCE_DIR}/common/custom_data/jak1_output_recipe_core.cmake")
include("${CMAKE_SOURCE_DIR}/common/custom_data/jak2_output_graph.cmake")

if(NOT TARGET jak2-output-recipe-core)
  add_library(jak2-output-recipe-core STATIC
              "${CMAKE_SOURCE_DIR}/common/custom_data/Jak2OutputRecipe.cpp"
              "${CMAKE_SOURCE_DIR}/goalc/make/Jak2OutputRecipeGenerator.cpp")
  target_include_directories(jak2-output-recipe-core PUBLIC "${CMAKE_SOURCE_DIR}")
  target_compile_features(jak2-output-recipe-core PUBLIC cxx_std_20)
  target_link_libraries(jak2-output-recipe-core
                        PUBLIC jak1-output-recipe-core jak2-output-graph)

  if(MSVC)
    target_compile_options(jak2-output-recipe-core PRIVATE /W4 /WX)
  else()
    target_compile_options(jak2-output-recipe-core PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()

  if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
      add_executable(jak2-output-recipe-test
                     "${CMAKE_SOURCE_DIR}/test/common/test_jak2_output_recipe.cpp")
      target_link_libraries(jak2-output-recipe-test PRIVATE jak2-output-recipe-core)
      target_compile_features(jak2-output-recipe-test PRIVATE cxx_std_20)
      if(MSVC)
        target_compile_options(jak2-output-recipe-test PRIVATE /W4 /WX)
      else()
        target_compile_options(jak2-output-recipe-test
                               PRIVATE -Wall -Wextra -Wpedantic -Werror)
      endif()
      add_test(NAME jak2-output-recipe-test COMMAND jak2-output-recipe-test)
    endif()
  endif()
endif()
