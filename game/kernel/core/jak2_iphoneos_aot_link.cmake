include("${CMAKE_CURRENT_LIST_DIR}/validate_jak2_aot.cmake")

add_custom_target(jak2-iphoneos-aot-corpus-check
  COMMAND "${CMAKE_COMMAND}"
          "-DOPENGOAL_JAK2_AOT_DIR=${JAK2_AOT_VALIDATED_DIR}"
          -P "${CMAKE_CURRENT_LIST_DIR}/validate_jak2_aot.cmake"
  VERBATIM)

# Jak 1 and Jak 2 generated code export overlapping names. Keep this executable deliberately
# single-game: the two kernel archives may coexist in the build tree, but this product links only
# jak2-kernel-core and the validated Jak 2 corpus.
add_executable(jak2-iphoneos-full-aot-link
  "${CMAKE_CURRENT_LIST_DIR}/jak2_iphoneos_aot_link_check.cpp"
  "${JAK2_AOT_MANIFEST_C}"
  ${JAK2_AOT_TRANSLATION_UNITS})
add_dependencies(jak2-iphoneos-full-aot-link jak2-iphoneos-aot-corpus-check)
set_source_files_properties(
  "${JAK2_AOT_MANIFEST_C}" ${JAK2_AOT_TRANSLATION_UNITS}
  PROPERTIES COMPILE_OPTIONS "-fno-strict-aliasing")
target_include_directories(jak2-iphoneos-full-aot-link PRIVATE "${JAK2_AOT_VALIDATED_DIR}")
target_link_libraries(jak2-iphoneos-full-aot-link PRIVATE jak2-kernel-core)
