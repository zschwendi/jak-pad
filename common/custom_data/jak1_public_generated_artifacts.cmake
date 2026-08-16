include("${CMAKE_SOURCE_DIR}/common/custom_data/jak1_output_graph.cmake")

if(NOT TARGET jak1-public-generated-artifacts)
  add_library(jak1-public-generated-artifacts STATIC
              "${CMAKE_SOURCE_DIR}/common/custom_data/GoalDataObjectBuilder.cpp"
              "${CMAKE_SOURCE_DIR}/common/custom_data/PublicGeneratedDataObjectCompiler.cpp"
              "${CMAKE_SOURCE_DIR}/common/custom_data/Jak1PublicGeneratedArtifacts.cpp")
  target_include_directories(jak1-public-generated-artifacts PUBLIC "${CMAKE_SOURCE_DIR}")
  target_include_directories(jak1-public-generated-artifacts
                             PRIVATE "${CMAKE_SOURCE_DIR}/third-party/fmt/include")
  target_compile_features(jak1-public-generated-artifacts PUBLIC cxx_std_20)
  target_compile_definitions(jak1-public-generated-artifacts PRIVATE FMT_HEADER_ONLY=1)
  target_link_libraries(jak1-public-generated-artifacts PUBLIC jak1-output-graph)

  if(MSVC)
    target_compile_options(jak1-public-generated-artifacts PRIVATE /W4 /WX)
  else()
    target_compile_options(jak1-public-generated-artifacts
                           PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()

  if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
      add_executable(
        jak1-public-generated-artifacts-test
        "${CMAKE_SOURCE_DIR}/test/common/test_jak1_public_generated_artifacts.cpp")
      target_link_libraries(jak1-public-generated-artifacts-test
                            PRIVATE jak1-public-generated-artifacts)
      target_compile_features(jak1-public-generated-artifacts-test PRIVATE cxx_std_20)
      if(MSVC)
        target_compile_options(jak1-public-generated-artifacts-test PRIVATE /W4 /WX)
      else()
        target_compile_options(jak1-public-generated-artifacts-test
                               PRIVATE -Wall -Wextra -Wpedantic -Werror)
      endif()
      add_test(NAME jak1-public-generated-artifacts-test
               COMMAND jak1-public-generated-artifacts-test)

      add_executable(
        jak1-public-generated-artifacts-exactness-test
        "${CMAKE_SOURCE_DIR}/test/goalc/test_jak1_public_generated_artifacts_exactness.cpp"
        "${CMAKE_SOURCE_DIR}/goalc/data_compiler/DataObjectGeneratorCore.cpp")
      target_link_libraries(jak1-public-generated-artifacts-exactness-test
                            PRIVATE jak1-public-generated-artifacts)
      target_include_directories(jak1-public-generated-artifacts-exactness-test
                                 PRIVATE "${CMAKE_SOURCE_DIR}")
      target_compile_features(jak1-public-generated-artifacts-exactness-test PRIVATE cxx_std_20)
      if(MSVC)
        target_compile_options(jak1-public-generated-artifacts-exactness-test PRIVATE /W4 /WX)
      else()
        target_compile_options(jak1-public-generated-artifacts-exactness-test
                               PRIVATE -Wall -Wextra -Wpedantic -Werror)
      endif()
      add_test(NAME jak1-public-generated-artifacts-exactness-test
               COMMAND jak1-public-generated-artifacts-exactness-test)
    endif()
  endif()
endif()
