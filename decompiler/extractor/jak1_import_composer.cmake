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

if(APPLE)
  find_program(OPENGOAL_APPLE_LIBTOOL
               NAMES libtool
               PATHS /usr/bin
               NO_DEFAULT_PATH)
  if(NOT OPENGOAL_APPLE_LIBTOOL)
    message(FATAL_ERROR "Apple libtool was not found at /usr/bin/libtool.")
  endif()
  set(JAK1_IMPORT_COMPOSER_APPLE_COMPONENTS
      jak1-import-composer
      jak1-iso-validation
      safe-iso-reader
      jak1-source-object-pack
      jak1-output-recipe-core
      jak1-extracted-generated-inputs
      jak1-public-generated-artifacts
      jak1-output-graph
      jak1-checked-dgo
      jak1-output-materializer
      jak1-fr3-preparer)
  set(JAK1_IMPORT_COMPOSER_APPLE_ARCHIVES)
  foreach(JAK1_IMPORT_COMPOSER_COMPONENT IN LISTS JAK1_IMPORT_COMPOSER_APPLE_COMPONENTS)
    if(NOT TARGET ${JAK1_IMPORT_COMPOSER_COMPONENT})
      message(FATAL_ERROR
              "Missing Jak 1 importer archive component: ${JAK1_IMPORT_COMPOSER_COMPONENT}")
    endif()
    list(APPEND JAK1_IMPORT_COMPOSER_APPLE_ARCHIVES
         "$<TARGET_FILE:${JAK1_IMPORT_COMPOSER_COMPONENT}>")
  endforeach()

  set(JAK1_IMPORT_COMPOSER_APPLE_OUTPUT_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/jak1-import-composer-apple")
  set(JAK1_IMPORT_COMPOSER_APPLE_OUTPUT
      "${JAK1_IMPORT_COMPOSER_APPLE_OUTPUT_DIR}/libjak1-import-composer-apple.a")
  add_custom_command(
      OUTPUT "${JAK1_IMPORT_COMPOSER_APPLE_OUTPUT}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory
              "${JAK1_IMPORT_COMPOSER_APPLE_OUTPUT_DIR}"
      COMMAND "${CMAKE_COMMAND}" -E remove -f
              "${JAK1_IMPORT_COMPOSER_APPLE_OUTPUT}"
      COMMAND "${OPENGOAL_APPLE_LIBTOOL}" -static -o
              "${JAK1_IMPORT_COMPOSER_APPLE_OUTPUT}"
              ${JAK1_IMPORT_COMPOSER_APPLE_ARCHIVES}
      DEPENDS ${JAK1_IMPORT_COMPOSER_APPLE_COMPONENTS}
      COMMAND_EXPAND_LISTS
      VERBATIM
      COMMENT "Flattening the Apple Jak 1 importer archive")
  add_custom_target(jak1-import-composer-apple-archive
                    DEPENDS "${JAK1_IMPORT_COMPOSER_APPLE_OUTPUT}")

  add_library(jak1-import-composer-apple STATIC IMPORTED GLOBAL)
  set_target_properties(
      jak1-import-composer-apple
      PROPERTIES IMPORTED_LOCATION "${JAK1_IMPORT_COMPOSER_APPLE_OUTPUT}"
                 INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}")
  add_dependencies(jak1-import-composer-apple jak1-import-composer-apple-archive)

  add_executable(
      jak1-import-composer-apple-link-proof
      "${CMAKE_SOURCE_DIR}/test/common/jak1_import_composer_link_proof.cpp")
  target_link_libraries(jak1-import-composer-apple-link-proof
                        PRIVATE jak1-import-composer-apple)
  target_compile_features(jak1-import-composer-apple-link-proof PRIVATE cxx_std_20)
  add_dependencies(jak1-import-composer-apple-link-proof
                   jak1-import-composer-apple-archive)
endif()

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
