#include "aot_boot_manifest.h"
#include "game/kernel/core/kernel_core.h"

int main() {
  goal_kernel_core_shutdown();
  if (goal_aot_boot_file_count != 840) {
    return 1;
  }
  return goal_aot_boot_files[0].tag && goal_aot_boot_files[839].tag ? 0 : 1;
}
