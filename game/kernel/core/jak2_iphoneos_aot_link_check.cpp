#include "aot_boot_manifest.h"
#include "game/kernel/core/dma_capture.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/pad.h"

int main() {
  void (*volatile retain_dma_install)(void) = goal_gfx_dma_install;
  if (!retain_dma_install) {
    return 1;
  }
  goal_kernel_core_shutdown();
  if (goal_pad_install() != GOAL_KERNEL_CORE_NOT_INITIALIZED) {
    return 1;
  }
  if (goal_aot_boot_file_count != 840) {
    return 1;
  }
  return goal_aot_boot_files[0].tag && goal_aot_boot_files[839].tag ? 0 : 1;
}
