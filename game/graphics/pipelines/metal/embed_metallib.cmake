# Converts a compiled metallib into a C source file with a byte array, so the
# precompiled GPU library can be embedded in the runtime and loaded with
# MTLDevice newLibraryWithData: (no runtime shader compilation).
#
# Usage: cmake -DINPUT=<file.metallib> -DOUTPUT=<file.c> -DSYMBOL=<name> -P embed_metallib.cmake

file(READ "${INPUT}" hex_content HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex_content}")
file(WRITE "${OUTPUT}"
     "/* generated from ${INPUT} - do not edit */\n"
     "const unsigned char ${SYMBOL}[] = {${bytes}};\n"
     "const unsigned long ${SYMBOL}_size = sizeof(${SYMBOL});\n")
