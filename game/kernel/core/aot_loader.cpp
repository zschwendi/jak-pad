#include "aot_loader.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/symbols.h"
#include "common/util/Assert.h"

#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

// The GOAL machine state the emitted C reads. goal_c_runtime.h declares them; the loader owns them.
extern "C" {
uint8_t* g_goal_mem = nullptr;
uint64_t g_goal_s7 = 0;
}

namespace {

/*! Room for the 64-bit native entry point, rounded up like every other heap object. */
constexpr u32 AOT_FUNCTION_OBJECT_SIZE = 0x10;

struct LoadedFile {
  std::string tag;
  std::vector<u32> static_addrs;
  std::vector<u32> function_addrs;
};

std::vector<LoadedFile> g_loaded;
std::string g_error;

/*! DGO object name -> the AOT translation unit that stands in for its code. */
std::vector<std::pair<std::string, goal_aot_object_file>> g_registered;

LoadedFile* find_file(const char* tag) {
  for (auto& file : g_loaded) {
    if (file.tag == tag) {
      return &file;
    }
  }
  return nullptr;
}

void set_error(const std::string& message) {
  g_error = message;
  lg::error("[aot-loader] {}", message);
}

/*!
 * Reproduces symlink_v3 from game/kernel/jak1/klink.cpp: goalc writes -1 into a static when it
 * wants the symbol's address, and anything else when it wants the symbol's offset from s7.
 */
void write_symbol_link(u32 addr, const char* name) {
  auto sym = jak1::intern_from_c(name);
  auto* slot = Ptr<s32>(addr).c();
  if (*slot == -1) {
    *slot = (s32)sym.offset;
  } else {
    *slot = (s32)(sym.cast<u32>() - s7);
  }
}

/*! Reproduces typelink_v3: intern the type, creating its vtable if this is the first reference. */
void write_type_link(u32 addr, const char* name, int method_count) {
  auto type = jak1::intern_type_from_c(name, (u64)method_count);
  *Ptr<s32>(addr).c() = (s32)type.offset;
}

}  // namespace

extern "C" {

int32_t* goal_symbol_slot(const char* name) {
  return (int32_t*)&jak1::intern_from_c(name)->value;
}

uint64_t goal_symbol_ptr(const char* name) {
  return jak1::intern_from_c(name).offset;
}

uint64_t goal_static_addr(const char* file, int index) {
  auto* loaded = find_file(file);
  if (!loaded || index < 0 || index >= (int)loaded->static_addrs.size()) {
    set_error(fmt::format("goal_static_addr: {}[{}] is not loaded", file, index));
    return 0;
  }
  return loaded->static_addrs.at(index);
}

/*!
 * GOAL address of the current stack pointer, for (suspend)'s stack-overflow check and with-sp.
 *
 * GOAL cooperative threads run on stacks inside GOAL memory (see docs/aot-stack-model.md), so this
 * is a real GOAL pointer into the running thread's stack and GOAL's own stack accounting works
 * unchanged. Reading it from a native stack means the caller is not running as a GOAL thread, and
 * there is no honest answer, so this refuses rather than returning a number that would make GOAL's
 * arithmetic quietly wrong.
 */
uint64_t goal_stack_pointer(const void* frame) {
  const uintptr_t base = (uintptr_t)g_ee_main_mem;
  const uintptr_t addr = (uintptr_t)frame;
  ASSERT_MSG(base && addr >= base && addr - base < EE_MAIN_MEM_SIZE,
             "GOAL read the stack pointer while running on a native stack. GOAL code that reads "
             "rsp must run on a GOAL-memory stack; see docs/aot-stack-model.md.");
  return (uint64_t)(addr - base);
}

uint64_t goal_function_addr(const char* file, int index) {
  auto* loaded = find_file(file);
  if (!loaded || index < 0 || index >= (int)loaded->function_addrs.size()) {
    set_error(fmt::format("goal_function_addr: {}[{}] is not loaded", file, index));
    return 0;
  }
  return loaded->function_addrs.at(index);
}

goal_kernel_core_status goal_aot_load(const goal_aot_object_file* file) {
  return goal_aot_load_into(file, 0);
}

goal_kernel_core_status goal_aot_load_into(const goal_aot_object_file* file, uint32_t heap_ptr) {
  if (!file || !file->tag || (file->static_count > 0 && !file->statics) ||
      (file->function_count > 0 && !file->functions)) {
    set_error("goal_aot_load: bad argument");
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  if (!goal_kernel_core_is_initialized()) {
    set_error("goal_aot_load: the kernel is not initialized");
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }
  if (find_file(file->tag)) {
    set_error(fmt::format("goal_aot_load: {} is already loaded", file->tag));
    return GOAL_KERNEL_CORE_ALREADY_INITIALIZED;
  }
  Ptr<kheapinfo> heap = heap_ptr ? Ptr<kheapinfo>(heap_ptr) : kglobalheap;

  g_goal_mem = g_ee_main_mem;
  g_goal_s7 = s7.offset;

  LoadedFile loaded;
  loaded.tag = file->tag;

  // One allocation for the whole file's static data, like link_control's "main-segment" copy.
  // kmalloc is 16-byte aligned, and no static asks for more than that.
  u32 total = 0;
  for (int i = 0; i < file->static_count; i++) {
    const int align = file->statics[i].align > 0 ? file->statics[i].align : 4;
    // A zero-size static takes up no room here, so every reference to it would resolve to whatever
    // static follows it. Nothing GOAL emits is legitimately empty, so this means the C backend has
    // a static kind it does not know how to write out.
    if (file->statics[i].size <= 0) {
      set_error(fmt::format("goal_aot_load: {} static {} has size {}", file->tag, i,
                            file->statics[i].size));
      return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
    }
    if (align > 16) {
      set_error(fmt::format("goal_aot_load: {} static {} wants {}-byte alignment", file->tag, i,
                            align));
      return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
    }
    total = (total + (u32)align - 1) / (u32)align * (u32)align;
    total += (u32)file->statics[i].size;
  }

  Ptr<u8> segment(0);
  if (total) {
    segment = kmalloc(heap, (s32)total, KMALLOC_MEMSET, "aot-static-segment");
    if (!segment.offset) {
      set_error(fmt::format("goal_aot_load: no room for {} bytes of {} static data", total,
                            file->tag));
      return GOAL_KERNEL_CORE_OUT_OF_MEMORY;
    }
  }

  u32 next = segment.offset;
  for (int i = 0; i < file->static_count; i++) {
    const auto& desc = file->statics[i];
    const u32 align = desc.align > 0 ? (u32)desc.align : 4;
    next = (next + align - 1) / align * align;
    loaded.static_addrs.push_back(next);
    if (desc.size) {
      memcpy(Ptr<u8>(next).c(), desc.data, (size_t)desc.size);
    }
    next += (u32)desc.size;
  }

  // A real GOAL function object per AOT function: type `function`, native entry point at its
  // address. Same shape as make_function_from_c, without any generated code.
  for (int i = 0; i < file->function_count; i++) {
    if (!file->functions[i]) {
      // the C backend could not translate this one; leave a hole so indices stay aligned
      loaded.function_addrs.push_back(0);
      continue;
    }
    // What alloc_heap_object does, against a kheapinfo rather than a heap symbol: level code
    // goes in the level's heap, and only the global heap has a symbol.
    const auto mem = kmalloc(heap, AOT_FUNCTION_OBJECT_SIZE, KMALLOC_MEMSET, "function");
    if (!mem.offset) {
      set_error(fmt::format("goal_aot_load: no room for a {} function object", file->tag));
      return GOAL_KERNEL_CORE_OUT_OF_MEMORY;
    }
    *Ptr<u32>(mem.offset).c() = *(s7 + jak1_symbols::FIX_SYM_FUNCTION_TYPE);
    const u32 obj = mem.offset + BASIC_OFFSET;
    const void* native = file->functions[i];
    memcpy(Ptr<u8>(obj).c(), &native, sizeof(native));
    loaded.function_addrs.push_back(obj);
  }

  // Register before relocating: relocations and the file's own link step both resolve through the
  // tag-keyed tables above.
  g_loaded.push_back(std::move(loaded));
  const auto& placed = g_loaded.back();

  if (file->link) {
    file->link();
  }

  for (int i = 0; i < file->static_count; i++) {
    const auto& desc = file->statics[i];
    for (int r = 0; r < desc.reloc_count; r++) {
      const goal_static_reloc& reloc = desc.relocs[r];
      const u32 addr = placed.static_addrs.at(i) + (u32)reloc.offset;
      switch (reloc.kind) {
        case GOAL_RELOC_SYMBOL_PTR:
          write_symbol_link(addr, reloc.name);
          break;
        case GOAL_RELOC_TYPE_PTR:
          write_type_link(addr, reloc.name, reloc.method_count);
          break;
        case GOAL_RELOC_STATIC_PTR:
          if (reloc.target_index < 0 || reloc.target_index >= (int)placed.static_addrs.size()) {
            set_error(fmt::format("goal_aot_load: {} static {} points at static {}", file->tag, i,
                                  reloc.target_index));
            return GOAL_KERNEL_CORE_NOT_FOUND;
          }
          *Ptr<u32>(addr).c() =
              placed.static_addrs.at(reloc.target_index) + (u32)reloc.target_offset;
          break;
        case GOAL_RELOC_FUNCTION_PTR:
          if (reloc.target_index < 0 || reloc.target_index >= (int)placed.function_addrs.size()) {
            set_error(fmt::format("goal_aot_load: {} static {} points at function {}", file->tag, i,
                                  reloc.target_index));
            return GOAL_KERNEL_CORE_NOT_FOUND;
          }
          *Ptr<u32>(addr).c() = placed.function_addrs.at(reloc.target_index);
          break;
        default:
          set_error(fmt::format("goal_aot_load: unknown relocation kind {}", reloc.kind));
          return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
      }
    }
  }

  return GOAL_KERNEL_CORE_OK;
}

goal_kernel_core_status goal_aot_register_object(const char* object_name,
                                                 const goal_aot_object_file* file) {
  if (!object_name || !object_name[0] || !file || !file->tag) {
    set_error("goal_aot_register_object: bad argument");
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  for (auto& entry : g_registered) {
    if (entry.first == object_name) {
      entry.second = *file;
      return GOAL_KERNEL_CORE_OK;
    }
  }
  g_registered.emplace_back(object_name, *file);
  return GOAL_KERNEL_CORE_OK;
}

const goal_aot_object_file* goal_aot_registered_object(const char* object_name) {
  if (!object_name) {
    return nullptr;
  }
  for (const auto& entry : g_registered) {
    if (entry.first == object_name) {
      return &entry.second;
    }
  }
  return nullptr;
}

int goal_aot_is_loaded(const char* tag) {
  return tag && find_file(tag) ? 1 : 0;
}

void goal_aot_forget(const char* tag) {
  for (auto it = g_loaded.begin(); it != g_loaded.end(); ++it) {
    if (it->tag == tag) {
      g_loaded.erase(it);
      return;
    }
  }
}

uint32_t goal_aot_function_object(const char* tag, int index) {
  auto* loaded = find_file(tag);
  if (!loaded || index < 0 || index >= (int)loaded->function_addrs.size()) {
    return 0;
  }
  return loaded->function_addrs.at(index);
}

uint32_t goal_aot_top_level_object(const char* tag) {
  auto* loaded = find_file(tag);
  if (!loaded || loaded->function_addrs.empty()) {
    return 0;
  }
  return loaded->function_addrs.back();
}

uint64_t goal_aot_call(uint32_t func, uint64_t a0, uint64_t a1, uint64_t a2) {
  return call_goal(Ptr<Function>(func), a0, a1, a2, s7.offset, g_ee_main_mem);
}

uint64_t goal_kernel_stack_top(void) {
  // ARM64 requires a 16-byte aligned stack pointer, so this is the last aligned address rather
  // than upstream's EE_MAIN_MEM_SIZE - 8. GOAL's own *stack-top* (#x07ffc000) is 16 kB below it
  // and the debug heap ends 64 kB below it.
  return (u64)(uintptr_t)g_ee_main_mem + EE_MAIN_MEM_SIZE - 16;
}

static goal_kernel_core_status run_top_level(const char* tag,
                                             uint64_t* out_result,
                                             bool switch_to_kernel_stack) {
  if (!tag) {
    set_error("goal_aot_run_top_level: bad argument");
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  if (!goal_kernel_core_is_initialized()) {
    set_error("goal_aot_run_top_level: the kernel is not initialized");
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }
  const u32 top_level = goal_aot_top_level_object(tag);
  if (!top_level) {
    set_error(fmt::format("goal_aot_run_top_level: {} has no top-level", tag));
    return GOAL_KERNEL_CORE_NOT_FOUND;
  }

  // A top-level's defmethod on a type whose subtypes already exist only reaches those subtypes
  // while *enable-method-set* is raised (jak1::method_set). Upstream raises it around the kernel
  // and engine DGO loads, which is the step this stands in for.
  *EnableMethodSet = *EnableMethodSet + 1;
  const u64 result = switch_to_kernel_stack
                         ? call_goal_on_stack(Ptr<Function>(top_level), goal_kernel_stack_top(),
                                              s7.offset, g_ee_main_mem)
                         : goal_aot_call(top_level, 0, 0, 0);
  *EnableMethodSet = *EnableMethodSet - 1;

  if (out_result) {
    *out_result = result;
  }
  return GOAL_KERNEL_CORE_OK;
}

goal_kernel_core_status goal_aot_run_top_level(const char* tag, uint64_t* out_result) {
  return run_top_level(tag, out_result, true);
}

goal_kernel_core_status goal_aot_run_top_level_here(const char* tag, uint64_t* out_result) {
  return run_top_level(tag, out_result, false);
}

goal_kernel_core_status goal_aot_call_symbol(const char* name,
                                             uint64_t a0,
                                             uint64_t a1,
                                             uint64_t a2,
                                             uint64_t* out_result) {
  if (!name) {
    set_error("goal_aot_call_symbol: bad argument");
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  if (!goal_kernel_core_is_initialized()) {
    set_error("goal_aot_call_symbol: the kernel is not initialized");
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }
  auto sym = jak1::find_symbol_from_c(name);
  if (!sym.offset || !sym->value) {
    set_error(fmt::format("goal_aot_call_symbol: '{}' holds no function", name));
    return GOAL_KERNEL_CORE_NOT_FOUND;
  }
  const u64 result = goal_aot_call(sym->value, a0, a1, a2);
  if (out_result) {
    *out_result = result;
  }
  return GOAL_KERNEL_CORE_OK;
}

void goal_aot_reset(void) {
  g_loaded.clear();
  g_registered.clear();
  g_goal_mem = nullptr;
  g_goal_s7 = 0;
}

}  // extern "C"
