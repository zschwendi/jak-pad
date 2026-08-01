/*!
 * @file gfx_host.cpp
 * The machine layer's graphics functions, answered out of a host's renderer.
 *
 * These are the same functions `game/kernel/jak1/kmachine.cpp` and
 * `game/kernel/common/kmachine.cpp` install on the desktop port, with the same bodies: the ones
 * that reach the renderer forward to the host table, and the ones the PS2 hardware owned
 * (`reset-graph`, `dma-sync`, the GS IMR pair) return 0, which is what upstream's
 * `game/sce/libgraph.cpp` and `game/sce/libdma.cpp` already do.
 *
 * The reason they are here rather than compiled from kmachine.cpp is the reason `pad.cpp` gives:
 * that file is the whole machine layer at once - SDL, OpenGL, Discord, sqlite - and none of it
 * exists in this library.
 */

#include "game/kernel/core/gfx_host.h"

#include <cstring>
#include <string>
#include <vector>

#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kernel_types.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

// Defined in desktop_seams.cpp; declared the way dgo_loader.cpp and sound_rpc.cpp declare it.
u64 goal_kernel_core_machine_stub_report(const char* what);

namespace {

goal_gfx_host g_host;

goal_gfx_host_stats g_stats;
std::string g_last_levels;

u64 report(const char* what) {
  return goal_kernel_core_machine_stub_report(what);
}

// -------------------------------------------------------------------------------------------
// the calls that reach the renderer
// -------------------------------------------------------------------------------------------

u64 send_gfx_dma_chain(u32 /*bank*/, u32 chain) {
  g_stats.chains++;
  if (g_host.send_chain) {
    g_host.send_chain(g_ee_main_mem, chain);
  } else {
    report("__send-gfx-dma-chain");
  }
  return 0;
}

u64 sync_v(u32 mode) {
  g_stats.vsyncs++;
  if (mode != 0) {
    // upstream asserts on this; the frame code only ever passes 0.
    return 0;
  }
  return g_host.vsync ? g_host.vsync() : report("syncv");
}

u64 sync_path(u32 mode, u32 timeout) {
  g_stats.sync_paths++;
  if (mode != 0 || timeout != 0) {
    return 0;
  }
  return g_host.sync_path ? g_host.sync_path() : report("sync-path");
}

u64 texture_upload_now(u32 page, u32 mode) {
  g_stats.texture_uploads++;
  if (g_host.texture_upload_now) {
    g_host.texture_upload_now(Ptr<u8>(page).c(), (int)mode, s7.offset);
  } else {
    report("__pc-texture-upload-now");
  }
  return 0;
}

u64 texture_relocate(u32 dst, u32 src, u32 format) {
  g_stats.texture_moves++;
  if (g_host.texture_relocate) {
    g_host.texture_relocate(dst, src, format);
  } else {
    report("__pc-texture-relocate");
  }
  return 0;
}

/*!
 * Copy of jak1::pc_set_levels: the two level names the load state is holding, with the game's own
 * "none" placeholders dropped.
 */
u64 set_levels(u32 l0, u32 l1) {
  if (!g_host.set_levels) {
    return report("__pc-set-levels");
  }
  std::vector<std::string> levels;
  for (u32 arg : {l0, l1}) {
    if (!arg) {
      continue;
    }
    const char* name = Ptr<String>(arg).c()->data();
    if (!name || !name[0] || std::strcmp(name, "none") == 0 || std::strcmp(name, "#f") == 0) {
      continue;
    }
    levels.emplace_back(name);
  }

  std::string joined;
  for (const auto& name : levels) {
    if (!joined.empty()) {
      joined += '+';
    }
    joined += name;
  }
  if (joined != g_last_levels) {
    g_last_levels = joined;
    g_stats.last_levels = g_last_levels.c_str();
    g_stats.level_sets++;
  }

  std::vector<const char*> names;
  names.reserve(levels.size());
  for (const auto& name : levels) {
    names.push_back(name.c_str());
  }
  g_host.set_levels(names.data(), (int)names.size());
  return 0;
}

/*!
 * Copy of jak1::PutDisplayEnv. Everything in the PS2 display environment is gone except byte 1,
 * the blackout alpha the game fades the screen with.
 */
u64 put_display_env(u32 ptr) {
  if (g_host.set_pmode_alp) {
    g_host.set_pmode_alp(Ptr<u8>(ptr).c()[1] / 255.f);
  }
  return 0;
}

// -------------------------------------------------------------------------------------------
// the calls the PS2 hardware owned. Upstream's desktop port keeps these as no-ops returning 0;
// so does this. They are implemented rather than stubbed because a frame calls them and there is
// nothing left for them to do.
// -------------------------------------------------------------------------------------------

u64 nothing_to_do() {
  return 0;
}

}  // namespace

extern "C" {

goal_kernel_core_status goal_gfx_host_install(const goal_gfx_host* host) {
  if (!goal_kernel_core_is_initialized()) {
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }
  if (host) {
    g_host = *host;
  } else {
    g_host = goal_gfx_host();
  }
  g_stats = goal_gfx_host_stats();
  g_last_levels.clear();
  g_stats.last_levels = g_last_levels.c_str();

  jak1::make_function_symbol_from_c("__send-gfx-dma-chain", (void*)send_gfx_dma_chain);
  jak1::make_function_symbol_from_c("syncv", (void*)sync_v);
  jak1::make_function_symbol_from_c("sync-path", (void*)sync_path);
  jak1::make_function_symbol_from_c("put-display-env", (void*)put_display_env);
  jak1::make_function_symbol_from_c("__pc-texture-upload-now", (void*)texture_upload_now);
  jak1::make_function_symbol_from_c("__pc-texture-relocate", (void*)texture_relocate);
  jak1::make_function_symbol_from_c("__pc-set-levels", (void*)set_levels);

  jak1::make_function_symbol_from_c("reset-path", (void*)nothing_to_do);
  jak1::make_function_symbol_from_c("reset-graph", (void*)nothing_to_do);
  jak1::make_function_symbol_from_c("dma-sync", (void*)nothing_to_do);
  jak1::make_function_symbol_from_c("flush-cache", (void*)nothing_to_do);
  jak1::make_function_symbol_from_c("gs-put-imr", (void*)nothing_to_do);
  jak1::make_function_symbol_from_c("gs-get-imr", (void*)nothing_to_do);
  jak1::make_function_symbol_from_c("gs-store-image", (void*)nothing_to_do);

  return GOAL_KERNEL_CORE_OK;
}

void goal_gfx_host_stats_get(goal_gfx_host_stats* out) {
  if (!out) {
    return;
  }
  g_stats.last_levels = g_last_levels.c_str();
  *out = g_stats;
}

}  // extern "C"
