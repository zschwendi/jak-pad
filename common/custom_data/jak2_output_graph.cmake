include("${CMAKE_SOURCE_DIR}/common/custom_data/jak1_output_graph.cmake")

if(NOT TARGET jak2-output-graph)
  set(OPENGOAL_JAK2_OUTPUT_GRAPH_GENERATED_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/jak2-output-graph-generated")
  set(OPENGOAL_JAK2_OUTPUT_GRAPH_GENERATED_INCLUDE
      "${OPENGOAL_JAK2_OUTPUT_GRAPH_GENERATED_DIR}/Jak2PublicOutputGraphData.inc")
  add_custom_command(
      OUTPUT "${OPENGOAL_JAK2_OUTPUT_GRAPH_GENERATED_INCLUDE}"
      COMMAND "${CMAKE_COMMAND}"
              -DINPUT=${CMAKE_SOURCE_DIR}/common/custom_data/Jak2PublicOutputGraph.bin
              -DOUTPUT=${OPENGOAL_JAK2_OUTPUT_GRAPH_GENERATED_INCLUDE}
              -P "${CMAKE_SOURCE_DIR}/cmake/embed_binary_as_bytes.cmake"
      DEPENDS "${CMAKE_SOURCE_DIR}/common/custom_data/Jak2PublicOutputGraph.bin"
              "${CMAKE_SOURCE_DIR}/cmake/embed_binary_as_bytes.cmake"
      VERBATIM)

  add_library(jak2-output-graph STATIC
              "${CMAKE_SOURCE_DIR}/common/custom_data/Jak2PublicOutputGraph.cpp"
              "${OPENGOAL_JAK2_OUTPUT_GRAPH_GENERATED_INCLUDE}")
  target_include_directories(jak2-output-graph PUBLIC "${CMAKE_SOURCE_DIR}")
  target_include_directories(jak2-output-graph
                             PRIVATE "${OPENGOAL_JAK2_OUTPUT_GRAPH_GENERATED_DIR}")
  target_compile_features(jak2-output-graph PUBLIC cxx_std_20)
  target_link_libraries(jak2-output-graph PUBLIC jak1-output-graph)
  if(MSVC)
    target_compile_options(jak2-output-graph PRIVATE /W4 /WX)
  else()
    target_compile_options(jak2-output-graph PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
