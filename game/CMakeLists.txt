# Set a more convenient ARM flag
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    set(ARM64_ARCH TRUE)
    message(STATUS "ARM64 architecture detected")
else()
    set(ARM64_ARCH FALSE)
    message(STATUS "Non-ARM64 architecture detected")
endif()

if(ARM64_ARCH)
    # Add your ARM64-specific configuration or build options here
    set(OG_ASM_FUNCS_FILE kernel/asm_funcs_arm64.s)
    enable_language(ASM)
    set(CMAKE_ASM_SOURCE_FILE_EXTENSIONS ${CMAKE_ASM_SOURCE_FILE_EXTENSIONS} s)
    set_source_files_properties(${OG_ASM_FUNCS_FILE} PROPERTIES COMPILE_FLAGS "-arch arm64 -g")
else()
    set(OG_ASM_FUNCS_FILE kernel/asm_funcs_x86_64.asm)
    enable_language(ASM_NASM)
    set(CMAKE_ASM_NASM_SOURCE_FILE_EXTENSIONS ${CMAKE_ASM_NASM_SOURCE_FILE_EXTENSIONS} asm)
    set(CMAKE_ASM_NASM_COMPILE_OBJECT "<CMAKE_ASM_NASM_COMPILER> <INCLUDES> -f ${CMAKE_ASM_NASM_OBJECT_FORMAT} -o <OBJECT> <SOURCE>")
    set_source_files_properties(${OG_ASM_FUNCS_FILE} PROPERTIES COMPILE_FLAGS "-g")
endif()

set(RUNTIME_SOURCE
        external/discord_jak1.cpp
        external/discord_jak2.cpp
        external/discord_jak3.cpp
        external/discord_jakx.cpp
        external/discord.cpp
        graphics/display.cpp
        graphics/gfx_test.cpp
        graphics/gfx.cpp
        graphics/jak2_texture_remap.cpp
        graphics/jak3_texture_remap.cpp
        graphics/jakx_texture_remap.cpp
        graphics/screenshot.cpp
        graphics/opengl_renderer/background/background_common.cpp
        graphics/opengl_renderer/background/Hfrag.cpp
        graphics/opengl_renderer/background/Shrub.cpp
        graphics/opengl_renderer/background/TFragment.cpp
        graphics/opengl_renderer/background/Tie3.cpp
        graphics/opengl_renderer/BlitDisplays.cpp
        graphics/opengl_renderer/BucketRenderer.cpp
        graphics/opengl_renderer/CollideMeshRenderer.cpp
        graphics/opengl_renderer/debug_gui.cpp
        graphics/opengl_renderer/DepthCue.cpp
        graphics/opengl_renderer/DirectRenderer.cpp
        graphics/opengl_renderer/DirectRenderer2.cpp
        graphics/opengl_renderer/dma_helpers.cpp
        graphics/opengl_renderer/EyeRenderer.cpp
        graphics/opengl_renderer/foreground/Generic2_Build.cpp
        graphics/opengl_renderer/foreground/Generic2_DMA.cpp
        graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp
        graphics/opengl_renderer/foreground/Generic2.cpp
        graphics/opengl_renderer/foreground/Generic2BucketRenderer.cpp
        graphics/opengl_renderer/foreground/Merc2.cpp
        graphics/opengl_renderer/foreground/Merc2BucketRenderer.cpp
        graphics/opengl_renderer/foreground/Shadow2.cpp
        graphics/opengl_renderer/loader/Loader.cpp
        graphics/opengl_renderer/loader/LoaderStages.cpp
        graphics/opengl_renderer/ocean/CommonOceanRenderer.cpp
        graphics/opengl_renderer/ocean/OceanEnvmap.cpp
        graphics/opengl_renderer/ocean/OceanMid_PS2.cpp
        graphics/opengl_renderer/ocean/OceanMid.cpp
        graphics/opengl_renderer/ocean/OceanMidAndFar.cpp
        graphics/opengl_renderer/ocean/OceanNear_PS2.cpp
        graphics/opengl_renderer/ocean/OceanNear.cpp
        graphics/opengl_renderer/ocean/OceanTexture_PC.cpp
        graphics/opengl_renderer/ocean/OceanTexture.cpp
        graphics/opengl_renderer/opengl_utils.cpp
        graphics/opengl_renderer/OpenGLRenderer.cpp
        graphics/opengl_renderer/Profiler.cpp
        graphics/opengl_renderer/ProgressRenderer.cpp
        graphics/opengl_renderer/Shader.cpp
        graphics/opengl_renderer/Shadow_PS2.cpp
        graphics/opengl_renderer/ShadowRenderer.cpp
        graphics/opengl_renderer/SkyBlendCPU.cpp
        graphics/opengl_renderer/SkyBlendGPU.cpp
        graphics/opengl_renderer/SkyRenderer.cpp
        graphics/opengl_renderer/SlowTimeEffect.cpp
        graphics/opengl_renderer/sprite/GlowRenderer.cpp
        graphics/opengl_renderer/sprite/Sprite3_Distort.cpp
        graphics/opengl_renderer/sprite/Sprite3_Glow.cpp
        graphics/opengl_renderer/sprite/Sprite3.cpp
        graphics/opengl_renderer/TextureAnimator.cpp
        graphics/opengl_renderer/TextureAnimatorDefs.cpp
        graphics/opengl_renderer/TextureUploadHandler.cpp
        graphics/opengl_renderer/VisDataHandler.cpp
        graphics/opengl_renderer/Warp.cpp
        graphics/pipelines/opengl.cpp
        graphics/sceGraphicsInterface.cpp
        graphics/texture/jak1_tpage_dir.cpp
        graphics/texture/jak2_tpage_dir.cpp
        graphics/texture/jak3_tpage_dir.cpp
        graphics/texture/TextureConverter.cpp
        graphics/texture/TexturePool.cpp
        ${OG_ASM_FUNCS_FILE}
        kernel/common/fileio.cpp
        kernel/common/kboot.cpp
        kernel/common/kdgo.cpp
        kernel/common/kdsnetm.cpp
        kernel/common/klink.cpp
        kernel/common/klisten.cpp
        kernel/common/kmachine.cpp
        kernel/common/kmalloc.cpp
        kernel/common/kmemcard.cpp
        kernel/common/kprint.cpp
        kernel/common/kscheme.cpp
        kernel/common/ksocket.cpp
        kernel/common/ksound.cpp
        kernel/jak1/fileio.cpp
        kernel/jak1/kboot.cpp
        kernel/jak1/kdgo.cpp
        kernel/jak1/klink.cpp
        kernel/jak1/klisten.cpp
        kernel/jak1/kmachine.cpp
        kernel/jak1/kprint.cpp
        kernel/jak1/kscheme.cpp
        kernel/jak1/ksound.cpp
        kernel/jak2/fileio.cpp
        kernel/jak2/kboot.cpp
        kernel/jak2/kdgo.cpp
        kernel/jak2/klink.cpp
        kernel/jak2/klisten.cpp
        kernel/jak2/kmachine.cpp
        kernel/jak2/kmachine_extras.cpp
        kernel/jak2/kmalloc.cpp
        kernel/jak2/kprint.cpp
        kernel/jak2/kscheme.cpp
        kernel/jak2/ksound.cpp
        kernel/jak3/fileio.cpp
        kernel/jak3/kboot.cpp
        kernel/jak3/kdgo.cpp
        kernel/jak3/kdsnetm.cpp
        kernel/jak3/klink.cpp
        kernel/jak3/klisten.cpp
        kernel/jak3/kmachine.cpp
        kernel/jak3/kmachine_extras.cpp
        kernel/jak3/kmalloc.cpp
        kernel/jak3/kmemcard.cpp
        kernel/jak3/kprint.cpp
        kernel/jak3/kscheme.cpp
        kernel/jak3/ksocket.cpp
        kernel/jak3/ksound.cpp
        kernel/jakx/fileio.cpp
        kernel/jakx/kboot.cpp
        kernel/jakx/kdgo.cpp
        kernel/jakx/kdsnetm.cpp
        kernel/jakx/klink.cpp
        kernel/jakx/klisten.cpp
        kernel/jakx/kmachine.cpp
        kernel/jakx/kmachine_extras.cpp
        kernel/jakx/kmalloc.cpp
        kernel/jakx/kmemcard.cpp
        kernel/jakx/kprint.cpp
        kernel/jakx/kscheme.cpp
        kernel/jakx/ksocket.cpp
        kernel/jakx/ksound.cpp
        mips2c/jak1_functions/bones.cpp
        mips2c/jak1_functions/collide_cache.cpp
        mips2c/jak1_functions/collide_edge_grab.cpp
        mips2c/jak1_functions/collide_func.cpp
        mips2c/jak1_functions/collide_mesh.cpp
        mips2c/jak1_functions/collide_probe.cpp
        mips2c/jak1_functions/draw_string.cpp
        mips2c/jak1_functions/generic_effect.cpp
        mips2c/jak1_functions/generic_effect2.cpp
        mips2c/jak1_functions/generic_merc.cpp
        mips2c/jak1_functions/generic_tie.cpp
        mips2c/jak1_functions/joint.cpp
        mips2c/jak1_functions/merc_blend_shape.cpp
        mips2c/jak1_functions/ocean_vu0.cpp
        mips2c/jak1_functions/ocean.cpp
        mips2c/jak1_functions/ripple.cpp
        mips2c/jak1_functions/shadow.cpp
        mips2c/jak1_functions/sky_tng.cpp
        mips2c/jak1_functions/sparticle_launcher.cpp
        mips2c/jak1_functions/sparticle.cpp
        mips2c/jak1_functions/test_func.cpp
        mips2c/jak1_functions/texture.cpp
        mips2c/jak1_functions/tfrag.cpp
        mips2c/jak1_functions/tie_methods.cpp
        mips2c/jak1_functions/time_of_day.cpp
        mips2c/jak2_functions/bones.cpp
        mips2c/jak2_functions/collide_cache.cpp
        mips2c/jak2_functions/collide_edge_grab.cpp
        mips2c/jak2_functions/collide_func.cpp
        mips2c/jak2_functions/collide_hash.cpp
        mips2c/jak2_functions/collide_mesh.cpp
        mips2c/jak2_functions/debug.cpp
        mips2c/jak2_functions/font.cpp
        mips2c/jak2_functions/foreground.cpp
        mips2c/jak2_functions/generic_effect.cpp
        mips2c/jak2_functions/generic_merc.cpp
        mips2c/jak2_functions/joint.cpp
        mips2c/jak2_functions/lights.cpp
        mips2c/jak2_functions/merc_blend_shape.cpp
        mips2c/jak2_functions/nav_control.cpp
        mips2c/jak2_functions/nav_mesh.cpp
        mips2c/jak2_functions/ocean_vu0.cpp
        mips2c/jak2_functions/ocean.cpp
        mips2c/jak2_functions/ripple.cpp
        mips2c/jak2_functions/shadow.cpp
        mips2c/jak2_functions/sky.cpp
        mips2c/jak2_functions/sparticle_launcher.cpp
        mips2c/jak2_functions/sparticle.cpp
        mips2c/jak2_functions/spatial_hash.cpp
        mips2c/jak2_functions/squid.cpp
        mips2c/jak2_functions/texture.cpp
        mips2c/jak2_functions/vortex.cpp
        mips2c/jak3_functions/collide_func.cpp
        mips2c/jak3_functions/debug.cpp
        mips2c/jak3_functions/font.cpp
        mips2c/jak3_functions/foreground.cpp
        mips2c/jak3_functions/generic_effect.cpp
        mips2c/jak3_functions/joint.cpp
        mips2c/jak3_functions/lights.cpp
        mips2c/jak3_functions/sky.cpp
        mips2c/jak3_functions/texture.cpp
        mips2c/jak3_functions/particle_curves.cpp
        mips2c/jak3_functions/cloth.cpp
        mips2c/jak3_functions/collide_cache.cpp
        mips2c/jak3_functions/collide_hash.cpp
        mips2c/jak3_functions/collide_edge_grab.cpp
        mips2c/jak3_functions/collide_mesh.cpp
        mips2c/jak3_functions/spatial_hash.cpp
        mips2c/jak3_functions/sparticle.cpp
        mips2c/jak3_functions/sparticle_launcher.cpp
        mips2c/jak3_functions/nav_control.cpp
        mips2c/jak3_functions/nav_engine.cpp
        mips2c/jak3_functions/merc_blend_shape.cpp
        mips2c/jak3_functions/wvehicle_part.cpp
        mips2c/jak3_functions/ripple.cpp
        mips2c/jak3_functions/shadow.cpp
        mips2c/jak3_functions/ocean.cpp
        mips2c/jak3_functions/ocean_vu0.cpp
        mips2c/jak3_functions/generic_merc.cpp
        mips2c/mips2c_table.cpp
        overlord/common/dma.cpp
        overlord/common/fake_iso.cpp
        overlord/common/iso_api.cpp
        overlord/common/iso.cpp
        overlord/common/isocommon.cpp
        overlord/common/989_plugins/plugin_strv.cpp
        overlord/common/overlord.cpp
        overlord/common/sbank.cpp
        overlord/common/soundcommon.cpp
        overlord/common/srpc.cpp
        overlord/common/ssound.cpp
        overlord/jak1/dma.cpp
        overlord/jak1/fake_iso.cpp
        overlord/jak1/iso_api.cpp
        overlord/jak1/iso_queue.cpp
        overlord/jak1/iso.cpp
        overlord/jak1/isocommon.cpp
        overlord/jak1/overlord.cpp
        overlord/jak1/ramdisk.cpp
        overlord/jak1/srpc.cpp
        overlord/jak1/ssound.cpp
        overlord/jak1/stream.cpp
        overlord/jak2/dma.cpp
        overlord/jak2/iso_api.cpp
        overlord/jak2/iso_cd.cpp
        overlord/jak2/iso_queue.cpp
        overlord/jak2/iso.cpp
        overlord/jak2/list.cpp
        overlord/jak2/overlord.cpp
        overlord/jak2/pages.cpp
        overlord/jak2/989_plugins/plugin_strv.cpp
        overlord/jak2/spustreams.cpp
        overlord/jak2/srpc.cpp
        overlord/jak2/ssound.cpp
        overlord/jak2/stream.cpp
        overlord/jak2/streamlfo.cpp
        overlord/jak2/streamlist.cpp
        overlord/jak2/vag.cpp
        overlord/jak3/overlord.cpp
        overlord/jak3/pagemanager.cpp
        overlord/jak3/iso_cd.cpp
        overlord/jak3/dma.cpp
        overlord/jak3/iso.cpp
        overlord/jak3/iso_queue.cpp
        overlord/jak3/srpc.cpp
        overlord/jak3/vag.cpp
        overlord/jak3/ssound.cpp
        overlord/jak3/iso_api.cpp
        overlord/jak3/spustreams.cpp
        overlord/jak3/list.cpp
        overlord/jak3/vblank_handler.cpp
        overlord/jak3/dvd_driver.cpp
        overlord/jak3/basefile.cpp
        overlord/jak3/basefilesystem.cpp
        overlord/jak3/ramdisk.cpp
        overlord/jak3/isocommon.cpp
        overlord/jak3/init.cpp
        overlord/jak3/stream.cpp
        overlord/jak3/sbank.cpp
        overlord/jak3/soundcommon.cpp
        overlord/jak3/streamlist.cpp
        overlord/jak3/989_plugins/plugin_strv.cpp
        overlord/jakx/overlord.cpp
        overlord/jakx/pagemanager.cpp
        overlord/jakx/iso_cd.cpp
        overlord/jakx/dma.cpp
        overlord/jakx/iso.cpp
        overlord/jakx/iso_queue.cpp
        overlord/jakx/srpc.cpp
        overlord/jakx/vag.cpp
        overlord/jakx/ssound.cpp
        overlord/jakx/iso_api.cpp
        overlord/jakx/spustreams.cpp
        overlord/jakx/list.cpp
        overlord/jakx/vblank_handler.cpp
        overlord/jakx/dvd_driver.cpp
        overlord/jakx/basefile.cpp
        overlord/jakx/basefilesystem.cpp
        overlord/jakx/ramdisk.cpp
        overlord/jakx/isocommon.cpp
        overlord/jakx/init.cpp
        overlord/jakx/stream.cpp
        overlord/jakx/sbank.cpp
        overlord/jakx/soundcommon.cpp
        overlord/jakx/streamlist.cpp
        runtime.cpp
        sce/deci2.cpp
        sce/iop.cpp
        sce/libcdvd_ee.cpp
        sce/libdma.cpp
        sce/libgraph.cpp
        sce/libpad.cpp
        sce/libscf.cpp
        sce/sif_ee_memcard.cpp
        sce/sif_ee.cpp
        sce/stubs.cpp
        settings/settings.cpp
        system/background_worker.cpp
        system/Deci2Server.cpp
        system/hid/devices/dualsense_effects.cpp
        system/hid/devices/game_controller.cpp
        system/hid/devices/keyboard.cpp
        system/hid/devices/mouse.cpp
        system/hid/display_manager.cpp
        system/hid/input_bindings.cpp
        system/hid/input_manager.cpp
        system/hid/sdl_util.cpp
        system/IOP_Kernel.cpp
        system/iop_thread.cpp
        system/SystemThread.cpp
        tools/filter_menu/filter_menu.cpp
        tools/subtitle_editor/subtitle_editor_db.cpp
        tools/subtitle_editor/subtitle_editor_repl_client.cpp
        tools/subtitle_editor/subtitle_editor.cpp
        )

add_subdirectory(sound)

include(kernel/jak1/data_arena.cmake)
if(APPLE)
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
        include(kernel/jak1/data_arena_arm64_invoker.cmake)
    elseif(NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        include(kernel/jak1/data_arena_arm64_invoker.cmake)
    endif()
endif()

# we build the runtime as a static library.
add_library(runtime STATIC ${RUNTIME_SOURCE} "../third-party/glad/src/glad.c")

target_link_libraries(runtime common fmt SDL3::SDL3 imgui discord-rpc sound stb_image libco libcurl)
if(WIN32)
    target_link_libraries(runtime mman)
else()
    target_link_libraries(runtime pthread dl)
endif()

add_executable(gk main.cpp)
target_link_libraries(gk runtime)
if(WIN32)
    set_target_properties(gk PROPERTIES VS_DPI_AWARE "PerMonitor")
endif()

# The portable kernel core is built the same way here as it is for iOS, so that host runs keep the
# cross-compiled configuration honest. It is a separate target from `runtime` on purpose: it must
# not gain a dependency on the desktop graphics/IOP/listener code.
if(ARM64_ARCH OR CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    include(kernel/core/kernel_core.cmake)
    if(GOAL_HOST_TESTS_ENABLED)
        add_executable(jak1-kernel-core-smoke-test kernel/core/host_smoke_test.cpp)
        target_link_libraries(jak1-kernel-core-smoke-test jak1-kernel-core)
        add_test(NAME jak1-kernel-core-smoke-test COMMAND jak1-kernel-core-smoke-test)

        # Real Jak 1 GOAL code, compiled ahead of time to C and then to native ARM64 by the host
        # compiler, executed inside the real kernel. goalc-cbackend emits C for the last source it
        # is given, so each object file needs its own run over the same ordered prefix.
        set(AOT_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/aot-generated")
        set(AOT_PREFIX_SOURCES
                "${CMAKE_SOURCE_DIR}/goal_src/jak1/compiler-setup.gc"
                "${CMAKE_SOURCE_DIR}/goal_src/jak1/kernel-defs.gc"
                "${CMAKE_SOURCE_DIR}/goal_src/jak1/kernel/gcommon.gc")
        set(AOT_GSTRING_SOURCES
                ${AOT_PREFIX_SOURCES}
                "${CMAKE_SOURCE_DIR}/goal_src/jak1/kernel/gstring-h.gc"
                "${CMAKE_SOURCE_DIR}/goal_src/jak1/kernel/gkernel-h.gc"
                "${CMAKE_SOURCE_DIR}/goal_src/jak1/kernel/gkernel.gc"
                "${CMAKE_SOURCE_DIR}/goal_src/jak1/kernel/pskernel.gc"
                "${CMAKE_SOURCE_DIR}/goal_src/jak1/kernel/gstring.gc")

        add_custom_command(
                OUTPUT "${AOT_GEN_DIR}/gcommon.c" "${AOT_GEN_DIR}/gcommon_generated.h"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${AOT_GEN_DIR}"
                COMMAND $<TARGET_FILE:goalc-cbackend>
                        --project-path "${CMAKE_SOURCE_DIR}" --tag gcommon
                        --output "${AOT_GEN_DIR}/gcommon.c"
                        --header "${AOT_GEN_DIR}/gcommon_generated.h"
                        ${AOT_PREFIX_SOURCES}
                DEPENDS goalc-cbackend ${AOT_PREFIX_SOURCES}
                COMMENT "Emitting C for jak1 kernel/gcommon.gc")

        add_custom_command(
                OUTPUT "${AOT_GEN_DIR}/gstring.c" "${AOT_GEN_DIR}/gstring_generated.h"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${AOT_GEN_DIR}"
                COMMAND $<TARGET_FILE:goalc-cbackend>
                        --project-path "${CMAKE_SOURCE_DIR}" --tag gstring
                        --output "${AOT_GEN_DIR}/gstring.c"
                        --header "${AOT_GEN_DIR}/gstring_generated.h"
                        ${AOT_GSTRING_SOURCES}
                DEPENDS goalc-cbackend ${AOT_GSTRING_SOURCES}
                COMMENT "Emitting C for jak1 kernel/gstring.gc")

        add_executable(jak1-aot-execution-test
                kernel/core/aot_execution_test.cpp
                "${AOT_GEN_DIR}/gcommon.c"
                "${AOT_GEN_DIR}/gstring.c")
        # GOAL reads and writes the same memory at several widths, exactly like the x86-64 backend
        # does, so the generated C must not be optimized under strict aliasing rules.
        set_source_files_properties(
                "${AOT_GEN_DIR}/gcommon.c" "${AOT_GEN_DIR}/gstring.c"
                PROPERTIES COMPILE_OPTIONS "-fno-strict-aliasing")
        target_include_directories(jak1-aot-execution-test PRIVATE "${AOT_GEN_DIR}")
        target_link_libraries(jak1-aot-execution-test jak1-kernel-core)
        add_test(NAME jak1-aot-execution-test COMMAND jak1-aot-execution-test)
    endif()
endif()
