#pragma once

/*!
 * @file metal_kernel_bridge.h
 * See metal_kernel_bridge.cpp: kernel calls re-exported for Objective-C++
 * translation units, which cannot include the kernel headers.
 */

#include "common/common_types.h"

// fake VIF interrupt for the game, called after each rendered bucket (mirror
// of the GL dispatch loop's vif_interrupt_callback calls)
void metal_vif_interrupt_callback(int bucket_id);

// s7 (GOAL symbol table) offset, needed by the texture upload handler
u32 metal_offset_of_s7();
