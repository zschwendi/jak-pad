#include <algorithm>
#include <vector>

#include "common/util/FileUtil.h"

#include "goalc/aot/AOTWriter.h"
#include "goalc/compiler/Compiler.h"
#include "goalc/emitter/InstructionSet.h"
#include "goalc/emitter/Register.h"
#include "gtest/gtest.h"

namespace {

void expect_no_reserved_arm64_registers(const std::vector<emitter::Register>& registers) {
  for (const auto reserved : {emitter::X20, emitter::X21, emitter::X22, emitter::SP,
                              emitter::X29, emitter::X30}) {
    EXPECT_EQ(std::find(registers.begin(), registers.end(), reserved), registers.end());
  }
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

TEST(Arm64Aot, rejects_source_outside_the_literal_42_proof) {
  Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);
  EXPECT_THROW(compiler.compile_top_level_source("41", "constant-41"), std::runtime_error);
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
