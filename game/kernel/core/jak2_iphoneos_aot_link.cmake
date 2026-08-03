include("${CMAKE_CURRENT_LIST_DIR}/validate_jak2_aot.cmake")

set(OPENGOAL_JAK2_IPHONEOS_BUNDLE_IDENTIFIER
    "org.example.Jak2AOTLinkProof"
    CACHE STRING "Bundle identifier for the Jak 2 iPhoneOS AOT link proof")
set(OPENGOAL_JAK2_IPHONEOS_DEVELOPMENT_TEAM
    ""
    CACHE STRING "Apple development team used to sign the Jak 2 iPhoneOS AOT link proof")

if(OPENGOAL_JAK2_IPHONEOS_BUNDLE_IDENTIFIER STREQUAL "")
  message(FATAL_ERROR
          "OPENGOAL_JAK2_IPHONEOS_BUNDLE_IDENTIFIER must not be empty.")
endif()

add_custom_target(jak2-iphoneos-aot-corpus-check
  COMMAND "${CMAKE_COMMAND}"
          "-DOPENGOAL_JAK2_AOT_DIR=${JAK2_AOT_VALIDATED_DIR}"
          -P "${CMAKE_CURRENT_LIST_DIR}/validate_jak2_aot.cmake"
  VERBATIM)

# Jak 1 and Jak 2 generated code export overlapping names. Keep this executable deliberately
# single-game: the two kernel archives may coexist in the build tree, but this product links only
# jak2-kernel-core and the validated Jak 2 corpus.
add_executable(jak2-iphoneos-full-aot-link MACOSX_BUNDLE
  "${CMAKE_CURRENT_LIST_DIR}/jak2_iphoneos_aot_link_check.cpp"
  "${JAK2_AOT_MANIFEST_C}"
  ${JAK2_AOT_TRANSLATION_UNITS})
add_dependencies(jak2-iphoneos-full-aot-link jak2-iphoneos-aot-corpus-check)
set_source_files_properties(
  "${JAK2_AOT_MANIFEST_C}" ${JAK2_AOT_TRANSLATION_UNITS}
  PROPERTIES COMPILE_OPTIONS "-fno-strict-aliasing")
target_include_directories(jak2-iphoneos-full-aot-link PRIVATE "${JAK2_AOT_VALIDATED_DIR}")
target_link_libraries(jak2-iphoneos-full-aot-link PRIVATE jak2-kernel-core)
set_target_properties(jak2-iphoneos-full-aot-link PROPERTIES
  MACOSX_BUNDLE_BUNDLE_NAME "OpenGOAL Jak II AOT Link Proof"
  MACOSX_BUNDLE_BUNDLE_VERSION "1"
  MACOSX_BUNDLE_GUI_IDENTIFIER "${OPENGOAL_JAK2_IPHONEOS_BUNDLE_IDENTIFIER}"
  MACOSX_BUNDLE_INFO_PLIST
    "${CMAKE_CURRENT_LIST_DIR}/jak2_iphoneos_aot_link_Info.plist.in"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
  XCODE_GENERATE_SCHEME TRUE
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER
    "${OPENGOAL_JAK2_IPHONEOS_BUNDLE_IDENTIFIER}")

if(CMAKE_GENERATOR STREQUAL "Xcode")
  if(OPENGOAL_JAK2_IPHONEOS_DEVELOPMENT_TEAM STREQUAL "")
    set_target_properties(jak2-iphoneos-full-aot-link PROPERTIES
      XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
      XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
  else()
    set_target_properties(jak2-iphoneos-full-aot-link PROPERTIES
      XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
      XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "YES"
      XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "YES"
      XCODE_ATTRIBUTE_DEVELOPMENT_TEAM
        "${OPENGOAL_JAK2_IPHONEOS_DEVELOPMENT_TEAM}")
  endif()
endif()
