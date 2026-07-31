add_library(jak1-data-arena STATIC "${CMAKE_CURRENT_LIST_DIR}/data_arena.cpp")
target_include_directories(jak1-data-arena PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../../..")
target_compile_features(jak1-data-arena PUBLIC cxx_std_20)
