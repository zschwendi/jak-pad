include("${CMAKE_CURRENT_LIST_DIR}/validate_jak2_aot.cmake")

set(OPENGOAL_JAK2_IPHONEOS_BUNDLE_IDENTIFIER
    "org.example.Jak2AOTLinkProof"
    CACHE STRING "Bundle identifier for the Jak 2 iPhoneOS AOT link proof")
set(OPENGOAL_JAK2_IPHONEOS_DEVELOPMENT_TEAM
    ""
    CACHE STRING "Apple development team used to sign the Jak 2 iPhoneOS AOT link proof")
set(OPENGOAL_JAK2_IPHONEOS_DISPLAY_TICK_BUNDLE_IDENTIFIER
    "org.example.Jak2DisplayTickProof"
    CACHE STRING "Bundle identifier for the development-only Jak 2 display-tick proof")

if(OPENGOAL_JAK2_IPHONEOS_BUNDLE_IDENTIFIER STREQUAL "")
  message(FATAL_ERROR
          "OPENGOAL_JAK2_IPHONEOS_BUNDLE_IDENTIFIER must not be empty.")
endif()
if(OPENGOAL_JAK2_IPHONEOS_DISPLAY_TICK_BUNDLE_IDENTIFIER STREQUAL "")
  message(FATAL_ERROR
          "OPENGOAL_JAK2_IPHONEOS_DISPLAY_TICK_BUNDLE_IDENTIFIER must not be empty.")
endif()

add_custom_target(jak2-iphoneos-aot-corpus-check
  COMMAND "${CMAKE_COMMAND}"
          "-DOPENGOAL_JAK2_AOT_DIR=${JAK2_AOT_VALIDATED_DIR}"
          -P "${CMAKE_CURRENT_LIST_DIR}/validate_jak2_aot.cmake"
  VERBATIM)

# Jak 1 and Jak 2 generated code export overlapping names. Keep this archive deliberately
# single-game: the two kernel archives may coexist in the build tree, but a product must link only
# one game's AOT corpus.
add_library(jak2-iphoneos-aot-corpus STATIC
  "${JAK2_AOT_MANIFEST_C}"
  ${JAK2_AOT_TRANSLATION_UNITS})
add_dependencies(jak2-iphoneos-aot-corpus jak2-iphoneos-aot-corpus-check)
set_source_files_properties(
  "${JAK2_AOT_MANIFEST_C}" ${JAK2_AOT_TRANSLATION_UNITS}
  PROPERTIES COMPILE_OPTIONS "-fno-strict-aliasing")
target_include_directories(jak2-iphoneos-aot-corpus PUBLIC "${JAK2_AOT_VALIDATED_DIR}")
target_link_libraries(jak2-iphoneos-aot-corpus PUBLIC jak2-kernel-core)
set_target_properties(jak2-iphoneos-aot-corpus PROPERTIES
  OUTPUT_NAME "opengoal-jak2-aot-corpus")

# Keep the reusable runtime driver outside the generated-code archive so the corpus remains the
# manifest plus its validated 840 translation units.
add_library(jak2-iphoneos-runtime STATIC EXCLUDE_FROM_ALL
  "${CMAKE_CURRENT_LIST_DIR}/jak2_runtime.cpp")
target_link_libraries(jak2-iphoneos-runtime PUBLIC jak2-iphoneos-aot-corpus)
set_target_properties(jak2-iphoneos-runtime PROPERTIES
  OUTPUT_NAME "opengoal-jak2-runtime")

add_executable(jak2-iphoneos-full-aot-link MACOSX_BUNDLE
  "${CMAKE_CURRENT_LIST_DIR}/jak2_iphoneos_aot_link_check.cpp")
target_link_libraries(jak2-iphoneos-full-aot-link PRIVATE jak2-iphoneos-aot-corpus)
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
  enable_language(OBJC)

  add_executable(jak2-iphoneos-display-tick-proof MACOSX_BUNDLE EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/jak2_iphoneos_display_tick_main.m")
  target_link_libraries(jak2-iphoneos-display-tick-proof PRIVATE
    jak2-iphoneos-runtime
    "-framework UIKit"
    "-framework QuartzCore"
    "-framework Foundation")
  set_target_properties(jak2-iphoneos-display-tick-proof PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "OpenGOAL Jak II Display Tick Proof"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    MACOSX_BUNDLE_GUI_IDENTIFIER
      "${OPENGOAL_JAK2_IPHONEOS_DISPLAY_TICK_BUNDLE_IDENTIFIER}"
    MACOSX_BUNDLE_INFO_PLIST
      "${CMAKE_CURRENT_LIST_DIR}/jak2_iphoneos_display_tick_Info.plist.in"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    XCODE_GENERATE_SCHEME TRUE
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER
      "${OPENGOAL_JAK2_IPHONEOS_DISPLAY_TICK_BUNDLE_IDENTIFIER}"
    XCODE_ATTRIBUTE_SKIP_INSTALL "YES"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")

  if(OPENGOAL_JAK2_IPHONEOS_DEVELOPMENT_TEAM STREQUAL "")
    set_target_properties(jak2-iphoneos-full-aot-link PROPERTIES
      XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
      XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
    set_target_properties(jak2-iphoneos-display-tick-proof PROPERTIES
      XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
      XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
  else()
    set_target_properties(jak2-iphoneos-full-aot-link PROPERTIES
      XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
      XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "YES"
      XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "YES"
      XCODE_ATTRIBUTE_DEVELOPMENT_TEAM
        "${OPENGOAL_JAK2_IPHONEOS_DEVELOPMENT_TEAM}")
    set_target_properties(jak2-iphoneos-display-tick-proof PROPERTIES
      XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
      XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "YES"
      XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "YES"
      XCODE_ATTRIBUTE_DEVELOPMENT_TEAM
        "${OPENGOAL_JAK2_IPHONEOS_DEVELOPMENT_TEAM}")
  endif()
endif()
