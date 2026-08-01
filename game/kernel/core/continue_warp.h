#pragma once

/*!
 * @file continue_warp.h
 * Start the game at one of its own continue points, the way a warp gate does.
 *
 * `engine/common-obs/basebutton.gc`'s `warp-gate` runs
 * `(start 'play (get-continue-by-name *game-info* <name>))`, and so does the end of a cutscene and
 * every death respawn. `target-continue` then wants the point's two levels, waits for them to
 * reach `'active`, and puts the target where the point says - so this is the game's own level
 * change, unlike writing the load state directly, which skips the state change the game does
 * first.
 *
 * A host uses it to get into a level without playing the whole game to it.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The GOAL address of the `continue-point` of that name, or 0. The names are the ones in
 * `engine/level/level-info.gc` - "village1-hut", "beach-start", "jungle-start" and so on.
 */
unsigned int goal_find_continue_point(const char* name);

/*!
 * `(start 'play <point>)` on GOAL's own stack. Returns 0 when there is no such continue point, or
 * when `start` is not loaded yet.
 *
 * Call it between frames: `start` spawns the target, and the spawn's first `(suspend)` returns to
 * whatever ran the last thread, so it must not be called from inside a frame the kernel is
 * dispatching.
 */
int goal_warp_to_continue(const char* name);

/*! What the last `goal_warp_to_continue` did, for a caller that wants to report it. */
typedef struct {
  const char* level;     //! the level the point is in
  const char* want0;     //! and the two levels it asks the load state for
  const char* want1;
  const char* vis_nick;
} goal_continue_point_info;

int goal_continue_point_describe(const char* name, goal_continue_point_info* out);

#ifdef __cplusplus
}
#endif
