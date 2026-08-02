if(NOT TARGET jak1-output-graph)
  set(OPENGOAL_JAK1_OUTPUT_GRAPH_GENERATED_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/jak1-output-graph-generated")
  set(OPENGOAL_JAK1_OUTPUT_GRAPH_GENERATED_INCLUDE
      "${OPENGOAL_JAK1_OUTPUT_GRAPH_GENERATED_DIR}/Jak1PublicOutputGraphData.inc")
  add_custom_command(
      OUTPUT "${OPENGOAL_JAK1_OUTPUT_GRAPH_GENERATED_INCLUDE}"
      COMMAND "${CMAKE_COMMAND}"
              -DINPUT=${CMAKE_SOURCE_DIR}/common/custom_data/Jak1PublicOutputGraph.bin
              -DOUTPUT=${OPENGOAL_JAK1_OUTPUT_GRAPH_GENERATED_INCLUDE}
              -P "${CMAKE_SOURCE_DIR}/cmake/embed_binary_as_bytes.cmake"
      DEPENDS "${CMAKE_SOURCE_DIR}/common/custom_data/Jak1PublicOutputGraph.bin"
              "${CMAKE_SOURCE_DIR}/cmake/embed_binary_as_bytes.cmake"
      VERBATIM)

  add_library(jak1-output-graph STATIC
              "${CMAKE_SOURCE_DIR}/common/custom_data/Jak1OutputGraph.cpp"
              "${CMAKE_SOURCE_DIR}/common/custom_data/Jak1PublicOutputGraph.cpp"
              "${OPENGOAL_JAK1_OUTPUT_GRAPH_GENERATED_INCLUDE}")
  target_include_directories(jak1-output-graph PUBLIC "${CMAKE_SOURCE_DIR}")
  target_include_directories(jak1-output-graph
                             PRIVATE "${OPENGOAL_JAK1_OUTPUT_GRAPH_GENERATED_DIR}")
  target_compile_features(jak1-output-graph PUBLIC cxx_std_20)

  add_library(jak1-output-recipe-core STATIC
              "${CMAKE_SOURCE_DIR}/goalc/make/Jak1OutputRecipeGenerator.cpp"
              "${CMAKE_SOURCE_DIR}/common/custom_data/Jak1OutputRecipe.cpp"
              "${CMAKE_SOURCE_DIR}/common/versions/jak1_iso_revisions.cpp")
  target_include_directories(jak1-output-recipe-core PUBLIC "${CMAKE_SOURCE_DIR}")
  target_compile_features(jak1-output-recipe-core PUBLIC cxx_std_20)
  target_link_libraries(jak1-output-recipe-core PUBLIC jak1-output-graph)

  foreach(OPENGOAL_RECIPE_CORE_TARGET jak1-output-graph jak1-output-recipe-core)
    if(MSVC)
      target_compile_options(${OPENGOAL_RECIPE_CORE_TARGET} PRIVATE /W4 /WX)
    else()
      target_compile_options(${OPENGOAL_RECIPE_CORE_TARGET}
                             PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
  endforeach()

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
