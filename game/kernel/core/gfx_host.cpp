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
#include "game/kernel/core/gfx_host_internal.h"
#include "game/kernel/core/kernel_game.h"
#include "game/runtime.h"

// Defined in desktop_seams.cpp; declared the way dgo_loader.cpp and sound_rpc.cpp declare it.
u64 goal_kernel_core_machine_stub_report(const char* what);

namespace {

goal_gfx_host g_host;

goal_gfx_host_stats g_stats;
std::string g_last_levels;
std::string g_last_active_levels;

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
  goal_game_gfx_before_vsync();
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

void forward_levels(const u32* level_name_offsets, int count, bool active) {
  auto callback = active ? g_host.set_active_levels : g_host.set_levels;
  const char* symbol = active ? "__pc-set-active-levels" : "__pc-set-levels";
  if (!callback) {
    report(symbol);
    return;
  }

  std::vector<std::string> levels;
  for (int i = 0; i < count; i++) {
    const u32 arg = level_name_offsets[i];
    if (!arg) {
      continue;
    }
    if (arg == goal_game_false_offset()) {
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
  auto& previous = active ? g_last_active_levels : g_last_levels;
  if (joined != previous) {
    previous = joined;
    if (active) {
      g_stats.active_level_sets++;
    } else {
      g_stats.level_sets++;
    }
  }

  std::vector<const char*> names;
  names.reserve(levels.size());
  for (const auto& name : levels) {
    names.push_back(name.c_str());
  }
  callback(names.data(), (int)names.size());
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
  g_last_active_levels.clear();
  g_stats.last_levels = g_last_levels.c_str();
  g_stats.last_active_levels = g_last_active_levels.c_str();

  goal_game_make_function_symbol("__send-gfx-dma-chain", (void*)send_gfx_dma_chain);
  goal_game_make_function_symbol("syncv", (void*)sync_v);
  goal_game_make_function_symbol("sync-path", (void*)sync_path);
  goal_game_make_function_symbol("__pc-texture-upload-now", (void*)texture_upload_now);
  goal_game_make_function_symbol("__pc-texture-relocate", (void*)texture_relocate);
  goal_game_install_gfx_adapters();

  goal_game_make_function_symbol("reset-path", (void*)nothing_to_do);
  goal_game_make_function_symbol("reset-graph", (void*)nothing_to_do);
  goal_game_make_function_symbol("dma-sync", (void*)nothing_to_do);
  goal_game_make_function_symbol("gs-put-imr", (void*)nothing_to_do);
  goal_game_make_function_symbol("gs-get-imr", (void*)nothing_to_do);
  goal_game_make_function_symbol("gs-store-image", (void*)nothing_to_do);

  return GOAL_KERNEL_CORE_OK;
}

void goal_gfx_host_stats_get(goal_gfx_host_stats* out) {
  if (!out) {
    return;
  }
  g_stats.last_levels = g_last_levels.c_str();
  g_stats.last_active_levels = g_last_active_levels.c_str();
  *out = g_stats;
}

}  // extern "C"

void goal_gfx_host_forward_desired_levels(const u32* level_name_offsets, int count) {
  if (level_name_offsets && count >= 0) {
    forward_levels(level_name_offsets, count, false);
  }
}

void goal_gfx_host_forward_active_levels(const u32* level_name_offsets, int count) {
  if (level_name_offsets && count >= 0) {
    forward_levels(level_name_offsets, count, true);
  }
}

void goal_gfx_host_forward_pmode_alpha(float alpha) {
  if (g_host.set_pmode_alp) {
    g_host.set_pmode_alp(alpha);
  }
}
