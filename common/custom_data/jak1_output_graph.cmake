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
  if(MSVC)
    target_compile_options(jak1-output-graph PRIVATE /W4 /WX)
  else()
    target_compile_options(jak1-output-graph PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
