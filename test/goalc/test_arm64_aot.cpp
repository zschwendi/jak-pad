#include <algorithm>
#include <vector>

#include "common/util/FileUtil.h"

#include "goalc/aot/AOTWriter.h"
#include "goalc/compiler/Compiler.h"
#include "goalc/compiler/IR.h"
#include "goalc/compiler/Val.h"
#include "goalc/emitter/InstructionSet.h"
#include "goalc/emitter/Register.h"
#include "goalc/regalloc/Allocator.h"
#include "goalc/regalloc/Allocator_v2.h"
#include "gtest/gtest.h"

namespace {

void expect_no_reserved_arm64_registers(const std::vector<emitter::Register>& registers) {
  for (const auto reserved : {emitter::X20, emitter::X21, emitter::X22, emitter::SP,
                              emitter::X29, emitter::X30}) {
    EXPECT_EQ(std::find(registers.begin(), registers.end(), reserved), registers.end());
  }
}

void expect_arm64_live_across_call_uses_saved_register(RegClass reg_class,
                                                       bool use_v2_allocator) {
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
  EXPECT_EQ(std::find(registers.get_call_clobbered().begin(),
                      registers.get_call_clobbered().end(), assignment.reg),
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

  IR_FunctionCall x86_call(&function, &result, {}, {}, std::nullopt,
                           emitter::InstructionSet::X86);
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
