#pragma once

/*!
 * @file gfx_host_internal.h
 * Private half of gfx_host.h used by the per-game kernel adapters.
 *
 * The public host sees ordinary strings and a normalized alpha. Each game's GOAL ABI reaches the
 * helpers below only after decoding its own argument layout.
 */

#include "common/common_types.h"

void goal_gfx_host_forward_levels(const u32* level_name_offsets, int count, bool active);
void goal_gfx_host_forward_pmode_alpha(float alpha);
