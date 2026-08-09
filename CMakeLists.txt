# Top Level CMakeLists.txt
cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(jak)

option(OPENGOAL_BUILD_JAK1_DATA_ARENA_ONLY
       "Build only the portable Jak 1 data-arena static library" OFF)
option(OPENGOAL_BUILD_JAK1_DATA_ARENA_ARM64_INVOKER_ONLY
       "Build only the Jak 1 data-arena ARM64 native-method invoker libraries" OFF)
option(OPENGOAL_BUILD_JAK1_KERNEL_CORE_ONLY
       "Build only the portable Jak 1 kernel core static library" OFF)
option(OPENGOAL_BUILD_JAK2_IPHONEOS_AOT_LINK_ONLY
       "Build the portable kernels, Jak 2 iPhoneOS AOT archive, and full link proof" OFF)
option(OPENGOAL_BUILD_SAFE_ISO_READER_ONLY
       "Build only the bounded, portable ISO9660 reader static library" OFF)
option(OPENGOAL_BUILD_JAK1_ISO_VALIDATION_ONLY
       "Build only the bounded Jak 1 ISO revision-validation static library" OFF)
option(OPENGOAL_BUILD_JAK1_CHECKED_DGO_ONLY
       "Build only the bounded Jak 1 DGO/CGO reader static library" OFF)
option(OPENGOAL_BUILD_JAK1_PREPARED_RETAIL_ONLY
       "Build only the checked Jak 1 prepared-retail catalog static library" OFF)
option(OPENGOAL_BUILD_JAK1_FR3_PREPARER_ONLY
       "Build only the portable Jak 1 FR3 preparation static library" OFF)
option(OPENGOAL_BUILD_JAK2_FR3_PREPARER_ONLY
       "Build only the portable Jak II FR3 preparation static library" OFF)
option(OPENGOAL_BUILD_JAK1_OUTPUT_MATERIALIZER_ONLY
       "Build only the portable Jak 1 output materializer static library" OFF)
option(OPENGOAL_BUILD_JAK1_OUTPUT_RECIPE_CORE_ONLY
       "Build only the portable Jak 1 output graph and recipe-generation libraries" OFF)
option(OPENGOAL_BUILD_JAK1_PUBLIC_GENERATED_ARTIFACTS_ONLY
       "Build only the portable checked Jak 1 public generated-artifact library" OFF)
option(OPENGOAL_BUILD_JAK1_SOURCE_OBJECT_PACK_ONLY
       "Build only the portable checked Jak 1 source-object-pack library" OFF)
option(OPENGOAL_BUILD_JAK1_EXTRACTED_GENERATED_INPUTS_ONLY
       "Build only the portable Jak 1 extracted-tree generated-input adapter" OFF)
option(OPENGOAL_BUILD_JAK2_EXTRACTED_GENERATED_INPUTS_ONLY
       "Build only the portable Jak II extracted-tree generated-input adapter" OFF)
option(OPENGOAL_BUILD_JAK1_IMPORT_COMPOSER_ONLY
       "Build only the portable Jak 1 raw-ISO import composer" OFF)
option(OPENGOAL_BUILD_JAK2_IMPORT_COMPOSER_ONLY
       "Build only the fail-closed Jak II raw-ISO importer foundation" OFF)
set(OPENGOAL_JAK2_AOT_DIR "" CACHE PATH
    "Host-generated Jak 2 AOT C directory used by the iPhoneOS link proof")
set(OPENGOAL_JAK1_SOURCE_OBJECT_PACK_OUTPUT "" CACHE PATH
    "New output directory for the checked host-generated Jak 1 source-object pack")

set(OPENGOAL_ISOLATED_IMPORT_MODE_COUNT 0)
foreach(OPENGOAL_ISOLATED_IMPORT_MODE
        OPENGOAL_BUILD_SAFE_ISO_READER_ONLY
        OPENGOAL_BUILD_JAK1_ISO_VALIDATION_ONLY
        OPENGOAL_BUILD_JAK1_CHECKED_DGO_ONLY
        OPENGOAL_BUILD_JAK1_PREPARED_RETAIL_ONLY
        OPENGOAL_BUILD_JAK1_FR3_PREPARER_ONLY
        OPENGOAL_BUILD_JAK2_FR3_PREPARER_ONLY
        OPENGOAL_BUILD_JAK1_OUTPUT_MATERIALIZER_ONLY
        OPENGOAL_BUILD_JAK1_OUTPUT_RECIPE_CORE_ONLY
        OPENGOAL_BUILD_JAK1_PUBLIC_GENERATED_ARTIFACTS_ONLY
        OPENGOAL_BUILD_JAK1_SOURCE_OBJECT_PACK_ONLY
        OPENGOAL_BUILD_JAK1_EXTRACTED_GENERATED_INPUTS_ONLY
        OPENGOAL_BUILD_JAK2_EXTRACTED_GENERATED_INPUTS_ONLY
        OPENGOAL_BUILD_JAK1_IMPORT_COMPOSER_ONLY
        OPENGOAL_BUILD_JAK2_IMPORT_COMPOSER_ONLY)
  if(${OPENGOAL_ISOLATED_IMPORT_MODE})
    math(EXPR OPENGOAL_ISOLATED_IMPORT_MODE_COUNT
         "${OPENGOAL_ISOLATED_IMPORT_MODE_COUNT} + 1")
  endif()
endforeach()

if(OPENGOAL_ISOLATED_IMPORT_MODE_COUNT GREATER 0 AND
   (OPENGOAL_BUILD_JAK2_IPHONEOS_AOT_LINK_ONLY OR
    OPENGOAL_BUILD_JAK1_KERNEL_CORE_ONLY OR
    OPENGOAL_BUILD_JAK1_DATA_ARENA_ONLY OR
    OPENGOAL_BUILD_JAK1_DATA_ARENA_ARM64_INVOKER_ONLY))
  message(FATAL_ERROR
          "The isolated ISO preparation modes are mutually exclusive with the runtime-only "
          "build modes.")
endif()

if(OPENGOAL_ISOLATED_IMPORT_MODE_COUNT GREATER 1)
  message(FATAL_ERROR
          "Select only one isolated import-library mode.")
endif()

if(OPENGOAL_ISOLATED_IMPORT_MODE_COUNT GREATER 0)
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" OPENGOAL_IMPORT_APPLE_SYSROOT)
    if(NOT APPLE OR
       NOT OPENGOAL_IMPORT_APPLE_SYSROOT MATCHES "iphone(os|simulator)")
      message(FATAL_ERROR
              "iOS import-library builds require CMAKE_OSX_SYSROOT=iphoneos or "
              "iphonesimulator.")
    endif()
    if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
      message(FATAL_ERROR
              "iOS import-library builds require CMAKE_OSX_ARCHITECTURES=arm64.")
    endif()
  endif()
  include(CTest)
  if(OPENGOAL_BUILD_SAFE_ISO_READER_ONLY OR OPENGOAL_BUILD_JAK1_ISO_VALIDATION_ONLY)
    include(common/util/safe_iso_reader.cmake)
  endif()
  if(OPENGOAL_BUILD_JAK1_ISO_VALIDATION_ONLY)
    include(decompiler/extractor/jak1_iso_validation.cmake)
  endif()
  if(OPENGOAL_BUILD_JAK1_CHECKED_DGO_ONLY)
    include(decompiler/extractor/jak1_checked_dgo.cmake)
  endif()
  if(OPENGOAL_BUILD_JAK1_PREPARED_RETAIL_ONLY)
    include(common/custom_data/jak1_prepared_retail.cmake)
  endif()
  if(OPENGOAL_BUILD_JAK1_FR3_PREPARER_ONLY OR OPENGOAL_BUILD_JAK2_FR3_PREPARER_ONLY)
    if(OPENGOAL_BUILD_JAK2_FR3_PREPARER_ONLY)
      set(OPENGOAL_FR3_PREPARER_GAME jak2)
    else()
      set(OPENGOAL_FR3_PREPARER_GAME jak1)
    endif()
    include(decompiler/extractor/jak1_fr3_preparer.cmake)
  endif()
  if(OPENGOAL_BUILD_JAK1_OUTPUT_MATERIALIZER_ONLY)
    include(common/custom_data/jak1_output_materializer.cmake)
  endif()
  if(OPENGOAL_BUILD_JAK1_OUTPUT_RECIPE_CORE_ONLY)
    include(common/custom_data/jak1_output_recipe_core.cmake)
  endif()
  if(OPENGOAL_BUILD_JAK1_PUBLIC_GENERATED_ARTIFACTS_ONLY)
    include(common/custom_data/jak1_public_generated_artifacts.cmake)
  endif()
  if(OPENGOAL_BUILD_JAK1_SOURCE_OBJECT_PACK_ONLY)
    include(common/custom_data/jak1_source_object_pack.cmake)
  endif()
  if(OPENGOAL_BUILD_JAK1_EXTRACTED_GENERATED_INPUTS_ONLY)
    include(decompiler/extractor/jak1_extracted_generated_inputs.cmake)
  endif()
  if(OPENGOAL_BUILD_JAK2_EXTRACTED_GENERATED_INPUTS_ONLY)
    include(decompiler/extractor/jak2_extracted_generated_inputs.cmake)
  endif()
  if(OPENGOAL_BUILD_JAK1_IMPORT_COMPOSER_ONLY)
    include(decompiler/extractor/jak1_import_composer.cmake)
  endif()
  if(OPENGOAL_BUILD_JAK2_IMPORT_COMPOSER_ONLY)
    include(decompiler/extractor/jak2_import_composer.cmake)
  endif()
  return()
endif()

if(OPENGOAL_BUILD_JAK2_IPHONEOS_AOT_LINK_ONLY AND
   (OPENGOAL_BUILD_JAK1_KERNEL_CORE_ONLY OR
    OPENGOAL_BUILD_JAK1_DATA_ARENA_ONLY OR
    OPENGOAL_BUILD_JAK1_DATA_ARENA_ARM64_INVOKER_ONLY))
  message(FATAL_ERROR
          "OPENGOAL_BUILD_JAK2_IPHONEOS_AOT_LINK_ONLY is mutually exclusive with the "
          "Jak 1-only build modes; the generated Jak 1 and Jak 2 products export "
          "overlapping symbols.")
endif()

if(OPENGOAL_BUILD_JAK2_IPHONEOS_AOT_LINK_ONLY)
  if(BUILD_TESTING)
    message(FATAL_ERROR
            "OPENGOAL_BUILD_JAK2_IPHONEOS_AOT_LINK_ONLY requires BUILD_TESTING=OFF.")
  endif()
  string(TOLOWER "${CMAKE_OSX_SYSROOT}" OPENGOAL_APPLE_SYSROOT)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" OR
     NOT OPENGOAL_APPLE_SYSROOT MATCHES "iphone(os|simulator)")
    message(FATAL_ERROR
            "OPENGOAL_BUILD_JAK2_IPHONEOS_AOT_LINK_ONLY requires an iOS SDK; set "
            "CMAKE_SYSTEM_NAME=iOS and CMAKE_OSX_SYSROOT=iphoneos or iphonesimulator.")
  endif()
  if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR
            "OPENGOAL_BUILD_JAK2_IPHONEOS_AOT_LINK_ONLY requires "
            "CMAKE_OSX_ARCHITECTURES=arm64.")
  endif()

  # The Jak 2 core's profiler references zstd. A static archive can leave that unresolved, but the
  # full-AOT executable deliberately performs final link resolution.
  add_subdirectory(third-party/zstd EXCLUDE_FROM_ALL)
  include(game/kernel/core/kernel_core.cmake)
  include(game/kernel/core/jak2_iphoneos_aot_link.cmake)
  return()
endif()

if(OPENGOAL_BUILD_JAK1_KERNEL_CORE_ONLY)
  if(BUILD_TESTING)
    message(FATAL_ERROR
            "OPENGOAL_BUILD_JAK1_KERNEL_CORE_ONLY requires BUILD_TESTING=OFF.")
  endif()
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" AND
     NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR
            "OPENGOAL_BUILD_JAK1_KERNEL_CORE_ONLY requires ARM64; the kernel core links "
            "game/kernel/asm_funcs_arm64.s.")
  endif()

  include(game/kernel/core/kernel_core.cmake)
  return()
endif()

if(OPENGOAL_BUILD_JAK1_DATA_ARENA_ONLY AND OPENGOAL_BUILD_JAK1_DATA_ARENA_ARM64_INVOKER_ONLY)
  message(FATAL_ERROR
          "OPENGOAL_BUILD_JAK1_DATA_ARENA_ONLY and "
          "OPENGOAL_BUILD_JAK1_DATA_ARENA_ARM64_INVOKER_ONLY are mutually exclusive.")
endif()

if(OPENGOAL_BUILD_JAK1_DATA_ARENA_ONLY)
  if(BUILD_TESTING)
    message(FATAL_ERROR
            "OPENGOAL_BUILD_JAK1_DATA_ARENA_ONLY requires BUILD_TESTING=OFF.")
  endif()

  include(game/kernel/jak1/data_arena.cmake)
  return()
endif()

if(OPENGOAL_BUILD_JAK1_DATA_ARENA_ARM64_INVOKER_ONLY)
  if(BUILD_TESTING)
    message(FATAL_ERROR
            "OPENGOAL_BUILD_JAK1_DATA_ARENA_ARM64_INVOKER_ONLY requires BUILD_TESTING=OFF.")
  endif()
  if(NOT APPLE)
    message(FATAL_ERROR
            "OPENGOAL_BUILD_JAK1_DATA_ARENA_ARM64_INVOKER_ONLY requires Apple ARM64.")
  endif()
  if(CMAKE_OSX_ARCHITECTURES)
    if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
      message(FATAL_ERROR
              "OPENGOAL_BUILD_JAK1_DATA_ARENA_ARM64_INVOKER_ONLY requires "
              "CMAKE_OSX_ARCHITECTURES=arm64.")
    endif()
  elseif(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    message(FATAL_ERROR
            "OPENGOAL_BUILD_JAK1_DATA_ARENA_ARM64_INVOKER_ONLY requires Apple ARM64.")
  endif()

  enable_language(ASM)
  include(game/kernel/jak1/data_arena.cmake)
  include(game/kernel/jak1/data_arena_arm64_invoker.cmake)
  return()
endif()

include(CTest)

# Include third-party modules
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/third-party/cmake/modules/)

# Default to Release mode
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

# Potentially statically build the project
option(STATICALLY_LINK "Build for release purposes (statically link everything)" OFF)
message(STATUS "Statically Link? ${STATICALLY_LINK}")
if(STATICALLY_LINK)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_STATIC_LIBS ON)
else()
    set(BUILD_SHARED_LIBS ON)
    set(BUILD_STATIC_LIBS OFF)
endif()

# For clangd
# file(COPY), not configure_file(): configure_file registers its input as a
# configure dependency, putting the ever-rewritten compile_commands.json into
# the Ninja regen rule and causing an endless reconfigure loop.
if (EXISTS "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json" )
    file(COPY
        "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json"
        DESTINATION "${PROJECT_SOURCE_DIR}/build")
endif()

# NOTE - for SDL, i think SDL3 regressed this - https://github.com/libsdl-org/SDL/pull/6455
# a more recent issue - https://github.com/libsdl-org/SDL/issues/12078
if (APPLE)
  enable_language(OBJC)
  if(NOT CMAKE_OSX_SYSROOT)
    execute_process(
      COMMAND xcrun --show-sdk-path
          OUTPUT_VARIABLE MACOSX_SYSROOT
          OUTPUT_STRIP_TRAILING_WHITESPACE
      )
      # Tell CMake to use the host SDK unless a caller selected an Apple platform SDK.
      set(CMAKE_OSX_SYSROOT "${MACOSX_SYSROOT}" CACHE PATH "macOS SDK path" FORCE)
  endif()
endif()

# Cross-compiled test executables cannot be discovered or run on the host. The explicit sysroot
# check also covers non-macOS Apple SDK selections that do not set CMAKE_CROSSCOMPILING.
set(GOAL_HOST_TESTS_ENABLED OFF)
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  set(GOAL_HOST_TESTS_ENABLED ON)
  if(APPLE)
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" GOAL_APPLE_SYSROOT)
    if(NOT GOAL_APPLE_SYSROOT MATCHES "macosx")
      set(GOAL_HOST_TESTS_ENABLED OFF)
    endif()
  endif()
endif()

# Setup compiler flags
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(STATUS "Clang Detected - Setting Defaults")
    set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} \
        -fcxx-exceptions \
        -fexceptions \
        -fdiagnostics-color=always \
        -mavx \
        -Wall \
        -Wno-c++11-narrowing \
        -Wno-c++98-compat \
        -Wno-deprecated-this-capture \
        -Wno-missing-field-initializers \
        -Wno-sign-compare \
        -Wno-ignored-qualifiers \
        -O3 \
        -D_CRT_SECURE_NO_WARNINGS")

    # Increase stack size for windows, who's default is too low
    if(WIN32)
        # Increase the reserved stack size for all threads to 16MB
        # Note: this is only _reserved_ memory, not necessarily _committed_ memory
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LDFLAGS} -Xlinker /STACK:16000000")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb -g")
    endif()

    # additional c++ and linker flags for release mode for our projects
    if(CMAKE_BUILD_TYPE MATCHES "Release" OR CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
    endif()

    if(CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LDFLAGS} -Xlinker /debug")
    endif()

    # Fuzzing
    if(NOT STATICALLY_LINK AND ASAN_BUILD)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -O1")
        message(STATUS "Doing ASAN build")
    endif()

    # Warnings
    set(THIRDPARTY_IGNORED_WARNINGS "-Wno-everything")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(STATUS "GCC detected - Setting Defaults")
    set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} \
        -Wall \
        -Winit-self \
        -ggdb \
        -Wextra \
        -Wno-cast-align \
        -Wcast-qual \
        -Wdisabled-optimization \
        -Wformat \
        -Wextra \
        -Woverloaded-virtual \
        -Wredundant-decls \
        -Wshadow \
        -Wsign-promo \
        -fdiagnostics-color=always \
        -mavx")

    # additional c++ flags for release mode for our projects
    if(CMAKE_BUILD_TYPE MATCHES "Release")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
    endif()
    set(THIRDPARTY_IGNORED_WARNINGS "-w")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    message(STATUS "AppleClang detected - Setting Defaults")
    set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} \
        -Wall \
        -Winit-self \
        -ggdb \
        -Wextra \
        -Wno-cast-align \
        -Wcast-qual \
        -Wdisabled-optimization \
        -Wformat \
        -Wextra \
        -Woverloaded-virtual \
        -Wredundant-decls \
        -Wshadow \
        -Wsign-promo \
        -fdiagnostics-color=always"
        )

    # TODO - make a proper flag for arm compiling
    if (CMAKE_APPLE_SILICON_PROCESSOR STREQUAL "x86_64")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native -mcrc")
    endif()

    # additional c++ flags for release mode for our projects
    if(CMAKE_BUILD_TYPE MATCHES "Release")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
    endif()
    set(THIRDPARTY_IGNORED_WARNINGS "-w")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-stack_size -Wl,0x20000000")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(STATUS "MSVC detected - Setting Defaults")

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /utf-8 /arch:AVX")

    # Increase stack size for windows, who's default is too low
    # Increase the reserved stack size for all threads to 16MB
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /STACK:16000000,16384")

    # additional c++ and linker flags for specific build types
    if(CMAKE_BUILD_TYPE MATCHES "Release" OR CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /O2 /Ob2")
    endif()

    if(CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /DEBUG")
    endif()
    set(THIRDPARTY_IGNORED_WARNINGS "/w")
else()
    message(FATAL_ERROR "Unknown Compiler '${CMAKE_CXX_COMPILER_ID}', get out!")
endif()

# Platform Specific Settings
if(WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# Code Coverage
option(CODE_COVERAGE "Enable Code Coverage Compiler Flags" OFF)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CODE_COVERAGE)
    include(CodeCoverage)
    append_coverage_compiler_flags()
    message(STATUS "Code Coverage build is enabled!")
endif()

function(build_third_party_lib dir_name target_name)
    add_subdirectory(third-party/${dir_name} EXCLUDE_FROM_ALL)
    # Convert all public include dirs to SYSTEM
    get_target_property(INCLUDES ${target_name} INTERFACE_INCLUDE_DIRECTORIES)
    if(INCLUDES)
        set_target_properties(${target_name} PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${INCLUDES}"
        )
    endif()
endfunction()

# Dependencies and Libraries
# includes relative to top level jak-project folder
include_directories(./)

# build templating engine library
include_directories(SYSTEM third-party/inja)

# libcurl for HTTP requests
# Enable SSL Support, most URLs now-a-days use SSL!
# TODO - probably integrate with ZSTD since we have it already
set(CURL_USE_LIBSSH2 OFF)
# another change in the macOS test runner environment that suddenly has broken
# but this is another part of libcurl we probably don't need (multi-threading/parallelism)
set(CURL_USE_LIBPSL OFF)
set(CURL_ZLIB OFF)
# these default to AUTO since curl 8.10 and would silently pick up system libs
set(CURL_BROTLI OFF)
set(CURL_ZSTD OFF)
# curl's BUILD_EXAMPLES cache option (default ON) would otherwise leak into
# discord-rpc's identically named option and break its examples add_subdirectory
set(BUILD_EXAMPLES OFF)
# suddenly became enabled on macOS atleast, only needed for unicode URLs
# wasn't being properly statically linked
set(USE_LIBIDN2 OFF)
if(WIN32)
    set(CURL_USE_SCHANNEL ON) # native Windows SSL support
else()
    # curl 8.15 removed the native macOS backend (CURL_USE_SECTRANSP), so macOS
    # uses OpenSSL like linux (Homebrew openssl@3, see the macOS build workflows)
    if(STATICALLY_LINK)
        set(OPENSSL_USE_STATIC_LIBS TRUE)
    endif()
    set(CURL_USE_OPENSSL ON)
endif()
include_directories(third-party/curl/include)
if(STATICALLY_LINK)
    build_third_party_lib(curl libcurl_static)
else()
    build_third_party_lib(curl libcurl_shared)
endif()

# build repl library
build_third_party_lib(replxx replxx)

# SQLite - Jak 2/3's built in editor
add_definitions(-DHAVE_USLEEP=1)
add_definitions(-DSQLITE_THREADSAFE=1)
add_definitions(-DSQLITE_ENABLE_JSON1)
add_definitions(-DSQLITE_ENABLE_RTREE)
build_third_party_lib(sqlite3 sqlite3)

# build tree-sitter parser
include_directories(third-party/tree-sitter/tree-sitter/lib/include)
build_third_party_lib(tree-sitter tree-sitter)

# native OS dialogs for error messages
build_third_party_lib(libtinyfiledialogs libtinyfiledialogs)

# build format library
include_directories(third-party/fmt/include)
build_third_party_lib(fmt fmt)

# build common library
add_subdirectory(common)

# build decompiler
add_subdirectory(decompiler)

# cubeb - audio
build_third_party_lib(cubeb cubeb)

# build LSP
add_subdirectory(lsp)

# build libco and zstd
build_third_party_lib(libco libco)
build_third_party_lib(zstd libzstd_static)

# build SDL
include(SDLOptions)
if(STATICALLY_LINK)
    build_third_party_lib(SDL SDL3-static)
else()
    build_third_party_lib(SDL SDL3-shared)
endif()

# build imgui
include_directories(third-party/glad/include)
include_directories(third-party/SDL/include)
build_third_party_lib(imgui imgui)

# build the game code in C++
add_subdirectory(game)

# build the compiler
add_subdirectory(goalc)

# build standalone tools
add_subdirectory(tools)

# build the gtest libraries
if(WIN32)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
endif()

build_third_party_lib(googletest gtest)
include(GoogleTest)

# build tests
include(test/CMakeLists.txt)

# build lzokay library
build_third_party_lib(lzokay lzokay)

build_third_party_lib(stb_image stb_image)
# build draco library for tinygltf compression support
include_directories(third-party/draco/src)
add_subdirectory(third-party/draco)
add_compile_definitions(TINYGLTF_ENABLE_DRACO)
build_third_party_lib(tiny_gltf tiny_gltf)
build_third_party_lib(xdelta3 xdelta3)

# discord rich presence
include_directories(third-party/discord-rpc/include)
build_third_party_lib(discord-rpc discord-rpc)

# build zydis third party library for disassembling x86
# TODO - Once under CMake 3.13's policy CMP0077, override with `set()` instead
option(ZYDIS_BUILD_TOOLS "Zydis: Build tools" OFF)
option(ZYDIS_BUILD_EXAMPLES "Zydis: Build examples" OFF)

if(STATICALLY_LINK)
    message(STATUS "Statically linking zydis")
    option(ZYDIS_BUILD_SHARED_LIB "Zydis: Build shared library" OFF)
else()
    message(STATUS "dynamically linking zydis")
    option(ZYDIS_BUILD_SHARED_LIB "Zydis: Build shared library" ON)
endif()

build_third_party_lib(zydis Zydis)

# windows memory management lib
if(WIN32)
    build_third_party_lib(mman mman)
endif()
