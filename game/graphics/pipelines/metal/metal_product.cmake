# Shared source graph for products that embed the Metal renderer without the desktop runtime.
# The archive deliberately links no game kernel; its final consumer selects exactly one.

set(OPENGOAL_METAL_PRODUCT_DIR "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(OPENGOAL_METAL_GAME_DIR "${OPENGOAL_METAL_PRODUCT_DIR}/../../.." ABSOLUTE)
get_filename_component(OPENGOAL_METAL_ROOT "${OPENGOAL_METAL_GAME_DIR}/.." ABSOLUTE)

set(OPENGOAL_METAL_RENDERER_SOURCES
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_bucket_renderer.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_direct_renderer.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_eye_renderer.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_generic2.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_glow_renderer.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_jak2_blit_display_plan.cpp"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_jak2_blit_display_renderer.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_jak2_fog_texture_convert.cpp"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_jak2_bucket4_mixed_executor.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_jak2_opcode27_skull_gem_cpu.cpp"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_jak2_opcode27_skull_gem_executor.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_jak2_opcode41_cloud_cpu.cpp"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_jak2_prison_clut_cpu.cpp"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_jak2_dark_jak_clut_executor.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_jak2_prison_clut_executor.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_jak2_raw_image_upload_executor.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_jak2_warp_renderer.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_shadow_renderer.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_level_data.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_merc.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_merc_model_pool.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_ocean_renderer.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_pool_texture.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_pso_cache.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_renderer.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_shrub.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_sky_renderer.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_sprite_renderer.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_texture.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_tfrag.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_tie.mm"
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_vis_data.cpp")

set(OPENGOAL_METAL_WINDOW_SHELL_SOURCE
    "${OPENGOAL_METAL_PRODUCT_DIR}/metal_pipeline.mm")

set(OPENGOAL_METAL_SHADER_SOURCES
    "${OPENGOAL_METAL_PRODUCT_DIR}/shaders/scaffold.metal"
    "${OPENGOAL_METAL_PRODUCT_DIR}/shaders/direct.metal"
    "${OPENGOAL_METAL_PRODUCT_DIR}/shaders/sprite.metal"
    "${OPENGOAL_METAL_PRODUCT_DIR}/shaders/sprite_glow.metal"
    "${OPENGOAL_METAL_PRODUCT_DIR}/shaders/background.metal"
    "${OPENGOAL_METAL_PRODUCT_DIR}/shaders/ocean.metal"
    "${OPENGOAL_METAL_PRODUCT_DIR}/shaders/merc2.metal"
    "${OPENGOAL_METAL_PRODUCT_DIR}/shaders/eye.metal"
    "${OPENGOAL_METAL_PRODUCT_DIR}/shaders/generic.metal"
    "${OPENGOAL_METAL_PRODUCT_DIR}/shaders/shadow.metal")

function(opengoal_embed_metal_library output_variable output_directory)
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" metal_sysroot)
    if(metal_sysroot MATCHES "iphonesimulator")
      set(metal_sdk iphonesimulator)
    else()
      set(metal_sdk iphoneos)
    endif()
  else()
    set(metal_sdk macosx)
  endif()
  set(metallib "${output_directory}/goalpad.metallib")
  set(embed "${output_directory}/goalpad_metallib_embed.c")
  set(module_cache "${CMAKE_BINARY_DIR}/metal-module-cache")
  add_custom_command(
      OUTPUT "${metallib}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_directory}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${module_cache}"
      COMMAND xcrun -sdk "${metal_sdk}" metal -Wall -Werror
              "-fmodules-cache-path=${module_cache}" -o "${metallib}"
              ${OPENGOAL_METAL_SHADER_SOURCES}
      DEPENDS ${OPENGOAL_METAL_SHADER_SOURCES}
      COMMENT "Compiling MSL shaders to ${metallib}"
      VERBATIM)
  add_custom_command(
      OUTPUT "${embed}"
      COMMAND "${CMAKE_COMMAND}" "-DINPUT=${metallib}" "-DOUTPUT=${embed}"
              -DSYMBOL=g_goalpad_metallib
              -P "${OPENGOAL_METAL_PRODUCT_DIR}/embed_metallib.cmake"
      DEPENDS "${metallib}" "${OPENGOAL_METAL_PRODUCT_DIR}/embed_metallib.cmake"
      COMMENT "Embedding ${metallib}"
      VERBATIM)
  set(${output_variable} "${embed}" PARENT_SCOPE)
endfunction()

function(opengoal_add_metal_product target)
  if(ARGC GREATER 1)
    set(metal_embed "${ARGV1}")
  else()
    opengoal_embed_metal_library(
        metal_embed "${CMAKE_CURRENT_BINARY_DIR}/${target}_metal_shaders")
  endif()
  add_library(${target} STATIC
      ${OPENGOAL_METAL_RENDERER_SOURCES}
      "${metal_embed}"
      "${OPENGOAL_METAL_PRODUCT_DIR}/metal_jak2_bucket_table.cpp"
      "${OPENGOAL_METAL_PRODUCT_DIR}/metal_host_seams.cpp"
      "${OPENGOAL_METAL_PRODUCT_DIR}/metal_kernel_bridge.cpp"
      "${OPENGOAL_METAL_PRODUCT_DIR}/metal_texture_upload_handler.cpp"
      "${OPENGOAL_METAL_GAME_DIR}/graphics/opengl_renderer/dma_helpers.cpp"
      "${OPENGOAL_METAL_GAME_DIR}/graphics/sprite_glow_math.cpp"
      "${OPENGOAL_METAL_GAME_DIR}/graphics/texture/jak1_tpage_dir.cpp"
      "${OPENGOAL_METAL_GAME_DIR}/graphics/texture/jak2_tpage_dir.cpp"
      "${OPENGOAL_METAL_GAME_DIR}/graphics/texture/jak3_tpage_dir.cpp"
      "${OPENGOAL_METAL_GAME_DIR}/graphics/texture/TextureConverter.cpp"
      "${OPENGOAL_METAL_GAME_DIR}/graphics/texture/TexturePool.cpp"
      "${OPENGOAL_METAL_GAME_DIR}/graphics/opengl_renderer/Shadow_PS2.cpp"
      "${OPENGOAL_METAL_GAME_DIR}/graphics/opengl_renderer/ShadowVu.cpp"
      "${OPENGOAL_METAL_GAME_DIR}/graphics/opengl_renderer/ocean/OceanMid_PS2.cpp"
      "${OPENGOAL_METAL_GAME_DIR}/graphics/opengl_renderer/ocean/OceanNear_PS2.cpp"
      "${OPENGOAL_METAL_GAME_DIR}/graphics/opengl_renderer/ocean/OceanTexture_VU.cpp"
      "${OPENGOAL_METAL_ROOT}/common/custom_data/TFrag3Data.cpp"
      "${OPENGOAL_METAL_ROOT}/common/custom_data/pack_helpers.cpp"
      "${OPENGOAL_METAL_ROOT}/common/dma/gs.cpp"
      "${OPENGOAL_METAL_ROOT}/common/texture/texture_slots.cpp"
      "${OPENGOAL_METAL_ROOT}/common/util/FrameLimiter.cpp")
  set_source_files_properties(${OPENGOAL_METAL_RENDERER_SOURCES}
      TARGET_DIRECTORY ${target}
      PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_include_directories(${target} PUBLIC
      "${OPENGOAL_METAL_ROOT}"
      "${OPENGOAL_METAL_ROOT}/third-party"
      "${OPENGOAL_METAL_ROOT}/third-party/fmt/include"
      "${OPENGOAL_METAL_ROOT}/third-party/glad/include"
      "${OPENGOAL_METAL_ROOT}/third-party/SDL/include")
  if(TARGET fmt)
    target_link_libraries(${target} PUBLIC fmt)
  else()
    target_compile_definitions(${target} PUBLIC FMT_HEADER_ONLY=1)
  endif()
  if(TARGET imgui)
    target_sources(${target} PRIVATE
        "${OPENGOAL_METAL_GAME_DIR}/graphics/texture/TexturePoolDebug.cpp")
    target_link_libraries(${target} PUBLIC imgui)
  endif()
  if(TARGET libzstd_static)
    target_link_libraries(${target} PUBLIC libzstd_static)
  endif()
  target_link_libraries(${target} PUBLIC
      "-framework Metal" "-framework QuartzCore" "-framework Foundation")
endfunction()
