include("${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_extracted_generated_inputs.cmake")
include("${CMAKE_SOURCE_DIR}/decompiler/extractor/jak2_extracted_generated_inputs.cmake")

if(NOT TARGET jak2-public-generated-artifacts)
  add_library(jak2-public-generated-artifacts STATIC
              "${CMAKE_SOURCE_DIR}/common/custom_data/Jak2PublicGeneratedArtifacts.cpp")
  target_include_directories(jak2-public-generated-artifacts PUBLIC "${CMAKE_SOURCE_DIR}")
  target_compile_features(jak2-public-generated-artifacts PUBLIC cxx_std_20)
  target_link_libraries(jak2-public-generated-artifacts
                        PUBLIC common
                               jak1-extracted-generated-inputs
                               jak2-extracted-generated-inputs)

  if(MSVC)
    target_compile_options(jak2-public-generated-artifacts PRIVATE /W4 /WX)
  else()
    target_compile_options(jak2-public-generated-artifacts
                           PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()

  if(BUILD_TESTING AND GOAL_HOST_TESTS_ENABLED)
    add_executable(jak2-public-generated-artifacts-test
                   "${CMAKE_SOURCE_DIR}/test/common/test_jak2_public_generated_artifacts.cpp"
                   "${CMAKE_SOURCE_DIR}/goalc/data_compiler/DataObjectGeneratorCore.cpp"
                   "${CMAKE_SOURCE_DIR}/goalc/data_compiler/game_text_common.cpp")
    target_link_libraries(jak2-public-generated-artifacts-test
                          PRIVATE jak2-public-generated-artifacts)
    target_compile_definitions(jak2-public-generated-artifacts-test
                               PRIVATE OPENGOAL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_compile_features(jak2-public-generated-artifacts-test PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(jak2-public-generated-artifacts-test PRIVATE /W4 /WX)
    else()
      target_compile_options(jak2-public-generated-artifacts-test
                             PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME jak2-public-generated-artifacts-test
             COMMAND jak2-public-generated-artifacts-test)
  endif()
endif()
