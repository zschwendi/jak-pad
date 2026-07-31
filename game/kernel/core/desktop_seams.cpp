/*!
 * @file desktop_seams.cpp
 * Definitions for the desktop-only symbols that the portable Jak 1 kernel subset still references
 * at link time.
 *
 * Every function in this file is a STUB. None of them are implemented, and none of them return a
 * plausible-looking failure value: they abort with a message naming the missing subsystem. They
 * exist so that the real kernel translation units can be linked without also linking the desktop
 * windowing, IOP, sound, and debugger-transport code.
 *
 * If you hit one of these at runtime, the answer is to implement the subsystem, not to soften the
 * stub.
 *
 * Subsystems intentionally not in this library:
 *   - game/kernel/{common,jak1}/kmachine.cpp   : IOP boot, video, pads, PC-port functions (SDL,
 *                                                OpenGL, Discord, sqlite)
 *   - game/kernel/{common,jak1}/ksound.cpp     : 989snd / overlord sound
 *   - game/kernel/jak1/kboot.cpp               : desktop boot + GOAL kernel dispatch loop
 *   - game/sce/sif_ee.cpp                      : EE<->IOP bridge and host file I/O (needs the IOP
 *                                                thread emulation and a desktop file layout)
 *   - game/sce/deci2.cpp, game/system/**       : DECI2 debugger transport and sockets
 *   - game/mips2c/**                           : hand-translated PS2 VU/asm renderer + collision
 */

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/kernel/common/kmachine.h"
#include "game/kernel/jak1/kmachine.h"
#include "game/mips2c/mips2c_table.h"
#include "game/sce/deci2.h"
#include "game/sce/sif_ee.h"

namespace {
[[noreturn]] void missing(const char* subsystem, const char* symbol) {
  lg::error("[kernel-core] {} is not part of this build; {} cannot be used.", subsystem, symbol);
  ASSERT_NOT_REACHED_MSG("kernel-core stub called");
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// game/kernel/common/kmachine.cpp
// ---------------------------------------------------------------------------------------------

/*!
 * Upstream's CacheFlush is already a no-op on PC (the PS2 cache instructions have no equivalent),
 * so this is a faithful implementation rather than a stub. It is duplicated here only because the
 * rest of kmachine.cpp is not portable.
 */
void CacheFlush(void* mem, int size) {
  (void)mem;
  (void)size;
}

// ---------------------------------------------------------------------------------------------
// game/kernel/jak1/kmachine.cpp
// ---------------------------------------------------------------------------------------------

namespace jak1 {
void InitMachineScheme() {
  missing("the machine layer (IOP/video/pad/PC-port functions)", "jak1::InitMachineScheme");
}
}  // namespace jak1

// ---------------------------------------------------------------------------------------------
// game/sce/sif_ee.cpp - host file I/O and the EE<->IOP RPC bridge
// ---------------------------------------------------------------------------------------------

namespace ee {

s32 sceOpen(const char* filename, s32 flag) {
  (void)filename;
  (void)flag;
  missing("host file I/O", "ee::sceOpen");
}

s32 sceClose(s32 fd) {
  (void)fd;
  missing("host file I/O", "ee::sceClose");
}

s32 sceRead(s32 fd, void* buf, s32 nbyte) {
  (void)fd;
  (void)buf;
  (void)nbyte;
  missing("host file I/O", "ee::sceRead");
}

s32 sceWrite(s32 fd, const void* buf, s32 nbyte) {
  (void)fd;
  (void)buf;
  (void)nbyte;
  missing("host file I/O", "ee::sceWrite");
}

s32 sceLseek(s32 fd, s32 offset, s32 where) {
  (void)fd;
  (void)offset;
  (void)where;
  missing("host file I/O", "ee::sceLseek");
}

s32 sceSifCallRpc(sceSifClientData* bd,
                  u32 fno,
                  u32 mode,
                  void* send,
                  s32 ssize,
                  void* recv,
                  s32 rsize,
                  void* end_func,
                  void* end_para) {
  (void)bd;
  (void)fno;
  (void)mode;
  (void)send;
  (void)ssize;
  (void)recv;
  (void)rsize;
  (void)end_func;
  (void)end_para;
  missing("the IOP (overlord) bridge", "ee::sceSifCallRpc");
}

s32 sceSifCheckStatRpc(sceSifRpcData* bd) {
  (void)bd;
  missing("the IOP (overlord) bridge", "ee::sceSifCheckStatRpc");
}

s32 sceSifBindRpc(sceSifClientData* bd, u32 request, u32 mode) {
  (void)bd;
  (void)request;
  (void)mode;
  missing("the IOP (overlord) bridge", "ee::sceSifBindRpc");
}

// -------------------------------------------------------------------------------------------
// game/sce/deci2.cpp - DECI2 debugger transport
// -------------------------------------------------------------------------------------------

s32 sceDeci2Open(u16 protocol, void* opt, void (*handler)(s32 event, s32 param, void* opt)) {
  (void)protocol;
  (void)opt;
  (void)handler;
  missing("the DECI2 listener transport", "ee::sceDeci2Open");
}

s32 sceDeci2Close(s32 s) {
  (void)s;
  missing("the DECI2 listener transport", "ee::sceDeci2Close");
}

s32 sceDeci2ReqSend(s32 s, char dest) {
  (void)s;
  (void)dest;
  missing("the DECI2 listener transport", "ee::sceDeci2ReqSend");
}

s32 sceDeci2ExRecv(s32 s, void* buf, u16 len) {
  (void)s;
  (void)buf;
  (void)len;
  missing("the DECI2 listener transport", "ee::sceDeci2ExRecv");
}

s32 sceDeci2ExSend(s32 s, void* buf, u16 len) {
  (void)s;
  (void)buf;
  (void)len;
  missing("the DECI2 listener transport", "ee::sceDeci2ExSend");
}

void LIBRARY_sceDeci2_run_sends() {
  missing("the DECI2 listener transport", "ee::LIBRARY_sceDeci2_run_sends");
}

}  // namespace ee

// ---------------------------------------------------------------------------------------------
// game/mips2c/mips2c_table.cpp
//
// The mips2c table names every hand-translated PS2 renderer/collision function for all three
// games, so linking it pulls in the whole game/mips2c tree. The link callback map is empty here,
// which means no mips2c function is ever registered, and any object file that asks the linker for
// one aborts below instead of silently linking against nothing.
// ---------------------------------------------------------------------------------------------

namespace Mips2C {

LinkedFunctionTable gLinkedFunctionTable;
PerGameVersion<std::unordered_map<std::string, std::vector<void (*)()>>> gMips2CLinkCallbacks = {
    {}, {}, {}, {}};

void LinkedFunctionTable::reg(const std::string& name, u64 (*exec)(void*), u32 goal_stack_size) {
  (void)exec;
  (void)goal_stack_size;
  lg::error("[kernel-core] mips2c function {} tried to register.", name);
  missing("the mips2c function library", "Mips2C::LinkedFunctionTable::reg");
}

u32 LinkedFunctionTable::get(const std::string& name) {
  lg::error("[kernel-core] mips2c function {} was requested by the linker.", name);
  missing("the mips2c function library", "Mips2C::LinkedFunctionTable::get");
}

}  // namespace Mips2C
