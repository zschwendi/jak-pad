#include "CBackend.h"

#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

#include "common/goal_constants.h"
#include "common/symbols.h"
#include "common/type_system/TypeSystem.h"

#include "goalc/compiler/Env.h"
#include "goalc/compiler/IR.h"
#include "goalc/emitter/Register.h"

#include "fmt/format.h"

namespace aot {

int CBackendResult::emitted_count() const {
  int count = 0;
  for (const auto& f : functions) {
    if (f.ok) {
      count++;
    }
  }
  return count;
}

int CBackendResult::total_count() const {
  return int(functions.size());
}

int CBackendResult::native_count() const {
  int count = 0;
  for (const auto& f : functions) {
    if (!f.ok && !f.native_symbol.empty()) {
      count++;
    }
  }
  return count;
}

namespace {

/*!
 * GOAL functions that manipulate the machine stack directly and so cannot be lowered to C, paired
 * with the native implementation the runtime provides for each
 * (game/kernel/core/goal_native_kernel.cpp).
 *
 * The backend still refuses to lower these: the guards that reject an rlet assigning to rsp stay
 * exactly as they are, because a lowering that wrote to a local instead would compile and be
 * silently wrong. This table only says where the real code is, so the emitted function table can
 * point at it and the loader can build a normal GOAL function object for it. That is what makes
 * the symbol definitions, the method tables and the static relocations all resolve to something
 * callable without any of them having to know this happened.
 *
 * The file tag is matched too, so a function elsewhere in the game that happens to share a name
 * with one of these cannot silently pick up the wrong implementation.
 *
 * These implementations read the thread and process fields through a per-game seam
 * (game/kernel/core/goal_native_kernel_game.h), so the table applies to the games that seam has
 * been written for: Jak 1 and Jak 2. Jak 3 counts the same functions as failures until its kernel
 * is actually ported.
 */
struct NativeImplementation {
  const char* file_tag;
  const char* goal_name;
  const char* c_symbol;
};

constexpr NativeImplementation kNativeImplementations[] = {
    {"gkernel", "return-from-thread", "goal_native_return_from_thread"},
    {"gkernel", "reset-and-call", "goal_native_reset_and_call"},
    {"gkernel", "(method thread-suspend cpu-thread)", "goal_native_thread_suspend"},
    {"gkernel", "(method thread-resume cpu-thread)", "goal_native_thread_resume"},
    {"gkernel", "(method new catch-frame)", "goal_native_catch_frame_new"},
    {"gkernel", "throw-dispatch", "goal_native_throw_dispatch"},
    {"gstate", "enter-state-run-code", "goal_native_enter_state_run_code"},
};

const char* native_implementation_for(GameVersion version,
                                      const std::string& file_tag,
                                      const std::string& goal_name) {
  if (version != GameVersion::Jak1 && version != GameVersion::Jak2) {
    return nullptr;
  }
  for (const auto& entry : kNativeImplementations) {
    if (file_tag == entry.file_tag && goal_name == entry.goal_name) {
      return entry.c_symbol;
    }
  }
  return nullptr;
}

std::string mangle(const std::string& name) {
  std::string out;
  for (char c : name) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out.push_back(c);
      continue;
    }
    switch (c) {
      case '+':
        out += "_plus";
        break;
      case '-':
        out += "_";
        break;
      case '*':
        out += "_star";
        break;
      case '/':
        out += "_slash";
        break;
      case '!':
        out += "_bang";
        break;
      case '?':
        out += "_p";
        break;
      case '<':
        out += "_lt";
        break;
      case '>':
        out += "_gt";
        break;
      case '=':
        out += "_eq";
        break;
      case '.':
        out += "_dot";
        break;
      case '%':
        out += "_pct";
        break;
      case '&':
        out += "_amp";
        break;
      case '~':
        out += "_tilde";
        break;
      default:
        out += "_";
        break;
    }
  }
  return out;
}

const char* c_type_for(RegClass rc) {
  switch (rc) {
    case RegClass::GPR_64:
      return "uint64_t";
    case RegClass::FLOAT:
      return "float";
    case RegClass::VECTOR_FLOAT:
      return "goal_vf";
    case RegClass::INT_128:
      return "goal_vi";
    default:
      throw std::runtime_error("unsupported register class");
  }
}

bool is_128(RegClass rc) {
  return rc == RegClass::VECTOR_FLOAT || rc == RegClass::INT_128;
}

/*!
 * Emits the body of one GOAL function as C. Shared per-file state (symbol references) lives in
 * FileEmitter.
 */
class FileEmitter {
 public:
  FileEmitter(FileEnv& file, std::string tag, GameVersion version, const TypeSystem& types)
      : m_file(file), m_tag(std::move(tag)), m_version(version), m_types(types) {
    const auto& statics = m_file.statics();
    for (size_t i = 0; i < statics.size(); i++) {
      m_static_index[statics.at(i).get()] = int(i);
    }
  }

  CBackendResult run();

 private:
  std::string emit_function(const FunctionEnv& func,
                            const std::string& c_name,
                            std::string* prototype);
  std::string emit_instruction(const FunctionEnv& func, IR* ir);
  void find_machine_state_regs(const FunctionEnv& func);
  bool is_machine_state(const RegVal* rv) const;
  void check_stack_pointer_use(const FunctionEnv& func) const;
  std::string rlet_reset_expr(const RegVal* rv);
  int symbol_index(const std::string& name);
  std::string symbol_pointer_expr(const std::string& name);
  std::string reg(const RegVal* rv) const;
  std::string move(const RegVal* dst, const RegVal* src);
  std::string call(const IR_FunctionCall* ir);
  std::string static_addr_expr(const StaticObject* obj);
  std::string emit_statics();

  FileEnv& m_file;
  std::string m_tag;
  GameVersion m_version;
  const TypeSystem& m_types;
  std::vector<std::string> m_symbols;
  std::map<std::string, int> m_symbol_index;
  std::map<const StaticObject*, int> m_static_index;
  const IR* m_argument_reset = nullptr;
  std::map<int, std::string> m_machine_state_regs;
  std::set<int> m_stack_pointer_regs;

  struct StaticSize {
    int align;
    int size;
    int addr_offset;
    int reloc_count;
  };
  std::vector<StaticSize> m_static_sizes;
};

/*!
 * Reproduces what StaticObject::generate() would have written into an object file, but as
 * read-only C arrays plus a relocation list. This is the data half of replacing the runtime
 * linker: the loader copies these into GOAL memory and applies the relocations once.
 */
std::string FileEmitter::emit_statics() {
  const auto& statics = m_file.statics();
  std::string out;

  for (size_t i = 0; i < statics.size(); i++) {
    // A pair's two words and its link records only exist after StaticPair::generate() has run, and
    // that is the object-file path, not this one. Build them so the StaticStructure branch below
    // sees a real 8-byte static: without this a pair is zero bytes, takes up no room in the
    // loader's segment, and every reference to one resolves to whatever static follows it.
    if (auto* pair = dynamic_cast<StaticPair*>(statics.at(i).get())) {
      pair->build_data();
    }
    const auto* obj = statics.at(i).get();
    std::vector<u8> data;
    std::vector<std::string> relocs;
    int align = 16;

    const auto add_reloc = [&](const char* kind, int offset, const std::string& name,
                               int target_index, int target_offset) {
      // a type reference also needs the method count, which is what intern-type takes
      const int method_count = name.empty() ? 0
                                            : (std::string(kind) == "GOAL_RELOC_TYPE_PTR"
                                                   ? int(m_types.get_type_method_count(name))
                                                   : 0);
      relocs.push_back(fmt::format("    {{{}, {}, {}, {}, {}, {}}},", kind, offset,
                                   name.empty() ? "0" : fmt::format("\"{}\"", name), target_index,
                                   target_offset, method_count));
    };

    if (const auto* f = dynamic_cast<const StaticFloat*>(obj)) {
      align = 4;
      data.resize(4);
      memcpy(data.data(), &f->value, 4);
    } else if (const auto* str = dynamic_cast<const StaticString*>(obj)) {
      // StaticString::generate() builds its bytes at object-file time; rebuild them here.
      add_reloc("GOAL_RELOC_TYPE_PTR", 0, "string", 0, 0);
      data.resize(POINTER_SIZE, 0);
      const u32 length = u32(str->text.size());
      for (int b = 0; b < 4; b++) {
        data.push_back(u8(length >> (8 * b)));
      }
      for (char c : str->text) {
        data.push_back(u8(c));
      }
      data.push_back(0);
    } else if (const auto* st = dynamic_cast<const StaticStructure*>(obj)) {
      if (dynamic_cast<const StaticPair*>(obj)) {
        // two 4-byte words. 8 keeps the pair offset of 2 where it belongs and does not pad every
        // pair out to 16 bytes; Jak 1's level data has thousands of them.
        align = 8;
      }
      data = st->data;
      for (const auto& sym : st->symbols) {
        add_reloc("GOAL_RELOC_SYMBOL_PTR", sym.offset, sym.name, 0, 0);
      }
      for (const auto& type : st->types) {
        add_reloc("GOAL_RELOC_TYPE_PTR", type.offset, type.name, 0, 0);
      }
      for (const auto& ptr : st->pointers) {
        auto it = m_static_index.find(ptr.dest);
        if (it == m_static_index.end()) {
          throw std::runtime_error("static pointer to an object outside this file");
        }
        add_reloc("GOAL_RELOC_STATIC_PTR", ptr.offset_in_this, "", it->second, ptr.offset_in_dest);
      }
      for (const auto& func : st->functions) {
        add_reloc("GOAL_RELOC_FUNCTION_PTR", func.offset_in_this, "", func.func->idx_in_file, 0);
      }
    } else {
      throw std::runtime_error("unsupported static object kind");
    }

    out += fmt::format("static const uint8_t goalc_static_{}_data[{}] = {{", i,
                       std::max<size_t>(data.size(), 1));
    for (size_t b = 0; b < data.size(); b++) {
      out += fmt::format("{}{}", b ? "," : "", int(data.at(b)));
    }
    if (data.empty()) {
      out += "0";
    }
    out += "};\n";

    if (!relocs.empty()) {
      out += fmt::format("static const goal_static_reloc goalc_static_{}_relocs[] = {{\n", i);
      for (const auto& r : relocs) {
        out += r + "\n";
      }
      out += "};\n";
    }
    m_static_sizes.push_back({align, int(data.size()), obj->get_addr_offset(), int(relocs.size())});
  }

  out += fmt::format("const goal_static_desc goal_{}_statics[{}] = {{\n", m_tag,
                     std::max<size_t>(statics.size(), 1));
  for (size_t i = 0; i < m_static_sizes.size(); i++) {
    const auto& d = m_static_sizes.at(i);
    out += fmt::format("    {{{}, {}, {}, goalc_static_{}_data, {}, {}}},\n", d.align, d.size,
                       d.addr_offset, i, d.reloc_count,
                       d.reloc_count ? fmt::format("goalc_static_{}_relocs", i) : "0");
  }
  if (m_static_sizes.empty()) {
    out += "    {0, 0, 0, 0, 0, 0},\n";
  }
  out += "};\n";
  out += fmt::format("const int goal_{}_static_count = {};\n\n", m_tag, statics.size());
  return out;
}

/*!
 * Some of the GOAL machine state lives in fixed registers that the allocator never touches: the
 * current process (r13), the symbol table (r14) and the memory base (r15). A variable bound to one
 * of those - by (rlet ((pp :reg r13 ...)) ...) or by the implicit `self` of a behavior - is not
 * storage of its own, it names that machine state. So these get no C local: every read and write
 * of them is rewritten into the runtime location that holds the state, which is what makes a
 * behavior's `self` and a callee's view of the current process the same thing.
 *
 * The register allocator is free to give one ireg to several RegVals, so this is keyed by ireg.
 */
void FileEmitter::find_machine_state_regs(const FunctionEnv& func) {
  m_machine_state_regs.clear();
  m_stack_pointer_regs.clear();
  const auto& info = emitter::get_register_info(emitter::InstructionSet::X86);
  for (const auto& rv : func.reg_vals()) {
    if (!rv->rlet_constraint().has_value()) {
      continue;
    }
    const auto constrained = rv->rlet_constraint().value();
    if (constrained == info.get_process_reg()) {
      m_machine_state_regs[rv->ireg().id] = "g_goal_current_process";
    } else if (constrained == info.get_st_reg()) {
      m_machine_state_regs[rv->ireg().id] = "g_goal_s7";
    } else if (constrained == info.get_offset_reg()) {
      // GOAL pointers are already offsets from the memory base in the C model, so the base is
      // zero and an ordinary local holding it behaves identically. rlet_reset_expr sets it.
      continue;
    } else if (constrained == emitter::RSP) {
      // Reading the stack pointer is expressible in C and is what (suspend) and with-sp do;
      // writing it is not. check_stack_pointer_use rejects the writers.
      //
      // It is machine state for the same reason the process and symbol-table registers are: on
      // x86-64 the binding *is* the register, so every read sees the value at that moment, and
      // GOAL's compiler emits one `:reset-here` per function no matter how many `(suspend)` sites
      // it has. Copying it into a local once would leave every later site reading a stale value -
      // or zero, when the single reset is on a path that did not run - and GOAL's own stack
      // accounting would be quietly wrong. Reading it at each use is exact: a C frame address does
      // not move within a function.
      m_stack_pointer_regs.insert(rv->ireg().id);
      m_machine_state_regs[rv->ireg().id] = "GOAL_STACK_POINTER()";
    } else if (info.get_info(constrained).special) {
      throw std::runtime_error(
          fmt::format("rlet binds machine register {}, which has no C equivalent",
                      constrained.print()));
    }
  }
}

/*!
 * A variable bound to rsp names the machine's stack pointer, so on x86-64 an assignment to it moves
 * the stack. C has no such thing: the compiler owns the stack pointer. Reading it is fine - GOAL
 * only reads it to measure how much of the current thread's stack is in use - so a read-only
 * binding becomes an ordinary local seeded by rlet_reset_expr. Anything that assigns to it, which
 * is gkernel's hand-written thread-switching code, has to fail here rather than silently write to a
 * local and leave the real stack alone. Those functions need a native implementation instead; see
 * docs/aot-stack-model.md.
 */
void FileEmitter::check_stack_pointer_use(const FunctionEnv& func) const {
  if (m_stack_pointer_regs.empty()) {
    return;
  }
  std::set<int> seeded;
  for (const auto& ir : func.code()) {
    const bool is_reset = dynamic_cast<IR_ValueReset*>(ir.get()) != nullptr;
    for (const auto& written : ir->to_rai().write) {
      if (!m_stack_pointer_regs.count(written.id)) {
        continue;
      }
      if (!is_reset) {
        throw std::runtime_error("rlet assigns to rsp, which C cannot express");
      }
      seeded.insert(written.id);
    }
  }
  for (int id : m_stack_pointer_regs) {
    if (!seeded.count(id)) {
      throw std::runtime_error("rlet binds rsp without :reset-here, so its value is undefined");
    }
  }
}

bool FileEmitter::is_machine_state(const RegVal* rv) const {
  return m_machine_state_regs.count(rv->ireg().id) != 0;
}

/*!
 * (rlet ((off :reg r15 :reset-here #t)) ...) binds a variable to whatever a fixed machine register
 * holds. Registers that name machine state directly never reach this; they are rewritten instead.
 */
std::string FileEmitter::rlet_reset_expr(const RegVal* rv) {
  const auto& info = emitter::get_register_info(emitter::InstructionSet::X86);
  const auto constrained = rv->rlet_constraint().value();
  if (constrained == info.get_offset_reg()) {
    return "GOAL_ADDR_OF(g_goal_mem)";
  }
  throw std::runtime_error(fmt::format(
      "rlet :reset-here on machine register {}, which has no C equivalent", constrained.print()));
}

std::string FileEmitter::reg(const RegVal* rv) const {
  auto it = m_machine_state_regs.find(rv->ireg().id);
  if (it != m_machine_state_regs.end()) {
    return it->second;
  }
  return fmt::format("r{}", rv->ireg().id);
}

int FileEmitter::symbol_index(const std::string& name) {
  auto it = m_symbol_index.find(name);
  if (it != m_symbol_index.end()) {
    return it->second;
  }
  const int index = int(m_symbols.size());
  m_symbols.push_back(name);
  m_symbol_index[name] = index;
  return index;
}

std::string FileEmitter::symbol_pointer_expr(const std::string& name) {
  if (name == "#f") {
    static_assert(false_symbol_offset() == 0, "false symbol location");
    return "g_goal_s7";
  }
  if (name == "#t") {
    return fmt::format("(g_goal_s7 + {})", true_symbol_offset(m_version));
  }
  if (name == "_empty_") {
    return fmt::format("(g_goal_s7 + {})", empty_pair_offset_from_s7(m_version));
  }
  return fmt::format("goalc_symbol_ptrs[{}]", symbol_index(name));
}

std::string FileEmitter::static_addr_expr(const StaticObject* obj) {
  auto it = m_static_index.find(obj);
  if (it == m_static_index.end()) {
    throw std::runtime_error("static object is not owned by this file");
  }
  return fmt::format("(goal_static_addr(\"{}\", {}) + {})", m_tag, it->second,
                     obj->get_addr_offset());
}

/*!
 * Reproduces regset_common() from IR.cpp, which is what the x86-64 backend does for a move
 * between two registers of possibly different classes.
 */
std::string FileEmitter::move(const RegVal* dst, const RegVal* src) {
  const auto dc = dst->ireg().reg_class;
  const auto sc = src->ireg().reg_class;
  const std::string d = reg(dst);
  const std::string s = reg(src);

  if (dc == sc) {
    return fmt::format("{} = {};", d, s);
  }
  if (sc == RegClass::FLOAT && dc == RegClass::GPR_64) {
    return fmt::format("{} = (uint64_t)(int64_t)(int32_t)goal_f32_bits({});", d, s);
  }
  if (sc == RegClass::GPR_64 && dc == RegClass::FLOAT) {
    return fmt::format("{} = goal_bits_f32((uint32_t){});", d, s);
  }
  if (is_128(sc) && dc == RegClass::FLOAT) {
    return fmt::format("{} = goal_vf_x((goal_vf){});", d, s);
  }
  if (sc == RegClass::FLOAT && is_128(dc)) {
    // movss between xmm registers only replaces the low element
    return fmt::format("{} = ({})goal_vf_set_x((goal_vf){}, {});", d, c_type_for(dc), d, s);
  }
  if (sc == RegClass::GPR_64 && is_128(dc)) {
    return fmt::format("{} = ({})goal_vf_from_u64({});", d, c_type_for(dc), s);
  }
  if (is_128(sc) && dc == RegClass::GPR_64) {
    return fmt::format("{} = goal_low64((goal_vf){});", d, s);
  }
  if (is_128(sc) && is_128(dc)) {
    return fmt::format("{} = ({}){};", d, c_type_for(dc), s);
  }
  throw std::runtime_error("unsupported register move");
}

std::string FileEmitter::call(const IR_FunctionCall* ir) {
  const auto* ret = ir->ret();
  std::string arg_types;
  std::string arg_values;
  for (size_t i = 0; i < ir->args().size(); i++) {
    const auto* arg = ir->args().at(i);
    if (i) {
      arg_types += ", ";
      arg_values += ", ";
    }
    arg_types += c_type_for(arg->ireg().reg_class);
    arg_values += reg(arg);
  }
  if (ir->args().empty()) {
    arg_types = "void";
  }

  const char* ret_type = c_type_for(ret->ireg().reg_class);
  return fmt::format("{} = (({}(*)({}))GOAL_FN({}))({});", reg(ret), ret_type, arg_types,
                     reg(ir->function()), arg_values);
}

std::string FileEmitter::emit_instruction(const FunctionEnv& func, IR* ir) {
  if (dynamic_cast<IR_Null*>(ir) || dynamic_cast<IR_Nop*>(ir)) {
    return ";";
  }

  if (auto* p = dynamic_cast<IR_ValueReset*>(ir)) {
    std::string out;
    if (p == m_argument_reset) {
      // a behavior's `self` is in this list too, but no caller ever passes it: it names the
      // current process, so it is not a C parameter and does not take up an argument slot.
      int arg_index = 0;
      for (const auto* arg : p->args()) {
        if (is_machine_state(arg)) {
          continue;
        }
        if (arg->rlet_constraint().has_value()) {
          throw std::runtime_error(
              fmt::format("argument bound to machine register {}, which the C calling convention "
                          "cannot express",
                          arg->rlet_constraint().value().print()));
        }
        out += fmt::format("{} = a{}; ", reg(arg), arg_index++);
      }
    } else {
      for (const auto* arg : p->args()) {
        // a machine-state binding already reads the state it names, so there is nothing to copy
        if (arg->rlet_constraint().has_value() && !is_machine_state(arg)) {
          out += fmt::format("{} = {}; ", reg(arg), rlet_reset_expr(arg));
        }
      }
    }
    return out.empty() ? ";" : out;
  }

  if (auto* p = dynamic_cast<IR_Return*>(ir)) {
    return move(p->return_register(), p->value());
  }

  if (auto* p = dynamic_cast<IR_LoadConstant64*>(ir)) {
    if (p->destination()->ireg().reg_class != RegClass::GPR_64) {
      throw std::runtime_error("constant load into a non-GPR");
    }
    return fmt::format("{} = {}ull;", reg(p->destination()), p->value());
  }

  if (auto* p = dynamic_cast<IR_LoadSymbolPointer*>(ir)) {
    const auto* dst = p->destination();
    const auto expr = symbol_pointer_expr(p->name());
    if (dst->ireg().reg_class == RegClass::GPR_64) {
      return fmt::format("{} = {};", reg(dst), expr);
    }
    if (is_128(dst->ireg().reg_class)) {
      return fmt::format("{} = ({})goal_vf_from_u64({});", reg(dst),
                         c_type_for(dst->ireg().reg_class), expr);
    }
    throw std::runtime_error("symbol pointer into an unsupported register class");
  }

  if (auto* p = dynamic_cast<IR_GetSymbolValue*>(ir)) {
    const auto index = symbol_index(p->source()->name());
    if (p->destination()->ireg().reg_class != RegClass::GPR_64) {
      throw std::runtime_error("symbol value load into a non-GPR");
    }
    if (p->sign_extend()) {
      return fmt::format("{} = (uint64_t)(int64_t)*goalc_symbol_slots[{}];", reg(p->destination()),
                         index);
    }
    return fmt::format("{} = (uint64_t)(uint32_t)*goalc_symbol_slots[{}];", reg(p->destination()),
                       index);
  }

  if (auto* p = dynamic_cast<IR_GetSymbolValueAsm*>(ir)) {
    const auto index = symbol_index(p->name());
    if (p->destination()->ireg().reg_class != RegClass::GPR_64) {
      throw std::runtime_error("asm symbol value load into a non-GPR");
    }
    if (p->sign_extend()) {
      return fmt::format("{} = (uint64_t)(int64_t)*goalc_symbol_slots[{}];", reg(p->destination()),
                         index);
    }
    return fmt::format("{} = (uint64_t)(uint32_t)*goalc_symbol_slots[{}];", reg(p->destination()),
                       index);
  }

  if (auto* p = dynamic_cast<IR_SetSymbolValue*>(ir)) {
    const auto index = symbol_index(p->destination()->name());
    if (p->source()->ireg().reg_class != RegClass::GPR_64) {
      throw std::runtime_error("symbol value store from a non-GPR");
    }
    return fmt::format("*goalc_symbol_slots[{}] = (int32_t){};", index, reg(p->source()));
  }

  if (auto* p = dynamic_cast<IR_RegSet*>(ir)) {
    return move(p->destination(), p->source());
  }

  if (auto* p = dynamic_cast<IR_RegSetAsm*>(ir)) {
    return move(p->destination(), p->source());
  }

  if (auto* p = dynamic_cast<IR_FunctionCall*>(ir)) {
    return call(p);
  }

  if (auto* p = dynamic_cast<IR_FunctionAddr*>(ir)) {
    return fmt::format("{} = goal_function_addr(\"{}\", {});", reg(p->destination()), m_tag,
                       p->function()->idx_in_file);
  }

  if (auto* p = dynamic_cast<IR_StaticVarAddr*>(ir)) {
    return fmt::format("{} = {};", reg(p->destination()), static_addr_expr(p->source()));
  }

  if (auto* p = dynamic_cast<IR_StaticVarLoad*>(ir)) {
    const auto* dst = p->destination();
    const auto info = p->source()->get_load_info();
    const auto addr = static_addr_expr(p->source());
    if (dst->ireg().reg_class == RegClass::FLOAT) {
      if (info.load_size != 4 || info.load_signed) {
        throw std::runtime_error("unsupported static float load");
      }
      return fmt::format("{} = *(float*)GOAL_PTR({}, 0);", reg(dst), addr);
    }
    if (is_128(dst->ireg().reg_class)) {
      return fmt::format("{} = ({})goal_load_vf(GOAL_PTR({}, 0));", reg(dst),
                         c_type_for(dst->ireg().reg_class), addr);
    }
    throw std::runtime_error("static var load into an unsupported register class");
  }

  if (auto* p = dynamic_cast<IR_RegValAddr*>(ir)) {
    return fmt::format("{} = GOAL_ADDR_OF(&{});", reg(p->destination()), reg(p->source()));
  }

  if (auto* p = dynamic_cast<IR_GetStackAddr*>(ir)) {
    (void)func;
    return fmt::format("{} = GOAL_ADDR_OF(&goal_stack[{}]);", reg(p->destination()), p->slot() * 8);
  }

  if (auto* p = dynamic_cast<IR_LoadConstOffset*>(ir)) {
    const auto* dst = p->destination();
    const auto& info = p->info();
    const auto ptr = fmt::format("GOAL_PTR({}, {})", reg(p->base()), p->offset());
    if (dst->ireg().reg_class == RegClass::GPR_64 && info.reg == RegClass::GPR_64) {
      return fmt::format("{} = goal_load_{}({}, {});", reg(dst), info.sign_extend ? "s" : "u", ptr,
                         info.size);
    }
    if (dst->ireg().reg_class == RegClass::FLOAT && info.size == 4 && !info.sign_extend) {
      return fmt::format("{} = *(float*){};", reg(dst), ptr);
    }
    if (is_128(dst->ireg().reg_class) && info.size == 16) {
      return fmt::format("{} = ({})goal_load_vf({});", reg(dst), c_type_for(dst->ireg().reg_class),
                         ptr);
    }
    throw std::runtime_error("unsupported constant-offset load");
  }

  if (auto* p = dynamic_cast<IR_StoreConstOffset*>(ir)) {
    const auto* value = p->value();
    const auto ptr = fmt::format("GOAL_PTR({}, {})", reg(p->base()), p->offset());
    if (value->ireg().reg_class == RegClass::GPR_64) {
      return fmt::format("goal_store({}, {}, {});", ptr, reg(value), p->size());
    }
    if (value->ireg().reg_class == RegClass::FLOAT && p->size() == 4) {
      return fmt::format("*(float*){} = {};", ptr, reg(value));
    }
    if (is_128(value->ireg().reg_class) && p->size() == 16) {
      return fmt::format("goal_store_vf({}, (goal_vf){});", ptr, reg(value));
    }
    throw std::runtime_error("unsupported constant-offset store");
  }

  if (auto* p = dynamic_cast<IR_IntegerMath*>(ir)) {
    const std::string d = reg(p->destination());
    const std::string a = p->argument() ? reg(p->argument()) : "";
    switch (p->get_kind()) {
      case IntegerMathKind::ADD_64:
        return fmt::format("{} += {};", d, a);
      case IntegerMathKind::SUB_64:
        return fmt::format("{} -= {};", d, a);
      case IntegerMathKind::AND_64:
        return fmt::format("{} &= {};", d, a);
      case IntegerMathKind::OR_64:
        return fmt::format("{} |= {};", d, a);
      case IntegerMathKind::XOR_64:
        return fmt::format("{} ^= {};", d, a);
      case IntegerMathKind::NOT_64:
        return fmt::format("{} = ~{};", d, d);
      case IntegerMathKind::IMUL_64:
        return fmt::format("{} = {} * {};", d, d, a);
      case IntegerMathKind::IMUL_32:
        return fmt::format("{} = goal_imul32({}, {});", d, d, a);
      case IntegerMathKind::IDIV_32:
        return fmt::format("{} = goal_idiv32({}, {});", d, d, a);
      case IntegerMathKind::IMOD_32:
        return fmt::format("{} = goal_imod32({}, {});", d, d, a);
      case IntegerMathKind::UDIV_32:
        return fmt::format("{} = goal_udiv32({}, {});", d, d, a);
      case IntegerMathKind::UMOD_32:
        return fmt::format("{} = goal_umod32({}, {});", d, d, a);
      case IntegerMathKind::SHLV_64:
        return fmt::format("{} = {} << ({} & 63);", d, d, a);
      case IntegerMathKind::SHRV_64:
        return fmt::format("{} = {} >> ({} & 63);", d, d, a);
      case IntegerMathKind::SARV_64:
        return fmt::format("{} = (uint64_t)((int64_t){} >> ({} & 63));", d, d, a);
      case IntegerMathKind::SHL_64:
        return fmt::format("{} = {} << {};", d, d, p->shift_amount());
      case IntegerMathKind::SHR_64:
        return fmt::format("{} = {} >> {};", d, d, p->shift_amount());
      case IntegerMathKind::SAR_64:
        return fmt::format("{} = (uint64_t)((int64_t){} >> {});", d, d, p->shift_amount());
      default:
        throw std::runtime_error("unsupported IntegerMathKind");
    }
  }

  if (auto* p = dynamic_cast<IR_FloatMath*>(ir)) {
    const std::string d = reg(p->destination());
    const std::string a = reg(p->argument());
    switch (p->get_kind()) {
      case FloatMathKind::ADD_SS:
        return fmt::format("{} += {};", d, a);
      case FloatMathKind::SUB_SS:
        return fmt::format("{} -= {};", d, a);
      case FloatMathKind::MUL_SS:
        return fmt::format("{} *= {};", d, a);
      case FloatMathKind::DIV_SS:
        return fmt::format("{} /= {};", d, a);
      case FloatMathKind::MIN_SS:
        return fmt::format("{} = goal_min_ss({}, {});", d, d, a);
      case FloatMathKind::MAX_SS:
        return fmt::format("{} = goal_max_ss({}, {});", d, d, a);
      case FloatMathKind::SQRT_SS:
        return fmt::format("{} = __builtin_sqrtf({});", d, a);
      default:
        throw std::runtime_error("unsupported FloatMathKind");
    }
  }

  if (auto* p = dynamic_cast<IR_FloatToInt*>(ir)) {
    return fmt::format("{} = goal_f2i({});", reg(p->destination()), reg(p->source()));
  }

  if (auto* p = dynamic_cast<IR_IntToFloat*>(ir)) {
    return fmt::format("{} = goal_i2f({});", reg(p->destination()), reg(p->source()));
  }

  if (auto* p = dynamic_cast<IR_GotoLabel*>(ir)) {
    if (!p->is_resolved()) {
      throw std::runtime_error("unresolved goto");
    }
    return fmt::format("goto L{};", p->destination()->idx);
  }

  if (auto* p = dynamic_cast<IR_ConditionalBranch*>(ir)) {
    if (!p->is_resolved()) {
      throw std::runtime_error("unresolved conditional branch");
    }
    const auto& c = p->condition;
    std::string op;
    switch (c.kind) {
      case ConditionKind::EQUAL:
        op = "==";
        break;
      case ConditionKind::NOT_EQUAL:
        op = "!=";
        break;
      case ConditionKind::LEQ:
        op = "<=";
        break;
      case ConditionKind::GEQ:
        op = ">=";
        break;
      case ConditionKind::LT:
        op = "<";
        break;
      case ConditionKind::GT:
        op = ">";
        break;
      default:
        throw std::runtime_error("unsupported condition kind");
    }

    std::string a = reg(c.a);
    std::string b = reg(c.b);
    if (!c.is_float) {
      const char* cast = c.is_signed ? "(int64_t)" : "(uint64_t)";
      a = cast + a;
      b = cast + b;
    }
    return fmt::format("if ({} {} {}) goto L{};", a, op, b, p->label.idx);
  }

  if (auto* p = dynamic_cast<IR_VFMath3Asm*>(ir)) {
    const std::string d = reg(p->destination());
    const std::string t = c_type_for(p->destination()->ireg().reg_class);
    const std::string a = fmt::format("(goal_vf){}", reg(p->source1()));
    const std::string b = fmt::format("(goal_vf){}", reg(p->source2()));
    switch (p->get_kind()) {
      case IR_VFMath3Asm::Kind::ADD:
        return fmt::format("{} = ({})({} + {});", d, t, a, b);
      case IR_VFMath3Asm::Kind::SUB:
        return fmt::format("{} = ({})({} - {});", d, t, a, b);
      case IR_VFMath3Asm::Kind::MUL:
        return fmt::format("{} = ({})({} * {});", d, t, a, b);
      case IR_VFMath3Asm::Kind::DIV:
        return fmt::format("{} = ({})({} / {});", d, t, a, b);
      case IR_VFMath3Asm::Kind::MIN:
        return fmt::format("{} = ({})goal_vf_min({}, {});", d, t, a, b);
      case IR_VFMath3Asm::Kind::MAX:
        return fmt::format("{} = ({})goal_vf_max({}, {});", d, t, a, b);
      case IR_VFMath3Asm::Kind::XOR:
        return fmt::format("{} = ({})goal_vf_xor({}, {});", d, t, a, b);
      default:
        throw std::runtime_error("unsupported vector float operation");
    }
  }

  if (auto* p = dynamic_cast<IR_PS2VUDivQ*>(ir)) {
    const std::string d = reg(p->destination());
    const std::string t = c_type_for(p->destination()->ireg().reg_class);
    const std::string numerator = fmt::format("(goal_vf){}", reg(p->numerator()));
    const std::string denominator = fmt::format("(goal_vf){}", reg(p->denominator()));
    return fmt::format("{} = ({})goal_vf_ps2_vu_div_q({}, {});", d, t, numerator, denominator);
  }

  if (auto* p = dynamic_cast<IR_VFMath2Asm*>(ir)) {
    const std::string d = reg(p->destination());
    const std::string t = c_type_for(p->destination()->ireg().reg_class);
    const std::string s = reg(p->source());
    switch (p->get_kind()) {
      case IR_VFMath2Asm::Kind::ITOF:
        return fmt::format("{} = ({})goal_vf_itof((goal_vi){});", d, t, s);
      case IR_VFMath2Asm::Kind::FTOI:
        return fmt::format("{} = ({})goal_vf_ftoi((goal_vf){});", d, t, s);
      default:
        throw std::runtime_error("unsupported vector float conversion");
    }
  }

  if (auto* p = dynamic_cast<IR_SqrtVF*>(ir)) {
    return fmt::format("{} = ({})goal_vf_sqrt((goal_vf){});", reg(p->destination()),
                       c_type_for(p->destination()->ireg().reg_class), reg(p->source()));
  }

  if (auto* p = dynamic_cast<IR_SplatVF*>(ir)) {
    int lane = 0;
    switch (p->element()) {
      case emitter::Register::VF_ELEMENT::X:
        lane = 0;
        break;
      case emitter::Register::VF_ELEMENT::Y:
        lane = 1;
        break;
      case emitter::Register::VF_ELEMENT::Z:
        lane = 2;
        break;
      case emitter::Register::VF_ELEMENT::W:
        lane = 3;
        break;
      default:
        throw std::runtime_error("unsupported splat element");
    }
    return fmt::format("{} = ({})goal_vf_splat((goal_vf){}, {});", reg(p->destination()),
                       c_type_for(p->destination()->ireg().reg_class), reg(p->source()), lane);
  }

  if (auto* p = dynamic_cast<IR_Int128Math3Asm*>(ir)) {
    const std::string d = reg(p->destination());
    const std::string t = c_type_for(p->destination()->ireg().reg_class);
    const std::string a = fmt::format("(goal_vi){}", reg(p->source1()));
    const std::string b = fmt::format("(goal_vi){}", reg(p->source2()));
    const auto helper = [&](const char* name) {
      return fmt::format("{} = ({}){}({}, {});", d, t, name, a, b);
    };
    switch (p->get_kind()) {
      case IR_Int128Math3Asm::Kind::PCPYUD:
        return helper("goal_pcpyud");
      case IR_Int128Math3Asm::Kind::PCPYLD:
        return helper("goal_pcpyld");
      case IR_Int128Math3Asm::Kind::PEXTLB:
        return helper("goal_pextlb");
      case IR_Int128Math3Asm::Kind::PEXTLH:
        return helper("goal_pextlh");
      case IR_Int128Math3Asm::Kind::PEXTLW:
        return helper("goal_pextlw");
      case IR_Int128Math3Asm::Kind::PEXTUB:
        return helper("goal_pextub");
      case IR_Int128Math3Asm::Kind::PEXTUH:
        return helper("goal_pextuh");
      case IR_Int128Math3Asm::Kind::PEXTUW:
        return helper("goal_pextuw");
      case IR_Int128Math3Asm::Kind::PCEQB:
        return helper("goal_pceqb");
      case IR_Int128Math3Asm::Kind::PCEQH:
        return helper("goal_pceqh");
      case IR_Int128Math3Asm::Kind::PCEQW:
        return helper("goal_pceqw");
      case IR_Int128Math3Asm::Kind::PCGTB:
        return helper("goal_pcgtb");
      case IR_Int128Math3Asm::Kind::PCGTH:
        return helper("goal_pcgth");
      case IR_Int128Math3Asm::Kind::PCGTW:
        return helper("goal_pcgtw");
      case IR_Int128Math3Asm::Kind::PADDB:
        return helper("goal_paddb");
      case IR_Int128Math3Asm::Kind::PACKUSWB:
        return helper("goal_packuswb");
      case IR_Int128Math3Asm::Kind::POR:
        return fmt::format("{} = ({})({} | {});", d, t, a, b);
      case IR_Int128Math3Asm::Kind::PXOR:
        return fmt::format("{} = ({})({} ^ {});", d, t, a, b);
      case IR_Int128Math3Asm::Kind::PAND:
        return fmt::format("{} = ({})({} & {});", d, t, a, b);
      case IR_Int128Math3Asm::Kind::PSUBW:
        return fmt::format("{} = ({})({} - {});", d, t, a, b);
      default:
        throw std::runtime_error(
            fmt::format("no C lowering for this PS2 128-bit integer operation: {}", ir->print()));
    }
  }

  if (auto* p = dynamic_cast<IR_Int128Math2Asm*>(ir)) {
    const std::string d = reg(p->destination());
    const std::string t = c_type_for(p->destination()->ireg().reg_class);
    const std::string s = fmt::format("(goal_vi){}", reg(p->source()));
    if (!p->immediate().has_value()) {
      throw std::runtime_error("128-bit integer shift without an immediate");
    }
    const int64_t imm = *p->immediate();

    // A shift count that reaches the lane width means different things on the two machines this
    // has to agree with: the PS2 keeps the low bits of the count, x86-64 produces zero (or all
    // sign bits for an arithmetic shift). Jak 1 only ever shifts by 6, 10 or 16, so rather than
    // pick a winner, refuse the ambiguous case and stay loud about it.
    const auto lane_shift = [&](const char* name, int lane_bits) {
      if (imm < 0 || imm >= lane_bits) {
        throw std::runtime_error(fmt::format(
            "shift of {} by {}, which the PS2 and x86-64 backends disagree about", name, imm));
      }
      return fmt::format("{} = ({}){}({}, {});", d, t, name, s, imm);
    };

    switch (p->get_kind()) {
      case IR_Int128Math2Asm::Kind::PW_SLL:
        return lane_shift("goal_pw_sll", 32);
      case IR_Int128Math2Asm::Kind::PW_SRL:
        return lane_shift("goal_pw_srl", 32);
      case IR_Int128Math2Asm::Kind::PW_SRA:
        return lane_shift("goal_pw_sra", 32);
      case IR_Int128Math2Asm::Kind::PH_SLL:
        return lane_shift("goal_ph_sll", 16);
      case IR_Int128Math2Asm::Kind::PH_SRL:
        return lane_shift("goal_ph_srl", 16);
      case IR_Int128Math2Asm::Kind::VPSRLDQ:
        return fmt::format("{} = ({})goal_vsrl_bytes({}, {});", d, t, s, imm);
      case IR_Int128Math2Asm::Kind::VPSLLDQ:
        return fmt::format("{} = ({})goal_vsll_bytes({}, {});", d, t, s, imm);
      case IR_Int128Math2Asm::Kind::VPSHUFLW:
        return fmt::format("{} = ({})goal_shuffle_low_halfwords({}, {});", d, t, s, imm);
      case IR_Int128Math2Asm::Kind::VPSHUFHW:
        return fmt::format("{} = ({})goal_shuffle_high_halfwords({}, {});", d, t, s, imm);
      default:
        throw std::runtime_error(
            fmt::format("no C lowering for this PS2 128-bit integer operation: {}", ir->print()));
    }
  }

  if (auto* p = dynamic_cast<IR_BlendVF*>(ir)) {
    return fmt::format("{} = ({})goal_blend_vf((goal_vf){}, (goal_vf){}, {});",
                       reg(p->destination()), c_type_for(p->destination()->ireg().reg_class),
                       reg(p->source1()), reg(p->source2()), int(p->mask()));
  }

  if (auto* p = dynamic_cast<IR_SwizzleVF*>(ir)) {
    return fmt::format("{} = ({})goal_vf_shuffle((goal_vf){}, {});", reg(p->destination()),
                       c_type_for(p->destination()->ireg().reg_class), reg(p->source()),
                       int(p->control_bytes()));
  }

  // .nop.vf and .wait.vf are VU0 macro-mode synchronisation: they make the main CPU wait for the
  // vector unit before reading back a result. The C backend has no separate vector unit - every
  // vector operation is an ordinary C statement in program order - so there is nothing to wait
  // for, and the correct translation is no code at all. This is not an unimplemented case.
  if (dynamic_cast<IR_AsmFNop*>(ir) || dynamic_cast<IR_AsmFWait*>(ir)) {
    return ";";
  }

  throw std::runtime_error(fmt::format("no C lowering for IR node: {}", ir->print()));
}

std::string FileEmitter::emit_function(const FunctionEnv& func,
                                       const std::string& c_name,
                                       std::string* prototype) {
  const auto& code = func.code();
  find_machine_state_regs(func);
  check_stack_pointer_use(func);

  // arguments come from the IR_ValueReset the function prologue emits, which is always first.
  // Later IR_ValueResets come from (rlet ... :reset-here #t) and are not arguments.
  std::vector<const RegVal*> args;
  m_argument_reset = nullptr;
  if (!code.empty()) {
    if (auto* p = dynamic_cast<IR_ValueReset*>(code.at(0).get())) {
      m_argument_reset = p;
      for (const auto* a : p->args()) {
        if (!is_machine_state(a)) {
          args.push_back(a);
        }
      }
    }
  }

  const RegVal* return_reg = nullptr;
  for (const auto& ir : code) {
    if (auto* p = dynamic_cast<IR_Return*>(ir.get())) {
      return_reg = p->return_register();
    }
  }

  // branch targets, including the end-of-function label
  std::set<int> labels;
  for (const auto& ir : code) {
    if (auto* p = dynamic_cast<IR_GotoLabel*>(ir.get())) {
      if (p->is_resolved()) {
        labels.insert(p->destination()->idx);
      }
    } else if (auto* p2 = dynamic_cast<IR_ConditionalBranch*>(ir.get())) {
      labels.insert(p2->label.idx);
    }
  }

  // translate the body first so an unsupported node aborts before anything is written
  std::string body;
  for (size_t i = 0; i < code.size(); i++) {
    if (labels.count(int(i))) {
      body += fmt::format("L{}:;\n", i);
    }
    body += fmt::format("  {}\n", emit_instruction(func, code.at(i).get()));
  }
  if (labels.count(int(code.size()))) {
    body += fmt::format("L{}:;\n", code.size());
  }

  std::string signature;
  for (size_t i = 0; i < args.size(); i++) {
    if (i) {
      signature += ", ";
    }
    signature += fmt::format("{} a{}", c_type_for(args.at(i)->ireg().reg_class), i);
  }
  if (args.empty()) {
    signature = "void";
  }

  const char* ret_type = return_reg ? c_type_for(return_reg->ireg().reg_class) : "uint64_t";

  *prototype = fmt::format("{} {}({})", ret_type, c_name, signature);

  std::string out;
  out += fmt::format("/* {} */\n", func.name());
  out += fmt::format("{} {{\n", *prototype);

  // rlet can bind a second RegVal to an existing ireg id, so declare each id exactly once
  std::map<int, RegClass> declared;
  for (const auto& rv : func.reg_vals()) {
    if (is_machine_state(rv.get())) {
      continue;
    }
    const auto id = rv->ireg().id;
    const auto rc = rv->ireg().reg_class;
    auto it = declared.find(id);
    if (it != declared.end()) {
      if (it->second != rc) {
        throw std::runtime_error(
            fmt::format("ireg {} is used with two different register classes", id));
      }
      continue;
    }
    declared[id] = rc;
    if (is_128(rc)) {
      out += fmt::format("  {} r{} = {{0, 0, 0, 0}};\n", c_type_for(rc), id);
    } else {
      out += fmt::format("  {} r{} = 0;\n", c_type_for(rc), id);
    }
  }
  const int stack_bytes = func.stack_slots_used_for_stack_vars() * 8;
  if (stack_bytes > 0) {
    out += fmt::format("  __attribute__((aligned(16))) uint8_t goal_stack[{}] = {{0}};\n",
                       stack_bytes);
  }
  out += body;
  if (return_reg) {
    out += fmt::format("  return {};\n", reg(return_reg));
  } else {
    out += "  return 0;\n";
  }
  out += "}\n";
  return out;
}

CBackendResult FileEmitter::run() {
  CBackendResult result;
  result.tag = m_tag;
  std::string functions_source;
  std::set<std::string> used_names;

  for (const auto& function : m_file.functions()) {
    const auto& func = *function;
    CBackendFunctionResult entry;
    entry.goal_name = func.name();
    entry.c_name = fmt::format("goal_{}_{}", m_tag, mangle(func.name()));
    if (!used_names.insert(entry.c_name).second) {
      int suffix = 2;
      while (!used_names.insert(fmt::format("{}_{}", entry.c_name, suffix)).second) {
        suffix++;
      }
      entry.c_name = fmt::format("{}_{}", entry.c_name, suffix);
    }
    try {
      functions_source += emit_function(func, entry.c_name, &entry.prototype);
      functions_source += "\n";
      entry.ok = true;
    } catch (const std::exception& e) {
      entry.ok = false;
      entry.error = e.what();
      if (const char* native = native_implementation_for(m_version, m_tag, entry.goal_name)) {
        entry.native_symbol = native;
        functions_source +=
            fmt::format("/* {}: {}. Supplied natively by {}. */\n"
                        "extern uint64_t {}(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,\n"
                        "                   uint64_t, uint64_t, uint64_t);\n\n",
                        func.name(), e.what(), native, native);
      } else {
        functions_source += fmt::format("/* skipped {}: {} */\n\n", func.name(), e.what());
      }
    }
    result.functions.push_back(entry);
  }

  std::string header;
  header += "/* Generated by goalc's AOT C backend. Do not edit. */\n";
  header += "#include \"goalc/aot/goal_c_runtime.h\"\n\n";

  header += fmt::format("static const char* const goalc_symbol_names[{}] = {{\n",
                        std::max<size_t>(m_symbols.size(), 1));
  if (m_symbols.empty()) {
    header += "    0,\n";
  }
  for (const auto& s : m_symbols) {
    header += fmt::format("    \"{}\",\n", s);
  }
  header += "};\n";
  header += fmt::format("static int32_t* goalc_symbol_slots[{}];\n",
                        std::max<size_t>(m_symbols.size(), 1));
  header += fmt::format("static uint64_t goalc_symbol_ptrs[{}];\n\n",
                        std::max<size_t>(m_symbols.size(), 1));
  header += fmt::format("void goal_{}_link(void) {{\n", m_tag);
  header += fmt::format("  for (int i = 0; i < {}; i++) {{\n", m_symbols.size());
  header += "    goalc_symbol_slots[i] = goal_symbol_slot(goalc_symbol_names[i]);\n";
  header += "    goalc_symbol_ptrs[i] = goal_symbol_ptr(goalc_symbol_names[i]);\n";
  header += "  }\n}\n\n";
  header += emit_statics();

  // index -> native entry point, so the loader can build a GOAL function object per function
  std::string function_table;
  function_table += fmt::format("const void* const goal_{}_functions[{}] = {{\n", m_tag,
                                std::max<size_t>(result.functions.size(), 1));
  for (const auto& f : result.functions) {
    if (f.ok) {
      function_table += fmt::format("    (const void*)&{},\n", f.c_name);
    } else if (!f.native_symbol.empty()) {
      function_table += fmt::format("    (const void*)&{},\n", f.native_symbol);
    } else {
      function_table += "    0,\n";
    }
  }
  if (result.functions.empty()) {
    function_table += "    0,\n";
  }
  function_table += "};\n";
  function_table +=
      fmt::format("const int goal_{}_function_count = {};\n", m_tag, result.functions.size());

  result.source = header + functions_source + function_table;

  result.header = "/* Generated by goalc's AOT C backend. Do not edit. */\n#pragma once\n";
  result.header += "#include \"goalc/aot/goal_c_runtime.h\"\n\n";
  result.header += fmt::format("void goal_{}_link(void);\n", m_tag);
  result.header += fmt::format("extern const goal_static_desc goal_{}_statics[];\n", m_tag);
  result.header += fmt::format("extern const int goal_{}_static_count;\n", m_tag);
  result.header += fmt::format("extern const void* const goal_{}_functions[];\n", m_tag);
  result.header += fmt::format("extern const int goal_{}_function_count;\n\n", m_tag);
  for (const auto& f : result.functions) {
    if (f.ok) {
      result.header += fmt::format("/* {} */\n{};\n", f.goal_name, f.prototype);
    }
  }
  return result;
}

}  // namespace

CBackendResult emit_c_file(FileEnv& file,
                           const std::string& file_tag,
                           GameVersion version,
                           const TypeSystem& types) {
  FileEmitter emitter(file, mangle(file_tag), version, types);
  return emitter.run();
}

}  // namespace aot
