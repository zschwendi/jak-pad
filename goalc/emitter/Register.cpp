#include "Register.h"

#include <algorithm>
#include <stdexcept>

namespace emitter {
RegisterInfo RegisterInfo::make_register_info(InstructionSet instruction_set) {
  RegisterInfo info;
  info.m_instruction_set = instruction_set;

  if (instruction_set == InstructionSet::ARM64) {
    for (int i = X0; i <= X30; i++) {
      info.m_info.at(i) = {false, false, "x" + std::to_string(i)};
    }
    info.m_info.at(SP) = {false, true, "sp"};

    for (int i = V0; i <= V31; i++) {
      info.m_info.at(i) = {false, false, "v" + std::to_string(i - V0)};
    }

    for (const auto reg : {X16, X17, X18}) {
      info.m_info.at(reg).special = true;
    }
    for (const auto reg : {X20, X21, X22, X29, X30}) {
      info.m_info.at(reg).saved = true;
      info.m_info.at(reg).special = true;
    }
    for (const auto reg : {X19, X23, X24, X25, X26, X27, X28}) {
      info.m_info.at(reg).saved = true;
    }
    for (const auto reg : {V8, V9, V10, V11, V12, V13, V14, V15}) {
      info.m_info.at(reg).saved = true;
    }

    info.m_gpr_arg_regs = {X0, X1, X2, X3, X4, X5, X6, X7};
    info.m_xmm_arg_regs = {V0, V1, V2, V3, V4, V5, V6, V7};
    info.m_saved_gprs = {X19, X23, X24, X25, X26, X27, X28};
    info.m_saved_xmms = {V8, V9, V10, V11, V12, V13, V14, V15};
    info.m_saved_all = info.m_saved_gprs;
    info.m_saved_all.insert(info.m_saved_all.end(), info.m_saved_xmms.begin(),
                            info.m_saved_xmms.end());

    info.m_process_reg = X20;
    info.m_st_reg = X21;
    info.m_offset_reg = X22;
    info.m_gpr_ret_reg = X0;
    info.m_xmm_ret_reg = V0;

    info.m_gpr_temp_only_alloc_order = {X0, X1, X2, X3, X4, X5, X6, X7,
                                         X8, X9, X10, X11, X12, X13, X14, X15};
    info.m_gpr_alloc_order = info.m_gpr_temp_only_alloc_order;
    info.m_gpr_alloc_order.insert(info.m_gpr_alloc_order.end(), info.m_saved_gprs.begin(),
                                  info.m_saved_gprs.end());
    info.m_gpr_spill_temp_alloc_order = info.m_gpr_alloc_order;

    info.m_xmm_temp_only_alloc_order = {V0, V1, V2, V3, V4, V5, V6, V7,
                                         V16, V17, V18, V19, V20, V21, V22, V23,
                                         V24, V25, V26, V27, V28, V29, V30, V31};
    info.m_xmm_alloc_order = info.m_xmm_temp_only_alloc_order;
    info.m_xmm_alloc_order.insert(info.m_xmm_alloc_order.end(), info.m_saved_xmms.begin(),
                                  info.m_saved_xmms.end());
    info.m_xmm_spill_temp_alloc_order = info.m_xmm_alloc_order;

    info.m_gpr_v2_temp_only_alloc_order = info.m_gpr_temp_only_alloc_order;
    info.m_xmm_v2_temp_only_alloc_order = info.m_xmm_temp_only_alloc_order;
    info.m_gpr_v2_temp_first_alloc_order = info.m_gpr_alloc_order;
    info.m_xmm_v2_temp_first_alloc_order = info.m_xmm_alloc_order;
    info.m_gpr_v2_saved_first_alloc_order = info.m_saved_gprs;
    info.m_gpr_v2_saved_first_alloc_order.insert(
        info.m_gpr_v2_saved_first_alloc_order.end(), info.m_gpr_temp_only_alloc_order.begin(),
        info.m_gpr_temp_only_alloc_order.end());
    info.m_xmm_v2_saved_first_alloc_order = info.m_saved_xmms;
    info.m_xmm_v2_saved_first_alloc_order.insert(
        info.m_xmm_v2_saved_first_alloc_order.end(), info.m_xmm_temp_only_alloc_order.begin(),
        info.m_xmm_temp_only_alloc_order.end());
    info.m_gpr_v2_torture_alloc_order = {X9, X10, X11, X12, X13, X14};
    info.m_xmm_v2_torture_alloc_order = {V0, V1, V2, V3, V4, V5, V6, V7, V16, V17};
    return info;
  }

  info.m_info[RAX] = {false, false, "rax"};  // return, temp
  info.m_info[RCX] = {false, false, "rcx"};  // gpr arg 3, temp
  info.m_info[RDX] = {false, false, "rdx"};  // gpr arg 2, temp
  info.m_info[RBX] = {true, false, "rbx"};   // saved
  info.m_info[RSP] = {false, true, "rsp"};   // stack pointer
  info.m_info[RBP] = {true, false, "rbp"};   // saved
  info.m_info[RSI] = {false, false, "rsi"};  // gpr arg 1, temp
  info.m_info[RDI] = {false, false, "rdi"};  // gpr arg 0, temp

  info.m_info[R8] = {false, false, "r8"};   // gpr arg 4, temp
  info.m_info[R9] = {false, false, "r9"};   // gpr arg 5, temp
  info.m_info[R10] = {true, false, "r10"};  // gpr arg 6, saved
  info.m_info[R11] = {true, false, "r11"};  // gpr arg 7, saved
  info.m_info[R12] = {true, false, "r12"};  // saved
  info.m_info[R13] = {false, true, "r13"};  // pp
  info.m_info[R14] = {false, true, "r14"};  // st
  info.m_info[R15] = {false, true, "r15"};  // offset.

  info.m_info[XMM0] = {false, false, "xmm0"};
  info.m_info[XMM1] = {false, false, "xmm1"};
  info.m_info[XMM2] = {false, false, "xmm2"};
  info.m_info[XMM3] = {false, false, "xmm3"};
  info.m_info[XMM4] = {false, false, "xmm4"};
  info.m_info[XMM5] = {false, false, "xmm5"};
  info.m_info[XMM6] = {false, false, "xmm6"};
  info.m_info[XMM7] = {false, false, "xmm7"};
  info.m_info[XMM8] = {true, false, "xmm8"};
  info.m_info[XMM9] = {true, false, "xmm9"};
  info.m_info[XMM10] = {true, false, "xmm10"};
  info.m_info[XMM11] = {true, false, "xmm11"};
  info.m_info[XMM12] = {true, false, "xmm12"};
  info.m_info[XMM13] = {true, false, "xmm13"};
  info.m_info[XMM14] = {true, false, "xmm14"};
  info.m_info[XMM15] = {true, false, "xmm15"};

  info.m_gpr_arg_regs = {RDI, RSI, RDX, RCX, R8, R9, R10, R11};
  // skip xmm0 so it can be used for return.
  info.m_xmm_arg_regs = {XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7, XMM8};
  info.m_saved_gprs = {RBX, RBP, R10, R11, R12};
  info.m_saved_xmms = {XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15};
  info.m_saved_all = info.m_saved_gprs;
  info.m_saved_all.insert(info.m_saved_all.end(), info.m_saved_xmms.begin(),
                          info.m_saved_xmms.end());

  info.m_process_reg = R13;
  info.m_st_reg = R14;
  info.m_offset_reg = R15;
  info.m_gpr_ret_reg = RAX;
  info.m_xmm_ret_reg = XMM0;

  // todo - experiment with better orders for allocation.
  info.m_gpr_alloc_order = {RAX, RCX, RDX, RBX, RBP, RSI, RDI, R8, R9, R10};  // arbitrary
  info.m_xmm_alloc_order = {XMM0, XMM1, XMM2, XMM3,  XMM4,  XMM5,  XMM6,
                            XMM7, XMM8, XMM9, XMM10, XMM11, XMM12, XMM13};

  // these should only be temp registers!
  info.m_gpr_temp_only_alloc_order = {RAX, RCX, RDX, RSI, RDI, R8, R9};
  info.m_xmm_temp_only_alloc_order = {XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7};

  info.m_gpr_spill_temp_alloc_order = {RAX, RCX, RDX, RBX, RBP, RSI,
                                       RDI, R8,  R9,  R10, R11, R12};  // arbitrary
  info.m_xmm_spill_temp_alloc_order = {XMM0, XMM1, XMM2,  XMM3,  XMM4,  XMM5,  XMM6,  XMM7,
                                       XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15};

  info.m_xmm_v2_saved_first_alloc_order = {XMM8, XMM9, XMM10, XMM11, XMM12, XMM13,
                                            XMM14, XMM15, XMM7,  XMM6,  XMM5,  XMM4,
                                            XMM3, XMM2, XMM1,  XMM0};
  info.m_gpr_v2_saved_first_alloc_order = {RBX, RBP, R12, R11, R10, R9,
                                            R8,  RCX, RDX, RSI, RDI, RAX};
  info.m_xmm_v2_temp_first_alloc_order = {XMM7, XMM6, XMM5,  XMM4,  XMM3,  XMM2,
                                           XMM1, XMM0, XMM8,  XMM9,  XMM10, XMM11,
                                           XMM12, XMM13, XMM14, XMM15};
  info.m_gpr_v2_temp_first_alloc_order = {R9, R8, RCX, RDX, RSI, RDI,
                                           RAX, RBX, RBP, R12, R11, R10};
  info.m_xmm_v2_torture_alloc_order = {XMM7, XMM6, XMM5, XMM4, XMM3,
                                        XMM2, XMM1, XMM0, XMM8, XMM9};
  info.m_gpr_v2_torture_alloc_order = {R9, RSI, RDI, RAX, RBP, R12};
  info.m_xmm_v2_temp_only_alloc_order = {XMM7, XMM6, XMM5, XMM4,
                                          XMM3, XMM2, XMM1, XMM0};
  info.m_gpr_v2_temp_only_alloc_order = {R9, R8, RCX, RDX, RSI, RDI, RAX};
  return info;
}

RegisterInfo gRegInfo = RegisterInfo::make_register_info(InstructionSet::X86);
RegisterInfo gArm64RegInfo = RegisterInfo::make_register_info(InstructionSet::ARM64);

const RegisterInfo& get_register_info(InstructionSet instruction_set) {
  switch (instruction_set) {
    case InstructionSet::X86:
      return gRegInfo;
    case InstructionSet::ARM64:
      return gArm64RegInfo;
    default:
      throw std::runtime_error("Unsupported instruction set");
  }
}

const std::vector<Register>& RegisterInfo::get_v2_alloc_order(HWRegKind kind,
                                                               bool saved_first,
                                                               bool asm_function,
                                                               bool torture_spills) const {
  if (kind == HWRegKind::GPR) {
    if (asm_function) {
      return m_gpr_v2_temp_only_alloc_order;
    }
    if (torture_spills) {
      return m_gpr_v2_torture_alloc_order;
    }
    return saved_first ? m_gpr_v2_saved_first_alloc_order : m_gpr_v2_temp_first_alloc_order;
  }
  if (kind == HWRegKind::XMM) {
    if (asm_function) {
      return m_xmm_v2_temp_only_alloc_order;
    }
    if (torture_spills) {
      return m_xmm_v2_torture_alloc_order;
    }
    return saved_first ? m_xmm_v2_saved_first_alloc_order : m_xmm_v2_temp_first_alloc_order;
  }
  throw std::runtime_error("Unsupported register kind");
}

bool RegisterInfo::is_allocatable(HWRegKind kind, Register reg) const {
  const auto& order = get_v2_alloc_order(kind, false, false, false);
  return std::find(order.begin(), order.end(), reg) != order.end();
}

std::string to_string(HWRegKind kind) {
  switch (kind) {
    case HWRegKind::GPR:
      return "gpr";
    case HWRegKind::XMM:
      return "xmm";
    default:
      throw std::runtime_error("Unsupported HWRegKind");
  }
}

HWRegKind reg_class_to_hw(RegClass reg_class) {
  switch (reg_class) {
    case RegClass::VECTOR_FLOAT:
    case RegClass::FLOAT:
    case RegClass::INT_128:
      return HWRegKind::XMM;
    case RegClass::GPR_64:
      return HWRegKind::GPR;
    default:
      ASSERT(false);
      return HWRegKind::INVALID;
  }
}

std::string Register::print() const {
  return print(m_instr_set);
}

std::string Register::print(InstructionSet instr_set) const {
  return get_register_info(instr_set).get_info(*this).name;
}

}  // namespace emitter
