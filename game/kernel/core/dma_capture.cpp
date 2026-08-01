/*!
 * @file dma_capture.cpp
 * `__send-gfx-dma-chain`: measure the DMA chain a frame built, and optionally write one to a file.
 *
 * This is the seam where a frame's work leaves GOAL. `engine/draw/drawable.gc` builds a DMA chain
 * in EE main memory and hands its address to `__send-gfx-dma-chain`; upstream passes it to the
 * renderer, which walks it with `FixedChunkDmaCopier` (common/dma/dma_copy.h) and draws it.
 *
 * There is no renderer here, so nothing is drawn. What this does instead is run the same copier -
 * which is where the chain is validated, since following it means reading every tag - and record
 * how big it was. That turns "the frame ran" into "the frame built a chain of this many bytes",
 * which is a fact about the frame rather than an absence of a crash.
 *
 * With a capture path set, the first chain is serialized to that file in the copier's own format
 * (`FixedChunkDmaCopier::serialize_last_result`), so the renderer track has a real frame of Jak 1
 * DMA to replay without needing the rest of the runtime. The file is the player's own game data
 * rendered into DMA commands, so it is written where the caller asks and never into the
 * repository.
 */

#include <cstdio>
#include <string>

#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/Serializer.h"

#include "game/kernel/common/Ptr.h"
#include "game/kernel/core/dma_capture.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

namespace {

FixedChunkDmaCopier* g_copier = nullptr;
goal_gfx_dma_stats g_stats;
std::string g_capture_path;

/*!
 * `(__send-gfx-dma-chain bank chain)`. `bank` is the PS2 DMA channel register address, which means
 * nothing without the hardware; `chain` is the GOAL address the chain starts at.
 */
u64 send_gfx_dma_chain(u32 bank, u32 chain) {
  (void)bank;
  if (!g_copier) {
    g_copier = new FixedChunkDmaCopier(EE_MAIN_MEM_SIZE);
  }

  const auto& result = g_copier->run(g_ee_main_mem, chain, false);
  g_stats.chains++;
  g_stats.last_bytes = (u32)result.data.size();
  if (g_stats.last_bytes > g_stats.largest_bytes) {
    g_stats.largest_bytes = g_stats.last_bytes;
  }

  if (!g_capture_path.empty()) {
    Serializer serializer;
    g_copier->serialize_last_result(serializer);
    FILE* fp = std::fopen(g_capture_path.c_str(), "wb");
    if (fp) {
      std::fwrite(serializer.get_save_result().first, 1, serializer.get_save_result().second, fp);
      std::fclose(fp);
      g_stats.captured_bytes = (u32)serializer.get_save_result().second;
      lg::info("[dma-capture] wrote {} bytes of chain {} to {}", g_stats.captured_bytes,
               g_stats.chains, g_capture_path);
    } else {
      lg::error("[dma-capture] cannot write {}", g_capture_path);
    }
    // one chain is what the renderer track asked for; capturing every frame would just overwrite
    g_capture_path.clear();
  }
  return 0;
}

}  // namespace

extern "C" {

void goal_gfx_dma_install(void) {
  g_stats = goal_gfx_dma_stats();
  jak1::make_function_symbol_from_c("__send-gfx-dma-chain", (void*)send_gfx_dma_chain);
}

void goal_gfx_dma_set_capture_path(const char* path) {
  g_capture_path = path ? path : "";
}

void goal_gfx_dma_get_stats(goal_gfx_dma_stats* out) {
  *out = g_stats;
}

}  // extern "C"
