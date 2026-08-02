add_library(compiler
        aot/CBackend.cpp
        emitter/CallingConvention.cpp
        emitter/CodeTester.cpp
        emitter/ObjectFileData.cpp
        emitter/ObjectGenerator.cpp
        emitter/Register.cpp
        emitter/IGen.cpp
        emitter/IGenX86.cpp
        debugger/disassemble.cpp
        build_level/common/build_level.cpp
        build_actor/common/animation_processing.cpp
        build_actor/common/MercExtract.cpp
        build_level/jak1/build_level.cpp
        build_level/jak2/build_level.cpp
        build_level/jak3/build_level.cpp
        build_level/collide/jak1/collide_bvh.cpp
        build_level/collide/jak1/collide_drawable.cpp
        build_level/collide/jak1/collide_pack.cpp
        build_level/collide/jak2/collide.cpp
        build_level/collide/jak3/collide.cpp
        build_level/common/color_quantization.cpp
        build_level/common/Entity.cpp
        build_level/jak1/Entity.cpp
        build_level/jak2/Entity.cpp
        build_level/jak3/Entity.cpp
        build_level/common/FileInfo.cpp
        build_level/jak1/FileInfo.cpp
        build_level/jak2/FileInfo.cpp
        build_level/jak3/FileInfo.cpp
        build_level/common/gltf_mesh_extract.cpp
        build_level/jak1/LevelFile.cpp
        build_level/jak2/LevelFile.cpp
        build_level/jak3/LevelFile.cpp
        build_level/common/ResLump.cpp
        build_level/common/Tfrag.cpp
        build_level/common/Tie.cpp
        build_level/jak1/ambient.cpp
        build_level/jak2/Region.cpp
        build_level/jak3/Region.cpp
        compiler/Compiler.cpp
        compiler/Env.cpp
        compiler/Val.cpp
        compiler/IR.cpp
        compiler/CompilerSettings.cpp
        compiler/CodeGenerator.cpp
        compiler/StaticObject.cpp
        compiler/symbol_info.cpp
        compiler/compilation/Asm.cpp
        compiler/compilation/Atoms.cpp
        compiler/compilation/CompilerControl.cpp
        compiler/compilation/ConstantPropagation.cpp
        compiler/compilation/Block.cpp
        compiler/compilation/Macro.cpp
        compiler/compilation/Math.cpp
        compiler/compilation/Define.cpp
        compiler/compilation/Debug.cpp
        compiler/compilation/Function.cpp
        compiler/compilation/ControlFlow.cpp
        compiler/compilation/Type.cpp
        compiler/compilation/State.cpp
        compiler/compilation/Static.cpp
        compiler/Util.cpp
        data_compiler/game_text_common.cpp
        data_compiler/dir_tpages.cpp
        data_compiler/game_count.cpp
        data_compiler/DataObjectGenerator.cpp
        data_compiler/DataObjectGeneratorCore.cpp
        build_actor/common/build_actor.cpp
        build_actor/jak1/build_actor.cpp
        build_actor/jak2/build_actor.cpp
        build_actor/jak3/build_actor.cpp
        debugger/Debugger.cpp
        debugger/DebugInfo.cpp
        debugger/DebugServer.cpp
        emitter/IGenX86.cpp
        listener/Listener.cpp
        listener/MemoryMap.cpp
        make/MakeSystem.cpp
        make/Tool.cpp
        make/Tools.cpp
        regalloc/IRegister.cpp
        regalloc/Allocator.cpp
        regalloc/allocator_interface.cpp
        regalloc/Allocator_v2.cpp
        compiler/docs/DocTypes.cpp)

target_link_libraries(compiler common Zydis tiny_gltf decomp)

include("${CMAKE_SOURCE_DIR}/common/custom_data/jak1_output_recipe_core.cmake")

add_library(jak1-output-recipe-generator make/Jak1OutputRecipeMakeSystem.cpp)
target_link_libraries(jak1-output-recipe-generator
                      PUBLIC jak1-output-recipe-core compiler decomp)
target_compile_features(jak1-output-recipe-generator PUBLIC cxx_std_20)

add_executable(jak1-public-output-graph-generator
               make/Jak1PublicOutputGraphGeneratorMain.cpp)
target_link_libraries(jak1-public-output-graph-generator
                      PRIVATE jak1-output-recipe-generator)
target_compile_features(jak1-public-output-graph-generator PRIVATE cxx_std_20)

if(BUILD_TESTING AND GOAL_HOST_TESTS_ENABLED)
  add_executable(jak1-public-output-graph-exactness-test
                 "${CMAKE_SOURCE_DIR}/test/goalc/test_jak1_public_output_graph_exactness.cpp")
  target_link_libraries(jak1-public-output-graph-exactness-test
                        PRIVATE jak1-output-recipe-generator)
  target_compile_features(jak1-public-output-graph-exactness-test PRIVATE cxx_std_20)
  add_test(NAME jak1-public-output-graph-exactness
           COMMAND jak1-public-output-graph-exactness-test)
  set_tests_properties(jak1-public-output-graph-exactness
                       PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

if (WIN32)
    target_link_libraries(compiler mman)
endif ()

add_executable(goalc main.cpp)
add_executable(goalc-simple simple_main.cpp)
add_executable(goalc-cbackend aot/c_backend_main.cpp)
add_executable(goalc-cbackend-sweep aot/c_backend_sweep_main.cpp)

add_executable(build_level build_level/main.cpp)
add_executable(build_actor build_actor/main.cpp)

target_link_libraries(goalc common Zydis compiler)
target_link_libraries(goalc-simple common Zydis compiler)
target_link_libraries(goalc-cbackend common Zydis compiler)
target_link_libraries(goalc-cbackend-sweep common Zydis compiler)
target_link_libraries(build_level common Zydis compiler)
target_link_libraries(build_actor common Zydis compiler)

add_test(
        NAME goalc-c-backend-gcommon
        COMMAND ${CMAKE_COMMAND}
        -DGOALC_CBACKEND=$<TARGET_FILE:goalc-cbackend>
        -DPROJECT_ROOT=${CMAKE_SOURCE_DIR}
        -DC_COMPILER=${CMAKE_C_COMPILER}
        -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/test-output/goalc-c-backend-gcommon
        -P ${CMAKE_SOURCE_DIR}/test/goalc/test_c_backend_cli.cmake)

add_test(
        NAME goalc-c-backend-process-register
        COMMAND ${CMAKE_COMMAND}
        -DGOALC_CBACKEND=$<TARGET_FILE:goalc-cbackend>
        -DPROJECT_ROOT=${CMAKE_SOURCE_DIR}
        -DC_COMPILER=${CMAKE_C_COMPILER}
        -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/test-output/goalc-c-backend-process-register
        -P ${CMAKE_SOURCE_DIR}/test/goalc/test_c_backend_process_cli.cmake)

add_test(
        NAME goalc-c-backend-vector-ops
        COMMAND ${CMAKE_COMMAND}
        -DGOALC_CBACKEND=$<TARGET_FILE:goalc-cbackend>
        -DPROJECT_ROOT=${CMAKE_SOURCE_DIR}
        -DC_COMPILER=${CMAKE_C_COMPILER}
        -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/test-output/goalc-c-backend-vector-ops
        -P ${CMAKE_SOURCE_DIR}/test/goalc/test_c_backend_vector_ops_cli.cmake)
