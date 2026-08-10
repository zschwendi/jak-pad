if(NOT TARGET jak1-output-materializer)
  add_library(jak1-output-materializer STATIC
            "${CMAKE_SOURCE_DIR}/common/custom_data/Jak1OutputMaterializer.cpp"
            "${CMAKE_SOURCE_DIR}/common/custom_data/Jak1OutputRecipe.cpp"
            "${CMAKE_SOURCE_DIR}/common/versions/jak1_iso_revisions.cpp"
            "${CMAKE_SOURCE_DIR}/common/versions/jak2_iso_revisions.cpp"
            "${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_checked_dgo.cpp"
            "${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_checked_dgo_writer.cpp"
            "${CMAKE_SOURCE_DIR}/decompiler/extractor/jak1_retail_object_catalog.cpp"
            "${CMAKE_SOURCE_DIR}/third-party/lzokay/lzokay.cpp"
            "${CMAKE_SOURCE_DIR}/third-party/zstd/lib/common/xxhash.c")
  target_include_directories(jak1-output-materializer PUBLIC "${CMAKE_SOURCE_DIR}")
  target_compile_features(jak1-output-materializer PUBLIC cxx_std_20)

  if(MSVC)
    target_compile_options(jak1-output-materializer PRIVATE /W4 /WX)
  else()
    target_compile_options(jak1-output-materializer
                           PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()

  if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
      if(APPLE)
        target_compile_definitions(jak1-output-materializer
                                   PRIVATE JAK1_OUTPUT_MATERIALIZER_ENABLE_TEST_HOOKS)
      endif()
      add_executable(jak1-output-materializer-test
                     "${CMAKE_SOURCE_DIR}/test/common/test_jak1_output_materializer.cpp")
      target_link_libraries(jak1-output-materializer-test PRIVATE jak1-output-materializer)
      target_compile_features(jak1-output-materializer-test PRIVATE cxx_std_20)
      if(APPLE)
        target_compile_definitions(jak1-output-materializer-test
                                   PRIVATE JAK1_OUTPUT_MATERIALIZER_ENABLE_TEST_HOOKS)
      endif()
      if(MSVC)
        target_compile_options(jak1-output-materializer-test PRIVATE /W4 /WX)
      else()
        target_compile_options(jak1-output-materializer-test
                               PRIVATE -Wall -Wextra -Wpedantic -Werror)
      endif()
      add_test(NAME jak1-output-materializer-test COMMAND jak1-output-materializer-test)

      if(NOT TARGET jak1-checked-dgo-writer-test)
        add_executable(jak1-checked-dgo-writer-test
                       "${CMAKE_SOURCE_DIR}/test/common/test_jak1_checked_dgo_writer.cpp")
        target_link_libraries(jak1-checked-dgo-writer-test PRIVATE jak1-output-materializer)
        target_compile_features(jak1-checked-dgo-writer-test PRIVATE cxx_std_20)
        if(MSVC)
          target_compile_options(jak1-checked-dgo-writer-test PRIVATE /W4 /WX)
        else()
          target_compile_options(jak1-checked-dgo-writer-test
                                 PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        add_test(NAME jak1-checked-dgo-writer-test COMMAND jak1-checked-dgo-writer-test)
      endif()

      if(NOT TARGET jak1-retail-object-catalog-test)
        add_executable(jak1-retail-object-catalog-test
                       "${CMAKE_SOURCE_DIR}/test/common/test_jak1_retail_object_catalog.cpp")
        target_link_libraries(jak1-retail-object-catalog-test PRIVATE jak1-output-materializer)
        target_compile_features(jak1-retail-object-catalog-test PRIVATE cxx_std_20)
        if(MSVC)
          target_compile_options(jak1-retail-object-catalog-test PRIVATE /W4 /WX)
        else()
          target_compile_options(jak1-retail-object-catalog-test
                                 PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        add_test(NAME jak1-retail-object-catalog-test COMMAND jak1-retail-object-catalog-test)
      endif()
    endif()
  endif()
endif()
