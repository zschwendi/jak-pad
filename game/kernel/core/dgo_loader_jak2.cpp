/*!
 * @file dgo_loader_jak2.cpp
 * The Jak 2 DGO seam for a platform with no IOP. Compiled only into jak2-kernel-core.
 *
 * The same design as dgo_loader.cpp, which explains it in full: the archive is read with ordinary
 * file calls against the player's data directory, and the code/data rule is applied to every
 * object - **code comes from the AOT path; data comes from the DGO.** A v3 object with no
 * registered translation unit is a failure, never a skip.
 *
 * Both loader entry paths use that rule: the host's C-driven `goal_dgo_load`, and GOAL's own
 * one-object-per-frame channel-3 DGO RPC plus `link-begin` / `link-resume` state machine.
 */

#include <cstdio>
#include <cstring>
#include <string>

#include "common/link_types.h"
#include "common/log/log.h"
#include "common/goal_constants.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"

#include "game/common/dgo_rpc_types.h"
#include "game/kernel/common/fileio.h"
#include "game/kernel/common/kdgo.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/dgo_loader.h"
#include "game/kernel/core/sound_rpc_jak2.h"
#include "game/kernel/jak2/kdgo.h"
#include "game/kernel/jak2/klink.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/runtime.h"
#include "game/sce/sif_ee.h"

#include "fmt/format.h"

// Defined beside the machine stubs in desktop_seams.cpp.
u64 goal_kernel_core_machine_stub_report(const char* what);

namespace {

std::string g_error;
goal_dgo_load_stats g_stats;
goal_dgo_rpc_stats g_rpc_stats;
bool g_verbose = false;

void set_error(const std::string& message) {
  g_error = message;
  lg::error("[dgo-loader] {}", message);
}

bool readable_ee_span(u32 address, u32 size) {
  return g_ee_main_mem && address >= (u32)EE_MAIN_MEM_LOW_PROTECT &&
         address <= (u32)EE_MAIN_MEM_SIZE && size <= (u32)EE_MAIN_MEM_SIZE - address;
}

/*! The archive being read. One at a time, which is all the C-driven load ever asks for. */
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

void record_dgo_result(int result) {
  g_rpc_stats.last_dgo_result = result;
  if (g_dgo.fd < 0) {
    g_rpc_stats.current_dgo_name[0] = '\0';
  }
  if (result == DGO_RPC_RESULT_ERROR) {
    g_rpc_stats.dgo_failures++;
    const char* error = g_error.empty() ? "DGO RPC returned an error" : g_error.c_str();
    std::snprintf(g_rpc_stats.last_dgo_error, sizeof(g_rpc_stats.last_dgo_error), "%s", error);
  }
}

void close_dgo() {
  if (g_dgo.fd >= 0) {
    ee::sceClose(g_dgo.fd);
  }
  g_dgo = DgoRead();
}

/*!
 * Read one object into the buffer the overlord would have put it in: the last object goes to the
 * top of the heap so it can be linked in place; everything else alternates between the two load
 * buffers.
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

  const u64 padded64 = ((u64)header.size + 0xf) & ~0xfull;
  if (padded64 > (u64)EE_MAIN_MEM_SIZE - sizeof(header) ||
      !readable_ee_span(dest.offset, (u32)(sizeof(header) + padded64))) {
    set_error(fmt::format("object {} ({}) does not fit at #x{:x}", g_dgo.objects_read,
                          header.name, dest.offset));
    g_dgo.failed = true;
    return false;
  }
  const s32 padded = (s32)padded64;
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

bool is_data_object(Ptr<u8> object) {
  const auto* header = (const LinkHeaderV2*)object.c();
  return header->type_tag == 0xffffffff && (header->version == 2 || header->version == 4);
}

/*!
 * Put the native translation of a code object in the heap and run its top-level, exactly as
 * dgo_loader.cpp does for jak1. Returns false and sets the error when there is no translation.
 */
bool load_code_object(const char* object_name,
                      u32 link_flags,
                      bool on_goal_stack,
                      u32 heap,
                      bool boot_method_propagation) {
  const goal_aot_object_file* aot = goal_aot_registered_object(object_name);
  if (!aot) {
    set_error(fmt::format("the code object '{}' has no native translation", object_name));
    return false;
  }
  const bool global = heap == 0 || heap == kglobalheap.offset;
  if (goal_aot_is_loaded(aot->tag)) {
    if (global) {
      g_stats.reused_code++;
      return true;
    }
    goal_aot_forget(aot->tag);
  }
  const u32 before = global ? 0 : Ptr<kheapinfo>(heap)->current.offset;
  if (goal_aot_load_into(aot, heap) != GOAL_KERNEL_CORE_OK) {
    set_error(fmt::format("could not load the native translation of '{}': {}", object_name,
                          goal_kernel_core_last_error()));
    return false;
  }
  if (!global) {
    g_rpc_stats.level_code_bytes += Ptr<kheapinfo>(heap)->current.offset - before;
  }
  if (link_flags & LINK_FLAG_EXECUTE) {
    const auto status =
        boot_method_propagation
            ? (on_goal_stack ? goal_aot_run_top_level_here(aot->tag, nullptr)
                             : goal_aot_run_top_level(aot->tag, nullptr))
            : (on_goal_stack ? goal_aot_run_top_level_here_for_link(aot->tag, link_flags, nullptr)
                             : goal_aot_run_top_level_for_link(aot->tag, link_flags, nullptr));
    if (status != GOAL_KERNEL_CORE_OK) {
      set_error(fmt::format("the top-level of '{}' failed: {}", object_name,
                            goal_kernel_core_last_error()));
      return false;
    }
  }
  return true;
}

void begin_loading_dgo(const char* name, Ptr<u8> buffer1, Ptr<u8> buffer2, Ptr<u8> currentHeap) {
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

Ptr<u8> get_next_dgo(u32* lastObjectFlag) {
  *lastObjectFlag = 1;
  if (g_dgo.failed || !g_dgo.most_recent.offset) {
    return Ptr<u8>(0);
  }
  *lastObjectFlag = g_dgo.objects_read == g_dgo.object_count ? 1 : 0;
  auto buffer = g_dgo.most_recent;
  g_dgo.most_recent.offset = 0;
  return buffer;
}

}  // namespace

namespace jak2 {

void kdgo_init_globals() {
  close_dgo();
  g_error.clear();
}

/*!
 * Load and link a DGO file. Blocks until the whole archive is done. Upstream's loop
 * (game/kernel/jak2/kdgo.cpp) with one branch added: an object whose code this platform cannot
 * execute is taken from the AOT path instead of from the archive.
 */
static void load_and_link_dgo_from_c_impl(const char* name,
                                          Ptr<kheapinfo> heap,
                                          u32 linkFlag,
                                          s32 bufferSize,
                                          bool jump_from_c_to_goal,
                                          bool boot_method_propagation) {
  g_error.clear();
  memset(&g_stats, 0, sizeof(g_stats));
  g_stats.heap_used_before = kheapused(heap);

  // remember where the heap top point is so we can clear temporary allocations
  auto oldHeapTop = heap->top;

  // allocate temporary buffers from top of the given heap; both named dgo-buffer-2 upstream
  auto buffer2 = kmalloc(heap, bufferSize, KMALLOC_TOP | KMALLOC_ALIGN_64, "dgo-buffer-2");
  auto buffer1 = kmalloc(heap, bufferSize, KMALLOC_TOP | KMALLOC_ALIGN_64, "dgo-buffer-2");
  if (!buffer1.offset || !buffer2.offset) {
    set_error(fmt::format("no room for two {}-byte DGO load buffers", bufferSize));
    heap->top = oldHeapTop;
    return;
  }

  // build filename. If no extension is given, default to CGO.
  char fileName[16];
  kstrcpyup(fileName, name);
  if (fileName[strlen(fileName) - 4] != '.') {
    strcat(fileName, ".CGO");
  }

  begin_loading_dgo(fileName, buffer1, buffer2,
                    Ptr<u8>((heap->current + 0x3f).offset & 0xffffffc0));

  u32 lastObjectLoaded = 0;
  while (!lastObjectLoaded && !g_dgo.failed) {
    auto dgoObj = get_next_dgo(&lastObjectLoaded);
    if (!dgoObj.offset) {
      break;
    }

    // the last object is loaded at heap->current, so the two dgo-buffer allocations can be
    // released before linking it, giving the last file the whole heap
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
      if (!load_code_object(objName, linkFlag, false, heap.offset, boot_method_propagation)) {
        g_dgo.failed = true;
        break;
      }
    }

    if (!lastObjectLoaded) {
      g_dgo.heap_top = Ptr<u8>((heap->current + 0x3f).offset & 0xffffffc0);
      read_next_object();
    }
  }

  heap->top = oldHeapTop;
  g_stats.heap_used_after = kheapused(heap);
  close_dgo();
}

void load_and_link_dgo_from_c(const char* name,
                              Ptr<kheapinfo> heap,
                              u32 linkFlag,
                              s32 bufferSize,
                              bool jump_from_c_to_goal) {
  load_and_link_dgo_from_c_impl(name, heap, linkFlag, bufferSize, jump_from_c_to_goal, false);
}

void load_and_link_dgo_from_c_fast(const char* name,
                                   Ptr<kheapinfo> heap,
                                   u32 linkFlag,
                                   s32 bufferSize) {
  // upstream's fast path skips the IOP round trips; this loader has none to skip
  load_and_link_dgo_from_c(name, heap, linkFlag, bufferSize, true);
}

/*!
 * GOAL's `dgo-load`.
 */
void load_and_link_dgo(u64 name_gstr, u64 heap_info, u64 flag, u64 buffer_size) {
  auto name = Ptr<char>((u32)name_gstr + 4).c();
  auto heap = Ptr<kheapinfo>((u32)heap_info);
  load_and_link_dgo_from_c(name, heap, (u32)flag, (s32)buffer_size, false);
}

}  // namespace jak2

// ================================================================================================
// GOAL's channel-3 DGO RPC and incremental linker entry points
// ================================================================================================

namespace {

struct DgoRpcCmd {
  u16 rsvd;
  u16 result;
  u32 buffer1;
  u32 buffer2;
  u32 buffer_heap_top;
  char name[16];
};
static_assert(sizeof(DgoRpcCmd) == 32, "Jak 2's DGO RPC wire element is exactly 32 bytes");

template <u64 (*Function)(const u64*)>
u64 stack_arg_shim(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
  const u64 args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
  return Function(args);
}

bool dgo_name(const char source[16], std::string* out) {
  size_t length = 0;
  while (length < 16 && source[length]) {
    length++;
  }
  if (!length) {
    return false;
  }

  out->assign(source, length);
  if (*out == "." || *out == "..") {
    return false;
  }
  for (char& c : *out) {
    if (c == '/' || c == '\\' || c == ':') {
      return false;
    }
    if (c >= 'a' && c <= 'z') {
      c -= 'a' - 'A';
    }
  }
  return true;
}

void answer_with_next_object(DgoRpcCmd* reply) {
  u32 last = 0;
  const auto object = get_next_dgo(&last);
  if (!object.offset) {
    reply->result = DGO_RPC_RESULT_ERROR;
    close_dgo();
    return;
  }

  g_rpc_stats.dgo_objects++;
  reply->buffer1 = object.offset;
  reply->result = last ? DGO_RPC_RESULT_DONE : DGO_RPC_RESULT_MORE;
  if (last) {
    close_dgo();
  }
}

u64 dgo_rpc(u32 function,
            u32 send_buffer,
            s32 send_size,
            u32 recv_buffer,
            s32 recv_size) {
  if (send_size != (s32)sizeof(DgoRpcCmd) || recv_size != (s32)sizeof(DgoRpcCmd) ||
      (send_buffer & 0xf) || (recv_buffer & 0xf) ||
      !readable_ee_span(send_buffer, sizeof(DgoRpcCmd)) ||
      !readable_ee_span(recv_buffer, sizeof(DgoRpcCmd))) {
    return goal_kernel_core_machine_stub_report("rpc-call (Jak 2 DGO, malformed buffers)");
  }

  DgoRpcCmd cmd;
  memcpy(&cmd, Ptr<u8>(send_buffer).c(), sizeof(cmd));
  switch (function) {
    case DGO_RPC_LOAD_FNO: {
      g_error.clear();
      std::string name;
      if (!dgo_name(cmd.name, &name)) {
        g_rpc_stats.dgo_archives++;
        set_error("the Jak 2 DGO RPC was given an invalid archive name");
        cmd.result = DGO_RPC_RESULT_ERROR;
        close_dgo();
        break;
      }
      if (g_rpc_stats.dgo_archives == 0) {
        std::snprintf(g_rpc_stats.first_dgo_name, sizeof(g_rpc_stats.first_dgo_name), "%s",
                      name.c_str());
      }
      g_rpc_stats.dgo_archives++;
      std::snprintf(g_rpc_stats.current_dgo_name, sizeof(g_rpc_stats.current_dgo_name), "%s",
                    name.c_str());
      std::snprintf(g_rpc_stats.last_dgo_name, sizeof(g_rpc_stats.last_dgo_name), "%s",
                    name.c_str());
      begin_loading_dgo(name.c_str(), Ptr<u8>(cmd.buffer1), Ptr<u8>(cmd.buffer2),
                        Ptr<u8>(cmd.buffer_heap_top));
      answer_with_next_object(&cmd);
      break;
    }
    case DGO_RPC_LOAD_NEXT_FNO:
      if (g_dgo.fd < 0 || g_dgo.failed) {
        if (g_error.empty()) {
          set_error("the Jak 2 DGO RPC was asked for another object without an active archive");
        }
        cmd.result = DGO_RPC_RESULT_ERROR;
        close_dgo();
        break;
      }
      // Jak 2 may borrow different buffers between objects, unlike Jak 1.
      g_dgo.buffer1 = Ptr<u8>(cmd.buffer1);
      g_dgo.buffer2 = Ptr<u8>(cmd.buffer2);
      g_dgo.heap_top = Ptr<u8>(cmd.buffer_heap_top);
      if (!read_next_object()) {
        if (g_error.empty()) {
          set_error("the Jak 2 DGO RPC could not read the next archive object");
        }
        cmd.result = DGO_RPC_RESULT_ERROR;
        close_dgo();
        break;
      }
      answer_with_next_object(&cmd);
      break;
    case DGO_RPC_CANCEL_FNO:
      close_dgo();
      cmd.result = DGO_RPC_RESULT_ABORTED;
      break;
    default:
      set_error(fmt::format("the Jak 2 DGO RPC was called with function number {}", function));
      cmd.result = DGO_RPC_RESULT_ERROR;
      break;
  }

  record_dgo_result(cmd.result);
  memcpy(Ptr<u8>(recv_buffer).c(), &cmd, sizeof(cmd));
  return 0;
}

u64 goal_rpc_call(const u64* args) {
  if (!args) {
    return goal_kernel_core_machine_stub_report("rpc-call (Jak 2 DGO, missing arguments)");
  }
  if ((s32)args[0] == DGO_RPC_CHANNEL) {
    return dgo_rpc((u32)args[1], (u32)args[3], (s32)args[4], (u32)args[5], (s32)args[6]);
  }
  return goal_jak2_sound_rpc_call(args);
}

u64 goal_rpc_busy(u64 channel) {
  if ((s32)channel == DGO_RPC_CHANNEL) {
    return 0;
  }
  return goal_jak2_sound_rpc_busy((s32)channel);
}

u64 goal_link_begin(const u64* args) {
  const Ptr<u8> object_data((u32)args[0]);
  const char* name = Ptr<char>((u32)args[1]).c();
  const u32 flags = (u32)args[4];

  if (is_data_object(object_data)) {
    g_rpc_stats.linked_data_objects++;
    return jak2::link_begin(const_cast<u64*>(args));
  }

  g_rpc_stats.linked_code_objects++;
  if (!load_code_object(name, flags, true, (u32)args[3], false)) {
    lg::error("[dgo-loader] link-begin: {}", g_error);
    ASSERT_NOT_REACHED_MSG("link-begin was given a code object this build cannot supply");
  }
  return 1;
}

u64 goal_link_resume() {
  return jak2::link_resume();
}

}  // namespace

// ================================================================================================
// The C entry points the host boot drives (dgo_loader.h)
// ================================================================================================

extern "C" {

void goal_dgo_goal_loader_stats(goal_dgo_rpc_stats* out) {
  if (out) {
    *out = g_rpc_stats;
  }
}

void goal_dgo_install_goal_loader(void) {
  g_rpc_stats = {};
  g_rpc_stats.last_dgo_result = DGO_RPC_RESULT_INIT;
  jak2::make_stack_arg_function_symbol_from_c("rpc-call", (void*)stack_arg_shim<goal_rpc_call>);
  jak2::make_function_symbol_from_c("rpc-busy?", (void*)goal_rpc_busy);
  jak2::make_stack_arg_function_symbol_from_c("link-begin", (void*)stack_arg_shim<goal_link_begin>);
  jak2::make_function_symbol_from_c("link-resume", (void*)goal_link_resume);
}

static goal_kernel_core_status goal_dgo_load_impl(const char* name,
                                                  uint32_t link_flags,
                                                  int32_t buffer_size,
                                                  goal_dgo_load_stats* out,
                                                  bool boot_method_propagation) {
  if (!name || buffer_size <= 0) {
    set_error("goal_dgo_load: bad argument");
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  if (!goal_kernel_core_is_initialized()) {
    set_error("goal_dgo_load: the kernel is not initialized");
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }
  g_error.clear();
  jak2::load_and_link_dgo_from_c_impl(name, kglobalheap, link_flags, buffer_size, true,
                                      boot_method_propagation);
  if (out) {
    *out = g_stats;
  }
  return g_error.empty() ? GOAL_KERNEL_CORE_OK : GOAL_KERNEL_CORE_NOT_FOUND;
}

goal_kernel_core_status goal_dgo_load(const char* name,
                                      uint32_t link_flags,
                                      int32_t buffer_size,
                                      goal_dgo_load_stats* out) {
  return goal_dgo_load_impl(name, link_flags, buffer_size, out, false);
}

goal_kernel_core_status goal_jak2_dgo_load_boot(const char* name,
                                                uint32_t link_flags,
                                                int32_t buffer_size,
                                                goal_dgo_load_stats* out) {
  return goal_dgo_load_impl(name, link_flags, buffer_size, out, true);
}

void goal_dgo_set_verbose(int on) {
  g_verbose = on != 0;
}

const char* goal_dgo_last_error(void) {
  return g_error.c_str();
}

}  // extern "C"
