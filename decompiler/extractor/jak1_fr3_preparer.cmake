if(NOT DEFINED OPENGOAL_FR3_PREPARER_GAME)
  set(OPENGOAL_FR3_PREPARER_GAME jak1)
endif()
if(NOT OPENGOAL_FR3_PREPARER_GAME MATCHES "^jak[12]$")
  message(FATAL_ERROR "The FR3 preparer supports only jak1 or jak2.")
endif()
set(OPENGOAL_FR3_PREPARER_TARGET "${OPENGOAL_FR3_PREPARER_GAME}-fr3-preparer")

set(FR3_PREPARER_SOURCES
    decompiler/analysis/analyze_inspect_method.cpp
    decompiler/analysis/atomic_op_builder.cpp
    decompiler/analysis/cfg_builder.cpp
    decompiler/analysis/expression_build.cpp
    decompiler/analysis/final_output.cpp
    decompiler/analysis/find_defpartgroup.cpp
    decompiler/analysis/find_defstates.cpp
    decompiler/analysis/find_skelgroups.cpp
    decompiler/analysis/inline_asm_rewrite.cpp
    decompiler/analysis/insert_lets.cpp
    decompiler/analysis/label_types.cpp
    decompiler/analysis/mips2c.cpp
    decompiler/analysis/reg_usage.cpp
    decompiler/analysis/stack_spill.cpp
    decompiler/analysis/static_refs.cpp
    decompiler/analysis/symbol_def_map.cpp
    decompiler/analysis/type_analysis.cpp
    decompiler/analysis/variable_naming.cpp
    decompiler/data/dir_tpages.cpp
    decompiler/data/game_count.cpp
    decompiler/data/game_text.cpp
    decompiler/data/streamed_audio.cpp
    decompiler/data/StrFileReader.cpp
    decompiler/data/TextureDB.cpp
    decompiler/data/tpage.cpp
    decompiler/Disasm/Instruction.cpp
    decompiler/Disasm/InstructionDecode.cpp
    decompiler/Disasm/InstructionMatching.cpp
    decompiler/Disasm/InstructionParser.cpp
    decompiler/Disasm/OpcodeInfo.cpp
    decompiler/Disasm/Register.cpp
    decompiler/extractor/extractor_util.cpp
    decompiler/extractor/jak1_checked_dgo.cpp
    decompiler/extractor/jak1_fr3_preparer.cpp
    decompiler/Function/BasicBlocks.cpp
    decompiler/Function/CfgVtx.cpp
    decompiler/Function/Function.cpp
    decompiler/IR2/AtomicOp.cpp
    decompiler/IR2/AtomicOpForm.cpp
    decompiler/IR2/AtomicOpTypeAnalysis.cpp
    decompiler/IR2/bitfields.cpp
    decompiler/IR2/Env.cpp
    decompiler/IR2/ExpressionHelpers.cpp
    decompiler/IR2/Form.cpp
    decompiler/IR2/FormExpressionAnalysis.cpp
    decompiler/IR2/FormStack.cpp
    decompiler/IR2/GenericElementMatcher.cpp
    decompiler/IR2/LabelDB.cpp
    decompiler/IR2/OpenGoalMapping.cpp
    decompiler/level_extractor/BspHeader.cpp
    decompiler/level_extractor/extract_actors.cpp
    decompiler/level_extractor/extract_collide_frags.cpp
    decompiler/level_extractor/extract_common.cpp
    decompiler/level_extractor/extract_hfrag.cpp
    decompiler/level_extractor/extract_joint_group.cpp
    decompiler/level_extractor/extract_anim.cpp
    decompiler/level_extractor/extract_level.cpp
    decompiler/level_extractor/extract_merc.cpp
    decompiler/level_extractor/extract_tfrag.cpp
    decompiler/level_extractor/extract_tie.cpp
    decompiler/level_extractor/extract_shrub.cpp
    decompiler/level_extractor/fr3_to_gltf.cpp
    decompiler/level_extractor/MercData.cpp
    decompiler/level_extractor/tfrag_tie_fixup.cpp
    decompiler/level_extractor/merc_replacement.cpp
    decompiler/ObjectFile/LinkedObjectFile.cpp
    decompiler/ObjectFile/LinkedObjectFileCreation.cpp
    decompiler/ObjectFile/ObjectFileDB.cpp
    decompiler/ObjectFile/ObjectFileDB_IR2.cpp
    decompiler/types2/ForwardProp.cpp
    decompiler/types2/types2.cpp
    decompiler/util/config_parsers.cpp
    decompiler/util/data_decompile.cpp
    decompiler/util/DataParser.cpp
    decompiler/util/DecompilerTypeSystem.cpp
    decompiler/util/goal_data_reader.cpp
    decompiler/util/sparticle_decompile.cpp
    decompiler/util/TP_Type.cpp
    decompiler/util/type_utils.cpp
    decompiler/VuDisasm/VuDisassembler.cpp
    decompiler/VuDisasm/VuInstruction.cpp
    decompiler/config.cpp
    common/custom_data/TFrag3Data.cpp
    common/custom_data/pack_helpers.cpp
    common/dma/dma.cpp
    common/dma/gs.cpp
    common/goos/Interpreter.cpp
    common/goos/Object.cpp
    common/goos/ParseHelpers.cpp
    common/goos/PrettyPrinter.cpp
    common/goos/PrettyPrinter2.cpp
    common/goos/Printer.cpp
    common/goos/Reader.cpp
    common/goos/TextDB.cpp
    common/log/log.cpp
    common/math/geometry.cpp
    common/serialization/text/text_ser.cpp
    common/texture/texture_slots.cpp
    common/type_system/defenum.cpp
    common/type_system/deftype.cpp
    common/type_system/state.cpp
    common/type_system/Type.cpp
    common/type_system/TypeFieldLookup.cpp
    common/type_system/TypeSpec.cpp
    common/type_system/TypeSystem.cpp
    common/util/Assert.cpp
    common/util/BitUtils.cpp
    common/util/compress.cpp
    common/util/crc32.cpp
    common/util/diff.cpp
    common/util/dgo_util.cpp
    common/util/FileUtil.cpp
    common/util/font/dbs/font_db_jak1.cpp
    common/util/font/dbs/font_db_jak2.cpp
    common/util/font/dbs/font_db_jak3.cpp
    common/util/font/font_utils.cpp
    common/util/font/font_utils_korean.cpp
    common/util/gltf_util.cpp
    common/util/image_resize.cpp
    common/util/json_util.cpp
    common/util/print_float.cpp
    common/util/SimpleThreadGroup.cpp
    common/util/string_util.cpp
    common/util/Timer.cpp
    common/util/unicode_util.cpp
    common/versions/jak1_iso_revisions.cpp
    third-party/lzokay/lzokay.cpp
    third-party/stb_image/stb_image.cpp
    third-party/tiny_gltf/tiny_gltf.cpp
    third-party/xdelta3/xdelta3.c)

file(GLOB FR3_ZSTD_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/third-party/zstd/lib/common/*.c"
     "${CMAKE_SOURCE_DIR}/third-party/zstd/lib/compress/*.c"
     "${CMAKE_SOURCE_DIR}/third-party/zstd/lib/decompress/*.c")
list(APPEND FR3_PREPARER_SOURCES ${FR3_ZSTD_SOURCES})

add_library(${OPENGOAL_FR3_PREPARER_TARGET} STATIC ${FR3_PREPARER_SOURCES})
target_include_directories(${OPENGOAL_FR3_PREPARER_TARGET}
                           PUBLIC "${CMAKE_SOURCE_DIR}"
                           PRIVATE "${CMAKE_SOURCE_DIR}/third-party"
                                   "${CMAKE_SOURCE_DIR}/third-party/fmt/include"
                                   "${CMAKE_SOURCE_DIR}/third-party/stb_image"
                                   "${CMAKE_SOURCE_DIR}/third-party/tiny_gltf"
                                   "${CMAKE_SOURCE_DIR}/third-party/tree-sitter/tree-sitter/lib/include"
                                   "${CMAKE_SOURCE_DIR}/third-party/zstd/lib"
                                   "${CMAKE_SOURCE_DIR}/third-party/zstd/lib/common")
target_compile_features(${OPENGOAL_FR3_PREPARER_TARGET} PUBLIC cxx_std_20)
target_compile_definitions(${OPENGOAL_FR3_PREPARER_TARGET}
                           PRIVATE FMT_HEADER_ONLY=1 OPENGOAL_FR3_PREPARER_ONLY=1)

if(MSVC)
  target_compile_options(${OPENGOAL_FR3_PREPARER_TARGET} PRIVATE /W4)
else()
  target_compile_options(${OPENGOAL_FR3_PREPARER_TARGET} PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(${OPENGOAL_FR3_PREPARER_TARGET}-link-proof
               "${CMAKE_SOURCE_DIR}/test/decompiler/${OPENGOAL_FR3_PREPARER_GAME}_fr3_preparer_link_proof.cpp")
target_link_libraries(${OPENGOAL_FR3_PREPARER_TARGET}-link-proof
                      PRIVATE ${OPENGOAL_FR3_PREPARER_TARGET})
target_compile_features(${OPENGOAL_FR3_PREPARER_TARGET}-link-proof PRIVATE cxx_std_20)

add_executable(${OPENGOAL_FR3_PREPARER_GAME}-fr3-prepare
               "${CMAKE_SOURCE_DIR}/decompiler/extractor/${OPENGOAL_FR3_PREPARER_GAME}_fr3_prepare_main.cpp")
target_link_libraries(${OPENGOAL_FR3_PREPARER_GAME}-fr3-prepare
                      PRIVATE ${OPENGOAL_FR3_PREPARER_TARGET})
target_include_directories(${OPENGOAL_FR3_PREPARER_GAME}-fr3-prepare
                           PRIVATE "${CMAKE_SOURCE_DIR}/third-party/fmt/include")
target_compile_features(${OPENGOAL_FR3_PREPARER_GAME}-fr3-prepare PRIVATE cxx_std_20)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  add_executable(${OPENGOAL_FR3_PREPARER_TARGET}-test
                 "${CMAKE_SOURCE_DIR}/test/decompiler/test_${OPENGOAL_FR3_PREPARER_GAME}_fr3_preparer.cpp")
  target_link_libraries(${OPENGOAL_FR3_PREPARER_TARGET}-test
                        PRIVATE ${OPENGOAL_FR3_PREPARER_TARGET})
  target_compile_features(${OPENGOAL_FR3_PREPARER_TARGET}-test PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(${OPENGOAL_FR3_PREPARER_TARGET}-test PRIVATE /W4 /WX)
  else()
    target_compile_options(${OPENGOAL_FR3_PREPARER_TARGET}-test
                           PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  if(OPENGOAL_FR3_PREPARER_GAME STREQUAL "jak2")
    add_test(NAME ${OPENGOAL_FR3_PREPARER_TARGET}-test
             COMMAND ${OPENGOAL_FR3_PREPARER_TARGET}-test ${CMAKE_SOURCE_DIR})
  else()
    add_test(NAME ${OPENGOAL_FR3_PREPARER_TARGET}-test
             COMMAND ${OPENGOAL_FR3_PREPARER_TARGET}-test)
  endif()
endif()
