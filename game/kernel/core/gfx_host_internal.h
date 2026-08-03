#pragma once

/*!
 * @file gfx_host_internal.h
 * Per-game GOAL ABI adapters for the renderer-neutral graphics host.
 *
 * The public host API sees ordinary strings and normalized alpha. Each game decodes its own GOAL
 * argument layout before forwarding here. Level names remain borrowed only during the callback.
 */

#include "common/common_types.h"

void goal_gfx_host_forward_desired_levels(const u32* level_name_offsets, int count);
void goal_gfx_host_forward_active_levels(const u32* level_name_offsets, int count);
void goal_gfx_host_forward_pmode_alpha(float alpha);
