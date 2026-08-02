if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "embed_binary_as_bytes.cmake requires INPUT and OUTPUT")
endif()

file(READ "${INPUT}" OPENGOAL_EMBEDDED_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," OPENGOAL_EMBEDDED_BYTES
                     "${OPENGOAL_EMBEDDED_HEX}")
get_filename_component(OPENGOAL_EMBEDDED_OUTPUT_DIR "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${OPENGOAL_EMBEDDED_OUTPUT_DIR}")
file(WRITE "${OUTPUT}"
     "// Generated from ${INPUT}; do not edit.\n${OPENGOAL_EMBEDDED_BYTES}\n")
