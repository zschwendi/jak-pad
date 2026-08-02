include("${CMAKE_SOURCE_DIR}/common/util/safe_iso_reader.cmake")
include("${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_iso_validation.cmake")
include("${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_checked_dgo.cmake")
include("${CMAKE_SOURCE_DIR}/common/custom_data/jak1_prepared_retail.cmake")
include("${CMAKE_SOURCE_DIR}/common/custom_data/jak1_output_recipe_core.cmake")
include("${CMAKE_SOURCE_DIR}/common/custom_data/jak1_public_generated_artifacts.cmake")
include("${CMAKE_SOURCE_DIR}/common/custom_data/jak1_source_object_pack.cmake")
include("${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_extracted_generated_inputs.cmake")
include("${CMAKE_SOURCE_DIR}/common/custom_data/jak1_output_materializer.cmake")
include("${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_fr3_preparer.cmake")

add_library(jak1-import-composer STATIC
            "${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_import_composer.cpp")
target_include_directories(jak1-import-composer PUBLIC "${CMAKE_SOURCE_DIR}")
target_compile_features(jak1-import-composer PUBLIC cxx_std_20)
target_link_libraries(
  jak1-import-composer
  PUBLIC jak1-iso-validation
         jak1-source-object-pack
         jak1-extracted-generated-inputs
         jak1-output-materializer
         jak1-fr3-preparer)

if(MSVC)
  target_compile_options(jak1-import-composer PRIVATE /W4 /WX)
else()
  target_compile_options(jak1-import-composer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(jak1-import-composer-link-proof
               "${CMAKE_SOURCE_DIR}/test/common/jak1_import_composer_link_proof.cpp")
target_link_libraries(jak1-import-composer-link-proof PRIVATE jak1-import-composer)
target_compile_features(jak1-import-composer-link-proof PRIVATE cxx_std_20)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(jak1-import-composer-test
                   "${CMAKE_SOURCE_DIR}/test/common/test_jak1_import_composer.cpp")
    target_link_libraries(jak1-import-composer-test PRIVATE jak1-import-composer)
    target_compile_features(jak1-import-composer-test PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(jak1-import-composer-test PRIVATE /W4 /WX)
    else()
      target_compile_options(jak1-import-composer-test
                             PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME jak1-import-composer-test COMMAND jak1-import-composer-test)
  endif()
endif()
