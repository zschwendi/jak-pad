/*!
 * @file dgo_loader.cpp
 * The DGO seam for a platform with no IOP.
 *
 * Where the objects come from
 * ---------------------------
 * Upstream reads a DGO through the overlord: `BeginLoadingDGO` sends an RPC to a thread that
 * emulates the IOP, that thread streams the archive off the disc image into two buffers in EE main
 * memory, and the EE links out of one buffer while the IOP fills the other. That machinery exists
 * to keep a 1x DVD drive and a 294 MHz CPU busy at the same time.
 *
 * There is no IOP here and no drive to hide the latency of, so this reads the archive with ordinary
 * file calls (`ee::sceOpen` / `ee::sceRead`, implemented in desktop_seams.cpp against the player's
 * data directory) and hands the objects to the same `link_and_exec` the game uses. The three
 * streaming entry points keep their contract exactly - which buffer an object lands in, when the
 * last object goes to the top of the heap, what `GetNextDGO` returns and when it sets the
 * last-object flag - so `load_and_link_dgo_from_c` below is upstream's loop, unchanged in
 * substance. Only the source of the bytes is different. If an asynchronous loader is ever wanted,
 * it can replace these three functions without touching anything above them.
 *
 * Code versus data: which half of an object file this platform can use
 * -------------------------------------------------------------------
 * A DGO holds two kinds of object file, and the linker already tells them apart
 * (`is_opengoal_object` in game/kernel/common/klink.cpp):
 *
 *   - **v3 objects are code.** Their segments are x86-64 machine code emitted by goalc. Nothing on
 *     this platform can execute them.
 *   - **v2 and v4 objects are data.** Art groups, texture pages, and the game count. They contain
 *     no machine code at all - only structures, symbol references, type references, and pointer
 *     relocations - so the real linker's output is the same on any architecture.
 *
 * So the precedence rule is:
 *
 *   **Code comes from the AOT path; data comes from the DGO.**
 *
 * For a v3 object the bytes read out of the archive are discarded and the AOT translation unit
 * registered under the same name (`goal_aot_register_object`) is loaded in its place, with its
 * top-level run where the linker would have run the object's. For a v2/v4 object the bytes are
 * copied into the heap and linked by `link_and_exec` exactly as upstream does, which ends by
 * calling the object type's own GOAL `login` method - itself AOT code.
 *
 * A v3 object with no registered translation unit is a failure and is reported as one. It is never
 * skipped: skipping it would mean the game is missing a file it asked for, and every symptom after
 * that would be a mystery.
 *
 * Two ways in
 * -----------
 * `load_and_link_dgo_from_c` above is the C-driven load: the boot uses it for KERNEL.CGO and
 * GAME.CGO, and it runs to completion before returning.
 *
 * GOAL's own level loader does not use it. `engine/load/load-dgo.gc` and `engine/level/level.gc`
 * drive a DGO one object per frame through the overlord's RPC (`rpc-call` / `rpc-busy?`) and link
 * each object with `link-begin` / `link-resume`. The second half of this file answers that RPC out
 * of the same reader, and installs a `link-begin` that applies the same code/data rule, so both
 * ways in agree about what an object file is.
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/link_types.h"
#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"

#include "game/common/dgo_rpc_types.h"
#include "game/common/play_rpc_types.h"
#include "game/common/ramdisk_rpc_types.h"
#include "game/common/str_rpc_types.h"
#include "game/kernel/common/fileio.h"
#include "game/kernel/common/kdgo.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/dgo_loader.h"
#include "game/kernel/jak1/kdgo.h"
#include "game/kernel/jak1/klink.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/sce/sif_ee.h"

#include "fmt/format.h"

// defined in desktop_seams.cpp, next to the machine-layer stubs it reports through
u64 goal_kernel_core_machine_stub_report(const char* what);

namespace {

std::string g_error;
goal_dgo_load_stats g_stats;
bool g_verbose = false;

void set_error(const std::string& message) {
  g_error = message;
  lg::error("[dgo-loader] {}", message);
}

/*!
 * The archive being read. One at a time, which is all `load_and_link_dgo_from_c` ever asks for.
 */
struct DgoRead {
  s32 fd = -1;
  u32 object_count = 0;
  u32 objects_read = 0;
  Ptr<u8> buffer1{0};
  Ptr<u8> buffer2{0};
  Ptr<u8> heap_top{0};
  Ptr<u8> most_recent{0};
  int next_buffer = 1;
  bool failed = false;
};

DgoRead g_dgo;

void close_dgo() {
  if (g_dgo.fd >= 0) {
    ee::sceClose(g_dgo.fd);
  }
  g_dgo = DgoRead();
}

/*!
 * Read one object into the buffer the overlord would have put it in: the last object of a
 * multi-object archive (and the only object of a single-object archive) goes to the top of the
 * heap so it can be linked in place; everything else alternates between the two load buffers.
 */
bool read_next_object() {
  if (g_dgo.fd < 0 || g_dgo.objects_read >= g_dgo.object_count) {
    return false;
  }

  const bool last = g_dgo.objects_read + 1 == g_dgo.object_count;
  Ptr<u8> dest;
  if (last) {
    dest = g_dgo.heap_top;
  } else if (g_dgo.next_buffer == 1) {
    dest = g_dgo.buffer1;
    g_dgo.next_buffer = 2;
  } else {
    dest = g_dgo.buffer2;
    g_dgo.next_buffer = 1;
  }

  ObjectHeader header;
  if (ee::sceRead(g_dgo.fd, &header, sizeof(header)) != (s32)sizeof(header)) {
    set_error(fmt::format("short read on the header of object {}", g_dgo.objects_read));
    g_dgo.failed = true;
    return false;
  }
  header.name[sizeof(header.name) - 1] = '\0';

  // The archive pads every object out to 16 bytes, and the overlord reads the padding too.
  const s32 padded = (s32)((header.size + 0xf) & ~0xfu);
  memcpy(dest.c(), &header, sizeof(header));
  if (ee::sceRead(g_dgo.fd, (dest + sizeof(header)).c(), padded) != padded) {
    set_error(fmt::format("short read on object {} ({}), wanted {} bytes", g_dgo.objects_read,
                          header.name, padded));
    g_dgo.failed = true;
    return false;
  }

  g_dgo.most_recent = dest;
  g_dgo.objects_read++;
  return true;
}

/*!
 * Which half of an object file this platform can use. See the file comment: v2 and v4 objects hold
 * no machine code and are linked out of the archive; v3 objects are x86-64 code and come from the
 * AOT path instead.
 */
bool is_data_object(Ptr<u8> object) {
  const auto* header = (const LinkHeaderV2*)object.c();
  return header->type_tag == 0xffffffff && (header->version == 2 || header->version == 4);
}

/*!
 * Put the native translation of a code object in the heap and run its top-level, which is what the
 * linker would have done with the object's own v3 code. Returns false and sets the error when
 * there is no translation - never silently skips, because a missing file makes every symptom after
 * it a mystery.
 *
 * `on_goal_stack` says whether the caller is already running as GOAL. A C-driven load is not, and
 * has to switch to GOAL's stack; a `link-begin` from the level loader already is, and switching
 * would overwrite the frames of the GOAL thread that called it.
 */
bool load_code_object(const char* object_name, u32 link_flags, bool on_goal_stack) {
  const goal_aot_object_file* aot = goal_aot_registered_object(object_name);
  if (!aot) {
    set_error(fmt::format("the code object '{}' has no native translation", object_name));
    return false;
  }
  if (goal_aot_is_loaded(aot->tag)) {
    // An earlier DGO already brought this file in. Upstream would relink it into the new heap;
    // here the native code and its statics are already placed and still valid, and running the
    // top-level a second time is not the same as loading it once.
    g_stats.reused_code++;
    return true;
  }
  if (goal_aot_load(aot) != GOAL_KERNEL_CORE_OK) {
    set_error(fmt::format("could not load the native translation of '{}': {}", object_name,
                          goal_kernel_core_last_error()));
    return false;
  }
  if (link_flags & LINK_FLAG_EXECUTE) {
    const auto status = on_goal_stack ? goal_aot_run_top_level_here(aot->tag, nullptr)
                                      : goal_aot_run_top_level(aot->tag, nullptr);
    if (status != GOAL_KERNEL_CORE_OK) {
      set_error(fmt::format("the top-level of '{}' failed: {}", object_name,
                            goal_kernel_core_last_error()));
      return false;
    }
  }
  return true;
}

}  // namespace

namespace jak1 {

void kdgo_init_globals() {
  close_dgo();
  g_error.clear();
}

/*!
 * Open the archive and read its first object. Upstream sends an RPC and returns without waiting;
 * the wait is in GetNextDGO either way.
 */
void BeginLoadingDGO(const char* name, Ptr<u8> buffer1, Ptr<u8> buffer2, Ptr<u8> currentHeap) {
  close_dgo();

  const std::string relative = std::string("iso/") + name;
  g_dgo.fd = ee::sceOpen(relative.c_str(), SCE_RDONLY);
  if (g_dgo.fd < 0) {
    set_error(fmt::format("cannot open {}: {}", relative, goal_kernel_core_last_error()));
    g_dgo.failed = true;
    return;
  }

  DgoHeader header;
  if (ee::sceRead(g_dgo.fd, &header, sizeof(header)) != (s32)sizeof(header)) {
    set_error(fmt::format("{} is too short to hold a DGO header", relative));
    g_dgo.failed = true;
    return;
  }
  header.name[sizeof(header.name) - 1] = '\0';
  if (header.object_count == 0 || header.object_count > 4096) {
    set_error(fmt::format("{} claims {} objects", relative, header.object_count));
    g_dgo.failed = true;
    return;
  }

  g_dgo.object_count = header.object_count;
  g_dgo.buffer1 = buffer1;
  g_dgo.buffer2 = buffer2;
  g_dgo.heap_top = currentHeap;
  g_dgo.next_buffer = 1;
  lg::debug("[dgo-loader] {}: {} objects", header.name, header.object_count);
  read_next_object();
}

/*!
 * The object most recently read, and whether it was the last one.
 */
Ptr<u8> GetNextDGO(u32* lastObjectFlag) {
  *lastObjectFlag = 1;
  if (g_dgo.failed || !g_dgo.most_recent.offset) {
    return Ptr<u8>(0);
  }
  *lastObjectFlag = g_dgo.objects_read == g_dgo.object_count ? 1 : 0;
  auto buffer = g_dgo.most_recent;
  g_dgo.most_recent.offset = 0;
  return buffer;
}

/*!
 * The caller has finished linking the previous object and is telling us where the heap ends now.
 * Read the next one.
 */
void ContinueLoadingDGO(Ptr<u8> heapPtr) {
  g_dgo.heap_top = heapPtr;
  read_next_object();
}

/*!
 * GOAL's `dgo-load`.
 */
void load_and_link_dgo(u64 name_gstr, u64 heap_info, u64 flag, u64 buffer_size) {
  auto name = Ptr<char>(name_gstr + 4).c();
  auto heap = Ptr<kheapinfo>(heap_info);
  load_and_link_dgo_from_c(name, heap, flag, buffer_size, false);
}

/*!
 * Load and link a DGO file. Blocks until the whole archive is done.
 *
 * This is upstream's loop with one branch added: an object whose code this platform cannot execute
 * is taken from the AOT path instead of from the archive. See the file comment.
 */
void load_and_link_dgo_from_c(const char* name,
                              Ptr<kheapinfo> heap,
                              u32 linkFlag,
                              s32 bufferSize,
                              bool jump_from_c_to_goal) {
  g_error.clear();
  memset(&g_stats, 0, sizeof(g_stats));
  g_stats.heap_used_before = kheapused(heap);

  // remember where the heap top point is so we can clear temporary allocations
  auto oldHeapTop = heap->top;

  // allocate temporary buffers from top of the given heap
  // align 64 for IOP DMA
  // note: both buffers named dgo-buffer-2
  auto buffer2 = kmalloc(heap, bufferSize, KMALLOC_TOP | KMALLOC_ALIGN_64, "dgo-buffer-2");
  auto buffer1 = kmalloc(heap, bufferSize, KMALLOC_TOP | KMALLOC_ALIGN_64, "dgo-buffer-2");
  if (!buffer1.offset || !buffer2.offset) {
    set_error(fmt::format("no room for two {}-byte DGO load buffers", bufferSize));
    heap->top = oldHeapTop;
    return;
  }

  // build filename.  If no extension is given, default to CGO.
  char fileName[16];
  kstrcpyup(fileName, name);
  if (fileName[strlen(fileName) - 4] != '.') {
    strcat(fileName, ".CGO");
  }

  BeginLoadingDGO(fileName, buffer1, buffer2,
                  Ptr<u8>((heap->current + 0x3f).offset & 0xffffffc0));

  u32 lastObjectLoaded = 0;
  while (!lastObjectLoaded && !g_dgo.failed) {
    auto dgoObj = GetNextDGO(&lastObjectLoaded);
    if (!dgoObj.offset) {
      break;
    }

    // if we're on the last object, it is loaded at heap->current. So we can safely reset the two
    // dgo-buffer allocations, before linking, so the last file has the whole heap available.
    if (lastObjectLoaded) {
      heap->top = oldHeapTop;
    }

    auto obj = dgoObj + 0x40;             // seek past dgo object header
    u32 objSize = *(dgoObj.cast<u32>());  // size from the object header

    char objName[64];
    strcpy(objName, (dgoObj + 4).cast<char>().c());
    g_stats.objects++;

    const bool is_data = is_data_object(obj);
    if (g_verbose) {
      const auto* header = (const LinkHeaderV2*)obj.c();
      std::printf("  [%3d/%3d] %-24s %s v%d %8d bytes at #x%x, heap #x%x\n", g_stats.objects,
                  g_dgo.object_count, objName, is_data ? "data" : "code",
                  is_data ? header->version : *(const u32*)(obj + 8).c(), objSize, obj.offset,
                  heap->current.offset);
      std::fflush(stdout);
    }
    if (is_data) {
      g_stats.data_objects++;
      link_and_exec(obj, objName, objSize, heap, linkFlag, jump_from_c_to_goal);
    } else {
      g_stats.code_objects++;
      if (!load_code_object(objName, linkFlag, false)) {
        g_dgo.failed = true;
        break;
      }
    }

    if (!lastObjectLoaded) {
      ContinueLoadingDGO(Ptr<u8>((heap->current + 0x3f).offset & 0xffffffc0));
    }
  }

  heap->top = oldHeapTop;
  g_stats.heap_used_after = kheapused(heap);
  close_dgo();
}

}  // namespace jak1

// ================================================================================================
// The DGO RPC, answered synchronously
//
// GOAL's level loader talks to the overlord instead of calling the loop above. `dgo-load-begin`,
// `dgo-load-get-next` and `dgo-load-continue` (engine/load/load-dgo.gc) put a 32-byte
// `load-dgo-msg` in an RPC buffer and send it on channel 3, then poll `rpc-busy?` until the reply
// comes back in the same buffer.
//
// There is no IOP here, so the RPC is answered where it is sent: `rpc-call` does the work and
// returns, and `rpc-busy?` is therefore always 0. That is a real behavioural difference from the
// PS2 - a whole object file is read inside one `rpc-call` instead of streaming while the EE runs -
// but it is a difference in *when* the bytes arrive, not in what GOAL sees. GOAL's own state
// machine is untouched: it still gets one object per frame, still toggles between the two load
// buffers, still gets the last object at the heap top, and still sees `more` until the archive is
// done.
//
// The three function numbers are upstream's (game/common/dgo_rpc_types.h): 0 begins a load, 1
// continues it, 2 cancels. The result codes are `load-msg-result` in load-dgo.gc.
// ================================================================================================

namespace {

/*!
 * The 32-byte command GOAL sends and the overlord replies in: `load-dgo-msg` in load-dgo.gc, and
 * `RPC_Dgo_Cmd` in game/common/dgo_rpc_types.h without the padding the later games added. GOAL's
 * RPC buffer element is 32 bytes, so only these fields may be touched.
 */
struct DgoRpcCmd {
  u16 rsvd;
  u16 result;
  u32 buffer1;
  u32 buffer2;
  u32 buffer_heap_top;
  char name[16];
};
static_assert(sizeof(DgoRpcCmd) == 32, "GOAL's DGO RPC buffer element is 32 bytes");

goal_dgo_rpc_stats g_rpc_stats;

/*! Fill in the reply the same way the overlord does: where the object landed, and whether there
 *  are more. `buffer1` is where GOAL reads the address from, whichever buffer was actually used. */
void answer_with_next_object(DgoRpcCmd* reply) {
  u32 last = 0;
  const auto object = jak1::GetNextDGO(&last);
  if (!object.offset) {
    reply->result = DGO_RPC_RESULT_ERROR;
    return;
  }
  g_rpc_stats.dgo_objects++;
  reply->buffer1 = object.offset;
  reply->result = last ? DGO_RPC_RESULT_DONE : DGO_RPC_RESULT_MORE;
}

/*!
 * GOAL's stack-argument convention hands the callee all eight argument registers as an array. A
 * natively compiled caller arrives through the ordinary C convention, so the shim builds the array
 * out of the C arguments. Same shape as the one in game/kernel/jak1/kscheme.cpp.
 */
template <u64 (*F)(u64*)>
u64 stack_arg_shim(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
  u64 args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
  return F(args);
}

u64 dgo_rpc(u32 fno, u32 send_buffer, u32 recv_buffer) {
  DgoRpcCmd cmd;
  memcpy(&cmd, Ptr<u8>(send_buffer).c(), sizeof(cmd));

  switch (fno) {
    case DGO_RPC_LOAD_FNO: {
      g_rpc_stats.dgo_archives++;
      // GOAL builds the name from a level's nickname, so it arrives lowercase and without a
      // directory; the archives on disc are uppercase. This is the same kstrcpyup the C-driven
      // load does.
      char name[16];
      memcpy(name, cmd.name, sizeof(cmd.name));
      name[sizeof(name) - 1] = '\0';
      char upper[16];
      kstrcpyup(upper, name);
      jak1::BeginLoadingDGO(upper, Ptr<u8>(cmd.buffer1), Ptr<u8>(cmd.buffer2),
                            Ptr<u8>(cmd.buffer_heap_top));
      answer_with_next_object(&cmd);
      break;
    }
    case DGO_RPC_LOAD_NEXT_FNO:
      jak1::ContinueLoadingDGO(Ptr<u8>(cmd.buffer_heap_top));
      answer_with_next_object(&cmd);
      break;
    case DGO_RPC_CANCEL_FNO:
      close_dgo();
      cmd.result = DGO_RPC_RESULT_ABORTED;
      break;
    default:
      set_error(fmt::format("the DGO RPC was called with function number {}", fno));
      cmd.result = DGO_RPC_RESULT_ERROR;
      break;
  }

  memcpy(Ptr<u8>(recv_buffer).c(), &cmd, sizeof(cmd));
  return 0;
}

// ------------------------------------------------------------------------------------------------
// The STR RPC, answered the same way
//
// Channel 4 loads a whole file, or one chunk of a chunked one, straight into GOAL memory. The game
// uses it for the text and subtitle banks (`engine/ui/text.gc`, `pc/subtitle.gc`) and for spooled
// art - the animations in `engine/load/loader.gc`, which links whatever comes back. That last one
// is why this is implemented rather than stubbed: a stub that says nothing went wrong leaves the
// spool buffer holding uninitialized memory and the linker is handed it as an object file.
//
// The chunked format is `StrFileHeaderJ1` in game/common/str_rpc_types.h: the first sector is a
// table of each chunk's start sector and byte size. `chunk_id` of -1 means the file is not chunked
// and the whole thing is wanted.
// ------------------------------------------------------------------------------------------------

/*!
 * `load-chunk-msg` in engine/load/load-dgo.gc: 64 bytes, which is the RPC buffer's element size.
 * `RPC_Str_Cmd_Jak1` in game/common/str_rpc_types.h declares a 64-byte name and so is larger; only
 * these bytes are ever transferred.
 */
struct StrRpcCmd {
  u16 rsvd;
  u16 result;
  u32 ee_addr;
  s32 chunk_id;
  u32 length;
  char basename[48];
};
static_assert(sizeof(StrRpcCmd) == 64, "GOAL's STR RPC buffer element is 64 bytes");

/*! GOAL only reads back the reply's result and length, and asks for 32 bytes of it. */
constexpr int kStrRpcReplySize = 32;

/*! "NDINTRO STR" -> "NDINTRO.STR": an ISO name is 8 space-padded name characters then 3 of
 *  extension, and the files on disc are named the ordinary way. */
std::string file_name_of_iso_name(const char* iso_name) {
  std::string name(iso_name, 8);
  while (!name.empty() && name.back() == ' ') {
    name.pop_back();
  }
  std::string extension(iso_name + 8, 3);
  while (!extension.empty() && extension.back() == ' ') {
    extension.pop_back();
  }
  return name + "." + extension;
}

/*! Read `size` bytes at `offset` into GOAL memory. Returns what was read, or -1. */
s32 read_data_file(const std::string& relative, u32 goal_address, s32 offset, s32 size) {
  const s32 fd = ee::sceOpen(relative.c_str(), SCE_RDONLY);
  if (fd < 0) {
    return -1;
  }
  if (offset && ee::sceLseek(fd, offset, SCE_SEEK_SET) != offset) {
    ee::sceClose(fd);
    return -1;
  }
  const s32 read = ee::sceRead(fd, Ptr<u8>(goal_address).c(), size);
  ee::sceClose(fd);
  return read;
}

u64 str_rpc(u32 send_buffer, u32 recv_buffer) {
  StrRpcCmd cmd;
  memcpy(&cmd, Ptr<u8>(send_buffer).c(), sizeof(cmd));
  cmd.basename[sizeof(cmd.basename) - 1] = '\0';
  cmd.result = STR_RPC_RESULT_ERROR;

  if (cmd.chunk_id < 0) {
    // A whole ordinary file, named the way GOAL named it.
    char upper[64];
    kstrcpyup(upper, cmd.basename);
    const s32 read = read_data_file(std::string("iso/") + upper, cmd.ee_addr, 0, (s32)cmd.length);
    if (read > 0) {
      cmd.length = (u32)read;
      cmd.result = STR_RPC_RESULT_DONE;
    } else {
      cmd.length = 0;
      lg::warn("[str-loader] could not read iso/{}", upper);
    }
  } else if (cmd.chunk_id >= SECTOR_TABLE_SIZE) {
    set_error(fmt::format("the STR RPC was asked for chunk {} of '{}', and a chunked file has {}",
                          cmd.chunk_id, cmd.basename, SECTOR_TABLE_SIZE));
  } else {
    // A chunk of an animation. Its file is named after the animation by a rule of its own.
    char iso_name[16];
    file_util::ISONameFromAnimationName(iso_name, cmd.basename);
    const std::string relative = std::string("iso/") + file_name_of_iso_name(iso_name);

    StrFileHeaderJ1 header;
    const s32 fd = ee::sceOpen(relative.c_str(), SCE_RDONLY);
    if (fd < 0) {
      lg::warn("[str-loader] no animation file {} for '{}'", relative, cmd.basename);
      cmd.length = 0;
    } else {
      const bool got_header = ee::sceRead(fd, &header, sizeof(header)) == (s32)sizeof(header);
      ee::sceClose(fd);
      const u32 size = got_header ? header.sizes[cmd.chunk_id] : 0;
      const u32 sector = got_header ? header.sectors[cmd.chunk_id] : 0;
      if (!size) {
        lg::warn("[str-loader] {} has no chunk {}", relative, cmd.chunk_id);
        cmd.length = 0;
      } else if (size > cmd.length) {
        set_error(fmt::format("chunk {} of {} is {} bytes and GOAL offered {}", cmd.chunk_id,
                              relative, size, cmd.length));
        cmd.length = 0;
      } else if (read_data_file(relative, cmd.ee_addr, (s32)(sector * SECTOR_SIZE), (s32)size) !=
                 (s32)size) {
        lg::warn("[str-loader] short read of chunk {} of {}", cmd.chunk_id, relative);
        cmd.length = 0;
      } else {
        cmd.length = size;
        cmd.result = STR_RPC_RESULT_DONE;
      }
    }
  }

  if (cmd.result == STR_RPC_RESULT_DONE) {
    g_rpc_stats.str_reads++;
  } else {
    g_rpc_stats.str_failures++;
  }
  memcpy(Ptr<u8>(recv_buffer).c(), &cmd, kStrRpcReplySize);
  return 0;
}

// ------------------------------------------------------------------------------------------------
// The ramdisk RPC, answered the same way
//
// Channel 2 is the overlord's "server": upstream keeps a whole file in the IOP's spare RAM and
// hands the EE 2 kB windows of it on request (game/overlord/jak1/ramdisk.cpp). Only one file is
// ever in it, and only visibility uses it: `vis-load` in engine/level/level.gc sends fno 1 to load
// `<nickname>.VIS`, and `update-vis!` in engine/load/decomp.gc then sends fno 0 for the compressed
// vis string of the camera's current BSP leaf.
//
// This is implemented rather than stubbed because a stub that reports nothing wrong leaves the
// level's vis buffer holding zeroes, and `unpack-comp-huf` is then handed an all-zero bitstream:
// it walks the dictionary's zero branch until it happens to reach the terminator symbol, which for
// village1 is 7066 bytes into a 2 kB scratchpad buffer and for the title level 16472. The
// decompressed visibility is wrong (`update-vis!`'s own check reports it) and the overrun runs off
// the end of the fake scratchpad's decompression buffers and into the scratchpad process stacks.
//
// There is no IOP, so the file lives in host memory here and the RPC is answered where it is sent.
// ------------------------------------------------------------------------------------------------

/*!
 * `ramdisk-rpc-fill` and `ramdisk-rpc-load` in engine/load/ramdisk.gc, which are the same 32 bytes
 * as `RPC_Ramdisk_LoadCmd` in game/common/ramdisk_rpc_types.h. `id` is the ramdisk file id for
 * both; `offset` and `length` are only meaningful to fno 0 and `name` only to fno 1.
 */
struct RamdiskRpcCmd {
  u32 rsvd;
  u32 id;
  u32 offset;
  u32 length;
  char name[16];
};
static_assert(sizeof(RamdiskRpcCmd) == 32, "GOAL's ramdisk RPC buffer element is 32 bytes");

/*! Upstream's `gReturnBuffer` is this big and a larger request is refused. */
constexpr u32 kRamdiskMaxRead = 0x2000;

/*! The one file the ramdisk holds. Upstream's fno 1 resets it before loading, and so does this. */
struct {
  std::vector<u8> bytes;
  u32 id = 0;
} g_ramdisk;

void ramdisk_reset_and_load(const RamdiskRpcCmd& cmd) {
  g_ramdisk.bytes.clear();
  g_ramdisk.id = 0;

  char name[17];
  memcpy(name, cmd.name, sizeof(cmd.name));
  name[sizeof(cmd.name)] = '\0';
  char upper[17];
  kstrcpyup(upper, name);
  const std::string relative = std::string("iso/") + upper;

  const s32 fd = ee::sceOpen(relative.c_str(), SCE_RDONLY);
  if (fd < 0) {
    lg::warn("[ramdisk] cannot open {}", relative);
    g_rpc_stats.ramdisk_misses++;
    return;
  }
  const s32 size = ee::sceLseek(fd, 0, SCE_SEEK_END);
  ee::sceLseek(fd, 0, SCE_SEEK_SET);
  if (size <= 0) {
    ee::sceClose(fd);
    lg::warn("[ramdisk] {} is {} bytes", relative, size);
    g_rpc_stats.ramdisk_misses++;
    return;
  }
  g_ramdisk.bytes.resize((size_t)size);
  const s32 read = ee::sceRead(fd, g_ramdisk.bytes.data(), size);
  ee::sceClose(fd);
  if (read != size) {
    g_ramdisk.bytes.clear();
    lg::warn("[ramdisk] short read of {}: {} of {}", relative, read, size);
    g_rpc_stats.ramdisk_misses++;
    return;
  }
  g_ramdisk.id = cmd.id;
  g_rpc_stats.ramdisk_files++;
  lg::debug("[ramdisk] loaded {} ({} bytes) as id {}", relative, size, cmd.id);
}

/*! fno 0: hand back `length` bytes at `offset`. Upstream replies through the RPC's receive
 *  buffer, which is the EE address GOAL passed to `call`. */
void ramdisk_get_data(const RamdiskRpcCmd& cmd, u32 recv_buffer, u32 recv_size) {
  const u32 length = std::min(cmd.length, recv_size);
  if (!recv_buffer || !length) {
    return;
  }
  if (length > kRamdiskMaxRead) {
    set_error(fmt::format("the ramdisk was asked for {} bytes and its buffer is {}", length,
                          kRamdiskMaxRead));
    g_rpc_stats.ramdisk_misses++;
    return;
  }
  if (g_ramdisk.bytes.empty() || cmd.id != g_ramdisk.id) {
    lg::warn("[ramdisk] no file {} is loaded", cmd.id);
    g_rpc_stats.ramdisk_misses++;
    return;
  }
  if (cmd.offset >= g_ramdisk.bytes.size()) {
    lg::warn("[ramdisk] offset {} is past the end of a {}-byte file", cmd.offset,
             g_ramdisk.bytes.size());
    g_rpc_stats.ramdisk_misses++;
    return;
  }
  // `ramdisk-load` always asks for 2 kB, which decomp.gc calls "a worst case if the string can't
  // be compressed". A vis string near the end of the file is shorter than that, and upstream's
  // overlord hands back whatever follows it in the ramdisk. Give it the file and nothing else.
  const u32 available = (u32)std::min<size_t>(length, g_ramdisk.bytes.size() - cmd.offset);
  memcpy(Ptr<u8>(recv_buffer).c(), g_ramdisk.bytes.data() + cmd.offset, available);
  if (available < length) {
    memset(Ptr<u8>(recv_buffer).c() + available, 0, length - available);
  }
  g_rpc_stats.ramdisk_reads++;
}

u64 ramdisk_rpc(u32 fno, u32 send_buffer, u32 recv_buffer, u32 recv_size) {
  RamdiskRpcCmd cmd;
  memcpy(&cmd, Ptr<u8>(send_buffer).c(), sizeof(cmd));
  switch (fno) {
    case RAMDISK_RESET_AND_LOAD_FNO:
      ramdisk_reset_and_load(cmd);
      break;
    case RAMDISK_GET_DATA_FNO:
      ramdisk_get_data(cmd, recv_buffer, recv_size);
      break;
    default:
      // fno 4 loads a file straight to the EE; nothing in Jak 1's GOAL sends it.
      return goal_kernel_core_machine_stub_report("rpc-call (ramdisk, unimplemented fno)");
  }
  return 0;
}

/*!
 * `rpc-call`: channel, function number, async flag, send buffer and size, receive buffer and size,
 * as eight GOAL stack arguments.
 *
 * The three channels that read files are answered. The rest belong to subsystems this library does
 * not contain - sound (0, 1) and streamed-audio playback (5) - and a call on one is reported by the
 * machine-layer stub path rather than quietly succeeding.
 */
u64 goal_rpc_call(u64* args) {
  const s32 channel = (s32)args[0];
  if (channel == DGO_RPC_CHANNEL) {
    return dgo_rpc((u32)args[1], (u32)args[3], (u32)args[5]);
  }
  if (channel == STR_RPC_CHANNEL) {
    return str_rpc((u32)args[3], (u32)args[5]);
  }
  if (channel == RAMDISK_RPC_CHANNEL) {
    return ramdisk_rpc((u32)args[1], (u32)args[3], (u32)args[5], (u32)args[6]);
  }
  return goal_kernel_core_machine_stub_report(channel == 0 || channel == 1 ? "rpc-call (sound)"
                                              : channel == PLAY_RPC_CHANNEL
                                                  ? "rpc-call (streamed audio)"
                                                  : "rpc-call (unknown channel)");
}

/*!
 * `rpc-busy?`: always 0. Every RPC this library answers is answered inside `rpc-call`, so nothing
 * is ever still in flight. GOAL's `check-busy` and `sync` therefore always find the channel free,
 * which is what makes its loader advance one object per frame instead of spinning.
 */
u64 goal_rpc_busy(s32 channel) {
  (void)channel;
  return 0;
}

/*!
 * `link-begin`, with this platform's code/data rule applied.
 *
 * Upstream's `link_begin` (game/kernel/jak1/klink.cpp) hands every object to the linker, which for
 * a v3 object copies x86-64 machine code into the heap and returns its entry point. That is the
 * one thing this platform cannot do, so a v3 object takes the native translation instead - exactly
 * as the C-driven loop above does - and reports itself finished in one call, because there is no
 * incremental work left to spread over frames.
 *
 * Arguments are GOAL's: object data, name, size, heap, link flags.
 *
 * A code object with no translation cannot be reported through the return value: 0 means "call
 * link-resume again" and 1 means "linked", and there is no third answer GOAL understands. It ends
 * the run instead of letting the level load continue over a file that is not there.
 */
u64 goal_link_begin(u64* args) {
  const Ptr<u8> object_data(args[0]);
  const char* name = Ptr<char>(args[1]).c();
  const u32 flags = (u32)args[4];

  if (is_data_object(object_data)) {
    g_rpc_stats.linked_data_objects++;
    return jak1::link_begin(args);
  }
  g_rpc_stats.linked_code_objects++;
  if (!load_code_object(name, flags, true)) {
    lg::error("[dgo-loader] link-begin: {}", g_error);
    ASSERT_NOT_REACHED_MSG("link-begin was given a code object this build cannot supply");
  }
  return 1;
}

/*! `link-resume`. Only a data object ever leaves work behind for it; see goal_link_begin. */
u64 goal_link_resume() {
  return jak1::link_resume();
}

}  // namespace

extern "C" {

/*!
 * Give GOAL the loader entry points it drives a level DGO with. Called after the machine-layer
 * stubs are installed, so these replace the stubs for `rpc-call` and `rpc-busy?`, and after
 * `InitScheme`, so they replace upstream's `link-begin` and `link-resume`.
 */
void goal_dgo_goal_loader_stats(goal_dgo_rpc_stats* out) {
  *out = g_rpc_stats;
}

void goal_dgo_install_goal_loader(void) {
  g_rpc_stats = goal_dgo_rpc_stats();
  jak1::make_stack_arg_function_symbol_from_c("rpc-call", (void*)stack_arg_shim<goal_rpc_call>);
  jak1::make_function_symbol_from_c("rpc-busy?", (void*)goal_rpc_busy);
  jak1::make_stack_arg_function_symbol_from_c("link-begin", (void*)stack_arg_shim<goal_link_begin>);
  jak1::make_function_symbol_from_c("link-resume", (void*)goal_link_resume);
}

goal_kernel_core_status goal_dgo_load(const char* name,
                                      uint32_t link_flags,
                                      int32_t buffer_size,
                                      goal_dgo_load_stats* out) {
  if (!name || buffer_size <= 0) {
    set_error("goal_dgo_load: bad argument");
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  if (!goal_kernel_core_is_initialized()) {
    set_error("goal_dgo_load: the kernel is not initialized");
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }

  jak1::load_and_link_dgo_from_c(name, kglobalheap, link_flags, buffer_size, true);
  if (out) {
    *out = g_stats;
  }
  if (!g_error.empty()) {
    return GOAL_KERNEL_CORE_NOT_FOUND;
  }
  return GOAL_KERNEL_CORE_OK;
}

void goal_dgo_set_verbose(int on) {
  g_verbose = on != 0;
}

const char* goal_dgo_last_error(void) {
  return g_error.c_str();
}

}  // extern "C"
