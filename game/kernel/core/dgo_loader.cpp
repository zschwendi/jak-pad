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
 */

#include <cstdio>
#include <cstring>
#include <string>

#include "common/link_types.h"
#include "common/log/log.h"

#include "game/kernel/common/fileio.h"
#include "game/kernel/common/kdgo.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/dgo_loader.h"
#include "game/kernel/jak1/kdgo.h"
#include "game/kernel/jak1/klink.h"
#include "game/sce/sif_ee.h"

#include "fmt/format.h"

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

    const auto* header = (const LinkHeaderV2*)obj.c();
    const bool is_data = header->type_tag == 0xffffffff && (header->version == 2 ||
                                                            header->version == 4);
    if (g_verbose) {
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
      const goal_aot_object_file* aot = goal_aot_registered_object(objName);
      if (!aot) {
        set_error(fmt::format("{} holds the code object '{}', which has no native translation",
                              fileName, objName));
        g_dgo.failed = true;
        break;
      }
      if (goal_aot_is_loaded(aot->tag)) {
        // an earlier DGO already brought this file in; upstream would relink it, but the native
        // code and its statics are already in the heap and running the top-level twice is not the
        // same as loading it once.
        g_stats.reused_code++;
      } else {
        if (goal_aot_load(aot) != GOAL_KERNEL_CORE_OK) {
          set_error(fmt::format("could not load the native translation of '{}': {}", objName,
                                goal_kernel_core_last_error()));
          g_dgo.failed = true;
          break;
        }
        if (linkFlag & LINK_FLAG_EXECUTE) {
          if (goal_aot_run_top_level(aot->tag, nullptr) != GOAL_KERNEL_CORE_OK) {
            set_error(fmt::format("the top-level of '{}' failed: {}", objName,
                                  goal_kernel_core_last_error()));
            g_dgo.failed = true;
            break;
          }
        }
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

extern "C" {

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
