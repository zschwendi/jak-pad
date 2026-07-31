# jak1-kernel-core: the real OpenGOAL Jak 1 kernel subset that can be built for a platform with no
# desktop windowing, no IOP/sound emulation, no DECI2 listener transport, and no runtime code
# generation.
#
# The desktop-only symbols this subset still references are defined as loudly-failing stubs in
# desktop_seams.cpp; that file also documents exactly which upstream translation units are absent.

set(JAK1_KERNEL_CORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

# common/versions/versions.cpp is deliberately absent: only the kmachine translation units call it,
# and it includes the CMake-generated common/versions/revision.h, which would force every consumer
# of this list — including the Xcode app target — to run a CMake configure first.
set(JAK1_KERNEL_CORE_SOURCES
    # common support
    "${JAK1_KERNEL_CORE_ROOT}/common/cross_os_debug/xdbg.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/common/log/log.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/common/util/Assert.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/common/util/crc32.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/common/util/diff.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/common/util/FileUtil.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/common/util/string_util.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/common/util/Timer.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/common/util/unicode_util.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/third-party/lzokay/lzokay.cpp"
    # game-version-independent kernel
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/common/fileio.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/common/kboot.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/common/kdgo.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/common/kdsnetm.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/common/klink.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/common/klisten.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/common/kmalloc.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/common/kmemcard.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/common/kprint.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/common/kscheme.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/common/ksocket.cpp"
    # Jak 1 kernel
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/jak1/fileio.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/jak1/kdgo.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/jak1/klink.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/jak1/klisten.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/jak1/kprint.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/jak1/kscheme.cpp"
    # memory card backing store (portable, uses only the C++ filesystem)
    "${JAK1_KERNEL_CORE_ROOT}/game/sce/sif_ee_memcard.cpp"
    # portable entry point + the stubs for everything deliberately left out
    "${CMAKE_CURRENT_LIST_DIR}/kernel_core.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/desktop_seams.cpp"
    # the ARM64 GOAL calling-convention trampolines
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/asm_funcs_arm64.s")

enable_language(ASM)
set(CMAKE_ASM_SOURCE_FILE_EXTENSIONS ${CMAKE_ASM_SOURCE_FILE_EXTENSIONS} s)

add_library(jak1-kernel-core STATIC ${JAK1_KERNEL_CORE_SOURCES})
target_compile_features(jak1-kernel-core PUBLIC cxx_std_20)
target_include_directories(
  jak1-kernel-core
  PUBLIC "${JAK1_KERNEL_CORE_ROOT}" "${JAK1_KERNEL_CORE_ROOT}/third-party"
         "${JAK1_KERNEL_CORE_ROOT}/third-party/fmt/include"
         # game/kernel/common/kmachine.h includes game/graphics/gfx.h, which includes
         # game/settings/settings.h, which includes <SDL3/SDL.h>. Only the declarations are used
         # (jak1/klink.cpp needs CacheFlush); no SDL code is compiled and no SDL library is linked.
         "${JAK1_KERNEL_CORE_ROOT}/third-party/SDL/include")

if(TARGET fmt)
  target_link_libraries(jak1-kernel-core PUBLIC fmt)
else()
  # Standalone builds (for example the iOS static library) compile the header-only fmt.
  target_compile_definitions(jak1-kernel-core PUBLIC FMT_HEADER_ONLY=1)
endif()
