#pragma once

/*!
 * @file gfx_host.h
 * The renderer seam: the machine-layer graphics functions, answered by a host the library never
 * names.
 *
 * `dma_capture.h` is the measuring half of this - it takes `__send-gfx-dma-chain`, follows the
 * chain and drops it. This is the drawing half: a host that has a renderer installs it here, and
 * every graphics function GOAL calls during a frame goes to that host instead of to a stub.
 *
 * Nothing here knows about Metal, SDL, a window or a display link. The host is a table of plain C
 * function pointers, so the same seam serves a desktop SDL/Metal shell and an iPadOS
 * `CAMetalLayer` bridge.
 */

#include <stdint.h>

#include "game/kernel/core/kernel_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * What a host has to answer. Every entry may be NULL, in which case that function keeps the
 * machine-layer stub behaviour (report once, return 0).
 */
typedef struct goal_gfx_host {
  /*! `__send-gfx-dma-chain`: the frame's DMA chain. `ee_base` is EE main memory and
   *  `chain_offset` is the GOAL pointer the chain starts at - the two arguments
   *  `GfxRendererModule::send_chain` takes. */
  void (*send_chain)(const void* ee_base, uint32_t chain_offset);

  /*! `syncv`: wait for the renderer to present. Returns 0 or 1 for even/odd frame, which is what
   *  the PS2's `sceGsSyncV` returned and what `engine/gfx/display.gc` reads. This is the call the
   *  game's frame rate comes from. */
  uint32_t (*vsync)(void);

  /*! `sync-path`: block until the renderer has consumed the chain that was sent. */
  uint32_t (*sync_path)(void);

  /*! `__pc-texture-upload-now`: a texture page uploaded outside the chain. `tpage` points into EE
   *  memory and `s7_ptr` is the symbol table, both as the renderer's texture pool wants them. */
  void (*texture_upload_now)(const uint8_t* tpage, int mode, uint32_t s7_ptr);

  /*! `__pc-texture-relocate`: a VRAM-to-VRAM copy, which is pure bookkeeping in the pool. */
  void (*texture_relocate)(uint32_t dst, uint32_t src, uint32_t format);

  /*! `__pc-set-levels`: which levels' art the renderer should have ready. `names` holds `count`
   *  level nicknames ("village1"), already filtered of the game's "none" placeholders, and is
   *  only valid for the duration of the call. The game calls this every frame from
   *  `(method 15 load-state)` in `engine/level/level.gc`. */
  void (*set_levels)(const char* const* names, int count);

  /*! `put-display-env`: the only field of the PS2 display environment that survives the port is
   *  the blackout alpha, which the game fades the screen with. 0 is black, 1 is normal. */
  void (*set_pmode_alp)(float alp);

  /*! `__pc-set-active-levels`: the levels currently participating in Jak 2 rendering. Kept
   *  separate from `set_levels`, which describes the desired/loading set. Jak 1 does not call
   *  this entry. Appended so existing host member offsets remain unchanged. */
  void (*set_active_levels)(const char* const* names, int count);
} goal_gfx_host;

/*!
 * Answer the machine layer's graphics functions out of `host`, replacing the stubs.
 *
 * Besides the entries above this also implements the PS2 graphics calls that have nothing left to
 * do once the hardware is gone, exactly as upstream's desktop port does: `reset-path`,
 * `reset-graph`, `dma-sync`, `gs-put-imr`, `gs-get-imr` and `gs-store-image` all return 0.
 * `flush-cache` stays owned by the shared portable machine seam.
 *
 * Call after `goal_kernel_core_stub_machine_layer`, whose stubs these replace, and before the
 * first frame. Passing NULL puts every one of them back on the stub path.
 */
goal_kernel_core_status goal_gfx_host_install(const goal_gfx_host* host);

/*! What the seam was asked for, so a run can report it instead of assuming it. */
typedef struct goal_gfx_host_stats {
  int chains;         /*! chains handed to the host */
  int vsyncs;         /*! syncv calls */
  int sync_paths;     /*! sync-path calls */
  int texture_uploads;/*! __pc-texture-upload-now calls */
  int texture_moves;  /*! __pc-texture-relocate calls */
  int level_sets;     /*! __pc-set-levels calls whose level list changed */
  /*! The levels named by the last `__pc-set-levels`, joined with '+', or "" before the first one.
   *  Owned by this file. */
  const char* last_levels;
  int active_level_sets; /*! __pc-set-active-levels calls whose level list changed */
  /*! The active levels named by the last `__pc-set-active-levels`, joined with '+'. */
  const char* last_active_levels;
} goal_gfx_host_stats;

void goal_gfx_host_stats_get(goal_gfx_host_stats* out);

#ifdef __cplusplus
}  // extern "C"
#endif
