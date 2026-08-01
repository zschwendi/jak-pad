/*!
 * @file metal_kernel_bridge.cpp
 * Plain-C++ bridge to the two kernel calls the Metal renderer needs
 * (vif_interrupt_callback and offset_of_s7). The kernel headers define a
 * global `Ptr` template that collides with the `Ptr` typedef Apple's
 * MacTypes.h brings into every Objective-C++ translation unit, so they cannot
 * be included from .mm files directly.
 */

#include "metal_kernel_bridge.h"

#include "game/kernel/common/kmachine.h"

void metal_vif_interrupt_callback(int bucket_id) {
  vif_interrupt_callback(bucket_id);
}

u32 metal_offset_of_s7() {
  return offset_of_s7();
}
