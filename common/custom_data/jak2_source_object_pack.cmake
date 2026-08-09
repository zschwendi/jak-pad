add_library(jak2-source-object-pack STATIC
            "${CMAKE_SOURCE_DIR}/common/custom_data/Jak2SourceObjectPack.cpp")
target_include_directories(jak2-source-object-pack PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(jak2-source-object-pack PUBLIC jak1-source-object-pack)
target_compile_features(jak2-source-object-pack PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(jak2-source-object-pack PRIVATE /W4 /WX)
else()
  target_compile_options(jak2-source-object-pack PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(jak2-source-object-pack-link-proof
               "${CMAKE_SOURCE_DIR}/test/common/jak2_source_object_pack_link_proof.cpp")
target_link_libraries(jak2-source-object-pack-link-proof PRIVATE jak2-source-object-pack)
target_compile_features(jak2-source-object-pack-link-proof PRIVATE cxx_std_20)

add_executable(jak2-source-object-pack-verify
               "${CMAKE_SOURCE_DIR}/common/custom_data/jak2_source_object_pack_verify.cpp")
target_link_libraries(jak2-source-object-pack-verify PRIVATE jak2-source-object-pack)
target_compile_features(jak2-source-object-pack-verify PRIVATE cxx_std_20)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(jak2-source-object-pack-test
                   "${CMAKE_SOURCE_DIR}/test/common/test_jak2_source_object_pack.cpp")
    target_link_libraries(jak2-source-object-pack-test PRIVATE jak2-source-object-pack)
    target_compile_features(jak2-source-object-pack-test PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(jak2-source-object-pack-test PRIVATE /W4 /WX)
    else()
      target_compile_options(jak2-source-object-pack-test
                             PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME jak2-source-object-pack-test COMMAND jak2-source-object-pack-test)
    add_test(NAME jak2-source-object-pack-link-proof COMMAND jak2-source-object-pack-link-proof)
    add_test(NAME jak2-source-object-pack-verify-help
             COMMAND jak2-source-object-pack-verify --help)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME fr3-project-bundle-check
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_SOURCE_DIR}/scripts/run-fr3-project-bundle-check.py")
  endif()
endif()
