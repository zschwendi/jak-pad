/*!
 * @file CodeGenerator.cpp
 * Generate object files from a FileEnv using an emitter::ObjectGenerator.
 * Populates a DebugInfo.
 * Currently owns the logic for emitting the function prologues/epilogues and stack spill ops.
 */

#include "CodeGenerator.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

#include "IR.h"

#include "goalc/debugger/DebugInfo.h"
#include "goalc/emitter/IGen.h"

#include "fmt/format.h"

using namespace emitter;

namespace {

void record_local_variables(const FunctionEnv* func, FunctionDebugInfo* debug) {
  const auto& allocations = func->allocations();
  if (!allocations.ok || allocations.ass_as_ranges.empty()) {
    return;
  }

  // ireg id -> source name, parameters first, then anything bound in a lexical scope
  std::unordered_map<int, std::pair<std::string, bool>> names;
  for (const auto& [symbol, reg_val] : func->params) {
    if (reg_val) {
      names[reg_val->ireg().id] = {symbol.name_ptr, true};
    }
  }
  for (const auto& env : func->child_envs()) {
    auto* lexical = dynamic_cast<LexicalEnv*>(env.get());
    if (!lexical) {
      continue;
    }
    for (const auto& [symbol, reg_val] : lexical->vars) {
      if (reg_val && names.find(reg_val->ireg().id) == names.end()) {
        names[reg_val->ireg().id] = {symbol.name_ptr, false};
      }
    }
  }

  if (names.empty()) {
    return;
  }

  const int instruction_count = int(func->code().size());

  for (const auto& [ireg_id, name_info] : names) {
    if (ireg_id < 0 || ireg_id >= int(allocations.ass_as_ranges.size()) ||
        ireg_id >= int(func->reg_vals().size())) {
      continue;
    }
    const auto& range = allocations.ass_as_ranges.at(ireg_id);

    LocalVariableDebugInfo local;
    local.name = name_info.first;
    local.is_parameter = name_info.second;
    local.type = func->reg_vals().at(ireg_id)->type();

    for (int instr = 0; instr < instruction_count; instr++) {
      if (!range.is_live_at_instr(instr)) {
        continue;
      }
      const auto& assignment = range.get(instr);
      if (!assignment.is_assigned()) {
        continue;
      }

      VariableLocation here;
      here.start_ir = instr;
      here.end_ir = instr;
      if (assignment.kind == Assignment::Kind::REGISTER) {
        here.kind = VariableLocation::Kind::REGISTER;
        here.reg = assignment.reg.id();
      } else if (assignment.kind == Assignment::Kind::STACK) {
        here.kind = VariableLocation::Kind::STACK;
        // spilled variables sit at rsp + slot * 8, matching how the spill ops address them
        here.stack_offset = allocations.get_slot_for_spill(assignment.stack_slot) * GPR_SIZE;
      } else {
        continue;
      }

      // extend the previous interval instead of starting a new one where nothing changed
      if (!local.locations.empty()) {
        auto& previous = local.locations.back();
        if (previous.end_ir == instr - 1 && previous.kind == here.kind &&
            previous.reg == here.reg && previous.stack_offset == here.stack_offset) {
          previous.end_ir = instr;
          continue;
        }
      }
      local.locations.push_back(here);
    }

    if (!local.locations.empty()) {
      debug->locals.push_back(std::move(local));
    }
  }

  // parameters first, then alphabetically
  std::sort(debug->locals.begin(), debug->locals.end(),
            [](const LocalVariableDebugInfo& a, const LocalVariableDebugInfo& b) {
              if (a.is_parameter != b.is_parameter) {
                return a.is_parameter;
              }
              return a.name < b.name;
            });
}

}  // namespace

CodeGenerator::CodeGenerator(FileEnv* env,
                             DebugInfo* debug_info,
                             GameVersion version,
                             InstructionSet instruction_set)
    : m_gen(version, instruction_set), m_fe(env), m_debug_info(debug_info) {}

/*!
 * Generate an object file.
 */
std::vector<u8> CodeGenerator::run(const TypeSystem* ts) {
  std::unordered_set<std::string> function_names;

  // first, add each function to the ObjectGenerator (but don't add any data)
  for (auto& f : m_fe->functions()) {
    if (function_names.find(f->name()) == function_names.end()) {
      function_names.insert(f->name());
    } else {
      printf("Failed to codegen, there are two functions with internal names [%s]\n",
             f->name().c_str());
      throw std::runtime_error("Failed to codegen.");
    }
    auto rec =
        m_gen.add_function_to_seg(f->segment, &m_debug_info->add_function(f->name(), m_fe->name()));
    for (auto& x : f->code_source()) {
      rec.debug->code_sources.push_back(x.heap_obj);
    }
    for (auto& x : f->code()) {
      rec.debug->ir_strings.push_back(x->print());
    }
    record_local_variables(f.get(), rec.debug);
  }

  // next, add all static objects.
  for (auto& static_obj : m_fe->statics()) {
    static_obj->generate(&m_gen);
  }

  // next, add instructions to functions
  for (size_t i = 0; i < m_fe->functions().size(); i++) {
    do_function(m_fe->functions().at(i).get(), i);
  }

  // generate a v3 object.
  return m_gen.generate_data_v3(ts).to_vector();
}

std::vector<u8> CodeGenerator::run_arm64_aot_function(
    const std::optional<std::string>& function_name) {
  if (m_gen.instr_set() != InstructionSet::ARM64) {
    throw std::runtime_error("ARM64 AOT proof requires the ARM64 instruction set.");
  }
  if (!m_fe->statics().empty()) {
    throw std::runtime_error("ARM64 AOT proof does not support static data or relocations.");
  }

  FunctionEnv* function = nullptr;
  if (function_name) {
    size_t matching_functions = 0;
    for (const auto& candidate : m_fe->functions()) {
      if (candidate->name() == *function_name) {
        function = candidate.get();
        matching_functions++;
      }
    }
    if (!function) {
      throw std::runtime_error(fmt::format(
          "ARM64 AOT function '{}' was not found in the compiled source.", *function_name));
    }
    if (matching_functions != 1) {
      throw std::runtime_error(
          fmt::format("ARM64 AOT function '{}' is defined more than once.", *function_name));
    }
    if (m_fe->functions().size() != 2) {
      throw std::runtime_error(
          "Named ARM64 AOT proof requires exactly one function and its top-level installer.");
    }

    const auto& installer = m_fe->top_level_function().code();
    const auto* function_address =
        installer.size() == 4 ? dynamic_cast<IR_FunctionAddr*>(installer.at(0).get()) : nullptr;
    const auto* symbol_value =
        installer.size() == 4 ? dynamic_cast<IR_SetSymbolValue*>(installer.at(1).get()) : nullptr;
    const bool supported_installer =
        function_address && symbol_value && dynamic_cast<IR_Return*>(installer.at(2).get()) &&
        dynamic_cast<IR_Null*>(installer.at(3).get()) && function_address->function() == function &&
        symbol_value->source() == function_address->destination() &&
        symbol_value->destination()->name() == *function_name;
    if (!supported_installer) {
      throw std::runtime_error(
          "Named ARM64 AOT proof only supports an unmodified top-level defun installer.");
    }
  } else {
    if (m_fe->functions().size() != 1) {
      throw std::runtime_error("ARM64 AOT proof only supports one top-level function.");
    }
    const auto* top_level = &m_fe->top_level_function();
    for (const auto& candidate : m_fe->functions()) {
      if (candidate.get() == top_level) {
        function = candidate.get();
        break;
      }
    }
    ASSERT(function);
  }

  auto rec = m_gen.add_function_to_seg(function->segment,
                                       &m_debug_info->add_function(function->name(), m_fe->name()));
  for (const auto& source : function->code_source()) {
    rec.debug->code_sources.push_back(source.heap_obj);
  }
  for (const auto& ir : function->code()) {
    rec.debug->ir_strings.push_back(ir->print());
  }
  record_local_variables(function, rec.debug);

  do_goal_function_arm64(function, 0);
  return m_gen.materialize_arm64_function(rec);
}

void CodeGenerator::do_function(FunctionEnv* env, int f_idx) {
  if (env->is_asm_func) {
    if (m_gen.instr_set() == InstructionSet::X86) {
      do_asm_function_x86(env, f_idx, env->asm_func_saved_regs);
    } else if (m_gen.instr_set() == InstructionSet::ARM64) {
      do_asm_function_arm64(env, f_idx, env->asm_func_saved_regs);
    } else {
      throw std::runtime_error("CodeGenerator::do_function, instruction set not supported");
    }
  } else {
    if (m_gen.instr_set() == InstructionSet::X86) {
      do_goal_function_x86(env, f_idx);
    } else if (m_gen.instr_set() == InstructionSet::ARM64) {
      do_goal_function_arm64(env, f_idx);
    } else {
      throw std::runtime_error("CodeGenerator::do_function, instruction set not supported");
    }
  }
}

/*!
 * Add instructions to the function, specified by index.
 * Generates prologues / epilogues.
 */
void CodeGenerator::do_goal_function_x86(FunctionEnv* env, int f_idx) {
  bool use_new_xmms = true;
  auto* debug = &m_debug_info->function_by_name(env->name());

  auto f_rec = m_gen.get_existing_function_record(f_idx);
  // todo, extra alignment settings

  auto& ri = emitter::gRegInfo;
  const auto& allocs = env->alloc_result();

  // compute how much stack we will use
  int stack_offset = 0;

  // count how many xmm's we have to backup
  int n_xmm_backups = 0;
  for (auto& saved_reg : allocs.used_saved_regs) {
    if (saved_reg.is_xmm(m_gen.instr_set())) {
      n_xmm_backups++;
    }
  }

  // only for new xmms. if n == 0, we don't use this at all.
  int xmm_backup_stack_offset = 8 + XMM_SIZE * n_xmm_backups;

  if (use_new_xmms) {
    if (n_xmm_backups > 0) {
      // offset the stack
      stack_offset += xmm_backup_stack_offset;
      m_gen.add_instr_no_ir(f_rec, IGen::sub_gpr64_imm(m_gen, RSP, xmm_backup_stack_offset),
                            InstructionInfo::Kind::PROLOGUE);
      // back up xmms
      int i = 0;
      for (auto& saved_reg : allocs.used_saved_regs) {
        if (saved_reg.is_xmm(m_gen.instr_set())) {
          int offset = i * XMM_SIZE;
          m_gen.add_instr_no_ir(f_rec,
                                IGen::store128_xmm128_reg_offset(m_gen, RSP, saved_reg, offset),
                                InstructionInfo::Kind::PROLOGUE);
          i++;
        }
      }
    }
  } else {
    // back up xmms (currently not aligned)
    for (auto& saved_reg : allocs.used_saved_regs) {
      if (saved_reg.is_xmm(m_gen.instr_set())) {
        m_gen.add_instr_no_ir(f_rec, IGen::sub_gpr64_imm8s(m_gen, RSP, XMM_SIZE),
                              InstructionInfo::Kind::PROLOGUE);
        m_gen.add_instr_no_ir(f_rec, IGen::store128_gpr64_simd128(m_gen, RSP, saved_reg),
                              InstructionInfo::Kind::PROLOGUE);
        stack_offset += XMM_SIZE;
      }
    }
  }

  // back up gprs
  for (auto& saved_reg : allocs.used_saved_regs) {
    if (saved_reg.is_gpr(m_gen.instr_set())) {
      m_gen.add_instr_no_ir(f_rec, IGen::push_gpr64(m_gen, saved_reg),
                            InstructionInfo::Kind::PROLOGUE);
      stack_offset += GPR_SIZE;
    }
  }

  // do we include an extra push to get 8 more bytes to keep the stack aligned?
  bool bonus_push = false;

  // the offset to add directly to rsp for stack variables or spills (no push/pop)
  int manually_added_stack_offset =
      GPR_SIZE * (allocs.stack_slots_for_spills + allocs.stack_slots_for_vars);
  stack_offset += manually_added_stack_offset;

  // do we need to align or manually offset?
  if (manually_added_stack_offset || allocs.needs_aligned_stack_for_spills ||
      env->needs_aligned_stack()) {
    if (!(stack_offset & 15)) {
      if (manually_added_stack_offset) {
        // if we're already adding to rsp, just add 8 more.
        manually_added_stack_offset += 8;
      } else {
        // otherwise to an extra push, and remember so we can do an extra pop later on.
        bonus_push = true;
        m_gen.add_instr_no_ir(f_rec, IGen::push_gpr64(m_gen, ri.get_saved_gpr(0)),
                              InstructionInfo::Kind::PROLOGUE);
      }
      stack_offset += 8;
    }

    ASSERT(stack_offset & 15);

    // do manual stack offset.
    if (manually_added_stack_offset) {
      m_gen.add_instr_no_ir(f_rec, IGen::sub_gpr64_imm(m_gen, RSP, manually_added_stack_offset),
                            InstructionInfo::Kind::PROLOGUE);
    }
  }
  debug->stack_usage = stack_offset;

  // emit each IR into x86 instructions.
  for (int ir_idx = 0; ir_idx < int(env->code().size()); ir_idx++) {
    auto& ir = env->code().at(ir_idx);
    // start of IR
    auto i_rec = m_gen.add_ir(f_rec);

    // load anything off the stack that was spilled and is needed.
    auto& bonus = allocs.stack_ops.at(ir_idx);
    for (auto& op : bonus.ops) {
      if (op.load) {
        if (op.reg.is_gpr(m_gen.instr_set()) && op.reg_class == RegClass::GPR_64) {
          // todo, s8 or 0 offset if possible?
          m_gen.add_instr(IGen::load64_gpr64_plus_s32(
                              m_gen, op.reg, allocs.get_slot_for_spill(op.slot) * GPR_SIZE, RSP),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) && op.reg_class == RegClass::FLOAT) {
          // load xmm32 off of the stack
          m_gen.add_instr(IGen::load_reg_offset_xmm32(
                              m_gen, op.reg, RSP, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) &&
                   (op.reg_class == RegClass::VECTOR_FLOAT || op.reg_class == RegClass::INT_128)) {
          m_gen.add_instr(IGen::load128_xmm128_reg_offset(
                              m_gen, op.reg, RSP, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else {
          ASSERT(false);
        }
      }
    }

    // do the actual op
    ir->do_codegen_x86(&m_gen, allocs, i_rec);

    // store things back on the stack if needed.
    for (auto& op : bonus.ops) {
      if (op.store) {
        if (op.reg.is_gpr(m_gen.instr_set()) && op.reg_class == RegClass::GPR_64) {
          // todo, s8 or 0 offset if possible?
          m_gen.add_instr(IGen::store64_gpr64_plus_s32(
                              m_gen, RSP, allocs.get_slot_for_spill(op.slot) * GPR_SIZE, op.reg),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) && op.reg_class == RegClass::FLOAT) {
          // store xmm32 on the stack
          m_gen.add_instr(IGen::store_reg_offset_xmm32(
                              m_gen, RSP, op.reg, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) &&
                   (op.reg_class == RegClass::VECTOR_FLOAT || op.reg_class == RegClass::INT_128)) {
          m_gen.add_instr(IGen::store128_xmm128_reg_offset(
                              m_gen, RSP, op.reg, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else {
          ASSERT(false);
        }
      }
    }
  }  // end IR loop

  // EPILOGUE
  if (manually_added_stack_offset || allocs.needs_aligned_stack_for_spills ||
      env->needs_aligned_stack()) {
    if (manually_added_stack_offset) {
      m_gen.add_instr_no_ir(f_rec, IGen::add_gpr64_imm(m_gen, RSP, manually_added_stack_offset),
                            InstructionInfo::Kind::EPILOGUE);
    }

    if (bonus_push) {
      ASSERT(!manually_added_stack_offset);
      m_gen.add_instr_no_ir(f_rec, IGen::pop_gpr64(m_gen, ri.get_saved_gpr(0)),
                            InstructionInfo::Kind::EPILOGUE);
    }
  }

  for (int i = int(allocs.used_saved_regs.size()); i-- > 0;) {
    auto& saved_reg = allocs.used_saved_regs.at(i);
    if (saved_reg.is_gpr(m_gen.instr_set())) {
      m_gen.add_instr_no_ir(f_rec, IGen::pop_gpr64(m_gen, saved_reg),
                            InstructionInfo::Kind::EPILOGUE);
    }
  }

  if (use_new_xmms) {
    if (n_xmm_backups > 0) {
      int j = n_xmm_backups;
      for (int i = int(allocs.used_saved_regs.size()); i-- > 0;) {
        auto& saved_reg = allocs.used_saved_regs.at(i);
        if (saved_reg.is_xmm(m_gen.instr_set())) {
          j--;
          int offset = j * XMM_SIZE;
          m_gen.add_instr_no_ir(f_rec,
                                IGen::load128_xmm128_reg_offset(m_gen, saved_reg, RSP, offset),
                                InstructionInfo::Kind::EPILOGUE);
        }
      }
      ASSERT(j == 0);
      m_gen.add_instr_no_ir(f_rec, IGen::add_gpr64_imm(m_gen, RSP, xmm_backup_stack_offset),
                            InstructionInfo::Kind::EPILOGUE);
    }
  } else {
    for (int i = int(allocs.used_saved_regs.size()); i-- > 0;) {
      auto& saved_reg = allocs.used_saved_regs.at(i);
      if (saved_reg.is_xmm(m_gen.instr_set())) {
        m_gen.add_instr_no_ir(f_rec, IGen::load128_simd128_gpr64(m_gen, saved_reg, RSP),
                              InstructionInfo::Kind::EPILOGUE);
        m_gen.add_instr_no_ir(f_rec, IGen::add_gpr64_imm8s(m_gen, RSP, XMM_SIZE),
                              InstructionInfo::Kind::EPILOGUE);
      }
    }
  }

  m_gen.add_instr_no_ir(f_rec, IGen::ret(m_gen), InstructionInfo::Kind::EPILOGUE);
}

void CodeGenerator::do_goal_function_arm64(FunctionEnv* env, int f_idx) {
  if (!m_fe->statics().empty()) {
    throw std::runtime_error("ARM64 AOT proof does not support static data or relocations.");
  }

  const auto& allocs = env->alloc_result();
  if (!allocs.ok) {
    throw std::runtime_error("ARM64 AOT proof requires successful register allocation.");
  }
  if (!allocs.used_saved_regs.empty()) {
    throw std::runtime_error("ARM64 AOT proof does not support saved registers.");
  }
  if (allocs.stack_slots_for_spills || allocs.num_spills || allocs.num_spilled_vars ||
      allocs.needs_aligned_stack_for_spills) {
    throw std::runtime_error("ARM64 AOT proof does not support spills.");
  }
  if (allocs.stack_slots_for_vars || env->needs_aligned_stack()) {
    throw std::runtime_error("ARM64 AOT proof does not support stack locals.");
  }
  if (allocs.stack_ops.size() != env->code().size()) {
    throw std::runtime_error("ARM64 AOT proof received invalid stack allocation metadata.");
  }
  for (const auto& stack_op : allocs.stack_ops) {
    if (!stack_op.ops.empty()) {
      throw std::runtime_error("ARM64 AOT proof does not support spill stack operations.");
    }
  }

  const auto& code = env->code();
  const auto is_allocated_to = [&allocs](const RegVal* value,
                                         int instruction,
                                         emitter::Register expected) {
    const auto ireg_id = value->ireg().id;
    if (ireg_id < 0 || ireg_id >= int(allocs.ass_as_ranges.size())) {
      return false;
    }
    const auto& assignments = allocs.ass_as_ranges.at(ireg_id);
    if (!assignments.has_info_at(instruction)) {
      return false;
    }
    const auto& assignment = assignments.get(instruction);
    return assignment.kind == Assignment::Kind::REGISTER && assignment.reg == expected;
  };
  const auto is_supported_top_level_result = [](IR* ir) {
    if (const auto* constant = dynamic_cast<IR_LoadConstant64*>(ir)) {
      return constant->value() == 42;
    }
    if (const auto* symbol = dynamic_cast<IR_LoadSymbolPointer*>(ir)) {
      return symbol->name() == "#f";
    }
    return false;
  };
  const bool supported_top_level =
      code.size() == 3 && is_supported_top_level_result(code.at(0).get()) &&
      dynamic_cast<IR_Return*>(code.at(1).get()) && dynamic_cast<IR_Null*>(code.at(2).get());
  const auto* value_reset =
      code.size() == 4 ? dynamic_cast<IR_ValueReset*>(code.at(0).get()) : nullptr;
  const auto* false_value =
      code.size() == 4 ? dynamic_cast<IR_LoadSymbolPointer*>(code.at(1).get()) : nullptr;
  const bool supported_false_function =
      env->name() == "false-func" && value_reset && value_reset->has_no_args() && false_value &&
      false_value->name() == "#f" && dynamic_cast<IR_Return*>(code.at(2).get()) &&
      dynamic_cast<IR_Null*>(code.at(3).get());
  const auto* true_value =
      code.size() == 4 ? dynamic_cast<IR_LoadSymbolPointer*>(code.at(1).get()) : nullptr;
  const auto* true_return =
      code.size() == 4 ? dynamic_cast<IR_Return*>(code.at(2).get()) : nullptr;
  const bool supported_true_function =
      env->name() == "true-func" && value_reset && value_reset->has_no_args() && true_value &&
      true_value->name() == "#t" && true_return &&
      true_return->value() == true_value->destination() &&
      dynamic_cast<IR_Null*>(code.at(3).get());
  const auto* lognot_value_reset =
      code.size() == 6 ? dynamic_cast<IR_ValueReset*>(code.at(0).get()) : nullptr;
  const auto* lognot_parameter_move =
      code.size() == 6 ? dynamic_cast<IR_RegSet*>(code.at(1).get()) : nullptr;
  const auto* lognot_value_move =
      code.size() == 6 ? dynamic_cast<IR_RegSet*>(code.at(2).get()) : nullptr;
  const auto* lognot_operation =
      code.size() == 6 ? dynamic_cast<IR_IntegerMath*>(code.at(3).get()) : nullptr;
  const auto* lognot_return =
      code.size() == 6 ? dynamic_cast<IR_Return*>(code.at(4).get()) : nullptr;
  const auto* lognot_argument = lognot_value_reset && lognot_value_reset->args().size() == 1
                                    ? lognot_value_reset->args().front()
                                    : nullptr;
  const auto* identity_value_reset =
      code.size() == 4 ? dynamic_cast<IR_ValueReset*>(code.at(0).get()) : nullptr;
  const auto* identity_move =
      code.size() == 4 ? dynamic_cast<IR_RegSet*>(code.at(1).get()) : nullptr;
  const auto* identity_return =
      code.size() == 4 ? dynamic_cast<IR_Return*>(code.at(2).get()) : nullptr;
  const auto* identity_argument =
      identity_value_reset && identity_value_reset->args().size() == 1
          ? identity_value_reset->args().front()
          : nullptr;
  const auto* glst_node_name_value_reset =
      code.size() == 5 ? dynamic_cast<IR_ValueReset*>(code.at(0).get()) : nullptr;
  const auto* glst_node_name_parameter_move =
      code.size() == 5 ? dynamic_cast<IR_RegSet*>(code.at(1).get()) : nullptr;
  const auto* glst_node_name_load =
      code.size() == 5 ? dynamic_cast<IR_LoadConstOffset*>(code.at(2).get()) : nullptr;
  const auto* glst_node_name_return =
      code.size() == 5 ? dynamic_cast<IR_Return*>(code.at(3).get()) : nullptr;
  const auto* glst_node_name_argument =
      glst_node_name_value_reset && glst_node_name_value_reset->args().size() == 1
          ? glst_node_name_value_reset->args().front()
          : nullptr;
  const auto& register_info = get_register_info(m_gen.instr_set());
  const auto first_argument_register = register_info.get_gpr_arg_reg(0);
  const auto return_register = register_info.get_gpr_ret_reg();
  const bool lognot_uses_apple_abi =
      lognot_argument && lognot_parameter_move && lognot_value_move && lognot_operation &&
      lognot_return && is_allocated_to(lognot_argument, 0, first_argument_register) &&
      is_allocated_to(lognot_parameter_move->source(), 1, first_argument_register) &&
      is_allocated_to(lognot_parameter_move->destination(), 1, return_register) &&
      is_allocated_to(lognot_value_move->source(), 2, return_register) &&
      is_allocated_to(lognot_value_move->destination(), 2, return_register) &&
      is_allocated_to(lognot_operation->destination(), 3, return_register) &&
      is_allocated_to(lognot_return->value(), 4, return_register);
  const bool supported_lognot_function =
      env->name() == "lognot" && lognot_argument && lognot_parameter_move && lognot_value_move &&
      lognot_operation && lognot_return && lognot_argument->type() == TypeSpec("int") &&
      lognot_argument->ireg().reg_class == RegClass::GPR_64 &&
      lognot_parameter_move->source() == lognot_argument &&
      lognot_parameter_move->destination()->ireg().reg_class == RegClass::GPR_64 &&
      lognot_value_move->source() == lognot_parameter_move->destination() &&
      lognot_value_move->destination()->ireg().reg_class == RegClass::GPR_64 &&
      lognot_operation->get_kind() == IntegerMathKind::NOT_64 &&
      lognot_operation->destination() == lognot_value_move->destination() &&
      lognot_operation->argument() == nullptr &&
      lognot_return->value() == lognot_operation->destination() && lognot_uses_apple_abi &&
      dynamic_cast<IR_Null*>(code.at(5).get());
  const bool identity_argument_uses_apple_abi =
      identity_argument && is_allocated_to(identity_argument, 0, first_argument_register) &&
      identity_move && is_allocated_to(identity_move->source(), 1, first_argument_register) &&
      is_allocated_to(identity_move->destination(), 1, return_register);
  const bool supported_identity_function =
      env->name() == "identity" && identity_argument && identity_move && identity_return &&
      identity_argument->type() == TypeSpec("object") &&
      identity_argument->ireg().reg_class == RegClass::GPR_64 &&
      identity_move->source() == identity_argument &&
      identity_move->destination()->ireg().reg_class == RegClass::GPR_64 &&
      identity_return->value() == identity_move->destination() &&
      identity_argument_uses_apple_abi &&
      dynamic_cast<IR_Null*>(code.at(3).get());
  const bool glst_node_name_uses_apple_abi =
      glst_node_name_argument && glst_node_name_parameter_move && glst_node_name_load &&
      glst_node_name_return &&
      is_allocated_to(glst_node_name_argument, 0, first_argument_register) &&
      is_allocated_to(glst_node_name_parameter_move->source(), 1, first_argument_register) &&
      is_allocated_to(glst_node_name_parameter_move->destination(), 1, return_register) &&
      is_allocated_to(glst_node_name_load->base(), 2, return_register) &&
      is_allocated_to(glst_node_name_load->destination(), 2, return_register) &&
      is_allocated_to(glst_node_name_return->value(), 3, return_register);
  const bool supported_glst_node_name_function =
      env->name() == "glst-node-name" && glst_node_name_argument && glst_node_name_parameter_move &&
      glst_node_name_load && glst_node_name_return &&
      glst_node_name_argument->type() == TypeSpec("glst-named-node") &&
      glst_node_name_argument->ireg().reg_class == RegClass::GPR_64 &&
      glst_node_name_parameter_move->source() == glst_node_name_argument &&
      glst_node_name_parameter_move->destination()->type() == TypeSpec("glst-named-node") &&
      glst_node_name_parameter_move->destination()->ireg().reg_class == RegClass::GPR_64 &&
      glst_node_name_load->base() == glst_node_name_parameter_move->destination() &&
      glst_node_name_load->destination()->type() == TypeSpec("string") &&
      glst_node_name_load->destination()->ireg().reg_class == RegClass::GPR_64 &&
      glst_node_name_load->offset() == 8 && glst_node_name_load->info().reg == RegClass::GPR_64 &&
      glst_node_name_load->info().size == 4 && !glst_node_name_load->info().sign_extend &&
      glst_node_name_return->value() == glst_node_name_load->destination() &&
      glst_node_name_uses_apple_abi && dynamic_cast<IR_Null*>(code.at(4).get());
  const auto* level_group_load_commands_value_reset =
      code.size() == 6 ? dynamic_cast<IR_ValueReset*>(code.at(0).get()) : nullptr;
  const auto* level_group_load_commands_this_move =
      code.size() == 6 ? dynamic_cast<IR_RegSet*>(code.at(1).get()) : nullptr;
  const auto* level_group_load_commands_value_move =
      code.size() == 6 ? dynamic_cast<IR_RegSet*>(code.at(2).get()) : nullptr;
  const auto* level_group_load_commands_store =
      code.size() == 6 ? dynamic_cast<IR_StoreConstOffset*>(code.at(3).get()) : nullptr;
  const auto* level_group_load_commands_return =
      code.size() == 6 ? dynamic_cast<IR_Return*>(code.at(4).get()) : nullptr;
  const auto* level_group_load_commands_this_argument =
      level_group_load_commands_value_reset &&
              level_group_load_commands_value_reset->args().size() == 2
          ? level_group_load_commands_value_reset->args().at(0)
          : nullptr;
  const auto* level_group_load_commands_value_argument =
      level_group_load_commands_value_reset &&
              level_group_load_commands_value_reset->args().size() == 2
          ? level_group_load_commands_value_reset->args().at(1)
          : nullptr;
  const auto second_argument_register = register_info.get_gpr_arg_reg(1);
  const bool level_group_load_commands_uses_apple_abi =
      level_group_load_commands_this_argument && level_group_load_commands_value_argument &&
      level_group_load_commands_this_move && level_group_load_commands_value_move &&
      level_group_load_commands_store && level_group_load_commands_return &&
      is_allocated_to(level_group_load_commands_this_argument, 0, first_argument_register) &&
      is_allocated_to(level_group_load_commands_value_argument, 0, second_argument_register) &&
      is_allocated_to(level_group_load_commands_this_move->source(), 1, first_argument_register) &&
      is_allocated_to(level_group_load_commands_this_move->destination(), 1,
                      first_argument_register) &&
      is_allocated_to(level_group_load_commands_value_move->source(), 2,
                      second_argument_register) &&
      is_allocated_to(level_group_load_commands_value_move->destination(), 2,
                      second_argument_register) &&
      is_allocated_to(level_group_load_commands_store->base(), 3, first_argument_register) &&
      is_allocated_to(level_group_load_commands_store->value(), 3, second_argument_register) &&
      is_allocated_to(level_group_load_commands_return->value(), 4, second_argument_register);
  const bool supported_level_group_load_commands_set_function =
      env->name() == "level-group-load-commands-set!" && level_group_load_commands_this_argument &&
      level_group_load_commands_value_argument && level_group_load_commands_this_move &&
      level_group_load_commands_value_move && level_group_load_commands_store &&
      level_group_load_commands_return &&
      level_group_load_commands_this_argument->type() == TypeSpec("level-group") &&
      level_group_load_commands_this_argument->ireg().reg_class == RegClass::GPR_64 &&
      level_group_load_commands_value_argument->type() == TypeSpec("pair") &&
      level_group_load_commands_value_argument->ireg().reg_class == RegClass::GPR_64 &&
      level_group_load_commands_this_move->source() == level_group_load_commands_this_argument &&
      level_group_load_commands_this_move->destination()->type() == TypeSpec("level-group") &&
      level_group_load_commands_this_move->destination()->ireg().reg_class == RegClass::GPR_64 &&
      level_group_load_commands_value_move->source() == level_group_load_commands_value_argument &&
      level_group_load_commands_value_move->destination()->type() == TypeSpec("pair") &&
      level_group_load_commands_value_move->destination()->ireg().reg_class == RegClass::GPR_64 &&
      level_group_load_commands_store->base() ==
          level_group_load_commands_this_move->destination() &&
      level_group_load_commands_store->value() ==
          level_group_load_commands_value_move->destination() &&
      level_group_load_commands_store->offset() == 0x20 &&
      level_group_load_commands_store->size() == 4 &&
      level_group_load_commands_return->value() ==
          level_group_load_commands_value_move->destination() &&
      level_group_load_commands_uses_apple_abi && dynamic_cast<IR_Null*>(code.at(5).get());
  const auto* want_vis_value_reset =
      code.size() == 7 ? dynamic_cast<IR_ValueReset*>(code.at(0).get()) : nullptr;
  const auto* want_vis_this_move =
      code.size() == 7 ? dynamic_cast<IR_RegSet*>(code.at(1).get()) : nullptr;
  const auto* want_vis_value_move =
      code.size() == 7 ? dynamic_cast<IR_RegSet*>(code.at(2).get()) : nullptr;
  const auto* want_vis_store =
      code.size() == 7 ? dynamic_cast<IR_StoreConstOffset*>(code.at(3).get()) : nullptr;
  const auto* want_vis_zero =
      code.size() == 7 ? dynamic_cast<IR_LoadConstant64*>(code.at(4).get()) : nullptr;
  const auto* want_vis_return =
      code.size() == 7 ? dynamic_cast<IR_Return*>(code.at(5).get()) : nullptr;
  const auto* want_vis_this_argument =
      want_vis_value_reset && want_vis_value_reset->args().size() == 2
          ? want_vis_value_reset->args().at(0)
          : nullptr;
  const auto* want_vis_value_argument =
      want_vis_value_reset && want_vis_value_reset->args().size() == 2
          ? want_vis_value_reset->args().at(1)
          : nullptr;
  const bool want_vis_uses_apple_abi =
      want_vis_this_argument && want_vis_value_argument && want_vis_this_move &&
      want_vis_value_move && want_vis_store && want_vis_zero && want_vis_return &&
      is_allocated_to(want_vis_this_argument, 0, first_argument_register) &&
      is_allocated_to(want_vis_value_argument, 0, second_argument_register) &&
      is_allocated_to(want_vis_this_move->source(), 1, first_argument_register) &&
      is_allocated_to(want_vis_this_move->destination(), 1, first_argument_register) &&
      is_allocated_to(want_vis_value_move->source(), 2, second_argument_register) &&
      is_allocated_to(want_vis_value_move->destination(), 2, second_argument_register) &&
      is_allocated_to(want_vis_store->base(), 3, first_argument_register) &&
      is_allocated_to(want_vis_store->value(), 3, second_argument_register) &&
      is_allocated_to(want_vis_zero->destination(), 4, return_register) &&
      is_allocated_to(want_vis_return->value(), 5, return_register);
  const bool supported_want_vis_function =
      env->name() == "want-vis" && want_vis_this_argument && want_vis_value_argument &&
      want_vis_this_move && want_vis_value_move && want_vis_store && want_vis_zero &&
      want_vis_return && want_vis_this_argument->type() == TypeSpec("load-state") &&
      want_vis_this_argument->ireg().reg_class == RegClass::GPR_64 &&
      want_vis_value_argument->type() == TypeSpec("symbol") &&
      want_vis_value_argument->ireg().reg_class == RegClass::GPR_64 &&
      want_vis_this_move->source() == want_vis_this_argument &&
      want_vis_this_move->destination()->type() == TypeSpec("load-state") &&
      want_vis_this_move->destination()->ireg().reg_class == RegClass::GPR_64 &&
      want_vis_value_move->source() == want_vis_value_argument &&
      want_vis_value_move->destination()->type() == TypeSpec("symbol") &&
      want_vis_value_move->destination()->ireg().reg_class == RegClass::GPR_64 &&
      want_vis_store->base() == want_vis_this_move->destination() &&
      want_vis_store->value() == want_vis_value_move->destination() &&
      want_vis_store->offset() == 0x20 && want_vis_store->size() == 4 &&
      want_vis_zero->value() == 0 && want_vis_return->value() == want_vis_zero->destination() &&
      want_vis_uses_apple_abi && dynamic_cast<IR_Null*>(code.at(6).get());
  if (!supported_top_level && !supported_false_function && !supported_true_function &&
      !supported_lognot_function && !supported_identity_function &&
      !supported_glst_node_name_function && !supported_level_group_load_commands_set_function &&
      !supported_want_vis_function) {
    throw std::runtime_error(
        "ARM64 AOT proof only supports top-level literal 42, top-level #f, or the zero-argument "
        "Jak 1 false or true function, one-argument identity function, or one-argument int lognot "
        "function, the one-argument Jak 1 glst-node-name function, or the direct two-argument "
        "Jak 1 level-group load-commands-set! body, or the direct two-argument Jak 1 want-vis "
        "body.");
  }

  auto* debug = &m_debug_info->function_by_name(env->name());
  debug->stack_usage = 0;
  const auto f_rec = m_gen.get_existing_function_record(f_idx);
  for (int ir_idx = 0; ir_idx < int(code.size()); ir_idx++) {
    const auto i_rec = m_gen.add_ir(f_rec);
    code.at(ir_idx)->do_codegen_arm64(&m_gen, allocs, i_rec);
  }
  m_gen.add_instr_no_ir(f_rec, IGen::ret(m_gen), InstructionInfo::Kind::EPILOGUE);
}

void CodeGenerator::do_asm_function_x86(FunctionEnv* env, int f_idx, bool allow_saved_regs) {
  auto f_rec = m_gen.get_existing_function_record(f_idx);
  const auto& allocs = env->alloc_result();

  if (!allow_saved_regs && !allocs.used_saved_regs.empty()) {
    std::string err = fmt::format(
        "ASM Function {}'s coloring using the following callee-saved registers: ", env->name());
    for (auto& x : allocs.used_saved_regs) {
      err += x.print();
      err += " ";
    }
    err.pop_back();
    err.push_back('.');
    throw std::runtime_error(err);
  }

  if (allocs.stack_slots_for_spills) {
    throw std::runtime_error("ASM Function has used the stack for spills.");
  }

  if (allocs.stack_slots_for_vars) {
    throw std::runtime_error("ASM Function has variables on the stack.");
  }

  // emit each IR into x86 instructions.
  for (int ir_idx = 0; ir_idx < int(env->code().size()); ir_idx++) {
    auto& ir = env->code().at(ir_idx);
    // start of IR
    auto i_rec = m_gen.add_ir(f_rec);

    // Make sure we aren't automatically accessing the stack.
    if (!allocs.stack_ops.at(ir_idx).ops.empty()) {
      throw std::runtime_error("ASM Function used a bonus op.");
    }

    // do the actual op
    ir->do_codegen_x86(&m_gen, allocs, i_rec);
  }
}

void CodeGenerator::do_asm_function_arm64(FunctionEnv* env, int f_idx, bool allow_saved_regs) {
  throw std::runtime_error("NYI - CodeGenerator::do_asm_function");
}
