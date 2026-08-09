include("${CMAKE_SOURCE_DIR}/common/util/safe_iso_reader.cmake")
include("${CMAKE_SOURCE_DIR}/common/custom_data/jak1_source_object_pack.cmake")
include("${CMAKE_SOURCE_DIR}/common/custom_data/jak2_source_object_pack.cmake")
include("${CMAKE_SOURCE_DIR}/decompiler/extractor/jak2_iso_validation.cmake")

add_library(jak2-import-composer STATIC
            "${CMAKE_SOURCE_DIR}/decompiler/extractor/jak2_import_composer.cpp")
target_include_directories(jak2-import-composer PUBLIC "${CMAKE_SOURCE_DIR}")
target_compile_features(jak2-import-composer PUBLIC cxx_std_20)
target_link_libraries(jak2-import-composer
                      PUBLIC jak2-iso-validation jak2-source-object-pack)

if(MSVC)
  target_compile_options(jak2-import-composer PRIVATE /W4 /WX)
else()
  target_compile_options(jak2-import-composer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(jak2-import-composer-link-proof
               "${CMAKE_SOURCE_DIR}/test/common/jak2_import_composer_link_proof.cpp")
target_link_libraries(jak2-import-composer-link-proof PRIVATE jak2-import-composer)
target_compile_features(jak2-import-composer-link-proof PRIVATE cxx_std_20)

if(APPLE)
  find_program(OPENGOAL_APPLE_LIBTOOL
               NAMES libtool
               PATHS /usr/bin
               NO_DEFAULT_PATH)
  if(NOT OPENGOAL_APPLE_LIBTOOL)
    message(FATAL_ERROR "Apple libtool was not found at /usr/bin/libtool.")
  endif()
  set(JAK2_IMPORT_COMPOSER_APPLE_COMPONENTS
      jak2-import-composer
      jak2-iso-validation
      safe-iso-reader
      jak2-source-object-pack
      jak1-source-object-pack
      jak1-output-recipe-core
      jak1-output-graph)
  set(JAK2_IMPORT_COMPOSER_APPLE_ARCHIVES)
  foreach(JAK2_IMPORT_COMPOSER_COMPONENT IN LISTS JAK2_IMPORT_COMPOSER_APPLE_COMPONENTS)
    if(NOT TARGET ${JAK2_IMPORT_COMPOSER_COMPONENT})
      message(FATAL_ERROR
              "Missing Jak II importer archive component: ${JAK2_IMPORT_COMPOSER_COMPONENT}")
    endif()
    list(APPEND JAK2_IMPORT_COMPOSER_APPLE_ARCHIVES
         "$<TARGET_FILE:${JAK2_IMPORT_COMPOSER_COMPONENT}>")
  endforeach()

  set(JAK2_IMPORT_COMPOSER_APPLE_OUTPUT_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/jak2-import-composer-apple")
  set(JAK2_IMPORT_COMPOSER_APPLE_OUTPUT
      "${JAK2_IMPORT_COMPOSER_APPLE_OUTPUT_DIR}/libjak2-import-composer-apple.a")
  add_custom_command(
      OUTPUT "${JAK2_IMPORT_COMPOSER_APPLE_OUTPUT}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory
              "${JAK2_IMPORT_COMPOSER_APPLE_OUTPUT_DIR}"
      COMMAND "${CMAKE_COMMAND}" -E remove -f
              "${JAK2_IMPORT_COMPOSER_APPLE_OUTPUT}"
      COMMAND "${OPENGOAL_APPLE_LIBTOOL}" -static -o
              "${JAK2_IMPORT_COMPOSER_APPLE_OUTPUT}"
              ${JAK2_IMPORT_COMPOSER_APPLE_ARCHIVES}
      DEPENDS ${JAK2_IMPORT_COMPOSER_APPLE_COMPONENTS}
      COMMAND_EXPAND_LISTS
      VERBATIM
      COMMENT "Flattening the Apple Jak II importer foundation archive")
  add_custom_target(jak2-import-composer-apple-archive
                    DEPENDS "${JAK2_IMPORT_COMPOSER_APPLE_OUTPUT}")

  add_library(jak2-import-composer-apple STATIC IMPORTED GLOBAL)
  set_target_properties(
      jak2-import-composer-apple
      PROPERTIES IMPORTED_LOCATION "${JAK2_IMPORT_COMPOSER_APPLE_OUTPUT}"
                 INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}")
  add_dependencies(jak2-import-composer-apple jak2-import-composer-apple-archive)

  add_executable(
      jak2-import-composer-apple-link-proof
      "${CMAKE_SOURCE_DIR}/test/common/jak2_import_composer_link_proof.cpp")
  target_link_libraries(jak2-import-composer-apple-link-proof
                        PRIVATE jak2-import-composer-apple)
  target_compile_features(jak2-import-composer-apple-link-proof PRIVATE cxx_std_20)
  add_dependencies(jak2-import-composer-apple-link-proof
                   jak2-import-composer-apple-archive)
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(jak2-import-composer-test
                   "${CMAKE_SOURCE_DIR}/test/common/test_jak2_import_composer.cpp")
    target_link_libraries(jak2-import-composer-test PRIVATE jak2-import-composer)
    target_compile_features(jak2-import-composer-test PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(jak2-import-composer-test PRIVATE /W4 /WX)
    else()
      target_compile_options(jak2-import-composer-test
                             PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME jak2-import-composer-test COMMAND jak2-import-composer-test)
  endif()
endif()
