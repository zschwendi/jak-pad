#include "kernel_core.h"

#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/mips2c_seam.h"

#include <cstring>
#include <string>

#include <sys/mman.h>

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/symbols.h"

#include "game/kernel/common/kboot.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kmemcard.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/common/memory_layout.h"
#include "game/kernel/core/kernel_game.h"
#include "game/runtime.h"
#include "game/sce/libscf.h"

// defined in desktop_seams.cpp, next to the machine-layer stubs it controls
void goal_kernel_core_set_machine_stub_mode(bool abort_when_called);

// These globals normally live in game/runtime.cpp, which is the desktop runtime entry point and
// is not part of this library. The kernel reaches them through game/runtime.h.
u8* g_ee_main_mem = nullptr;
GameVersion g_game_version = goal_game_version();

namespace {

bool g_initialized = false;
std::string g_last_error;
std::string g_data_directory;
std::string g_saves_directory;

// Kernel-core GOAL code is ahead-of-time compiled into the signed image. The arena contains only
// data, function objects that hold native __TEXT pointers, and GOAL stacks.
constexpr int kMainMemoryProtection = PROT_READ | PROT_WRITE;
static_assert((kMainMemoryProtection & PROT_EXEC) == 0,
              "the portable kernel core must never request an executable GOAL arena");

void set_error(const char* msg) {
  g_last_error = msg;
  lg::error("[kernel-core] {}", msg);
}

void clear_error() {
  g_last_error.clear();
}

/*!
 * Map EE main memory as data. Desktop OpenGOAL stores generated code in this arena, but the
 * portable kernel core does not: AOT and mips2c function objects store pointers to signed native
 * entry points instead. Requesting PROT_EXEC as a probe is unsafe because some Apple device builds
 * grant it, leaving the live heap and GOAL stacks writable and executable even though no code uses
 * that permission.
 */
bool map_main_memory() {
  void* mem = mmap(nullptr, EE_MAIN_MEM_SIZE, kMainMemoryProtection, MAP_ANONYMOUS | MAP_PRIVATE,
                   -1, 0);
  if (mem == MAP_FAILED) {
    return false;
  }
  g_ee_main_mem = (u8*)mem;
  memset(g_ee_main_mem, 0, EE_MAIN_MEM_SIZE);
  // The PS2 kernel lives in the low 512 kB and GOAL never touches it. Trapping it turns GOAL null
  // pointer dereferences into a crash instead of silent corruption.
  mprotect(g_ee_main_mem, EE_MAIN_MEM_LOW_PROTECT, PROT_NONE);
  return true;
}

/*!
 * The boot configuration block from jak1::goal_main (game/kernel/jak1/kboot.cpp), which is the
 * desktop entry point and is not part of this library. `scf-get-volume`, `scf-get-language` and
 * `scf-get-aspect` read it, and GOAL reads those once, when it builds *setting-control*.
 *
 * The volume matters most. settings.gc derives every volume the game does not keep on the memory
 * card from `(scf-get-volume)`: the ambient group, and the movie/hint volumes that ambient speech
 * and cutscenes apply as a percentage. With this left at zero the ambient sound group is muted
 * outright and every ambient-speech window multiplies the sfx, music and dialog volumes by zero.
 */
void init_boot_config() {
  masterConfig.aspect = (u16)ee::sceScfGetAspect();
  masterConfig.language = (u16)ee::sceScfGetLanguage();
  masterConfig.inactive_timeout = 0;
  masterConfig.timeout = 0;
  masterConfig.volume = 100;

  switch (masterConfig.language) {
    case SCE_SPANISH_LANGUAGE:
      masterConfig.language = (u16)Language::Spanish;
      break;
    case SCE_FRENCH_LANGUAGE:
      masterConfig.language = (u16)Language::French;
      break;
    case SCE_GERMAN_LANGUAGE:
      masterConfig.language = (u16)Language::German;
      break;
    case SCE_ITALIAN_LANGUAGE:
      masterConfig.language = (u16)Language::Italian;
      break;
    case SCE_JAPANESE_LANGUAGE:
      masterConfig.language = (u16)Language::Japanese;
      break;
    default:
      masterConfig.language = (u16)Language::English;
      break;
  }
}

/*!
 * The equivalent of the heap setup at the top of jak1::InitMachine. The machine layer itself
 * (IOP, video, sound, listener) is not part of this library.
 */
void init_heaps() {
  const u32 global_heap_size = GLOBAL_HEAP_END - HEAP_START;
  kinitheap(kglobalheap, Ptr<u8>(HEAP_START), global_heap_size);

  const u32 debug_heap_end = (0xffffffff - DEBUG_HEAP_SPACE_FOR_STACK + 1) & 0x7ffffff;
  kinitheap(kdebugheap, Ptr<u8>(DEBUG_HEAP_START), debug_heap_end - DEBUG_HEAP_START);
}

}  // namespace

extern "C" {

goal_kernel_core_status goal_kernel_core_initialize(void) {
  if (g_initialized) {
    set_error("already initialized");
    return GOAL_KERNEL_CORE_ALREADY_INITIALIZED;
  }
  clear_error();

  if (!map_main_memory()) {
    set_error("failed to map EE main memory");
    return GOAL_KERNEL_CORE_MAIN_MEMORY_FAILED;
  }

  goal_game_init_kernel_globals();
  init_boot_config();

  // No compiler is connected and none can be: the listener transport is not part of this library.
  // MasterDebug drives the listener buffers and the debug-segment symbol, so it stays off.
  MasterDebug = 0;
  DebugSegment = 0;
  // No DGO data is loaded, so the GOAL kernel object file is not linked in.
  MasterUseKernel = 0;

  init_heaps();
  init_output();
  clear_print();

  const s32 status = goal_game_init_symbol_and_types();
  if (status < 0) {
    set_error("InitSymbolAndTypes failed");
    munmap(g_ee_main_mem, EE_MAIN_MEM_SIZE);
    g_ee_main_mem = nullptr;
    return GOAL_KERNEL_CORE_SYMBOL_INIT_FAILED;
  }

  // The hand-translated PS2 assembly functions GOAL's `def-mips2c` asks for by name. Upstream
  // registers each file's as that file is linked; this runtime does it once, here. See
  // mips2c_seam.cpp.
  goal_game_register_mips2c();

  g_initialized = true;
  return GOAL_KERNEL_CORE_OK;
}

void goal_kernel_core_shutdown(void) {
  // every AOT object file and every mips2c trampoline was placed in the heap that is about to go
  // away
  goal_aot_reset();
  goal_mips2c_reset();
  if (!g_ee_main_mem) {
    g_initialized = false;
    return;
  }
  mprotect(g_ee_main_mem, EE_MAIN_MEM_LOW_PROTECT, PROT_READ | PROT_WRITE);
  munmap(g_ee_main_mem, EE_MAIN_MEM_SIZE);
  g_ee_main_mem = nullptr;
  g_initialized = false;
  s7.offset = 0;
  SymbolTable2.offset = 0;
  LastSymbol.offset = 0;
  NumSymbols = 0;
  kglobalheap.offset = 0;
  kdebugheap.offset = 0;
  clear_error();
}

int goal_kernel_core_is_initialized(void) {
  return g_initialized ? 1 : 0;
}

goal_kernel_core_status goal_kernel_core_stub_machine_layer(int abort_when_called) {
  if (!g_initialized) {
    set_error("goal_kernel_core_stub_machine_layer: not initialized");
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }
  goal_kernel_core_set_machine_stub_mode(abort_when_called != 0);
  goal_game_init_machine_scheme();
  return GOAL_KERNEL_CORE_OK;
}

goal_kernel_core_status goal_kernel_core_get_state(goal_kernel_core_state* out) {
  if (!out) {
    set_error("goal_kernel_core_get_state: out is NULL");
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  if (!g_initialized) {
    set_error("goal_kernel_core_get_state: not initialized");
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }

  memset(out, 0, sizeof(*out));
  out->main_memory_address = (uint64_t)g_ee_main_mem;
  out->main_memory_size = EE_MAIN_MEM_SIZE;
  out->main_memory_executable = 0;

  out->global_heap_base_offset = kglobalheap->base.offset;
  out->global_heap_current_offset = kglobalheap->current.offset;
  out->global_heap_top_offset = kglobalheap->top.offset;
  out->global_heap_top_base_offset = kglobalheap->top_base.offset;
  out->global_heap_used_bytes = kheapused(kglobalheap);

  out->debug_heap_base_offset = kdebugheap.offset ? kdebugheap->base.offset : 0;
  out->debug_heap_top_base_offset = kdebugheap.offset ? kdebugheap->top_base.offset : 0;

  out->symbol_table_offset = SymbolTable2.offset;
  out->s7_offset = s7.offset;
  out->last_symbol_offset = LastSymbol.offset;
  out->symbol_count = NumSymbols;

  out->empty_pair_offset = goal_game_empty_pair_offset();
  out->false_offset = goal_game_false_offset();
  out->true_offset = goal_game_true_offset();
  return GOAL_KERNEL_CORE_OK;
}

goal_kernel_core_status goal_kernel_core_global_alloc(int32_t size,
                                                      const char* name,
                                                      uint32_t* out_offset) {
  if (!out_offset || size <= 0) {
    set_error("goal_kernel_core_global_alloc: bad argument");
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  if (!g_initialized) {
    set_error("goal_kernel_core_global_alloc: not initialized");
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }
  auto mem = kmalloc(kglobalheap, size, KMALLOC_MEMSET, name ? name : "bridge-alloc");
  if (!mem.offset) {
    set_error("goal_kernel_core_global_alloc: global heap is full");
    return GOAL_KERNEL_CORE_OUT_OF_MEMORY;
  }
  *out_offset = mem.offset;
  return GOAL_KERNEL_CORE_OK;
}

goal_kernel_core_status goal_kernel_core_intern(const char* name, uint32_t* out_symbol_offset) {
  if (!name || !out_symbol_offset) {
    set_error("goal_kernel_core_intern: bad argument");
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  if (!g_initialized) {
    set_error("goal_kernel_core_intern: not initialized");
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }
  *out_symbol_offset = goal_game_intern(name);
  return GOAL_KERNEL_CORE_OK;
}

goal_kernel_core_status goal_kernel_core_lookup(const char* name,
                                                uint32_t* out_symbol_offset,
                                                uint32_t* out_value) {
  if (!name) {
    set_error("goal_kernel_core_lookup: bad argument");
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  if (!g_initialized) {
    set_error("goal_kernel_core_lookup: not initialized");
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }
  uint32_t value = 0;
  const uint32_t sym = goal_game_find_symbol(name, &value);
  if (!sym) {
    set_error("goal_kernel_core_lookup: symbol not found");
    return GOAL_KERNEL_CORE_NOT_FOUND;
  }
  if (out_symbol_offset) {
    *out_symbol_offset = sym;
  }
  if (out_value) {
    *out_value = value;
  }
  return GOAL_KERNEL_CORE_OK;
}

goal_kernel_core_status goal_kernel_core_type_name_of_symbol(const char* name,
                                                             char* buffer,
                                                             size_t buffer_size) {
  if (!name || !buffer || buffer_size == 0) {
    set_error("goal_kernel_core_type_name_of_symbol: bad argument");
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  if (!g_initialized) {
    set_error("goal_kernel_core_type_name_of_symbol: not initialized");
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }
  const char* type_name = goal_game_type_name_of_symbol(name);
  if (!type_name) {
    set_error("goal_kernel_core_type_name_of_symbol: no such symbol, or it holds no type");
    return GOAL_KERNEL_CORE_NOT_FOUND;
  }
  strncpy(buffer, type_name, buffer_size - 1);
  buffer[buffer_size - 1] = '\0';
  return GOAL_KERNEL_CORE_OK;
}

goal_kernel_core_status goal_kernel_core_set_data_directory(const char* path) {
  g_data_directory = path ? path : "";
  while (g_data_directory.size() > 1 && g_data_directory.back() == '/') {
    g_data_directory.pop_back();
  }
  return GOAL_KERNEL_CORE_OK;
}

const char* goal_kernel_core_data_directory(void) {
  return g_data_directory.c_str();
}

goal_kernel_core_status goal_kernel_core_set_saves_directory(const char* path) {
  g_saves_directory = path ? path : "";
  while (g_saves_directory.size() > 1 && g_saves_directory.back() == '/') {
    g_saves_directory.pop_back();
  }
  kmemcard_set_directory(g_saves_directory.c_str());
  return GOAL_KERNEL_CORE_OK;
}

const char* goal_kernel_core_saves_directory(void) {
  return g_saves_directory.c_str();
}

goal_kernel_core_status goal_kernel_core_resolve_data_path(const char* name,
                                                           char* out,
                                                           size_t out_size) {
  if (!name || !out || out_size == 0) {
    set_error("goal_kernel_core_resolve_data_path: bad argument");
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  std::string resolved = name;
  if (resolved.empty() || resolved.front() != '/') {
    if (g_data_directory.empty()) {
      set_error("no data directory is set; call goal_kernel_core_set_data_directory first");
      return GOAL_KERNEL_CORE_NOT_FOUND;
    }
    resolved = g_data_directory + "/" + resolved;
  }
  if (resolved.size() + 1 > out_size) {
    set_error("goal_kernel_core_resolve_data_path: path does not fit");
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  memcpy(out, resolved.c_str(), resolved.size() + 1);
  return GOAL_KERNEL_CORE_OK;
}

const char* goal_kernel_core_last_error(void) {
  return g_last_error.c_str();
}

}  // extern "C"
