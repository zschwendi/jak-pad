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
    # Jak 1 kernel. game/kernel/jak1/kdgo.cpp is replaced by core/dgo_loader.cpp below: it defines
    # the same jak1 entry points, but reads the archive with file calls instead of through the IOP
    # RPC, and takes each object's code from the AOT path. See that file's comment.
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/jak1/fileio.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/jak1/klink.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/jak1/klisten.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/jak1/kprint.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/jak1/kscheme.cpp"
    # memory card backing store (portable, uses only the C++ filesystem)
    "${JAK1_KERNEL_CORE_ROOT}/game/sce/sif_ee_memcard.cpp"
    # portable entry point + the stubs for everything deliberately left out
    "${CMAKE_CURRENT_LIST_DIR}/kernel_core.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/desktop_seams.cpp"
    # loader for object files produced by the AOT C backend
    "${CMAKE_CURRENT_LIST_DIR}/aot_loader.cpp"
    # synchronous DGO reader, in place of game/kernel/jak1/kdgo.cpp
    "${CMAKE_CURRENT_LIST_DIR}/dgo_loader.cpp"
    # cpad-open / cpad-get-data, over controller state the host pushes in
    "${CMAKE_CURRENT_LIST_DIR}/pad.cpp"
    # the sound RPC channels, in place of game/overlord/jak1/srpc.cpp's two IOP threads, plus the
    # seam a host pulls mixed audio out of
    "${CMAKE_CURRENT_LIST_DIR}/sound_rpc.cpp"
    # the overlord's sound tables, used unchanged: the 64 sound slots with their falloff/pan math,
    # the 6 bank slots, and the globals both share. Everything else in game/overlord is the IOP.
    "${JAK1_KERNEL_CORE_ROOT}/game/overlord/common/sbank.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/overlord/common/soundcommon.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/overlord/common/srpc.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/overlord/common/ssound.cpp"
    # 989snd: the sequencer, the SPU voice model and the mixer, used unchanged. Built without an
    # output backend (GOALPAD_SND_NO_CUBEB below); goal_sound_pull_audio is the seam instead.
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/sdshim.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/sndshim.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/common/envelope.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/common/synth.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/common/voice.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/989snd/ame_handler.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/989snd/blocksound_handler.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/989snd/lfo.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/989snd/loader.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/989snd/midi_handler.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/989snd/musicbank.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/989snd/player.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/989snd/plugin.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/989snd/sfxblock.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/989snd/sfxgrain.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/989snd/vagvoice.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/game/sound/989snd/util.cpp"
    # __send-gfx-dma-chain: measure and optionally capture the chain a frame built
    "${CMAKE_CURRENT_LIST_DIR}/dma_capture.cpp"
    # the rest of the machine layer's graphics functions, answered by a host's renderer
    "${CMAKE_CURRENT_LIST_DIR}/gfx_host.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/common/dma/dma_copy.cpp"
    "${JAK1_KERNEL_CORE_ROOT}/common/dma/dma.cpp"
    # the Jak 1 mips2c function library, in place of game/mips2c/mips2c_table.cpp
    "${CMAKE_CURRENT_LIST_DIR}/mips2c_seam.cpp"
    # native implementations of the GOAL kernel routines that switch stacks
    "${CMAKE_CURRENT_LIST_DIR}/goal_native_kernel.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/goal_thread_arm64.s"
    # the ARM64 GOAL calling-convention trampolines
    "${JAK1_KERNEL_CORE_ROOT}/game/kernel/asm_funcs_arm64.s")

# The Jak 1 half of game/mips2c. Upstream's mips2c_table.cpp names all four games and would pull
# in all four function libraries; mips2c_seam.cpp registers only these.
file(GLOB JAK1_MIPS2C_SOURCES CONFIGURE_DEPENDS
     "${JAK1_KERNEL_CORE_ROOT}/game/mips2c/jak1_functions/*.cpp")
list(APPEND JAK1_KERNEL_CORE_SOURCES ${JAK1_MIPS2C_SOURCES})

enable_language(ASM)
set(CMAKE_ASM_SOURCE_FILE_EXTENSIONS ${CMAKE_ASM_SOURCE_FILE_EXTENSIONS} s)

add_library(jak1-kernel-core STATIC ${JAK1_KERNEL_CORE_SOURCES})
target_compile_features(jak1-kernel-core PUBLIC cxx_std_20)

# The same warnings game/sound/CMakeLists.txt turns off for these files upstream. Scoped to them so
# this library's own code keeps every warning.
if(NOT MSVC)
  set(JAK1_KERNEL_CORE_SOUND_SOURCES ${JAK1_KERNEL_CORE_SOURCES})
  list(FILTER JAK1_KERNEL_CORE_SOUND_SOURCES INCLUDE REGEX "/game/(sound|overlord)/")
  set_source_files_properties(
    ${JAK1_KERNEL_CORE_SOUND_SOURCES} TARGET_DIRECTORY jak1-kernel-core
    PROPERTIES COMPILE_OPTIONS
               "-Wno-unknown-warning-option;-Wno-unused-private-field;-Wno-unused-parameter;-Wno-shadow;-Wno-deprecated-declarations"
  )
endif()

# 989snd's own output backend is cubeb, which needs a desktop audio device and a third-party build.
# This library has no device: goal_sound_pull_audio hands the host the frames instead. See
# game/sound/989snd/player.h.
target_compile_definitions(jak1-kernel-core PUBLIC GOALPAD_SND_NO_CUBEB=1)
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
