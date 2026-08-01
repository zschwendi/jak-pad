/*!
 * @file metal_host_seams.cpp
 * The handful of runtime globals the Metal renderer reads, defined for a build that links the
 * renderer without the desktop runtime.
 *
 * `game/graphics/gfx.cpp` and `game/kernel/common/kmachine.cpp` define these upstream, and both
 * files pull in the whole desktop port: the OpenGL pipeline, the IOP, the listener, and a second
 * copy of the Jak 1 kernel that `jak1-kernel-core` already provides. The `goalpad-metal-gfx`
 * library exists to link the Metal renderer against that portable kernel instead, so it defines
 * the three things the renderer actually uses and nothing else.
 *
 * Only this file is a duplicate. Everything below the `GfxRendererModule` seam is the same source
 * the `runtime` library compiles.
 */

#include "game/graphics/gfx.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kscheme.h"
#include "game/runtime.h"

namespace Gfx {
GfxGlobalSettings g_global_settings;
// Gfx::g_debug_settings is deliberately absent: it is the imgui debug UI's settings object, only
// reachable through GfxDisplay::set_imgui_visible, and this build has no debug UI. A build that
// grows one has to define it, which is a link error rather than a silent default.
}  // namespace Gfx

// game/runtime.cpp's globals. `g_ee_main_mem` and `g_game_version` are not here: the portable
// kernel owns EE main memory and defines both in kernel_core.cpp.
std::thread::id g_main_thread_id = std::thread::id();

/*!
 * The fake VIF interrupt the graphics thread raises after each bucket. Upstream this calls the
 * GOAL function `install-handler` registered; this build's machine layer does not implement
 * `install-handler` (it is one of the reported stubs in desktop_seams.cpp), so no handler is ever
 * registered and there is nothing to call. It is defined rather than left out so that the bucket
 * dispatch keeps its upstream shape and gains a real handler the day that stub does.
 */
void vif_interrupt_callback(int /*bucket_id*/) {}

u32 offset_of_s7() {
  return s7.offset;
}
