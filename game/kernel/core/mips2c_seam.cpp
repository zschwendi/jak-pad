/*!
 * @file mips2c_seam.cpp
 * The Jak 1 mips2c function library, wired into this runtime.
 *
 * `def-mips2c` in GOAL source names a function that was never GOAL: PS2 VU or MIPS assembly that
 * OpenGOAL hand-translated into C++ under game/mips2c/jak1_functions. GOAL's top-level asks for it
 * by name through `__pc-get-mips2c`, so a runtime that answers 0 leaves the symbol empty and the
 * first caller dereferences it. Loading any art group hits this immediately: `login` on an
 * art-group reaches `adgif-shader-login-fast`, which calls the mips2c
 * `adgif-shader<-texture-with-update!`.
 *
 * Two things are different here from upstream's game/mips2c/mips2c_table.cpp, which is not in this
 * library because it names all four games and would pull all four function libraries in:
 *
 * **The trampoline is C, not generated machine code.** Upstream writes an x86-64 stub into the
 * GOAL heap that pushes the C function and the stack size and jumps to `_mips2c_call_systemv`.
 * Nothing can be written into the GOAL heap and executed here, so each registered function gets a
 * distinct native entry point instead - the same shape as every other function object in this
 * runtime, see aot_loader.h - which builds the ExecutionContext itself.
 *
 * **Everything is registered up front.** Upstream registers a file's mips2c functions when that
 * file is linked, through `gMips2CLinkCallbacks`. Registration only fills a name table that
 * `__pc-get-mips2c` reads, so doing it once at startup has the same effect and does not depend on
 * the linker knowing which object file it is working on.
 *
 * Note that a mips2c function that calls back into GOAL (`jalr`) is not supported on ARM64 - see
 * ExecutionContext::jalr in game/mips2c/mips2c_private.h, which has no ARM64 case. None of the
 * functions on the art-group login path do, and the ones that do abort rather than return a value
 * they never computed.
 */

#include <string>
#include <unordered_map>

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/core/kernel_game.h"
#include "game/kernel/core/mips2c_seam.h"
#include "game/mips2c/mips2c_private.h"
#include "game/mips2c/mips2c_table.h"
#include "game/runtime.h"
#include "goalc/aot/goal_c_runtime.h"

#include "fmt/format.h"

namespace Mips2C {

/*! Take the mips2c scratch stack out of the global heap. See run_mips2c_slot. */
void reserve_mips2c_stack();
/*! Drop every registration, for a kernel that is being torn down. */
void forget_mips2c_registrations();

LinkedFunctionTable gLinkedFunctionTable;
Rng gRng;

// Upstream links a file's mips2c functions when that file is linked. This runtime registers them
// all at startup instead (see the file comment), so the map stays empty and klink.cpp's lookup
// never finds anything to do.
PerGameVersion<std::unordered_map<std::string, std::vector<void (*)()>>> gMips2CLinkCallbacks = {
    {}, {}, {}, {}};

namespace {

/*! One registered mips2c function. The index into this table is baked into its native entry. */
struct Mips2CSlot {
  u64 (*exec)(void*) = nullptr;
  u32 goal_stack_size = 0;
};

// Jak 1 registers 94 functions, Jak 2 registers 137; one game per process.
constexpr int kMips2CSlotCount = 160;
Mips2CSlot g_slots[kMips2CSlotCount];
int g_slot_count = 0;

/*!
 * Scratch memory for the translated code's MIPS stack.
 *
 * `sp` in a mips2c function has to be a GOAL address, because the translated code reaches it
 * through `g_ee_main_mem + sp`. Upstream points it at the native stack, which only means anything
 * when GOAL is already running on a GOAL-memory stack - and the linker calls a data object's
 * `login` method straight from C, where it is not. So this runtime gives mips2c a small dedicated
 * region of GOAL memory instead, used as a stack: each call takes its declared size off the top
 * and puts it back on return, so nesting works. The largest declared size in Jak 1 is 1024 bytes.
 */
constexpr u32 kMips2CStackSize = 0x10000;
Ptr<u8> g_mips2c_stack_base{0};
u32 g_mips2c_stack_top = 0;

/*!
 * Run a mips2c function with GOAL's arguments, the way _mips2c_call_systemv does on x86-64: the
 * translated code reads its arguments out of the MIPS a0-a3/t0-t3 registers, the current process
 * out of s6, and the symbol table out of s7.
 */
u64 run_mips2c_slot(int index, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
  const Mips2CSlot& slot = g_slots[index];
  const u32 frame = (slot.goal_stack_size + 15) & ~15u;
  ASSERT_MSG(g_mips2c_stack_base.offset && g_mips2c_stack_top >= g_mips2c_stack_base.offset + frame,
             "[mips2c] the scratch stack is exhausted");

  const u32 saved_top = g_mips2c_stack_top;
  g_mips2c_stack_top -= frame;

  ExecutionContext ctx;
  ctx.gprs[Mips2C::a0].du64[0] = a0;
  ctx.gprs[Mips2C::a1].du64[0] = a1;
  ctx.gprs[Mips2C::a2].du64[0] = a2;
  ctx.gprs[Mips2C::a3].du64[0] = a3;
  ctx.gprs[Mips2C::t0].du64[0] = a4;
  ctx.gprs[Mips2C::t1].du64[0] = a5;
  ctx.gprs[Mips2C::t2].du64[0] = a6;
  ctx.gprs[Mips2C::t3].du64[0] = a7;
  ctx.gprs[Mips2C::s6].du64[0] = g_goal_current_process;
  ctx.gprs[Mips2C::s7].du64[0] = ::s7.offset;
  ctx.gprs[Mips2C::sp].du64[0] = saved_top;

  const u64 result = slot.exec(&ctx);
  g_mips2c_stack_top = saved_top;
  return result;
}

// one distinct native entry point per slot, so a function object can name its slot
template <int Index>
u64 mips2c_entry(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
  return run_mips2c_slot(Index, a0, a1, a2, a3, a4, a5, a6, a7);
}

template <int... Index>
void collect_entries(void* (&out)[kMips2CSlotCount], std::integer_sequence<int, Index...>) {
  ((out[Index] = (void*)&mips2c_entry<Index>), ...);
}

void* const* entry_points() {
  static void* entries[kMips2CSlotCount];
  static bool built = false;
  if (!built) {
    collect_entries(entries, std::make_integer_sequence<int, kMips2CSlotCount>{});
    built = true;
  }
  return entries;
}

}  // namespace

void forget_mips2c_registrations() {
  gLinkedFunctionTable = LinkedFunctionTable();
  g_slot_count = 0;
  g_mips2c_stack_base.offset = 0;
  g_mips2c_stack_top = 0;
}

void reserve_mips2c_stack() {
  g_mips2c_stack_base = kmalloc(kglobalheap, kMips2CStackSize, KMALLOC_MEMSET, "mips2c-stack");
  ASSERT_MSG(g_mips2c_stack_base.offset, "[mips2c] no room for the scratch stack");
  g_mips2c_stack_top = g_mips2c_stack_base.offset + kMips2CStackSize;
}

void LinkedFunctionTable::reg(const std::string& name, u64 (*exec)(void*), u32 goal_stack_size) {
  if (m_executes.find(name) != m_executes.end()) {
    lg::warn("[mips2c] {} was registered twice; keeping the first registration.", name);
    return;
  }
  ASSERT_MSG(g_slot_count < kMips2CSlotCount, "[mips2c] out of trampoline slots");

  const int index = g_slot_count++;
  g_slots[index] = {exec, goal_stack_size};
  auto trampoline = Ptr<u8>(goal_game_make_function_from_native(entry_points()[index]));
  m_executes.insert({name, {exec, trampoline}});
}

u32 LinkedFunctionTable::get(const std::string& name) {
  const auto& it = m_executes.find(name);
  if (it == m_executes.end()) {
    lg::error("[mips2c] GOAL asked for the function {}, which is not registered.", name);
    ASSERT_NOT_REACHED_MSG("unregistered mips2c function");
  }
  return it->second.goal_trampoline.offset;
}

}  // namespace Mips2C

void goal_mips2c_reset(void) {
  Mips2C::forget_mips2c_registrations();
}
