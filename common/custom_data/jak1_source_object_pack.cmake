include("${CMAKE_SOURCE_DIR}/common/custom_data/jak1_output_recipe_core.cmake")

add_library(jak1-source-object-pack STATIC
            "${CMAKE_SOURCE_DIR}/common/custom_data/Jak1SourceObjectPack.cpp")
target_include_directories(jak1-source-object-pack PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(jak1-source-object-pack PUBLIC jak1-output-recipe-core)
target_compile_features(jak1-source-object-pack PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(jak1-source-object-pack PRIVATE /W4 /WX)
else()
  target_compile_options(jak1-source-object-pack PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(jak1-source-object-pack-link-proof
               "${CMAKE_SOURCE_DIR}/test/common/jak1_source_object_pack_link_proof.cpp")
target_link_libraries(jak1-source-object-pack-link-proof PRIVATE jak1-source-object-pack)
target_compile_features(jak1-source-object-pack-link-proof PRIVATE cxx_std_20)

add_executable(jak1-source-object-pack-verify
               "${CMAKE_SOURCE_DIR}/common/custom_data/jak1_source_object_pack_verify.cpp")
target_link_libraries(jak1-source-object-pack-verify PRIVATE jak1-source-object-pack)
target_compile_features(jak1-source-object-pack-verify PRIVATE cxx_std_20)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(jak1-source-object-pack-test
                   "${CMAKE_SOURCE_DIR}/test/common/test_jak1_source_object_pack.cpp")
    target_link_libraries(jak1-source-object-pack-test PRIVATE jak1-source-object-pack)
    target_compile_features(jak1-source-object-pack-test PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(jak1-source-object-pack-test PRIVATE /W4 /WX)
    else()
      target_compile_options(jak1-source-object-pack-test
                             PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME jak1-source-object-pack-test COMMAND jak1-source-object-pack-test)
  endif()
endif()
