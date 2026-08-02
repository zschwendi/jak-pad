set(JAK2_AOT_EXPECTED_FILE_COUNT 840)

if(NOT DEFINED OPENGOAL_JAK2_AOT_DIR OR OPENGOAL_JAK2_AOT_DIR STREQUAL "")
  message(FATAL_ERROR
          "OPENGOAL_JAK2_AOT_DIR is required and must name a host-generated Jak 2 AOT directory.")
endif()

get_filename_component(JAK2_AOT_VALIDATED_DIR "${OPENGOAL_JAK2_AOT_DIR}" ABSOLUTE)
if(NOT IS_DIRECTORY "${JAK2_AOT_VALIDATED_DIR}")
  message(FATAL_ERROR
          "Jak 2 AOT directory does not exist: ${JAK2_AOT_VALIDATED_DIR}")
endif()

set(JAK2_AOT_MANIFEST_C "${JAK2_AOT_VALIDATED_DIR}/aot_boot_manifest.c")
set(JAK2_AOT_MANIFEST_H "${JAK2_AOT_VALIDATED_DIR}/aot_boot_manifest.h")
if(NOT EXISTS "${JAK2_AOT_MANIFEST_C}" OR NOT EXISTS "${JAK2_AOT_MANIFEST_H}")
  message(FATAL_ERROR
          "Jak 2 AOT corpus must contain aot_boot_manifest.c and aot_boot_manifest.h: "
          "${JAK2_AOT_VALIDATED_DIR}")
endif()

file(STRINGS "${JAK2_AOT_MANIFEST_C}" JAK2_AOT_COUNT_DECLARATIONS
     REGEX "^const int goal_aot_boot_file_count = 840;$")
list(LENGTH JAK2_AOT_COUNT_DECLARATIONS JAK2_AOT_COUNT_DECLARATION_COUNT)
if(NOT JAK2_AOT_COUNT_DECLARATION_COUNT EQUAL 1)
  message(FATAL_ERROR
          "Jak 2 AOT manifest must declare exactly "
          "'const int goal_aot_boot_file_count = 840;': ${JAK2_AOT_MANIFEST_C}")
endif()

file(STRINGS "${JAK2_AOT_MANIFEST_C}" JAK2_AOT_ARRAY_DECLARATIONS
     REGEX "^const goal_aot_boot_entry goal_aot_boot_files\\[840\\] = \\{$")
list(LENGTH JAK2_AOT_ARRAY_DECLARATIONS JAK2_AOT_ARRAY_DECLARATION_COUNT)
if(NOT JAK2_AOT_ARRAY_DECLARATION_COUNT EQUAL 1)
  message(FATAL_ERROR
          "Jak 2 AOT manifest must define goal_aot_boot_files[840]: "
          "${JAK2_AOT_MANIFEST_C}")
endif()

file(STRINGS "${JAK2_AOT_MANIFEST_C}" JAK2_AOT_ENTRY_LINES
     REGEX "^[ \t]*\\{\"goal_src/")
list(LENGTH JAK2_AOT_ENTRY_LINES JAK2_AOT_ENTRY_COUNT)
if(NOT JAK2_AOT_ENTRY_COUNT EQUAL JAK2_AOT_EXPECTED_FILE_COUNT)
  message(FATAL_ERROR
          "Jak 2 AOT manifest must contain ${JAK2_AOT_EXPECTED_FILE_COUNT} goal_src entries; "
          "found ${JAK2_AOT_ENTRY_COUNT}: ${JAK2_AOT_MANIFEST_C}")
endif()

file(STRINGS "${JAK2_AOT_MANIFEST_C}" JAK2_AOT_GENERATED_INCLUDES
     REGEX "^#include \"[A-Za-z0-9_]+_generated\\.h\"$")
list(LENGTH JAK2_AOT_GENERATED_INCLUDES JAK2_AOT_GENERATED_INCLUDE_COUNT)
if(NOT JAK2_AOT_GENERATED_INCLUDE_COUNT EQUAL JAK2_AOT_EXPECTED_FILE_COUNT)
  message(FATAL_ERROR
          "Jak 2 AOT manifest must include ${JAK2_AOT_EXPECTED_FILE_COUNT} generated headers; "
          "found ${JAK2_AOT_GENERATED_INCLUDE_COUNT}: ${JAK2_AOT_MANIFEST_C}")
endif()

set(JAK2_AOT_TRANSLATION_UNITS "")
set(JAK2_AOT_EXPECTED_HEADERS "")
set(JAK2_AOT_TAGS "")
foreach(JAK2_AOT_INCLUDE IN LISTS JAK2_AOT_GENERATED_INCLUDES)
  string(REGEX REPLACE
         "^#include \"([A-Za-z0-9_]+)_generated\\.h\"$" "\\1"
         JAK2_AOT_TAG "${JAK2_AOT_INCLUDE}")
  list(FIND JAK2_AOT_TAGS "${JAK2_AOT_TAG}" JAK2_AOT_DUPLICATE_INDEX)
  if(NOT JAK2_AOT_DUPLICATE_INDEX EQUAL -1)
    message(FATAL_ERROR
            "Jak 2 AOT manifest contains duplicate tag '${JAK2_AOT_TAG}': "
            "${JAK2_AOT_MANIFEST_C}")
  endif()
  list(APPEND JAK2_AOT_TAGS "${JAK2_AOT_TAG}")
  list(APPEND JAK2_AOT_TRANSLATION_UNITS
       "${JAK2_AOT_VALIDATED_DIR}/${JAK2_AOT_TAG}.c")
  list(APPEND JAK2_AOT_EXPECTED_HEADERS
       "${JAK2_AOT_VALIDATED_DIR}/${JAK2_AOT_TAG}_generated.h")
endforeach()

file(GLOB JAK2_AOT_ACTUAL_C_FILES "${JAK2_AOT_VALIDATED_DIR}/*.c")
list(REMOVE_ITEM JAK2_AOT_ACTUAL_C_FILES "${JAK2_AOT_MANIFEST_C}")
list(LENGTH JAK2_AOT_ACTUAL_C_FILES JAK2_AOT_ACTUAL_C_COUNT)
if(NOT JAK2_AOT_ACTUAL_C_COUNT EQUAL JAK2_AOT_EXPECTED_FILE_COUNT)
  message(FATAL_ERROR
          "Jak 2 AOT corpus must contain ${JAK2_AOT_EXPECTED_FILE_COUNT} translation units plus "
          "aot_boot_manifest.c; found ${JAK2_AOT_ACTUAL_C_COUNT} translation units in "
          "${JAK2_AOT_VALIDATED_DIR}")
endif()

file(GLOB JAK2_AOT_ACTUAL_HEADERS "${JAK2_AOT_VALIDATED_DIR}/*_generated.h")
list(LENGTH JAK2_AOT_ACTUAL_HEADERS JAK2_AOT_ACTUAL_HEADER_COUNT)
if(NOT JAK2_AOT_ACTUAL_HEADER_COUNT EQUAL JAK2_AOT_EXPECTED_FILE_COUNT)
  message(FATAL_ERROR
          "Jak 2 AOT corpus must contain ${JAK2_AOT_EXPECTED_FILE_COUNT} generated headers; "
          "found ${JAK2_AOT_ACTUAL_HEADER_COUNT} in ${JAK2_AOT_VALIDATED_DIR}")
endif()

list(SORT JAK2_AOT_TRANSLATION_UNITS)
list(SORT JAK2_AOT_ACTUAL_C_FILES)
if(NOT "${JAK2_AOT_TRANSLATION_UNITS}" STREQUAL "${JAK2_AOT_ACTUAL_C_FILES}")
  message(FATAL_ERROR
          "Jak 2 AOT C corpus does not match the tags named by aot_boot_manifest.c: "
          "${JAK2_AOT_VALIDATED_DIR}")
endif()

list(SORT JAK2_AOT_EXPECTED_HEADERS)
list(SORT JAK2_AOT_ACTUAL_HEADERS)
if(NOT "${JAK2_AOT_EXPECTED_HEADERS}" STREQUAL "${JAK2_AOT_ACTUAL_HEADERS}")
  message(FATAL_ERROR
          "Jak 2 AOT header corpus does not match the tags named by aot_boot_manifest.c: "
          "${JAK2_AOT_VALIDATED_DIR}")
endif()

message(STATUS
        "Validated complete Jak 2 AOT corpus: ${JAK2_AOT_EXPECTED_FILE_COUNT} units in "
        "${JAK2_AOT_VALIDATED_DIR}")
