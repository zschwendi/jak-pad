# The AOT stack model

Status: **Decided and implemented,** including GOAL's thread switch. Every Jak 1 function now has
native ARM64 code behind it: 10595 of 10602 through the C backend and the remaining 7 written by
hand (`game/kernel/core/goal_native_kernel.cpp`, `goal_thread_arm64.s`).

## The question

Ahead-of-time compiled GOAL functions are ordinary native functions in `__TEXT`. Whose stack do
they run on: a stack inside GOAL memory, the way upstream OpenGOAL runs JIT-compiled GOAL code, or
the ordinary native stack?

This had to be settled because 1059 of the 10601 Jak 1 functions - 10% of the game - failed to
translate on `rlet` bindings of `rsp`, and because GOAL's cooperative threads are built out of
explicit stack manipulation that the answer determines the shape of.

## Decision

**GOAL runs on stacks inside GOAL memory, exactly as upstream does.** Nothing about the AOT model
changes that, because nothing about it needs to.

## Why

### GOAL's own kernel requires it, in code we are not rewriting

`goal_src/jak1/kernel/gkernel.gc` is not agnostic about where its stacks are:

- `reset-and-call` sets the machine stack pointer to `(-> this stack-top)` plus the GOAL memory
  base. `stack-top` is a GOAL pointer into a `cpu-thread`'s stack, allocated from the process heap
  by `(method new cpu-thread)`.
- `(method thread-suspend cpu-thread)` copies the live part of that stack - everything between the
  current stack pointer and `stack-top` - into the thread object's backup buffer, and
  `thread-resume` copies it back to the same addresses. Suspending is a stack *copy*, not a stack
  switch, so the addresses have to be stable GOAL addresses that a `cpu-thread` can describe.
- `game/kernel/jak1/kmachine.cpp` hardcodes the kernel's own stack into GOAL memory:
  `*stack-top*` is `0x07ffc000`, `*stack-base*` is `0x07ffffff` and `*stack-size*` is `0x4000` -
  the top of the 128 MiB EE main memory.
- `(suspend)` (`kernel/gkernel-h.gc:461`) and `with-sp` (`:535`) read `rsp` and subtract the memory
  base register to get a GOAL pointer, then compare it against the running thread's `stack-top` and
  `stack-size`.

Choosing native stacks would mean rewriting all of that, including the process/thread data types,
and giving up GOAL's stack accounting. There is no benefit to pay for it.

### It works on ARM64, measured

`game/kernel/core/aot_execution_test.cpp` allocates 32 KiB from the real global heap, switches to
it with `call_goal_on_stack`, and runs the AOT-compiled Jak 1 function `fact` there:

```
stack region: GOAL #x1cb4e0 - #x1d34e0 (32768 bytes), native top #x10558f4e0
ok   stack top 16-byte aligned        = #x0
ok   stack region zeroed by kmalloc   = #x0
ok   (fact 10) on the GOAL stack      = 3628800
ok   ...and as call_goal saw it       = 3628800
trampoline frame at native #x10558f4bf, 33 bytes below the top
184 bytes of the region are non-zero afterwards, deepest write 448 bytes below the top
```

The running frame is inside the region, the region was written to, and the native stack survives
the round trip. Ten levels of recursion in clang-compiled GOAL used 448 bytes, against the
`#x6000` (24 KiB) `PROCESS_STACK_SIZE` the PC port already uses.

EE main memory is mapped read/write and **not** executable. A stack does not need to be executable,
so this costs nothing against the iPadOS constraint that motivated the whole AOT design.

Getting there required fixing `_call_goal_on_stack_asm_arm64`
(`game/kernel/asm_funcs_arm64.s`), which saved the caller's stack pointer on the old stack and read
it back from the new one. It restored garbage into `sp` and faulted on return with
`EXC_BAD_ACCESS code=259`, the ARM64 stack-alignment fault. It now switches first and stashes the
old stack pointer on the new stack, which is what the working x86-64 routine does.

## What this makes possible in C, and what it does not

Reading `rsp` becomes expressible: the machine stack pointer has a GOAL address, so
`(- sp off)` is a real pointer into the running thread's stack and GOAL's stack accounting works
unchanged. The C backend lowers an `rsp` binding to `GOAL_STACK_POINTER()`, which is
`goal_stack_pointer(__builtin_frame_address(0))` (`goalc/aot/goal_c_runtime.h`,
`game/kernel/core/aot_loader.cpp`). That runtime function refuses to answer when the current stack
is not a GOAL stack, because there is no correct answer then and a made-up one would make GOAL's
arithmetic quietly wrong.

Writing `rsp` stays impossible: in C the compiler owns the stack pointer. The backend rejects any
`rlet` that assigns to it, and any that binds it without `:reset-here`, rather than writing to a
local and leaving the real stack alone.

Whole-game C backend coverage went from **9539/10601 (89.98%)** to **10590/10601 (99.90%)**. The
11 that remained were exactly the functions that manipulate the stack or other machine registers
directly. Four of them turned out not to need machine code at all; the other seven are written by
hand in ARM64:

| Function | File | What became of it |
| --- | --- | --- |
| `return-from-thread` | `kernel/gkernel.gc` | native: restores the context `*kernel-sp*` names |
| `reset-and-call` | `kernel/gkernel.gc` | native: saves the kernel context, runs the function on the thread's stack |
| `(method thread-suspend cpu-thread)` | `kernel/gkernel.gc` | native: context to the stack, stack to the backup, back to the kernel |
| `(method thread-resume cpu-thread)` | `kernel/gkernel.gc` | native: the inverse, plus the first-run path |
| `(method new catch-frame)` | `kernel/gkernel.gc` | native: captures a context, runs the function, pops the frame |
| `throw-dispatch` | `kernel/gkernel.gc` | native: goes back to a catch frame's context with a value |
| `enter-state-run-code` | `kernel/gstate.gc` | native, and new: the machine-level tail of `enter-state`, lifted into a function so the rest of `enter-state` is translatable |
| `return-from-thread-dead` | `kernel/gkernel.gc` | plain GOAL: `(deactivate pp)` then `(return-from-thread)` |
| `set-to-run-bootstrap` | `kernel/gkernel.gc` | plain GOAL: reads the function and arguments out of the thread instead of out of saved registers |
| `(method deactivate process)` | `kernel/gkernel.gc` | plain GOAL: `abandon-thread` is a call now, not a forged return address |
| `(method compute-alignment! align-control)` | `engine/anim/aligner.gc` | plain GOAL, for the same reason |

The three GOAL rewrites are identical in effect on x86-64. `return-from-thread` never returns - it
sets the stack pointer from `*kernel-sp*` and returns from there - so the return address an ordinary
`call` pushes is discarded exactly like the forged one was.

## Consequences

**The machine context lives on the stack, not in the object.** A resumable ARM64 context is
`x19`-`x28`, `x29`, `x30`, `sp` and `d8`-`d15`: 176 bytes. A `cpu-thread` has seven 64-bit `rreg`
slots and a `catch-frame` five, sized for the five registers x86-64 GOAL saves, so the context does
not fit in either. It is written to the running GOAL stack instead and the object records its GOAL
address - which is what those objects' `sp` fields already mean, and which is inside the region
`thread-suspend` copies, so suspend and resume carry the context along with everything else.

`(-> catch-frame ra)` and `(-> cpu-thread pc)` have nothing left to hold: an ARM64 return address is
a `__TEXT` address that no 32-bit GOAL field can express. `ra` is set to 0. `pc` records the GOAL
address of the saved context, so `thread-resume` can tell a suspended thread from one `set-to-run`
has never started; those are the only two states it is ever in.

**Return addresses are not forged; trampolines take their place.** x86-64 GOAL pushes
`return-from-thread` and jumps to the user function. Ahead-of-time compiled GOAL functions are
ordinary native functions reached with `bl`, so there is nothing to forge with. The native routines
call the user function on the thread's stack and then do what the forged return address would have
done. Which value reaches the kernel is unchanged, so `reset-and-call` still returns what the
listener function returned.

**Three primitives are all the assembly there is.** `goal_thread_arm64.s` holds
`goal_context_save_and_call` (capture a context at the current stack pointer and call something that
is not expected to return), `goal_context_restore` (go back to one from anywhere, with a value) and
`goal_call_on_stack_arm64` (run a function on a different stack). Everything else is C reading and
writing GOAL structures.

**Platform graphics callbacks leave the GOAL stack synchronously.** GOAL calls the renderer while
a process stack is active, but native window, IOSurface and Metal calls can use far more stack than
the game process reserved. The native-to-GOAL entry trampoline therefore publishes the bottom of
its suspended native frame, and the graphics-host seam uses `goal_call_on_stack_arm64` to run each
host callback there before returning to the same GOAL frame. The call remains synchronous: DMA and
level-name pointers stay borrowed only for the call, and `sync-path` ordering does not change.
Host callbacks are a nonthrowing ABI. A C++ exception is caught before it can unwind across the
assembly bridge, and terminates the process after the bridge has restored the GOAL stack.

**Stack copying keeps working.** ARM64 frames contain saved frame pointers that point into the same
stack region and return addresses that point into `__TEXT`. `thread-suspend` restores the bytes to
the same addresses, so both stay valid. This is the property that would have been lost if threads
ran on native stacks at whatever address the OS handed out.

**Backup stacks had to be resized, and the sizes are one rule now.** Upstream already raised
`PROCESS_STACK_SIZE` from the PS2's `#x1c00` to `#x6000` because compiled x86-64 GOAL uses more
stack than PS2 MIPS did. The *backup* sizes had the same problem and had not been touched: a
process's default was the PS2's 256 bytes, individual processes lower their own to 128 with
`stack-size-set!`, and the ARM64 context alone is 176. `jak1-data-boot-test --frames 1` measured
one process needing 400 bytes into a 128-byte buffer.

Those numbers are PS2 measurements of one thing - how deep a process's code is when it suspends -
and translating them for a different code generator is one rule, so it lives in one place.
`process-stack-save-size` in `kernel/gkernel-h.gc` is `512 + 3 x`, `stack-size-set!` applies it to
whatever a process asks for, and `PROCESS_STACK_SAVE_SIZE` is the PS2 default put through the same
rule. The game's own numbers stay as they are, which keeps them re-measurable and keeps the
downstream delta to one macro.

Measured after: across 1000 frames of the title level and `village1` streaming in, 14424 suspends,
the deepest backing up 752 bytes of its 2048, and the one that came closest to filling its buffer
using 400 of 896. `jak1-data-boot-test` reports both numbers at the end of a run
(`goal_thread_stack_watermark`), so the headroom is a measurement and not a hope.

**A backup stack comes out of the process's own heap, and nothing checked that it fit.** Raising
the sizes turned that into a real failure: `camera-slave` overran its 4 KB process heap by 544
bytes, and every suspend then copied its live stack over the process the pool had put after it -
whose main thread later failed `thread-resume` with a `stack-size` of 1. `process-heap-overrun-check`
in `gkernel.gc` now reports and stops on that, and the fixed-size dead pools grow by
`PROCESS_STACK_SAVE_GROWTH`, the most the conversion can add to one process.

The variable-sized pool needed the same growth, and gameplay is where that showed. Actor process
heap sizes are PS2 measurements too - `*entity-info*` in `engine/entity/entity-table.gc` is a table
of them - and the tight ones stopped fitting. Entering Sandover Village, `babak` asked for its
`#x2800`, ran out 480 bytes into `(method new joint-control)`, and `object-new` returned 0, which
GOAL then wrote through. `(method get-process dead-pool-heap)` grows the size it is given by
`PROCESS_STACK_SAVE_GROWTH` before it looks for a gap, so the gap and the process heap agree, and
so does every caller that never sees the number.

**Reading `rsp` had to become a read, not a copy.** The C backend lowered an `rlet` binding of
`rsp` to a local seeded once by `GOAL_STACK_POINTER()`. GOAL's compiler emits one `:reset-here` per
function no matter how many `(suspend)` sites it has - on x86-64 the binding *is* the register, so
one is enough - and 300-odd Jak 1 functions have more than one. Every later site read a stale value
or, when the single reset was on a path that had not run, zero, and `(suspend)`'s own overflow
check reported nonsense on every spooled animation. The binding is machine state now
(`find_machine_state_regs`), so each use reads the stack pointer, which is exact: a C frame address
does not move within a function.

**Guard pages.** A GOAL-memory stack has none: an overflow runs off the bottom of the region into
whatever the process heap put below it, silently. That is exactly the situation upstream is in, and
GOAL's own `(suspend)` check is the mitigation. A future improvement is to `mprotect` a page below
each thread stack, which is possible because the stacks come out of a heap the runtime controls.

**Nothing here needs writable-executable memory.** GOAL-memory stacks are data, and the native
routines are ordinary signed code in `__TEXT`. The `arm64-apple-ios` build of
`jak1-thread-switch-test` links and code-signs with `__TEXT` at `r-x` and no writable-executable
segment.

## What this turned up

Three things were wrong in the runtime and only showed up once GOAL started spawning processes.

**Top-levels ran with `*enable-method-set*` clear.** `method_set` only propagates a method to
subtypes that already exist while that symbol is raised, and upstream raises it around the kernel
and engine DGO loads. Nothing raised it here, so `(defmethod stack-size-set! ((this thread) ...))`
in `gkernel.gc` never reached `cpu-thread`, whose type was built one file earlier - the method slot
was 0 and the first process to call it faulted. `goal_aot_run_top_level` now raises it, and runs
the top-level on GOAL's own stack, which is the other thing a process spawn needs: `(new 'stack ...)`
gives a C local a GOAL address, and a catch-frame's address is stored in a 32-bit field.

**Process allocation was disabled on ARM64.** `copy_basic`, `new_basic` and `alloc_heap_object`
reach the current process through a fourth argument that the x86-64 trampoline fills from `r13`. The
ARM64 shims passed `UNKNOWN_PP`, so every `(new 'process ...)` aborted. They read
`g_goal_current_process`, which is where ARM64 keeps `r13`.

**GOAL's symbol hash table ran on a table of zeroes.** `init_crc` fills the CRC table GOAL hashes
symbol names with, and upstream calls it from `jak1::goal_main` - the desktop entry point, which is
not part of the portable kernel. `kscheme_init_globals_common` zeroes the table, so nothing filled
it in. Interning still worked, because it was self-consistent, but `EMPTY_HASH` no longer matched
and `intern_from_c("_empty_")` made an ordinary symbol instead of returning the empty pair. Every
static field holding `'()` linked to that symbol, `(null? ...)` said no, and the first walk over one
- `get-continue-by-name`, on the game's own startup - dereferenced the car of a list that was not a
list. `goal_kernel_core_initialize` calls `init_crc` now.

## Rejected alternative: native stacks

Running AOT GOAL on the ordinary native stack would avoid the ARM64 stack-switch assembly. It was
rejected because:

- `cpu-thread`'s `stack-top` / `stack-size` / `stack` fields, and the suspend/resume stack copy,
  would all have to be redesigned, in GOAL source that is otherwise reconstructed correctly.
- The `(suspend)` overflow check and `with-sp` would have to be deleted rather than implemented,
  losing a diagnostic that GOAL uses to catch a real class of bug.
- One native stack per GOAL thread would have to be allocated outside GOAL memory and tracked
  separately, for no gain: GOAL memory is already mapped, already the right size, and already
  the thing `(method new cpu-thread)` allocates from.
- It would diverge from upstream for no platform reason. iPadOS objects to executable memory, not
  to where the stack pointer points.
