#pragma once

/*!
 * @file mips2c_seam.h
 * The hand-translated PS2 assembly functions that `def-mips2c` in GOAL source refers to.
 * See mips2c_seam.cpp for what this runtime does differently from upstream.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Make every Jak 1 mips2c function findable by name, so GOAL's `__pc-get-mips2c` can answer.
 * Call once, after the kernel is initialized and before any object file's top-level runs.
 */
void goal_mips2c_register_jak1(void);

/*!
 * The same, for the Jak 2 function library. Only one game's library is compiled into a kernel
 * core library, so a runtime calls exactly one of these.
 */
void goal_mips2c_register_jak2(void);

/*!
 * Forget every registration. The trampolines are GOAL function objects in the heap the kernel is
 * about to unmap, so they cannot outlive it.
 */
void goal_mips2c_reset(void);

#ifdef __cplusplus
}  // extern "C"
#endif
