if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64" AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR "The GOALPad runtime is ARM64-only.")
endif()
set(ARM64_ARCH TRUE)

set(OG_ASM_FUNCS_FILE kernel/asm_funcs_arm64.s)
enable_language(ASM)
set(CMAKE_ASM_SOURCE_FILE_EXTENSIONS ${CMAKE_ASM_SOURCE_FILE_EXTENSIONS} s)
set_source_files_properties(${OG_ASM_FUNCS_FILE} PROPERTIES COMPILE_FLAGS "-arch arm64 -g")

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
        graphics/opengl_renderer/ocean/OceanTexture_VU.cpp
        graphics/opengl_renderer/ocean/OceanTexture.cpp
        graphics/opengl_renderer/opengl_utils.cpp
        graphics/opengl_renderer/OpenGLRenderer.cpp
        graphics/opengl_renderer/Profiler.cpp
        graphics/opengl_renderer/ProgressRenderer.cpp
        graphics/opengl_renderer/Shader.cpp
        graphics/opengl_renderer/Shadow_PS2.cpp
        graphics/opengl_renderer/ShadowRenderer.cpp
        graphics/opengl_renderer/ShadowVu.cpp
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

if(APPLE)
    # experimental Metal graphics pipeline (see game/graphics/pipelines/metal/)
    set(METAL_PIPELINE_SOURCES
        graphics/pipelines/metal/metal_bucket_renderer.mm
        graphics/pipelines/metal/metal_direct_renderer.mm
        graphics/pipelines/metal/metal_eye_renderer.mm
        graphics/pipelines/metal/metal_generic2.mm
        graphics/pipelines/metal/metal_shadow_renderer.mm
        graphics/pipelines/metal/metal_level_data.mm
        graphics/pipelines/metal/metal_merc.mm
        graphics/pipelines/metal/metal_merc_model_pool.mm
        graphics/pipelines/metal/metal_ocean_renderer.mm
        graphics/pipelines/metal/metal_pipeline.mm
        graphics/pipelines/metal/metal_pso_cache.mm
        graphics/pipelines/metal/metal_renderer.mm
        graphics/pipelines/metal/metal_shrub.mm
        graphics/pipelines/metal/metal_sky_renderer.mm
        graphics/pipelines/metal/metal_sprite_renderer.mm
        graphics/pipelines/metal/metal_texture.mm
        graphics/pipelines/metal/metal_tfrag.mm
        graphics/pipelines/metal/metal_tie.mm)
    list(APPEND RUNTIME_SOURCE ${METAL_PIPELINE_SOURCES})
    set_source_files_properties(${METAL_PIPELINE_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    # plain C++ parts of the Metal path (testable / kernel-header-safe without
    # Objective-C++)
    list(APPEND RUNTIME_SOURCE
        graphics/pipelines/metal/metal_jak2_bucket_table.cpp
        graphics/pipelines/metal/metal_kernel_bridge.cpp
        graphics/pipelines/metal/metal_texture_upload_handler.cpp)

    # MSL shaders are checked-in source compiled at build time to a metallib and
    # embedded in the runtime as a byte array. No shader compilation happens at
    # runtime, which keeps the app signing-clean for iPadOS.
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(METAL_SDK iphoneos)
    else()
        set(METAL_SDK macosx)
    endif()
    set(METAL_SHADER_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/pipelines/metal/shaders/scaffold.metal
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/pipelines/metal/shaders/direct.metal
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/pipelines/metal/shaders/sprite.metal
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/pipelines/metal/shaders/background.metal
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/pipelines/metal/shaders/ocean.metal
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/pipelines/metal/shaders/merc2.metal
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/pipelines/metal/shaders/eye.metal
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/pipelines/metal/shaders/generic.metal
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/pipelines/metal/shaders/shadow.metal)
    set(METAL_LIB ${CMAKE_CURRENT_BINARY_DIR}/metal_shaders/goalpad.metallib)
    add_custom_command(
        OUTPUT ${METAL_LIB}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/metal_shaders
        COMMAND xcrun -sdk ${METAL_SDK} metal -Wall -Werror -o ${METAL_LIB} ${METAL_SHADER_SOURCES}
        DEPENDS ${METAL_SHADER_SOURCES}
        COMMENT "Compiling MSL shaders to ${METAL_LIB}")
    set(METAL_LIB_EMBED ${CMAKE_CURRENT_BINARY_DIR}/metal_shaders/goalpad_metallib_embed.c)
    set(METAL_EMBED_SCRIPT
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/pipelines/metal/embed_metallib.cmake)
    add_custom_command(
        OUTPUT ${METAL_LIB_EMBED}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${METAL_LIB} -DOUTPUT=${METAL_LIB_EMBED}
                -DSYMBOL=g_goalpad_metallib -P ${METAL_EMBED_SCRIPT}
        DEPENDS ${METAL_LIB} ${METAL_EMBED_SCRIPT}
        COMMENT "Embedding ${METAL_LIB}")
    list(APPEND RUNTIME_SOURCE ${METAL_LIB_EMBED})
endif()

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

if(GOAL_HOST_TESTS_ENABLED)
    add_executable(metal-bucket-chain-semantics-test
            graphics/pipelines/metal/metal_bucket_chain_semantics_test.cpp)
    add_test(NAME metal-bucket-chain-semantics-test COMMAND metal-bucket-chain-semantics-test)

    add_executable(jak2-metal-bucket-table-test
            graphics/pipelines/metal/metal_jak2_bucket_table.cpp
            graphics/pipelines/metal/metal_jak2_bucket_table_test.cpp)
    add_test(NAME jak2-metal-bucket-table-test COMMAND jak2-metal-bucket-table-test)
endif()

# The portable kernel core is built the same way here as it is for iOS, so that host runs keep the
# cross-compiled configuration honest. It is a separate target from `runtime` on purpose: it must
# not gain a dependency on the desktop graphics/IOP/listener code.
if(ARM64_ARCH OR CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    include(kernel/core/kernel_core.cmake)
    if(GOAL_HOST_TESTS_ENABLED)
        add_executable(metal-camera-trace-test
                graphics/pipelines/metal/metal_camera_trace_test.cpp)
        add_test(NAME metal-camera-trace-test COMMAND metal-camera-trace-test)

        add_executable(metal-merc-transform-trace-test
                graphics/pipelines/metal/metal_merc_transform_trace_test.cpp)
        add_test(NAME metal-merc-transform-trace-test COMMAND metal-merc-transform-trace-test)

        add_executable(jak1-mips2c-joint-regression-test
                kernel/core/jak1_mips2c_joint_regression_test.cpp
                mips2c/jak1_functions/joint.cpp)
        target_include_directories(jak1-mips2c-joint-regression-test PRIVATE
                "${CMAKE_SOURCE_DIR}/third-party/fmt/include")
        add_test(NAME jak1-mips2c-joint-regression-test
                COMMAND jak1-mips2c-joint-regression-test)

        add_executable(jak1-kernel-core-smoke-test kernel/core/host_smoke_test.cpp)
        target_link_libraries(jak1-kernel-core-smoke-test jak1-kernel-core)
        add_test(NAME jak1-kernel-core-smoke-test COMMAND jak1-kernel-core-smoke-test)

        add_executable(jak1-gfx-host-test kernel/core/jak1_gfx_host_test.cpp)
        target_link_libraries(jak1-gfx-host-test jak1-kernel-core)
        add_test(NAME jak1-gfx-host-test COMMAND jak1-gfx-host-test)

        add_executable(jak2-pad-seam-test kernel/core/jak2_pad_test.cpp)
        target_link_libraries(jak2-pad-seam-test jak2-kernel-core)
        add_test(NAME jak2-pad-seam-test COMMAND jak2-pad-seam-test)

        add_executable(jak2-handler-seam-test kernel/core/jak2_handler_test.cpp)
        target_link_libraries(jak2-handler-seam-test jak2-kernel-core)
        add_test(NAME jak2-handler-seam-test COMMAND jak2-handler-seam-test)

        add_executable(jak2-pc-rand-test kernel/core/jak2_pc_rand_test.cpp)
        target_link_libraries(jak2-pc-rand-test jak2-kernel-core)
        add_test(NAME jak2-pc-rand-test COMMAND jak2-pc-rand-test)

        add_executable(jak2-lightweight-machine-test
                kernel/core/jak2_lightweight_machine_test.cpp)
        target_link_libraries(jak2-lightweight-machine-test jak2-kernel-core)
        add_test(NAME jak2-lightweight-machine-test COMMAND jak2-lightweight-machine-test)

        add_executable(jak2-dma-boundary-test kernel/core/jak2_dma_boundary_test.cpp)
        target_link_libraries(jak2-dma-boundary-test jak2-kernel-core)
        add_test(NAME jak2-dma-boundary-test COMMAND jak2-dma-boundary-test)

        add_executable(jak2-gfx-host-test kernel/core/jak2_gfx_host_test.cpp)
        target_link_libraries(jak2-gfx-host-test jak2-kernel-core)
        add_test(NAME jak2-gfx-host-test COMMAND jak2-gfx-host-test)

        add_executable(jak2-display-tick-coordinator-test
                kernel/core/jak2_display_tick_coordinator_test.cpp)
        target_link_libraries(jak2-display-tick-coordinator-test jak2-kernel-core)
        add_test(NAME jak2-display-tick-coordinator-test
                COMMAND jak2-display-tick-coordinator-test)

        add_executable(jak2-sound-rpc-test kernel/core/jak2_sound_rpc_test.cpp)
        target_link_libraries(jak2-sound-rpc-test jak2-kernel-core)
        add_test(NAME jak2-sound-rpc-test COMMAND jak2-sound-rpc-test)

        add_executable(jak2-dgo-rpc-test kernel/core/jak2_dgo_rpc_test.cpp)
        target_link_libraries(jak2-dgo-rpc-test jak2-kernel-core)
        add_test(NAME jak2-dgo-rpc-test COMMAND jak2-dgo-rpc-test)

        add_executable(jak2-sblk-preflight-test
                kernel/core/jak2_sblk_preflight_test.cpp
                kernel/core/sblk_preflight.cpp)
        target_compile_features(jak2-sblk-preflight-test PRIVATE cxx_std_20)
        add_test(NAME jak2-sblk-preflight-test COMMAND jak2-sblk-preflight-test)

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

        # The boot probe: Jak 1's object files loaded into the real kernel in the order game.gp
        # builds them, with each one's top-level run, going as far as the runtime can get.
        #
        # goalc-cbackend-sweep reads that order out of game.gp and emits one C file per source plus
        # aot_boot_manifest.c. The tag list below is that same order and is what tells CMake which
        # files the command produces; it comes from
        #   goalc-cbackend-sweep --limit ${JAK1_AOT_BOOT_FILE_COUNT} --c-output-dir ...
        # and has to be updated together with the count.
        set(AOT_BOOT_DIR "${CMAKE_CURRENT_BINARY_DIR}/aot-boot")
        #
        # This is now the whole game - all 518 object files - because jak1-data-boot-test needs
        # every file GAME.CGO can name. jak1-aot-boot-test still walks them in game.gp order with
        # no game data loaded, and stops at file 512, pc/hud-classes-pc.gc, for want of data
        # rather than for want of runtime: its top-level spawns a hud process whose
        # init-particles! dereferences *fuelcell-naked-sg*, an art group that only exists once
        # level art has been loaded. JAK1_AOT_BOOT_FRONTIER below is that documented frontier.
        set(AOT_BOOT_TAGS
                gcommon gstring_h gkernel_h gkernel pskernel gstring dgo_h gstate types_h vu1_macros
                math vector_h gravity_h bounding_box_h matrix_h quaternion_h euler_h transform_h
                geometry_h trigonometry_h transformq_h bounding_box matrix transform quaternion
                euler geometry trigonometry gsound_h timer_h timer vif_h dma_h video_h vu1_user_h
                dma dma_buffer pckernel_h pc_cheats pckernel_impl pad dma_bucket dma_disasm gs
                display_h vector file_io loader_h texture_h level_h math_camera_h math_camera font_h
                decomp_h display connect text_h settings_h speedruns_h capture memory_usage_h
                texture main_h mspace_h drawable_h drawable_group_h drawable_inline_array_h
                draw_node_h drawable_tree_h drawable_actor_h drawable_ambient_h game_task_h
                hint_control_h generic_h lights_h ocean_h ocean_trans_tables ocean_tables
                ocean_frames sky_h mood_h time_of_day_h art_h generic_vu1_h merc_h generic_merc_h
                generic_tie_h generic_work_h shadow_cpu_h shadow_vu1_h memcard_h game_info_h wind_h
                prototype_h joint_h bones_h engines res_h res lights dynamics_h surface_h pat_h
                fact_h aligner_h game_h generic_obs_h pov_camera_h sync_info_h smush_control_h
                trajectory_h debug_h joint_mod_h collide_func_h collide_mesh_h collide_shape_h
                collide_target_h collide_touch_h collide_edge_grab_h process_drawable_h
                effect_control_h collide_frag_h projectiles_h target_h pc_anim_util depth_cue_h
                stats_h bsp_h collide_cache_h collide_h shrubbery_h tie_h tfrag_h background_h
                subdivide_h entity_h sprite_h shadow_h eye_h sparticle_launcher_h sparticle_h
                actor_link_h camera_h cam_debug_h cam_interface_h cam_update_h assert_h hud_h
                progress_h rpc_h path_h navigate_h load_dgo ramdisk gsound transformq collide_func
                joint cylinder wind bsp subdivide sprite sprite_distort debug_sphere debug merc_vu1
                merc_blend_shape merc ripple bones generic_vu0 generic generic_vu1 generic_effect
                generic_merc generic_tie shadow_cpu shadow_vu1 depth_cue font decomp background
                draw_node shrubbery shrub_work tfrag_near tfrag tfrag_methods tfrag_work tie
                tie_near tie_work tie_methods sync_info trajectory sparticle_launcher sparticle
                entity_table loader task_control_h game_info game_save settings mood_tables mood
                weather_part time_of_day sky_utils sky sky_tng load_boundary_h load_boundary
                load_boundary_data level_info level text collide_probe collide_frag collide_mesh
                collide_touch collide_edge_grab collide_shape collide_shape_rider collide merc_death
                water_h camera cam_interface cam_master cam_states cam_states_dbg cam_combiner
                cam_update vol_h cam_layout cam_debug cam_start process_drawable hint_control
                ambient assert generic_obs target_util target_part collide_reaction_target
                logic_target sidekick voicebox target_handler target target2 target_death menu
                drawable drawable_group drawable_inline_array drawable_tree prototype main_collide
                video autosplit_h pckernel_common pckernel main collide_cache relocate memory_usage
                entity path vol navigate aligner effect_control water collectables_part collectables
                task_control process_taskable pov_camera powerups crates hud hud_classes
                progress_static progress_part progress_draw progress credits projectiles ocean
                ocean_vu0 ocean_texture ocean_mid ocean_transition ocean_near shadow eye glist_h
                glist anim_tester viewer part_tester default_menu texture_upload rigid_body_h
                water_anim dark_eco_pool rigid_body nav_enemy_h nav_enemy baseplat basebutton tippy
                joint_exploder babak sharkey orb_cache plat plat_button plat_eco ropebridge ticky
                villagep_obs oracle blocking_plane launcherdoor battlecontroller target_racer_h
                racer_part racer target_racer racer_states collide_reaction_racer flut_part flutflut
                target_flut air_h air wobbler twister beach_obs bird_lady bird_lady_beach mayor
                sculptor pelican lurkerworm lurkercrab lurkerpuppy beach_rocks seagull beach_part
                jungle_elevator bouncer hopper junglesnake darkvine jungle_obs jungle_mirrors
                junglefish fisher jungle_part farmer explorer assistant sage yakow village_obs
                fishermans_boat village1_part village1_part2 sequence_a_village1 jungleb_obs
                plat_flip aphid plant_boss evilbro mistycannon babak_with_cannon misty_obs
                misty_warehouse misty_conveyor mud muse bonelurker quicksandlurker
                misty_teetertotter balloonlurker misty_part sidekick_human swamp_obs swamp_bat
                swamp_rat swamp_rat_nest kermit swamp_part billy sunken_part sunken_part2
                sunken_part3 sunken_part4 sunken_part5 target_tube sunken_obs shover square_platform
                sun_iris_door orbit_plat wedge_plats wall_plat qbert_plat steam_cap sun_exit_chamber
                floating_launcher sunken_water whirlpool sunken_pipegame bully double_lurker
                helix_water puffer sunken_fish target_snowball target_ice ice_cube snow_ball
                snow_obs snow_flutflut_obs snow_bumper snow_ram_h snow_ram_boss snow_ram snow_part
                yeti snow_bunny firecanyon_part assistant_firecanyon firecanyon_obs ogre_part
                ogreboss ogre_obs flying_lurker village2_part village2_obs village2_part2 gambler
                warrior geologist swamp_blimp sage_bluehut flutflut_bluehut assistant_village2
                sunken_elevator rolling_obs rolling_lightning_mole rolling_robber rolling_race_ring
                village3_part village3_obs minecart miners assistant_village3 sage_village3
                training_obs training_part cavecrystal_light darkcave_obs maincave_obs maincave_part
                spiderwebs dark_crystal baby_spider mother_spider_h mother_spider_egg
                mother_spider_proj mother_spider gnawer driller_lurker cave_trap spider_egg
                robocave_part lavatube_obs lavatube_energy lavatube_part assistant_lavatube
                citadel_part citadel_obs citb_plat citadel_sages citb_bunny citb_drop_plat
                assistant_citadel robotboss_h robotboss_part sage_finalboss_part light_eco
                robotboss_weapon robotboss_misc green_eco_lurker robotboss final_door sage_finalboss
                static_screen demo_obs progress_pc title_obs knuth_rand autosplit speedruns subtitle
                hud_classes_pc anim_tester_x entity_debug default_menu_pc pc_debug_common
                pc_debug_methods test_zone_obs)
        list(LENGTH AOT_BOOT_TAGS JAK1_AOT_BOOT_FILE_COUNT)
        set(JAK1_AOT_BOOT_FRONTIER 511)

        set(AOT_BOOT_OUTPUTS "${AOT_BOOT_DIR}/aot_boot_manifest.c"
                "${AOT_BOOT_DIR}/aot_boot_manifest.h")
        set(AOT_BOOT_C_SOURCES "")
        foreach(tag IN LISTS AOT_BOOT_TAGS)
            list(APPEND AOT_BOOT_C_SOURCES "${AOT_BOOT_DIR}/${tag}.c")
            list(APPEND AOT_BOOT_OUTPUTS "${AOT_BOOT_DIR}/${tag}.c"
                    "${AOT_BOOT_DIR}/${tag}_generated.h")
        endforeach()

        file(GLOB_RECURSE AOT_BOOT_GOAL_SOURCES CONFIGURE_DEPENDS
                "${CMAKE_SOURCE_DIR}/goal_src/jak1/*.gc")
        add_custom_command(
                OUTPUT ${AOT_BOOT_OUTPUTS}
                COMMAND $<TARGET_FILE:goalc-cbackend-sweep>
                        --project-path "${CMAKE_SOURCE_DIR}"
                        --limit ${JAK1_AOT_BOOT_FILE_COUNT}
                        --c-output-dir "${AOT_BOOT_DIR}"
                        --report "${AOT_BOOT_DIR}/coverage.tsv"
                DEPENDS goalc-cbackend-sweep ${AOT_BOOT_GOAL_SOURCES}
                        "${CMAKE_SOURCE_DIR}/goal_src/goal-lib.gc"
                        "${CMAKE_SOURCE_DIR}/goal_src/jak1/game.gp"
                COMMENT
                "Emitting C for the first ${JAK1_AOT_BOOT_FILE_COUNT} Jak 1 object files")

        add_executable(jak1-aot-boot-test
                kernel/core/aot_boot_test.cpp
                "${AOT_BOOT_DIR}/aot_boot_manifest.c"
                ${AOT_BOOT_C_SOURCES})
        set_source_files_properties(
                "${AOT_BOOT_DIR}/aot_boot_manifest.c" ${AOT_BOOT_C_SOURCES}
                PROPERTIES COMPILE_OPTIONS "-fno-strict-aliasing")
        target_include_directories(jak1-aot-boot-test PRIVATE "${AOT_BOOT_DIR}")
        target_compile_definitions(jak1-aot-boot-test
                PRIVATE JAK1_AOT_BOOT_FRONTIER=${JAK1_AOT_BOOT_FRONTIER})
        target_link_libraries(jak1-aot-boot-test jak1-kernel-core)
        add_test(NAME jak1-aot-boot-test COMMAND jak1-aot-boot-test)

        # The same object files, loaded the way the game loads them: out of KERNEL.CGO and
        # GAME.CGO, in DGO order, with each object's code coming from the AOT path and each
        # object's data coming from the archive. Registered three times: --synthetic uses a DGO
        # this test writes itself and needs no game data; the other two need the player's own
        # extracted data and skip cleanly without it. The --play run goes on to the engine's own
        # startup, which drives a level DGO through the RPC and runs the GOAL kernel's frame loop.
        add_executable(jak1-data-boot-test
                kernel/core/data_boot_test.cpp
                kernel/core/continue_warp.cpp
                kernel/core/scripted_walk.cpp
                "${AOT_BOOT_DIR}/aot_boot_manifest.c"
                ${AOT_BOOT_C_SOURCES})
        target_include_directories(jak1-data-boot-test PRIVATE "${AOT_BOOT_DIR}")
        target_link_libraries(jak1-data-boot-test jak1-kernel-core)
        add_test(NAME jak1-dgo-synthetic-test COMMAND jak1-data-boot-test --synthetic)
        add_test(NAME jak1-data-boot-test COMMAND jak1-data-boot-test)
        add_test(NAME jak1-play-frames-test COMMAND jak1-data-boot-test --play --frames 60)
        # Sound. The title screen mutes music while the intro plays and puts it back in
        # target-title-wait, so this needs enough frames to get that far before it can ask whether
        # the mixer produced anything. It measures the audio rather than writing it: the suite
        # never produces a file of the player's own game audio. --capture-audio does that, on
        # request, wherever the caller says.
        add_test(NAME jak1-title-audio-test
                COMMAND jak1-data-boot-test --play --frames 1400 --require-audio)
        # Unload and reload a level through the level system's own load state. Level code is linked
        # into the level's heap, which unloading resets, so the global heap has to stand still while
        # the level's object files are linked over again.
        add_test(NAME jak1-level-cycle-test
                COMMAND jak1-data-boot-test --play --frames 200 --level-frames 600
                        --levels title,title+village1,title)
        # The controller seam against the real cpad-info structure. Needs no game data, so this
        # always runs.
        add_test(NAME jak1-pad-seam-test COMMAND jak1-data-boot-test --pad-seam)
        # Play the game with a scripted controller: leave the title screen, start a new game
        # through the progress menu, sit through the whole intro - whose length the streamed
        # dialogue now decides - and then walk and jump on Geyser Rock. Every press is scheduled
        # off a state the target entered rather than off a frame number, so the game's own load
        # and scene timing decides when each input lands. `target-hit-ground` is the warp drop
        # onto Geyser Rock, the first state that only exists once the intro has handed control
        # over.
        add_test(NAME jak1-gameplay-test
                COMMAND jak1-data-boot-test --play --frames 24800
                        --press start@target-title-wait+60
                        --press x@target-title-wait+160
                        --press down@target-title-wait+240
                        --press down@target-title-wait+260
                        --press down@target-title-wait+280
                        --press down@target-title-wait+300
                        --press x@target-title-wait+340
                        --stick 127,0@target-hit-ground+120:400
                        --press x@target-walk+700
                        --expect-state target-title-wait
                        --expect-state target-clone-anim
                        --expect-state target-hit-ground
                        --expect-state target-walk
                        --expect-state target-jump
                        --expect-travel 10)
        set_tests_properties(jak1-gameplay-test PROPERTIES TIMEOUT 900)

        # Level transitions. Jak 1 is a connected world, and the two things that move a player
        # between its levels are a load boundary and a continue point. Both are exercised here
        # against the player's own data.
        #
        # The boundary test is the walk: it starts a new game, warps to the village hut the way a
        # warp gate does, holds the debug menu's own border-mode flag on so `render-boundaries`
        # runs, and then walks south with the stick until the target crosses the village1/beach
        # display boundary. `beach` is only 'loaded when the walk begins - the hut's continue point
        # asks for it without displaying it - so it reaching 'active is the boundary crossing and
        # nothing else. The route is aimed by measuring which way the camera-relative stick moved
        # the target, so it needs no camera knowledge and survives the camera turning.
        add_test(NAME jak1-level-boundary-test
                COMMAND jak1-data-boot-test --play --frames 9000 --no-sound --border-mode
                        --press start@target-title-wait+60
                        --press x@target-title-wait+160
                        --press down@target-title-wait+240
                        --press down@target-title-wait+260
                        --press down@target-title-wait+280
                        --press down@target-title-wait+300
                        --press x@target-title-wait+340
                        --warp village1-hut@target-stance+300
                        --walk-to -120,170@target-stance+400
                        --walk-to -135,60@target-stance+400
                        --walk-to -135,10@target-stance+400
                        --expect-state target-walk
                        --expect-level beach
                        --expect-travel 100)
        # The tour: every level a continue point names, loaded and displayed through the level
        # system, one after another. It is the level heap that this is about - a level's code and
        # data are linked into it and freed with it - so the global heap must stay nearly still
        # while 2 MB of level code is linked over and over. The limit is what a device with a
        # memory ceiling has to pay for having been everywhere.
        add_test(NAME jak1-level-tour-test
                COMMAND jak1-data-boot-test --play --frames 9000 --no-sound --report-levels
                        --press start@target-title-wait+60
                        --press x@target-title-wait+160
                        --press down@target-title-wait+240
                        --press down@target-title-wait+260
                        --press down@target-title-wait+280
                        --press down@target-title-wait+300
                        --press x@target-title-wait+340
                        --warp beach-start@target-stance+300
                        --warp jungle-start@target-stance+1000
                        --warp firecanyon-start@target-stance+1700
                        --warp village2-start@target-stance+2400
                        --warp sunken-start@target-stance+3100
                        --warp village3-start@target-stance+3800
                        --warp lavatube-start@target-stance+4500
                        --warp citadel-start@target-stance+5200
                        --warp finalboss-start@target-stance+5900
                        --warp training-start@target-stance+6600
                        --expect-level beach
                        --expect-level jungle
                        --expect-level firecanyon
                        --expect-level village2
                        --expect-level sunken
                        --expect-level village3
                        --expect-level lavatube
                        --expect-level citadel
                        --expect-level finalboss
                        --expect-level training
                        --expect-heap-growth 262144)

        # The thread-switch test: the same GOAL kernel, plus a fixture that drives the routines
        # that had to be written in native ARM64 - catch frames, throw, state entry, temporary
        # threads, suspend and resume. The kernel prefix is the first 8 object files in game.gp
        # build order, which is everything through kernel/gstate.gc.
        set(AOT_TSW_DIR "${CMAKE_CURRENT_BINARY_DIR}/aot-thread-switch")
        set(AOT_TSW_FIXTURE "${CMAKE_SOURCE_DIR}/test/goalc/aot/thread_switch_test.gc")
        set(AOT_TSW_TAGS gcommon gstring_h gkernel_h gkernel pskernel gstring dgo_h gstate
                thread_switch_test)
        list(LENGTH AOT_TSW_TAGS AOT_TSW_KERNEL_COUNT)
        math(EXPR AOT_TSW_KERNEL_COUNT "${AOT_TSW_KERNEL_COUNT} - 1")

        set(AOT_TSW_OUTPUTS "${AOT_TSW_DIR}/aot_boot_manifest.c"
                "${AOT_TSW_DIR}/aot_boot_manifest.h")
        set(AOT_TSW_C_SOURCES "")
        foreach(tag IN LISTS AOT_TSW_TAGS)
            list(APPEND AOT_TSW_C_SOURCES "${AOT_TSW_DIR}/${tag}.c")
            list(APPEND AOT_TSW_OUTPUTS "${AOT_TSW_DIR}/${tag}.c"
                    "${AOT_TSW_DIR}/${tag}_generated.h")
        endforeach()

        add_custom_command(
                OUTPUT ${AOT_TSW_OUTPUTS}
                COMMAND $<TARGET_FILE:goalc-cbackend-sweep>
                        --project-path "${CMAKE_SOURCE_DIR}"
                        --limit ${AOT_TSW_KERNEL_COUNT}
                        --extra-source test/goalc/aot/thread_switch_test.gc
                        --c-output-dir "${AOT_TSW_DIR}"
                        --report "${AOT_TSW_DIR}/coverage.tsv"
                DEPENDS goalc-cbackend-sweep ${AOT_BOOT_GOAL_SOURCES} ${AOT_TSW_FIXTURE}
                        "${CMAKE_SOURCE_DIR}/goal_src/goal-lib.gc"
                        "${CMAKE_SOURCE_DIR}/goal_src/jak1/game.gp"
                COMMENT "Emitting C for the GOAL kernel and the thread-switch fixture")

        add_executable(jak1-thread-switch-test
                kernel/core/thread_switch_test.cpp
                kernel/core/goal_thread_test_arm64.s
                "${AOT_TSW_DIR}/aot_boot_manifest.c"
                ${AOT_TSW_C_SOURCES})
        set_source_files_properties(
                "${AOT_TSW_DIR}/aot_boot_manifest.c" ${AOT_TSW_C_SOURCES}
                PROPERTIES COMPILE_OPTIONS "-fno-strict-aliasing")
        target_include_directories(jak1-thread-switch-test PRIVATE "${AOT_TSW_DIR}")
        target_link_libraries(jak1-thread-switch-test jak1-kernel-core)
        add_test(NAME jak1-thread-switch-test COMMAND jak1-thread-switch-test)

        # The Jak 2 boot probe: every jak2 object file, emitted by the C backend for jak2 in
        # game.gp build order, loaded into the jak2 kernel out of the player's own KERNEL.CGO,
        # then the jak2 kernel dispatcher frame after frame. The whole game is emitted - like
        # jak1's list above - so the exploratory GAME.CGO attempt (--with-game) can name any
        # object the archive holds. Skips cleanly without GOALPAD_JAK2_DATA_DIR.
        set(JAK2_AOT_BOOT_DIR "${CMAKE_CURRENT_BINARY_DIR}/aot-boot-jak2")
        set(JAK2_AOT_BOOT_TAGS
        gcommon gstring_h gkernel_h gkernel pskernel gstring dgo_h gstate types_h vu1_macros math
        vector_h gravity_h bounding_box_h matrix_h quaternion_h euler_h transform_h geometry_h
        trigonometry_h transformq_h bounding_box matrix transform quaternion euler trigonometry
        gsound_h timer_h vif_h dma_h video_h vu1_user_h profile_h dma dma_buffer dma_bucket
        dma_disasm statistics_h pc_cheats pckernel_h pckernel_impl pc_debug_common pad gs display_h
        geometry timer vector file_io loader_h texture_h texture_anim_h lights_h mood_h level_h
        capture_h math_camera_h math_camera font_h decomp_h profile display connect text_id_h
        text_h camera_defs_h trail_h minimap_h bigmap_h settings_h capture memory_usage_h
        blit_displays_h texture main_h mspace_h drawable_h drawable_group_h drawable_inline_array_h
        draw_node_h drawable_tree_h drawable_actor_h region_h traffic_h game_task_h task_control_h
        generic_h sky_h ocean_h ocean_trans_tables ocean_tables ocean_frames time_of_day_h art_h
        generic_vu1_h merc_h generic_merc_h generic_tie_h generic_work_h shadow_cpu_h shadow_vu1_h
        memcard_h game_info_h gui_h ambient_h speech_h wind_h prototype_h joint_h bones_h
        foreground_h engines lightning_h res_h res lights dynamics_h surface_h pat_h fact_h
        aligner_h penetrate_h game_h script_h scene_h sync_info_h pov_camera_h smush_control_h
        debug_h joint_mod_h collide_func_h collide_mesh_h collide_shape_h generic_obs_h
        trajectory_h collide_target_h collide_touch_h collide_edge_grab_h process_drawable_h
        process_focusable process_taskable_h focus effect_control_h collide_frag_h collide_hash_h
        chain_physics_h projectile_h find_nearest_h target_h stats_h bsp_h collide_cache_h
        collide_h shrubbery_h tie_h tfrag_h background_h subdivide_h entity_h sprite_h
        simple_sprite_h eye_h sparticle_launcher_h sparticle_h actor_link_h camera_h cam_debug_h
        cam_interface_h cam_update_h hud_h progress_h progress_h_pc progress_generic_h_pc rpc_h
        path_h nav_mesh_h nav_control_h spatial_hash_h actor_hash_h load_dgo ramdisk gsound
        transformq collide_func joint joint_mod chain_physics cylinder wind_work wind bsp subdivide
        sprite sprite_distort sprite_glow debug_sphere debug history merc_vu1 emerc_vu1
        merc_blend_shape merc emerc ripple bones debug_foreground foreground generic_vu0
        generic_vu1 generic_effect generic_merc generic_tie shadow_cpu shadow_vu1 warp texture_anim
        texture_anim_funcs texture_anim_tables blit_displays font_data font decomp background
        draw_node shrubbery shrub_work tfrag_near tfrag tfrag_methods tfrag_work tie etie_vu1
        etie_near_vu1 tie_near tie_work tie_methods sync_info trajectory sparticle_launcher
        sparticle entity_table loader game_info game_task game_save settings autosplit_h autosplit
        popup_menu_h speedruns_h mood_tables mood_tables2 mood mood_funcs mood_funcs2 weather_part
        time_of_day sky_data sky_tng load_state pc_debug_methods level_info level text collide_hash
        collide_probe collide_frag collide_mesh collide_touch collide_edge_grab collide_shape
        collide_shape_rider collide spatial_hash actor_hash merc_death water_flow water_h camera
        cam_interface cam_master cam_states cam_states_dbg cam_combiner cam_update vol_h cam_layout
        cam_debug cam_start process_drawable ambient speech region fma_sphere script generic_obs
        lightning carry_h pilot_h gun_h board_h darkjak_h target_util target_part gun_part
        collide_reaction_target logic_target sidekick effect_control voicebox collectables_part
        debug_part find_nearest task_arrow projectile target_handler target_anim target target2
        target_swim target_carry target_darkjak target_death target_gun gun_util gun_blue_shot
        gun_yellow_shot gun_red_shot gun_dark_shot gun_states board_util target_board board_part
        board_states mech_h menu popup_menu speedruns drawable drawable_group drawable_inline_array
        drawable_tree prototype main_collide video capture_pc pckernel_common pckernel subtitle2_h
        subtitle2 main collide_cache collide_debug relocate memory_usage entity path vol nav_mesh
        nav_control aligner water collectables task_control scene pov_camera powerups crates hud
        hud_classes progress_static progress_static_pc progress progress_pc progress_generic_pc
        progress_draw progress_draw_pc progress_generic_draw_pc ocean ocean_vu0 ocean_texture
        ocean_mid ocean_transition ocean_near minimap bigmap_data bigmap eye glist_h glist
        anim_tester viewer part_tester editable_h editable editable_player mysql_nav_graph
        nav_graph_editor sampler default_menu anim_tester_x vag_player default_menu_pc
        texture_upload texture_finish los_control_h airlock water_anim blocking_plane idle_control
        dark_eco_pool enemy_h nav_enemy_h rigid_body_h damage_number statistics enemy nav_enemy
        base_plat plat basebutton bouncer conveyor elevator rigid_body rigid_body_queue
        rigid_body_plat joint_exploder simple_focus simple_nav_sphere process_taskable scene_actor
        scene_looper warp_gate guard_projectile metalhead_projectile los_control grunt flitter
        battle elec_gate cty_guard_turret_button entity_debug nav_graph_h traffic_engine_h
        vehicle_h citizen_h height_map_h ctywide_obs_h height_map traffic_height_map nav_graph
        vehicle_rider vehicle_control vehicle_part vehicle_effects vehicle vehicle_util
        vehicle_physics vehicle_states vehicle_guard transport bike car test_bike test_car citizen
        civilian guard citizen_norm citizen_fat citizen_chick citizen_enemy metalhead_predator
        metalhead_grunt metalhead_flitter traffic_engine trail_graph trail traffic_manager
        ctywide_texture ctywide_part ctywide_obs ctywide_tasks ctywide_scenes ctywide_speech
        ctyport_obs target_pilot pilot_states searchlight ai_task_h bot_h bot bot_states hal_h
        hal_task hal kid_h kor_h hal3_course kid_task kid kid_states kor_task kor kor_states
        kid3_course kor3_course ctysluma_part neon_baron_part ctyslumb_part ctyslumc_part
        ctyinda_part ctyinda_obs ctyindb_part ctyindb_obs ctyport_part ctypal_part ctypal_obs
        ctygena_part neon_praxis_part ctygenb_part ctygenc_part hopper stadiumb_texture
        stadium_part stadiumb_part stadium_obs stadium_scenes ctymark_obs ctymarka_part
        ctymarkb_part ash_h ash_task ash_shot ash ash_states juicer ctyasha_obs ash4_course
        ctyfarm_obs ctyfarma_part ctyfarmb_part yakow hiphog_obs hiphog_part hiphog_scenes
        hideout_scenes hideout_part hideout_obs gun_dummy gungame_data gungame_obs gungame_part
        onintent_part onintent_scenes onin_game vinroom_part vinroom_scenes vinroom_obs kiosk_part
        oracle_texture oracle_part oracle_scenes oracle_training ctywide_bbush meet_brutter
        ctypower shuttle kidesc_h crocesc_h hal4_course kidesc_task kidesc kidesc_states
        crocesc_task crocesc crocesc_states kidesc4_course crocesc4_course bombbot_h bombbot_path
        bombbot race_h race_mesh race_part race_obs vehicle_racer race_info race_manager race_hud
        pilot_recorder errol_chal_part errol_chal gun_buoy protect delivery_task portrun helldog
        collection_task whack title_obs intro_ocean intro_scenes intro_obs vortex_data vortex
        skatea_part skatea_obs racebike stadium_race_obs intro_texture prison_part prison_obs
        crimson_guard_level guard_conversation transport_level forexita_part forexita_obs
        target_tube fortress_obs forexitb_part fordumpa_part fordumpa_obs fordumpa_texture
        fort_robotank_turret fort_robotank fort_turret fordumpb_obs fordumpb_part fort_floor_spike
        fordumpc_part fordumpc_obs fortress_scenes hover_formation_h hover_nav_control_h
        hover_enemy_h hover_nav_network hover_nav_control hover_enemy hover_enemy_battle
        hover_formation hover_nav_edit wasp_part wasp crimson_guard_hover flamer forresca_part
        forresca_obs forrescb_part forrescb_obs spydroid mech_part mech target_mech mech_states
        grunt_mech ruins_ocean ruins_scenes mechtest_obs ruins_part breakable_wall pillar_collapse
        ruins_obs rapid_gunner amphibian fodder grenadier atoll_part atoll_obs sig_h sig_task
        sig_shot sig_plasma sig sig_states spyder sig0_course ash1_course atoll_tank atoll_scenes
        sniper metalmonk ruf_h ruf_task ruf ruf_states jinx_h jinx_shot jinx_bomb jinx jinx_states
        mog_h mog grim_h grim sewer_part sewer_ocean sewer_obs sewer_obs2 hosehead hosehead_fake
        gator hal2_course mog2_course jinx2_course grim2_course sewer_scenes sew_gunturret rhino
        rhino_wall mountain_ocean mountain_obs mountain_obs2 mountain_scenes mountain_part
        centurion mincan_obs canyon_scenes tomb_part tomb_obs tomb_water tomb_beetle widow_part
        widow_baron widow_extras widow_more_extras widow widow2 monster_frog tomb_scenes
        tomb_baby_spider com_elevator target_indax tomb_boulder target_turret_shot target_turret
        drill_turret drillmid_obs drill_part drill_part2 drill_mech_master drill_obs drill_panel
        drill_obs2 drill_spool drill_baron drill_scenes ginsu palshaft_part palcab_part palcab_obs
        pal_obs palroof_part palroof_obs palboss_texture palboss_part palboss_scenes throne_part
        palace_scenes squid_part squid_setup squid_extras squid_states palent_part palace_ocean
        metalkor_texture strip_scenes strip_part strip_ocean strip_rescue strip_drop strip_obs
        chaincrate caspad_obs caspad_part castle_tasks roboguard_level castle_texture castle_obs
        castle_part casboss_texture castle_baron castle_scenes casboss_part dig_digger dig_obs
        dig1_obs dig2_obs dig3_obs dig_texture dig_scenes dig_part forest_part forest_obs pegasus
        wren fish predator_h predator_graph predator forest_scenes under_shoot_block underb_master
        under_obs under_sig_obs under_laser pipe_grunt jellyfish under_part centipede
        sig5_cent1_path0 sig5_cent2_path0 sig5_course under_scenes sig_recorder consite_texture
        consite_part consite_scenes consite_obs consiteb_part nest_obs mantis mammoth flying_spider
        nest_ocean nest_scenes nest_part nest_texture metalkor_part metalkor_setup metalkor_states
        metalkor_extras nestb_scenes nestb_part credits outro_scenes demo_obs test_zone_obs)
        list(LENGTH JAK2_AOT_BOOT_TAGS JAK2_AOT_BOOT_FILE_COUNT)

        set(JAK2_AOT_BOOT_OUTPUTS "${JAK2_AOT_BOOT_DIR}/aot_boot_manifest.c"
                "${JAK2_AOT_BOOT_DIR}/aot_boot_manifest.h")
        set(JAK2_AOT_BOOT_C_SOURCES "")
        foreach(tag IN LISTS JAK2_AOT_BOOT_TAGS)
            list(APPEND JAK2_AOT_BOOT_C_SOURCES "${JAK2_AOT_BOOT_DIR}/${tag}.c")
            list(APPEND JAK2_AOT_BOOT_OUTPUTS "${JAK2_AOT_BOOT_DIR}/${tag}.c"
                    "${JAK2_AOT_BOOT_DIR}/${tag}_generated.h")
        endforeach()

        file(GLOB_RECURSE JAK2_AOT_BOOT_GOAL_SOURCES CONFIGURE_DEPENDS
                "${CMAKE_SOURCE_DIR}/goal_src/jak2/*.gc")
        add_custom_command(
                OUTPUT ${JAK2_AOT_BOOT_OUTPUTS}
                COMMAND $<TARGET_FILE:goalc-cbackend-sweep>
                        --game jak2
                        --project-path "${CMAKE_SOURCE_DIR}"
                        --limit ${JAK2_AOT_BOOT_FILE_COUNT}
                        --c-output-dir "${JAK2_AOT_BOOT_DIR}"
                        --report "${JAK2_AOT_BOOT_DIR}/coverage.tsv"
                DEPENDS goalc-cbackend-sweep ${JAK2_AOT_BOOT_GOAL_SOURCES}
                        "${CMAKE_SOURCE_DIR}/goal_src/goal-lib.gc"
                        "${CMAKE_SOURCE_DIR}/goal_src/jak2/game.gp"
                COMMENT
                "Emitting C for the first ${JAK2_AOT_BOOT_FILE_COUNT} Jak 2 object files")

        add_executable(jak2-data-boot-test
                kernel/core/jak2_boot_test.cpp
                kernel/core/jak2_runtime.cpp
                "${JAK2_AOT_BOOT_DIR}/aot_boot_manifest.c"
                ${JAK2_AOT_BOOT_C_SOURCES})
        set_source_files_properties(
                "${JAK2_AOT_BOOT_DIR}/aot_boot_manifest.c" ${JAK2_AOT_BOOT_C_SOURCES}
                PROPERTIES COMPILE_OPTIONS "-fno-strict-aliasing")
        target_include_directories(jak2-data-boot-test PRIVATE "${JAK2_AOT_BOOT_DIR}")
        target_link_libraries(jak2-data-boot-test jak2-kernel-core)
        add_test(NAME jak2-data-boot-test COMMAND jak2-data-boot-test --frames 100)

        # The jak2 thread-switch test: the same fixture and driver as jak1's, compiled on top of
        # the jak2 kernel prefix, driving the native ARM64 thread routines through the jak2
        # process and thread layouts. Needs no game data, so it always runs.
        set(JAK2_AOT_TSW_DIR "${CMAKE_CURRENT_BINARY_DIR}/aot-thread-switch-jak2")
        set(JAK2_AOT_TSW_TAGS gcommon gstring_h gkernel_h gkernel pskernel gstring dgo_h gstate
                thread_switch_test)
        list(LENGTH JAK2_AOT_TSW_TAGS JAK2_AOT_TSW_KERNEL_COUNT)
        math(EXPR JAK2_AOT_TSW_KERNEL_COUNT "${JAK2_AOT_TSW_KERNEL_COUNT} - 1")

        set(JAK2_AOT_TSW_OUTPUTS "${JAK2_AOT_TSW_DIR}/aot_boot_manifest.c"
                "${JAK2_AOT_TSW_DIR}/aot_boot_manifest.h")
        set(JAK2_AOT_TSW_C_SOURCES "")
        foreach(tag IN LISTS JAK2_AOT_TSW_TAGS)
            list(APPEND JAK2_AOT_TSW_C_SOURCES "${JAK2_AOT_TSW_DIR}/${tag}.c")
            list(APPEND JAK2_AOT_TSW_OUTPUTS "${JAK2_AOT_TSW_DIR}/${tag}.c"
                    "${JAK2_AOT_TSW_DIR}/${tag}_generated.h")
        endforeach()

        add_custom_command(
                OUTPUT ${JAK2_AOT_TSW_OUTPUTS}
                COMMAND $<TARGET_FILE:goalc-cbackend-sweep>
                        --game jak2
                        --project-path "${CMAKE_SOURCE_DIR}"
                        --limit ${JAK2_AOT_TSW_KERNEL_COUNT}
                        --extra-source test/goalc/aot/thread_switch_test.gc
                        --c-output-dir "${JAK2_AOT_TSW_DIR}"
                        --report "${JAK2_AOT_TSW_DIR}/coverage.tsv"
                DEPENDS goalc-cbackend-sweep ${JAK2_AOT_BOOT_GOAL_SOURCES} ${AOT_TSW_FIXTURE}
                        "${CMAKE_SOURCE_DIR}/goal_src/goal-lib.gc"
                        "${CMAKE_SOURCE_DIR}/goal_src/jak2/game.gp"
                COMMENT "Emitting C for the jak2 GOAL kernel and the thread-switch fixture")

        add_executable(jak2-thread-switch-test
                kernel/core/thread_switch_test.cpp
                kernel/core/goal_thread_test_arm64.s
                "${JAK2_AOT_TSW_DIR}/aot_boot_manifest.c"
                ${JAK2_AOT_TSW_C_SOURCES})
        set_source_files_properties(
                "${JAK2_AOT_TSW_DIR}/aot_boot_manifest.c" ${JAK2_AOT_TSW_C_SOURCES}
                PROPERTIES COMPILE_OPTIONS "-fno-strict-aliasing")
        target_include_directories(jak2-thread-switch-test PRIVATE "${JAK2_AOT_TSW_DIR}")
        target_link_libraries(jak2-thread-switch-test jak2-kernel-core)
        add_test(NAME jak2-thread-switch-test COMMAND jak2-thread-switch-test)
    endif()
endif()

if(APPLE)
    target_link_libraries(runtime "-framework Metal" "-framework QuartzCore" "-framework Foundation")

    # renders a textured, depth-tested frame through the Metal GfxRendererModule
    # and verifies the output pixels; --replay renders a captured game DMA
    # chain. see game/graphics/pipelines/metal/
    add_executable(metal-proof
        graphics/pipelines/metal/metal_proof_main.cpp
        graphics/pipelines/metal/metal_chain_replay.cpp)
    target_link_libraries(metal-proof runtime)

    # goalpad-play: the game and the renderer in one process.
    #
    # It cannot link `runtime`: that library carries a second copy of the Jak 1 kernel (kmachine,
    # kdgo, the IOP, the listener) which jak1-kernel-core replaces, so the two collide at link
    # time by design. `goalpad-metal-gfx` is therefore the Metal renderer plus the graphics
    # support it needs, compiled from the same sources `runtime` uses, without the desktop
    # runtime. The only file that is not shared is metal_host_seams.cpp, which defines the three
    # runtime globals the renderer reads.
    if(TARGET jak1-kernel-core AND GOAL_HOST_TESTS_ENABLED)
        add_library(goalpad-metal-gfx STATIC
            ${METAL_PIPELINE_SOURCES}
            ${METAL_LIB_EMBED}
            graphics/pipelines/metal/metal_jak2_bucket_table.cpp
            graphics/pipelines/metal/metal_host_seams.cpp
            graphics/pipelines/metal/metal_kernel_bridge.cpp
            graphics/pipelines/metal/metal_texture_upload_handler.cpp
            graphics/opengl_renderer/dma_helpers.cpp
            graphics/texture/jak1_tpage_dir.cpp
            graphics/texture/jak2_tpage_dir.cpp
            graphics/texture/jak3_tpage_dir.cpp
            graphics/texture/TextureConverter.cpp
            graphics/texture/TexturePool.cpp
            # the VU1 emulators, shared with the GL renderer
            graphics/opengl_renderer/Shadow_PS2.cpp
            graphics/opengl_renderer/ShadowVu.cpp
            graphics/opengl_renderer/ocean/OceanMid_PS2.cpp
            graphics/opengl_renderer/ocean/OceanNear_PS2.cpp
            graphics/opengl_renderer/ocean/OceanTexture_VU.cpp
            # common pieces jak1-kernel-core does not already compile
            "${CMAKE_SOURCE_DIR}/common/custom_data/TFrag3Data.cpp"
            "${CMAKE_SOURCE_DIR}/common/custom_data/pack_helpers.cpp"
            "${CMAKE_SOURCE_DIR}/common/dma/gs.cpp"
            "${CMAKE_SOURCE_DIR}/common/global_profiler/GlobalProfiler.cpp"
            "${CMAKE_SOURCE_DIR}/common/texture/texture_slots.cpp"
            "${CMAKE_SOURCE_DIR}/common/util/compress.cpp"
            "${CMAKE_SOURCE_DIR}/common/util/FrameLimiter.cpp"
            "${CMAKE_SOURCE_DIR}/common/versions/versions.cpp")
        set_source_files_properties(${METAL_PIPELINE_SOURCES}
            TARGET_DIRECTORY goalpad-metal-gfx
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        # imgui: TexturePool's debug view calls it. No debug UI is created here, but the
        # translation unit is shared with the GL renderer, which has one.
        target_link_libraries(goalpad-metal-gfx PUBLIC jak1-kernel-core SDL3::SDL3 imgui
            libzstd_static
            "-framework Metal" "-framework QuartzCore" "-framework Foundation")

        add_executable(jak2-metal-dispatch-proof
            graphics/pipelines/metal/metal_jak2_dispatch_proof.mm)
        set_source_files_properties(
            graphics/pipelines/metal/metal_jak2_dispatch_proof.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(jak2-metal-dispatch-proof goalpad-metal-gfx)
        add_test(NAME jak2-metal-dispatch-proof COMMAND jak2-metal-dispatch-proof)

        add_executable(jak2-metal-submit-readback-proof
            graphics/pipelines/metal/metal_jak2_submit_readback_proof.mm)
        set_source_files_properties(
            graphics/pipelines/metal/metal_jak2_submit_readback_proof.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(jak2-metal-submit-readback-proof goalpad-metal-gfx)
        add_test(NAME jak2-metal-submit-readback-proof COMMAND jak2-metal-submit-readback-proof)

        add_executable(goalpad-play
            goalpad_play.cpp
            goalpad_audio.cpp
            kernel/core/continue_warp.cpp
            kernel/core/scripted_walk.cpp
            "${AOT_BOOT_DIR}/aot_boot_manifest.c"
            ${AOT_BOOT_C_SOURCES})
        target_include_directories(goalpad-play PRIVATE "${AOT_BOOT_DIR}")
        # CoreAudio: the host's audio device. The vendored SDL is built with SDL_AUDIO OFF.
        target_link_libraries(goalpad-play goalpad-metal-gfx
            "-framework AudioToolbox" "-framework CoreAudio")
    endif()
endif()
