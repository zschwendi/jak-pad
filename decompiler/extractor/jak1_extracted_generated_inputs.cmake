include("${CMAKE_SOURCE_DIR}/common/custom_data/jak1_public_generated_artifacts.cmake")

if(NOT TARGET jak1-checked-dgo)
  include("${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_checked_dgo.cmake")
endif()

add_library(jak1-extracted-generated-inputs STATIC
            "${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_extracted_generated_inputs.cpp"
            "${CMAKE_SOURCE_DIR}/common/versions/jak1_iso_revisions.cpp")
target_include_directories(jak1-extracted-generated-inputs PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(jak1-extracted-generated-inputs
                      PUBLIC jak1-public-generated-artifacts jak1-checked-dgo)
target_compile_features(jak1-extracted-generated-inputs PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(jak1-extracted-generated-inputs PRIVATE /W4 /WX)
else()
  target_compile_options(jak1-extracted-generated-inputs
                         PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(jak1-extracted-generated-inputs-link-proof
               "${CMAKE_SOURCE_DIR}/test/common/jak1_extracted_generated_inputs_link_proof.cpp")
target_link_libraries(jak1-extracted-generated-inputs-link-proof
                      PRIVATE jak1-extracted-generated-inputs)
target_compile_features(jak1-extracted-generated-inputs-link-proof PRIVATE cxx_std_20)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(jak1-extracted-generated-inputs-test
                   "${CMAKE_SOURCE_DIR}/test/common/test_jak1_extracted_generated_inputs.cpp")
    target_link_libraries(jak1-extracted-generated-inputs-test
                          PRIVATE jak1-extracted-generated-inputs)
    target_compile_features(jak1-extracted-generated-inputs-test PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(jak1-extracted-generated-inputs-test PRIVATE /W4 /WX)
    else()
      target_compile_options(jak1-extracted-generated-inputs-test
                             PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME jak1-extracted-generated-inputs-test
             COMMAND jak1-extracted-generated-inputs-test)

    add_executable(
      jak1-extracted-generated-inputs-exactness-test
      "${CMAKE_SOURCE_DIR}/test/goalc/test_jak1_extracted_generated_inputs_exactness.cpp"
      "${CMAKE_SOURCE_DIR}/goalc/data_compiler/DataObjectGeneratorCore.cpp")
    target_link_libraries(jak1-extracted-generated-inputs-exactness-test
                          PRIVATE jak1-extracted-generated-inputs)
    target_compile_features(jak1-extracted-generated-inputs-exactness-test PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(jak1-extracted-generated-inputs-exactness-test PRIVATE /W4 /WX)
    else()
      target_compile_options(jak1-extracted-generated-inputs-exactness-test
                             PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME jak1-extracted-generated-inputs-exactness-test
             COMMAND jak1-extracted-generated-inputs-exactness-test)
  endif()
endif()
