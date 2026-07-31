#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "common/util/FileUtil.h"

#include "goalc/aot/AOTWriter.h"
#include "goalc/compiler/Compiler.h"
#include "goalc/compiler/IR.h"
#include "goalc/compiler/Val.h"
#include "goalc/debugger/DebugInfo.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/InstructionSet.h"
#include "goalc/emitter/ObjectGenerator.h"
#include "goalc/emitter/Register.h"
#include "goalc/regalloc/Allocator.h"
#include "goalc/regalloc/Allocator_v2.h"
#include "gtest/gtest.h"

namespace {

void expect_no_reserved_arm64_registers(const std::vector<emitter::Register>& registers) {
  for (const auto reserved :
       {emitter::X20, emitter::X21, emitter::X22, emitter::SP, emitter::X29, emitter::X30}) {
    EXPECT_EQ(std::find(registers.begin(), registers.end(), reserved), registers.end());
  }
}

void expect_arm64_live_across_call_uses_saved_register(RegClass reg_class, bool use_v2_allocator) {
  const auto& registers = emitter::get_register_info(emitter::InstructionSet::ARM64);
  AllocationInput input;
  input.instruction_set = emitter::InstructionSet::ARM64;
  input.max_vars = 1;
  input.function_name = use_v2_allocator ? "arm64-call-clobber-v2" : "arm64-call-clobber-v1";

  RegAllocInstr define;
  define.write.push_back({reg_class, 0});
  input.instructions.push_back(define);

  RegAllocInstr call;
  call.clobber = registers.get_call_clobbered();
  input.instructions.push_back(call);

  RegAllocInstr use;
  use.read.push_back({reg_class, 0});
  input.instructions.push_back(use);

  const auto allocation =
      use_v2_allocator ? allocate_registers_v2(input) : allocate_registers(input);
  ASSERT_TRUE(allocation.ok);
  ASSERT_EQ(allocation.ass_as_ranges.size(), 1);
  const auto& assignment = allocation.ass_as_ranges.at(0).get(1);
  ASSERT_EQ(assignment.kind, Assignment::Kind::REGISTER);

  const auto& expected_saved = registers.get_all_saved();
  EXPECT_NE(std::find(expected_saved.begin(), expected_saved.end(), assignment.reg),
            expected_saved.end());
  EXPECT_EQ(std::find(registers.get_call_clobbered().begin(), registers.get_call_clobbered().end(),
                      assignment.reg),
            registers.get_call_clobbered().end());
  EXPECT_EQ(assignment.reg.instruction_set(), emitter::InstructionSet::ARM64);
}

}  // namespace

TEST(Arm64Aot, compiles_top_level_literal_and_renders_apple_text) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);
  const auto source = file_util::read_text_file(
      file_util::get_file_path({"test/goalc/source_templates/arm64-aot/constant-42.gc"}));
  const auto code = compiler.compile_top_level_source(source, "constant-42");

  EXPECT_EQ(code, (std::vector<u8>{0x40, 0x05, 0x80, 0xd2, 0xc0, 0x03, 0x5f, 0xd6}));

  const aot::AppleArm64Function function{"goalpad_aot_constant_42", code};
  EXPECT_EQ(aot::render_apple_arm64_assembly(function),
            ".section __TEXT,__text,regular,pure_instructions\n"
            ".p2align 2\n"
            ".globl _goalpad_aot_constant_42\n"
            "_goalpad_aot_constant_42:\n"
            "  .long 0xd2800540\n"
            "  .long 0xd65f03c0\n"
            ".subsections_via_symbols\n");
}

TEST(Arm64Aot, compiles_jak1_false_expression_and_renders_apple_text) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);
  const auto source = file_util::read_text_file(file_util::get_file_path(
      {"test/goalc/source_templates/arm64-aot/false-from-jak1-gcommon.gc"}));
  const auto code = compiler.compile_top_level_source(source, "false-from-jak1-gcommon");

  EXPECT_EQ(code, (std::vector<u8>{0xe0, 0x03, 0x15, 0xaa, 0xc0, 0x03, 0x5f, 0xd6}));

  const aot::AppleArm64Function function{"goalpad_aot_false", code};
  EXPECT_EQ(aot::render_apple_arm64_assembly(function),
            ".section __TEXT,__text,regular,pure_instructions\n"
            ".p2align 2\n"
            ".globl _goalpad_aot_false\n"
            "_goalpad_aot_false:\n"
            "  .long 0xaa1503e0\n"
            "  .long 0xd65f03c0\n"
            ".subsections_via_symbols\n");
}

TEST(Arm64Aot, compiles_full_jak1_false_func_and_renders_apple_text) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);
  const auto source = file_util::read_text_file(file_util::get_file_path(
      {"test/goalc/source_templates/arm64-aot/full-false-func-from-jak1-gcommon.gc"}));
  const auto code = compiler.compile_arm64_aot_source(source, "full-false-func-from-jak1-gcommon",
                                                      std::optional<std::string>{"false-func"});

  EXPECT_EQ(code, (std::vector<u8>{0xe0, 0x03, 0x15, 0xaa, 0xc0, 0x03, 0x5f, 0xd6}));

  const aot::AppleArm64Function function{"goalpad_aot_false_func", code};
  EXPECT_EQ(aot::render_apple_arm64_assembly(function),
            ".section __TEXT,__text,regular,pure_instructions\n"
            ".p2align 2\n"
            ".globl _goalpad_aot_false_func\n"
            "_goalpad_aot_false_func:\n"
            "  .long 0xaa1503e0\n"
            "  .long 0xd65f03c0\n"
            ".subsections_via_symbols\n");
}

TEST(Arm64Aot, compiles_full_jak1_identity_and_renders_apple_text) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);
  const auto source = file_util::read_text_file(file_util::get_file_path(
      {"test/goalc/source_templates/arm64-aot/full-identity-from-jak1-gcommon.gc"}));
  const auto code = compiler.compile_arm64_aot_source(
      source, "full-identity-from-jak1-gcommon", std::optional<std::string>{"identity"});

  EXPECT_EQ(code, (std::vector<u8>{0xc0, 0x03, 0x5f, 0xd6}));

  const aot::AppleArm64Function function{"goalpad_aot_identity", code};
  EXPECT_EQ(aot::render_apple_arm64_assembly(function),
            ".section __TEXT,__text,regular,pure_instructions\n"
            ".p2align 2\n"
            ".globl _goalpad_aot_identity\n"
            "_goalpad_aot_identity:\n"
            "  .long 0xd65f03c0\n"
            ".subsections_via_symbols\n");
}

TEST(Arm64Aot, compiles_full_jak1_true_func_and_renders_apple_text) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);
  const auto source = file_util::read_text_file(file_util::get_file_path(
      {"test/goalc/source_templates/arm64-aot/full-true-func-from-jak1-gcommon.gc"}));
  const auto code = compiler.compile_arm64_aot_source(source, "full-true-func-from-jak1-gcommon",
                                                      std::optional<std::string>{"true-func"});

  EXPECT_EQ(code, (std::vector<u8>{0xe0, 0x03, 0x15, 0xaa, 0x00, 0x20,
                                   0x00, 0x91, 0xc0, 0x03, 0x5f, 0xd6}));

  const aot::AppleArm64Function function{"goalpad_aot_true_func", code};
  EXPECT_EQ(aot::render_apple_arm64_assembly(function),
            ".section __TEXT,__text,regular,pure_instructions\n"
            ".p2align 2\n"
            ".globl _goalpad_aot_true_func\n"
            "_goalpad_aot_true_func:\n"
            "  .long 0xaa1503e0\n"
            "  .long 0x91002000\n"
            "  .long 0xd65f03c0\n"
            ".subsections_via_symbols\n");
}

TEST(Arm64Aot, compiles_full_jak1_lognot_and_renders_apple_text) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);
  const auto source = file_util::read_text_file(file_util::get_file_path(
      {"test/goalc/source_templates/arm64-aot/full-lognot-from-jak1-gcommon.gc"}));
  const auto code = compiler.compile_arm64_aot_source(source, "full-lognot-from-jak1-gcommon",
                                                      std::optional<std::string>{"lognot"});

  EXPECT_EQ(code, (std::vector<u8>{0xe0, 0x03, 0x20, 0xaa, 0xc0, 0x03, 0x5f, 0xd6}));

  const aot::AppleArm64Function function{"goalpad_aot_lognot", code};
  EXPECT_EQ(aot::render_apple_arm64_assembly(function),
            ".section __TEXT,__text,regular,pure_instructions\n"
            ".p2align 2\n"
            ".globl _goalpad_aot_lognot\n"
            "_goalpad_aot_lognot:\n"
            "  .long 0xaa2003e0\n"
            "  .long 0xd65f03c0\n"
            ".subsections_via_symbols\n");
}

TEST(Arm64Aot, renders_zero_argument_native_export_metadata) {
  const aot::NativeExport0 native_export{"false-func", "goalpad_aot_false_func"};

  EXPECT_EQ(aot::render_cpp_xmacro_export0(native_export),
            "#ifndef OPENGOAL_AOT_EXPORT0\n"
            "#error \"Define OPENGOAL_AOT_EXPORT0 before including this file.\"\n"
            "#endif\n"
            "OPENGOAL_AOT_EXPORT0(\"false-func\", goalpad_aot_false_func)\n");
}

TEST(Arm64Aot, renders_true_func_zero_argument_native_export_metadata) {
  const aot::NativeExport0 native_export{"true-func", "goalpad_aot_true_func"};

  EXPECT_EQ(aot::render_cpp_xmacro_export0(native_export),
            "#ifndef OPENGOAL_AOT_EXPORT0\n"
            "#error \"Define OPENGOAL_AOT_EXPORT0 before including this file.\"\n"
            "#endif\n"
            "OPENGOAL_AOT_EXPORT0(\"true-func\", goalpad_aot_true_func)\n");
}

TEST(Arm64Aot, renders_one_argument_native_export_metadata) {
  const aot::NativeExport1 native_export{"identity", "goalpad_aot_identity"};

  EXPECT_EQ(aot::render_cpp_xmacro_export1(native_export),
            "#ifndef OPENGOAL_AOT_EXPORT1\n"
            "#error \"Define OPENGOAL_AOT_EXPORT1 before including this file.\"\n"
            "#endif\n"
            "OPENGOAL_AOT_EXPORT1(\"identity\", goalpad_aot_identity)\n");
}

TEST(Arm64Aot, rejects_native_exports_outside_the_zero_argument_proof) {
  EXPECT_THROW(aot::render_cpp_xmacro_export0({"", "goalpad_aot_false_func"}),
               std::invalid_argument);
  EXPECT_THROW(aot::render_cpp_xmacro_export0({"other", "goalpad_aot_false_func"}),
               std::invalid_argument);
  EXPECT_THROW(aot::render_cpp_xmacro_export0({"false-func", "invalid-symbol"}),
               std::invalid_argument);
  EXPECT_THROW(aot::render_cpp_xmacro_export0({"false-func", "__reserved"}), std::invalid_argument);
  EXPECT_THROW(aot::render_cpp_xmacro_export0({"false-func", "_Reserved"}), std::invalid_argument);
  EXPECT_THROW(aot::render_cpp_xmacro_export0({"false-func", "class"}), std::invalid_argument);
  EXPECT_THROW(aot::render_cpp_xmacro_export0({"false-func", "main"}), std::invalid_argument);
  EXPECT_THROW(aot::render_cpp_xmacro_export0({"true-func", "invalid-symbol"}),
               std::invalid_argument);
  EXPECT_THROW(aot::render_cpp_xmacro_export0({"false-func", "goalpad_aot_true_func"}),
               std::invalid_argument);
  EXPECT_THROW(aot::render_cpp_xmacro_export0({"true-func", "goalpad_aot_false_func"}),
               std::invalid_argument);
  EXPECT_THROW(aot::render_cpp_xmacro_export0({"identity", "goalpad_aot_identity"}),
               std::invalid_argument);
}

TEST(Arm64Aot, rejects_native_exports_outside_the_identity_proof) {
  EXPECT_THROW(aot::render_cpp_xmacro_export1({"", "goalpad_aot_identity"}),
               std::invalid_argument);
  EXPECT_THROW(aot::render_cpp_xmacro_export1({"false-func", "goalpad_aot_identity"}),
               std::invalid_argument);
  EXPECT_THROW(aot::render_cpp_xmacro_export1({"identity", "goalpad_aot_false_func"}),
               std::invalid_argument);
}

TEST(Arm64Aot, rejects_unknown_named_function) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);
  const auto source = file_util::read_text_file(file_util::get_file_path(
      {"test/goalc/source_templates/arm64-aot/full-false-func-from-jak1-gcommon.gc"}));

  EXPECT_THROW(compiler.compile_arm64_aot_source(source, "full-false-func-from-jak1-gcommon",
                                                 std::optional<std::string>{"missing-false-func"}),
               std::runtime_error);
}

TEST(Arm64Aot, rejects_named_constant_function_outside_the_false_func_proof) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);

  EXPECT_THROW(compiler.compile_arm64_aot_source("(defun answer () 42)", "answer",
                                                 std::optional<std::string>{"answer"}),
               std::runtime_error);
}

TEST(Arm64Aot, rejects_false_func_name_with_a_non_false_body) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);

  EXPECT_THROW(compiler.compile_arm64_aot_source("(defun false-func () 42)", "non-false-body",
                                                 std::optional<std::string>{"false-func"}),
               std::runtime_error);
}

TEST(Arm64Aot, rejects_false_body_under_a_different_name) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);

  EXPECT_THROW(compiler.compile_arm64_aot_source("(defun other () '#f)", "other-false",
                                                 std::optional<std::string>{"other"}),
               std::runtime_error);
}

TEST(Arm64Aot, rejects_duplicate_named_functions) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);

  EXPECT_THROW(compiler.compile_arm64_aot_source("(defun false-func () '#f)\n"
                                                 "(defun false-func () '#f)",
                                                 "duplicate-false-func",
                                                 std::optional<std::string>{"false-func"}),
               std::runtime_error);
}

TEST(Arm64Aot, rejects_named_function_with_extra_top_level_effects) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);

  EXPECT_THROW(compiler.compile_arm64_aot_source("(defun false-func () '#f)\n"
                                                 "42",
                                                 "false-func-with-extra-top-level-effect",
                                                 std::optional<std::string>{"false-func"}),
               std::runtime_error);
}

TEST(Arm64Aot, rejects_identity_with_a_different_body) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);

  EXPECT_THROW(compiler.compile_arm64_aot_source("(defun identity ((x object)) '#f)",
                                                 "identity-with-false-body",
                                                 std::optional<std::string>{"identity"}),
               std::runtime_error);
}

TEST(Arm64Aot, rejects_identity_with_multiple_parameters) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);

  EXPECT_THROW(compiler.compile_arm64_aot_source("(defun identity ((x object) (y object)) x)",
                                                 "identity-with-two-parameters",
                                                 std::optional<std::string>{"identity"}),
               std::runtime_error);
}

TEST(Arm64Aot, rejects_identity_shaped_function_with_a_different_name) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);

  EXPECT_THROW(compiler.compile_arm64_aot_source("(defun not-identity ((x object)) x)",
                                                 "not-identity",
                                                 std::optional<std::string>{"not-identity"}),
               std::runtime_error);
}

TEST(Arm64Aot, rejects_true_func_with_a_false_body) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);

  EXPECT_THROW(compiler.compile_arm64_aot_source("(defun true-func () '#f)", "true-with-false-body",
                                                 std::optional<std::string>{"true-func"}),
               std::runtime_error);
}

TEST(Arm64Aot, rejects_true_body_under_a_different_name) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);

  EXPECT_THROW(compiler.compile_arm64_aot_source("(defun other () '#t)", "other-true",
                                                 std::optional<std::string>{"other"}),
               std::runtime_error);
}

TEST(Arm64Aot, rejects_true_func_with_an_argument) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);

  EXPECT_THROW(compiler.compile_arm64_aot_source("(defun true-func ((x object)) '#t)",
                                                 "true-with-argument",
                                                 std::optional<std::string>{"true-func"}),
               std::runtime_error);
}

TEST(Arm64Aot, rejects_lognot_with_a_different_body) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);

  EXPECT_THROW(
      compiler.compile_arm64_aot_source("(defun lognot ((a int)) a)", "lognot-with-identity-body",
                                        std::optional<std::string>{"lognot"}),
      std::runtime_error);
}

TEST(Arm64Aot, rejects_lognot_body_under_a_different_name) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);

  EXPECT_THROW(
      compiler.compile_arm64_aot_source("(defun other ((a int)) (lognot a))", "other-lognot",
                                        std::optional<std::string>{"other"}),
      std::runtime_error);
}

TEST(Arm64Aot, rejects_lognot_with_multiple_parameters) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);

  EXPECT_THROW(compiler.compile_arm64_aot_source("(defun lognot ((a int) (b int)) (lognot a))",
                                                 "lognot-with-two-parameters",
                                                 std::optional<std::string>{"lognot"}),
               std::runtime_error);
}

TEST(Arm64Aot, emits_unary_gpr_integer_not) {
  RegVal value{{RegClass::GPR_64, 0}, TypeSpec("int")};
  IR_IntegerMath integer_not(IntegerMathKind::NOT_64, &value, nullptr);

  Assignment assignment;
  assignment.kind = Assignment::Kind::REGISTER;
  assignment.reg = emitter::X0;
  AllocationResult allocations;
  allocations.ass_as_ranges = {AssignmentRange(0, {true}, {assignment})};

  FunctionDebugInfo debug{};
  emitter::ObjectGenerator generator(GameVersion::Jak1, emitter::InstructionSet::ARM64);
  const auto function = generator.add_function_to_seg(MAIN_SEGMENT, &debug);
  integer_not.do_codegen_arm64(&generator, allocations, generator.add_ir(function));
  generator.add_instr_no_ir(function, emitter::IGen::ret(generator),
                            InstructionInfo::Kind::EPILOGUE);

  EXPECT_EQ(generator.materialize_arm64_function(function),
            (std::vector<u8>{0xe0, 0x03, 0x20, 0xaa, 0xc0, 0x03, 0x5f, 0xd6}));
}

TEST(Arm64Aot, rejects_unsupported_arm64_integer_math) {
  RegVal destination{{RegClass::GPR_64, 0}, TypeSpec("int")};
  RegVal source{{RegClass::GPR_64, 1}, TypeSpec("int")};
  IR_IntegerMath integer_add(IntegerMathKind::ADD_64, &destination, &source);

  Assignment destination_assignment;
  destination_assignment.kind = Assignment::Kind::REGISTER;
  destination_assignment.reg = emitter::X0;
  Assignment source_assignment;
  source_assignment.kind = Assignment::Kind::REGISTER;
  source_assignment.reg = emitter::X1;
  AllocationResult allocations;
  allocations.ass_as_ranges = {
      AssignmentRange(0, {true}, {destination_assignment}),
      AssignmentRange(0, {true}, {source_assignment}),
  };

  FunctionDebugInfo debug{};
  emitter::ObjectGenerator generator(GameVersion::Jak1, emitter::InstructionSet::ARM64);
  const auto function = generator.add_function_to_seg(MAIN_SEGMENT, &debug);
  EXPECT_THROW(integer_add.do_codegen_arm64(&generator, allocations, generator.add_ir(function)),
               std::runtime_error);
}

TEST(Arm64Aot, emits_non_coalesced_gpr_register_move) {
  const RegVal destination{{RegClass::GPR_64, 0}, TypeSpec("object")};
  const RegVal source{{RegClass::GPR_64, 1}, TypeSpec("object")};
  IR_RegSet move(&destination, &source);

  Assignment destination_assignment;
  destination_assignment.kind = Assignment::Kind::REGISTER;
  destination_assignment.reg = emitter::X0;
  Assignment source_assignment;
  source_assignment.kind = Assignment::Kind::REGISTER;
  source_assignment.reg = emitter::X1;
  AllocationResult allocations;
  allocations.ass_as_ranges = {
      AssignmentRange(0, {true}, {destination_assignment}),
      AssignmentRange(0, {true}, {source_assignment}),
  };

  FunctionDebugInfo debug{};
  emitter::ObjectGenerator generator(GameVersion::Jak1, emitter::InstructionSet::ARM64);
  const auto function = generator.add_function_to_seg(MAIN_SEGMENT, &debug);
  move.do_codegen_arm64(&generator, allocations, generator.add_ir(function));
  generator.add_instr_no_ir(function, emitter::IGen::ret(generator),
                            InstructionInfo::Kind::EPILOGUE);

  EXPECT_EQ(generator.materialize_arm64_function(function),
            (std::vector<u8>{0xe0, 0x03, 0x01, 0xaa, 0xc0, 0x03, 0x5f, 0xd6}));
}

TEST(Arm64Aot, rejects_source_outside_the_supported_aot_proof) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);
  EXPECT_THROW(compiler.compile_top_level_source("41", "constant-41"), std::runtime_error);
  EXPECT_THROW(compiler.compile_top_level_source("'#t", "true-from-jak1-gcommon"),
               std::runtime_error);
}

TEST(Arm64Aot, uses_apple_abi_registers_without_goal_specials) {
  const auto& registers = emitter::get_register_info(emitter::InstructionSet::ARM64);

  const std::vector<emitter::Register> expected_argument_registers = {
      emitter::X0, emitter::X1, emitter::X2, emitter::X3,
      emitter::X4, emitter::X5, emitter::X6, emitter::X7,
  };
  for (int index = 0; index < emitter::RegisterInfo::N_ARGS; index++) {
    EXPECT_EQ(registers.get_gpr_arg_reg(index), expected_argument_registers.at(index));
  }

  EXPECT_EQ(registers.get_gpr_ret_reg(), emitter::X0);
  EXPECT_EQ(registers.get_process_reg(), emitter::X20);
  EXPECT_EQ(registers.get_st_reg(), emitter::X21);
  EXPECT_EQ(registers.get_offset_reg(), emitter::X22);
  EXPECT_NE(emitter::Register(emitter::RAX), emitter::Register(emitter::X0));
  EXPECT_NE(emitter::Register(emitter::X0).logical_id(),
            emitter::Register(emitter::V0).logical_id());

  expect_no_reserved_arm64_registers(registers.get_gpr_temp_alloc_order());
  expect_no_reserved_arm64_registers(registers.get_gpr_alloc_order());
  expect_no_reserved_arm64_registers(registers.get_gpr_spill_alloc_order());
  expect_no_reserved_arm64_registers(
      registers.get_v2_alloc_order(emitter::HWRegKind::GPR, false, false, false));
  expect_no_reserved_arm64_registers(
      registers.get_v2_alloc_order(emitter::HWRegKind::GPR, true, false, false));
  expect_no_reserved_arm64_registers(
      registers.get_v2_alloc_order(emitter::HWRegKind::GPR, false, true, false));
  expect_no_reserved_arm64_registers(
      registers.get_v2_alloc_order(emitter::HWRegKind::GPR, false, false, true));
}

TEST(Arm64Aot, function_calls_clobber_only_target_temporaries) {
  RegVal function({RegClass::GPR_64, 0}, TypeSpec("function"));
  RegVal result({RegClass::GPR_64, 1}, TypeSpec("object"));

  IR_FunctionCall arm64_call(&function, &result, {}, {}, std::nullopt,
                             emitter::InstructionSet::ARM64);
  const auto arm64_rai = arm64_call.to_rai();
  const std::vector<emitter::Register> expected_arm64_clobbers = {
      emitter::X0,  emitter::X1,  emitter::X2,  emitter::X3,  emitter::X4,  emitter::X5,
      emitter::X6,  emitter::X7,  emitter::X8,  emitter::X9,  emitter::X10, emitter::X11,
      emitter::X12, emitter::X13, emitter::X14, emitter::X15, emitter::V0,  emitter::V1,
      emitter::V2,  emitter::V3,  emitter::V4,  emitter::V5,  emitter::V6,  emitter::V7,
      emitter::V16, emitter::V17, emitter::V18, emitter::V19, emitter::V20, emitter::V21,
      emitter::V22, emitter::V23, emitter::V24, emitter::V25, emitter::V26, emitter::V27,
      emitter::V28, emitter::V29, emitter::V30, emitter::V31,
  };
  EXPECT_EQ(arm64_rai.clobber, expected_arm64_clobbers);
  expect_no_reserved_arm64_registers(arm64_rai.clobber);
  for (const auto& reg : arm64_rai.clobber) {
    EXPECT_EQ(reg.instruction_set(), emitter::InstructionSet::ARM64);
  }

  IR_FunctionCall x86_call(&function, &result, {}, {}, std::nullopt, emitter::InstructionSet::X86);
  const auto x86_rai = x86_call.to_rai();
  const std::vector<emitter::Register> expected_x86_clobbers = {
      emitter::RAX,  emitter::RCX,  emitter::RDX,  emitter::RSI,  emitter::RDI,
      emitter::R8,   emitter::R9,   emitter::XMM0, emitter::XMM1, emitter::XMM2,
      emitter::XMM3, emitter::XMM4, emitter::XMM5, emitter::XMM6, emitter::XMM7,
  };
  EXPECT_EQ(x86_rai.clobber, expected_x86_clobbers);
  for (const auto& reg : x86_rai.clobber) {
    EXPECT_EQ(reg.instruction_set(), emitter::InstructionSet::X86);
  }
}

TEST(Arm64Aot, allocators_preserve_values_live_across_function_calls) {
  for (const bool use_v2_allocator : {false, true}) {
    expect_arm64_live_across_call_uses_saved_register(RegClass::GPR_64, use_v2_allocator);
    expect_arm64_live_across_call_uses_saved_register(RegClass::INT_128, use_v2_allocator);
  }
}
