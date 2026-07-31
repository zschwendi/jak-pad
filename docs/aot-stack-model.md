# The AOT stack model

Status: **Decided and partly implemented.** The decision below is implemented for everything
except GOAL's thread switch itself, which is listed as remaining work at the end.

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
11 that remain are exactly the functions that manipulate the stack or other machine registers
directly:

| Function | File | What it needs |
| --- | --- | --- |
| `return-from-thread` | `kernel/gkernel.gc` | assigns `rsp`, pops saved registers, `.ret` |
| `return-from-thread-dead` | `kernel/gkernel.gc` | same, after `deactivate` |
| `reset-and-call` | `kernel/gkernel.gc` | switches to a thread's stack and jumps |
| `(method thread-suspend cpu-thread)` | `kernel/gkernel.gc` | saves registers, copies the stack, returns to the kernel |
| `(method thread-resume cpu-thread)` | `kernel/gkernel.gc` | the inverse |
| `(method new catch-frame)` | `kernel/gkernel.gc` | captures the return address and stack pointer |
| `throw-dispatch` | `kernel/gkernel.gc` | restores them |
| `enter-state` | `kernel/gstate.gc` | reuses the caller's frame for a state's code |
| `set-to-run-bootstrap` | `kernel/gkernel.gc` | `.add` on the process register |
| `(method deactivate process)` | `kernel/gkernel.gc` | `.push` |
| `(method compute-alignment! align-control)` | `engine/anim/aligner.gc` | `.add` on a register pair |

## Consequences

**The seven thread and exception routines need native implementations.** They are the same kind of
thing `game/mips2c/**` already is: a native function standing in for a GOAL function, reached
through a real GOAL `function` object. On ARM64 that means saving `x19`-`x28`, `d8`-`d15`, `sp` and
`lr`, which is what `thread-suspend` already does for the x86-64 callee-saved set, into the
`cpu-thread`'s `regs` and `freg` arrays. Nothing about this is blocked; it is simply not written
yet.

This is not a theoretical item. `jak1-aot-boot-test` loads 207 of Jak 1's 518 object files in
build order and stops at file 208, `engine/gfx/mood/time-of-day.gc`, whose `top-level` runs
`(process-spawn time-of-day-proc ...)`. That reaches `run-function-in-process` and then
`(new 'stack 'catch-frame ...)`, which is `(method new catch-frame)` in the table above. These
routines are the next thing gating the boot path, not a cleanup task.

**Stack copying keeps working.** ARM64 frames contain saved frame pointers that point into the same
stack region and return addresses that point into `__TEXT`. `thread-suspend` restores the bytes to
the same addresses, so both stay valid. This is the property that would have been lost if threads
ran on native stacks at whatever address the OS handed out.

**Stack size needs watching, not redesigning.** Upstream already raised `PROCESS_STACK_SIZE` from
the PS2's `#x1c00` to `#x6000` because compiled x86-64 GOAL uses more stack than PS2 MIPS did.
Clang-compiled ARM64 GOAL is in the same class - 448 bytes for ten frames of `fact` - but the
deepest engine call chains have not been measured. `(suspend)`'s stack-overflow check now works
in AOT builds, so GOAL itself will report the problem if it happens.

**Guard pages.** A GOAL-memory stack has none: an overflow runs off the bottom of the region into
whatever the process heap put below it, silently. That is exactly the situation upstream is in, and
GOAL's own `(suspend)` check is the mitigation. A future improvement is to `mprotect` a page below
each thread stack, which is possible because the stacks come out of a heap the runtime controls.

**Nothing here needs writable-executable memory.** GOAL-memory stacks are data.

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
