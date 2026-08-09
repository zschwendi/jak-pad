include("${CMAKE_SOURCE_DIR}/common/custom_data/jak2_output_graph.cmake")

if(NOT TARGET jak1-checked-dgo)
  include("${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_checked_dgo.cmake")
endif()

add_library(jak2-extracted-generated-inputs STATIC
            "${CMAKE_SOURCE_DIR}/decompiler/extractor/jak2_extracted_generated_inputs.cpp"
            "${CMAKE_SOURCE_DIR}/common/versions/jak2_iso_revisions.cpp")
target_include_directories(jak2-extracted-generated-inputs PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(jak2-extracted-generated-inputs PUBLIC jak2-output-graph jak1-checked-dgo)
target_compile_features(jak2-extracted-generated-inputs PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(jak2-extracted-generated-inputs PRIVATE /W4 /WX)
else()
  target_compile_options(jak2-extracted-generated-inputs PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(jak2-extracted-generated-inputs-link-proof
               "${CMAKE_SOURCE_DIR}/test/common/jak2_extracted_generated_inputs_link_proof.cpp")
target_link_libraries(jak2-extracted-generated-inputs-link-proof
                      PRIVATE jak2-extracted-generated-inputs)
target_compile_features(jak2-extracted-generated-inputs-link-proof PRIVATE cxx_std_20)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(jak2-extracted-generated-inputs-test
                   "${CMAKE_SOURCE_DIR}/test/common/test_jak2_extracted_generated_inputs.cpp")
    target_link_libraries(jak2-extracted-generated-inputs-test
                          PRIVATE jak2-extracted-generated-inputs)
    target_compile_features(jak2-extracted-generated-inputs-test PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(jak2-extracted-generated-inputs-test PRIVATE /W4 /WX)
    else()
      target_compile_options(jak2-extracted-generated-inputs-test
                             PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME jak2-extracted-generated-inputs-test
             COMMAND jak2-extracted-generated-inputs-test)
  endif()
endif()
