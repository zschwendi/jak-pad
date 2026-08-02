add_library(
        decomp

        analysis/analyze_inspect_method.cpp
        analysis/atomic_op_builder.cpp
        analysis/cfg_builder.cpp
        analysis/expression_build.cpp
        analysis/final_output.cpp
        analysis/find_defpartgroup.cpp
        analysis/find_defstates.cpp
        analysis/find_skelgroups.cpp
        analysis/inline_asm_rewrite.cpp
        analysis/insert_lets.cpp
        analysis/label_types.cpp
        analysis/mips2c.cpp
        analysis/reg_usage.cpp
        analysis/stack_spill.cpp
        analysis/static_refs.cpp
        analysis/symbol_def_map.cpp
        analysis/type_analysis.cpp
        analysis/variable_naming.cpp

        data/dir_tpages.cpp
        data/game_count.cpp
        data/game_text.cpp
        data/streamed_audio.cpp
        data/StrFileReader.cpp
        data/TextureDB.cpp
        data/tpage.cpp

        Disasm/Instruction.cpp
        Disasm/InstructionDecode.cpp
        Disasm/InstructionMatching.cpp
        Disasm/InstructionParser.cpp
        Disasm/OpcodeInfo.cpp
        Disasm/Register.cpp

        extractor/extractor_util.cpp
        extractor/jak1_checked_dgo.cpp
        extractor/jak1_fr3_preparer.cpp

        ../common/versions/jak1_iso_revisions.cpp

        Function/BasicBlocks.cpp
        Function/CfgVtx.cpp
        Function/Function.cpp

        IR2/AtomicOp.cpp
        IR2/AtomicOpForm.cpp
        IR2/AtomicOpTypeAnalysis.cpp
        IR2/bitfields.cpp
        IR2/Env.cpp
        IR2/ExpressionHelpers.cpp
        IR2/Form.cpp
        IR2/FormExpressionAnalysis.cpp
        IR2/FormStack.cpp
        IR2/GenericElementMatcher.cpp
        IR2/LabelDB.cpp
        IR2/OpenGoalMapping.cpp

        level_extractor/BspHeader.cpp
        level_extractor/extract_actors.cpp
        level_extractor/extract_collide_frags.cpp
        level_extractor/extract_common.cpp
        level_extractor/extract_hfrag.cpp
        level_extractor/extract_joint_group.cpp
        level_extractor/extract_anim.cpp
        level_extractor/extract_level.cpp
        level_extractor/extract_merc.cpp
        level_extractor/extract_tfrag.cpp
        level_extractor/extract_tie.cpp
        level_extractor/extract_shrub.cpp
        level_extractor/fr3_to_gltf.cpp
        level_extractor/MercData.cpp
        level_extractor/tfrag_tie_fixup.cpp
        level_extractor/merc_replacement.cpp

        ObjectFile/LinkedObjectFile.cpp
        ObjectFile/LinkedObjectFileCreation.cpp
        ObjectFile/ObjectFileDB.cpp
        ObjectFile/ObjectFileDB_IR2.cpp

        types2/ForwardProp.cpp
        types2/types2.cpp

        util/config_parsers.cpp
        util/data_decompile.cpp
        util/DataParser.cpp
        util/DecompilerTypeSystem.cpp
        util/goal_data_reader.cpp
        util/sparticle_decompile.cpp
        util/TP_Type.cpp
        util/type_utils.cpp

        VuDisasm/VuDisassembler.cpp
        VuDisasm/VuInstruction.cpp

        config.cpp
        decompilation_process.cpp)

target_link_libraries(decomp
        lzokay
        common
        fmt
        stb_image
        xdelta3
        tiny_gltf
)

add_executable(decompiler
        main.cpp)

target_link_libraries(decompiler
        decomp
        common
        lzokay
        fmt
        stb_image
        tiny_gltf)

add_executable(jak1-fr3-prepare
        extractor/jak1_fr3_prepare_main.cpp)

target_link_libraries(jak1-fr3-prepare
        decomp
        common
        lzokay
        fmt
        stb_image
        tiny_gltf)


add_executable(extractor
        extractor/main.cpp)

target_link_libraries(extractor
        decomp
        common
        lzokay
        fmt
        compiler
        stb_image
        tiny_gltf)

if(BUILD_TESTING AND GOAL_HOST_TESTS_ENABLED)
  add_executable(jak1-iso-database-test
                 ../test/decompiler/test_jak1_iso_database.cpp)
  target_link_libraries(jak1-iso-database-test PRIVATE decomp common)
  target_compile_features(jak1-iso-database-test PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(jak1-iso-database-test PRIVATE /W4 /WX)
  else()
    target_compile_options(jak1-iso-database-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME jak1-iso-database-test COMMAND jak1-iso-database-test)

  add_executable(jak1-fr3-preparer-test
                 ../test/decompiler/test_jak1_fr3_preparer.cpp)
  target_link_libraries(jak1-fr3-preparer-test PRIVATE decomp common)
  target_compile_features(jak1-fr3-preparer-test PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(jak1-fr3-preparer-test PRIVATE /W4 /WX)
  else()
    target_compile_options(jak1-fr3-preparer-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME jak1-fr3-preparer-test COMMAND jak1-fr3-preparer-test)
endif()
