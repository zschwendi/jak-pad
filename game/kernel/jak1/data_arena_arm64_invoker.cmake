if(NOT APPLE)
  message(FATAL_ERROR "jak1-data-arena-arm64-invoker requires Apple ARM64.")
endif()
if(CMAKE_OSX_ARCHITECTURES)
  if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR
            "jak1-data-arena-arm64-invoker requires CMAKE_OSX_ARCHITECTURES=arm64.")
  endif()
elseif(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
  message(FATAL_ERROR "jak1-data-arena-arm64-invoker requires Apple ARM64.")
endif()

enable_language(ASM)
set(CMAKE_ASM_SOURCE_FILE_EXTENSIONS ${CMAKE_ASM_SOURCE_FILE_EXTENSIONS} s)

add_library(jak1-data-arena-arm64-invoker STATIC
            "${CMAKE_CURRENT_LIST_DIR}/data_arena_arm64_invoker.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/data_arena_arm64_invoker.s")
target_link_libraries(jak1-data-arena-arm64-invoker PUBLIC jak1-data-arena)
target_include_directories(jak1-data-arena-arm64-invoker
                           PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../..")
target_compile_features(jak1-data-arena-arm64-invoker PUBLIC cxx_std_20)
