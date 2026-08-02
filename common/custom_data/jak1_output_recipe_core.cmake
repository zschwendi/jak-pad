include("${CMAKE_SOURCE_DIR}/common/custom_data/jak1_output_graph.cmake")

if(NOT TARGET jak1-output-recipe-core)
  add_library(jak1-output-recipe-core STATIC
              "${CMAKE_SOURCE_DIR}/goalc/make/Jak1OutputRecipeGenerator.cpp"
              "${CMAKE_SOURCE_DIR}/common/custom_data/Jak1OutputRecipe.cpp"
              "${CMAKE_SOURCE_DIR}/common/versions/jak1_iso_revisions.cpp")
  target_include_directories(jak1-output-recipe-core PUBLIC "${CMAKE_SOURCE_DIR}")
  target_compile_features(jak1-output-recipe-core PUBLIC cxx_std_20)
  target_link_libraries(jak1-output-recipe-core PUBLIC jak1-output-graph)

  if(MSVC)
    target_compile_options(jak1-output-recipe-core PRIVATE /W4 /WX)
  else()
    target_compile_options(jak1-output-recipe-core PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()

  if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
      add_executable(jak1-output-recipe-core-test
                     "${CMAKE_SOURCE_DIR}/test/common/test_jak1_output_recipe_core.cpp")
      target_link_libraries(jak1-output-recipe-core-test PRIVATE jak1-output-recipe-core)
      target_compile_features(jak1-output-recipe-core-test PRIVATE cxx_std_20)
      if(MSVC)
        target_compile_options(jak1-output-recipe-core-test PRIVATE /W4 /WX)
      else()
        target_compile_options(jak1-output-recipe-core-test
                               PRIVATE -Wall -Wextra -Wpedantic -Werror)
      endif()
      add_test(NAME jak1-output-recipe-core-test COMMAND jak1-output-recipe-core-test)
    endif()
  endif()
endif()
