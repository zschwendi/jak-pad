# jak1-kernel-core

The real OpenGOAL Jak 1 kernel subset, built as a static library for platforms that have no
desktop windowing, no IOP/sound emulation, no DECI2 listener transport, and no runtime code
generation. It is the library an iPadOS application links through an Objective-C++ bridge.

The shared translation units here never name a game: everything game-specific goes through the
per-game seam in `kernel_game.h`, implemented by `kernel_game_jak1.cpp` and
`kernel_game_jak2.cpp`. One game per library, chosen at link time: `jak1-kernel-core` is
everything documented below; `jak2-kernel-core` (**Experimental**) is the same core keyed to the
Jak 2 kernel, with `dgo_loader_jak2.cpp` carrying both the C-driven DGO load and the native
channel-3 incremental begin/continue/cancel RPC. Its top-level router answers DGO traffic while
delegating sound channels 0, 1, and 4 to `sound_rpc_jak2.cpp`, which handles command-aware framing,
the IRX 4.0 version handshake, checked SBlk loads, ordinary named-SFX PLAY/update commands, and
ordinary STR files. A bounded user-local bank is validated in memory before the same bytes reach
989snd, and unsafe PLAY falloff parameters are rejected before spatial volume calculation. The
upstream Jak 2 mips2c translations are registered through the native-function seam, and the kernel
owns its sound-system shutdown. Command 2 has no reply payload: its zero return only means the
synchronous transport completed, not that a bank loaded; failures are available through host logs
and `goal_jak2_sound_rpc_stats`. The shared pushed-state pad seam implements `cpad-open` and
`cpad-get-data` for Jak 2 as well as Jak 1. `install-handler` faithfully stores, replaces, or clears
the vblank and VIF1 handler references. A complete graphics host dispatches the retained vblank
handler immediately before its `syncv` callback; without that host the graphics functions remain
diagnostic stubs. `pc-rand` uses the same process-lifetime generator as upstream. Music and
streaming still report through the machine stubs. `--play-dma` installs a complete synchronous
headless graphics host and composes DMA measurement behind its `send-chain` callback, so one
frontier proves host dispatch, `syncv`, `sync-path`, and a valid completed chain without competing
for the GOAL symbol. `--play-gfx-host` isolates the same host boundary without parsing DMA. Both
discard every chain and never render. Plain `--play` retains its title-only
behavior, while the Jak 1 boot and gameplay probes retain their existing DMA measurement and
capture behavior. `display_tick_coordinator` is the matching portable host gate: while
foregrounded, one display callback synchronously runs one supplied frame; while paused, callbacks
are discarded without a queue or timestamp-derived catch-up. It does not own a display link,
renderer, or presentation. `jak1-gfx-host-test`, `jak2-gfx-host-test`, `jak2-pad-seam-test`,
`jak2-handler-seam-test`, `jak2-pc-rand-test`, `jak2-lightweight-machine-test`,
`jak2-dma-boundary-test`, `jak2-display-tick-coordinator-test`, and `jak2-sound-rpc-test` cover
those seams, while
`jak2-dgo-rpc-test` covers the exact 32-byte DGO protocol, composed-router delegation and rejection
behavior, and incremental AOT-code/data-object linking. All use original synthetic data only.
`jak2-data-boot-test` loads the player's own Jak 2 KERNEL.CGO through the AOT path and runs the Jak 2
kernel dispatcher headless. Its explicit `--with-game` mode also loads all of GAME.CGO as an
exploratory integration probe; the registered CTest does not enable that mode.
`--play` stops after the first linked title object. `--play-dma` additionally installs the
synchronous validation host and requires one complete 327-bucket Jak 2 graphics chain before
stopping. It requires matching host/DMA chain counts plus at least one `syncv` and `sync-path`
call, and explicitly reports zero rendered and zero presented frames.
`--play-gfx-host` instead requires at least one host chain, `syncv`, and `sync-path` call, prints
all graphics-host callback counts reached by the bounded dispatch, and does not require level
callbacks when that frontier has not reached them.
`jak2-thread-switch-test` drives the native ARM64 thread routines through the Jak 2 process and
thread layouts.

**Status: Experimental.** It initializes real kernel state, executes real Jak 1 GOAL code that was
compiled ahead of time by goalc's AOT C backend, and loads the player's own extracted game data out
of the game's DGO archives. No game data ships with it and none is ever written into the
repository: the data directory is configuration (`goal_kernel_core_set_data_directory`).

## What it does

`goal_kernel_core_initialize()` (see `kernel_core.h`):

1. maps 128 MiB of EE main memory and traps the low 512 kB,
2. runs the upstream kernel `*_init_globals` functions,
3. initializes the real global and debug `kheapinfo` heaps at the upstream memory layout,
4. allocates the GOAL print buffer through `init_output()`,
5. runs `jak1::InitSymbolAndTypes()` — the real symbol table allocation, `s7` setup, fundamental
   type bootstrap, and C-function symbol export from `game/kernel/jak1/kscheme.cpp`.

`goal_aot_load()` (see `aot_loader.h`) then loads an object file produced by
`goalc/aot/CBackend.cpp`:

1. copies the file's static data into the real global heap in one `kmalloc`,
2. creates one real GOAL `function` object per AOT function on that heap,
3. applies the file's relocations against the real symbol table with the same rules
   `game/kernel/jak1/klink.cpp` uses (`symlink_v3`, `typelink_v3`, `ptr_link_v3`),
4. and leaves the file's `top-level` function ready to run through `call_goal`.

`goal_sound_install()` (see `sound_rpc.h`) starts 989snd and takes over the sound RPC channels, and
`goal_sound_pull_audio()` is the seam a host renders from. See **Sound** below.

`goal_dgo_load()` (see `dgo_loader.h`) loads one of the game's DGO archives out of
`<data directory>/iso/`. It reads the archive with ordinary file calls instead of through the IOP,
and it splits every object two ways - see **Code and data in a DGO** below.

`InitSymbolAndTypes` is the front half of upstream `InitHeapAndSymbol`, split out unchanged.
`InitHeapAndSymbol` still exists and still does the same thing; it now calls
`InitSymbolAndTypes` and then does the DGO kernel load, listener init, and `InitMachineScheme`.

The library runs with `MasterDebug = 0`, `DebugSegment = 0`, and `MasterUseKernel = 0`, because
none of the subsystems those flags select are present.

## Building

Host (ARM64 macOS), as part of the normal build:

```sh
cmake --preset=Release-macos-arm64-clang
cmake --build build/Release/bin -j 4 --target jak1-kernel-core-smoke-test
./build/Release/bin/game/jak1-kernel-core-smoke-test
```

`jak1-kernel-core-smoke-test` prints the real values the kernel produced (heap pointers, `s7`, the
symbol table, the fundamental types read back out of the heap) and is registered with CTest.

```sh
cmake --build build/Release/bin -j 4 --target jak1-aot-execution-test
./build/Release/bin/game/jak1-aot-execution-test
```

`jak1-aot-execution-test` builds `kernel/gcommon.gc` and `kernel/gstring.gc` through
`goalc-cbackend` and the host C compiler, loads both into this kernel, runs their `top-level`
functions through `call_goal`, and then calls real Jak 1 GOAL functions by symbol. It also runs
GOAL code on a stack allocated from the real global heap, which is where GOAL's cooperative threads
put their stacks; see `docs/aot-stack-model.md`. It is registered with CTest.

```sh
cmake --build build/Release/bin -j 4 --target jak1-aot-boot-test
./build/Release/bin/game/jak1-aot-boot-test
```

```sh
cmake --build build/Release/bin -j 4 --target jak1-thread-switch-test
./build/Release/bin/game/jak1-thread-switch-test
```

`jak1-thread-switch-test` builds the kernel through `kernel/gstate.gc` plus
`test/goalc/aot/thread_switch_test.gc`, a fixture written in ordinary GOAL, and drives the routines
that had to be written in native ARM64: catch frames, `throw`, `go`, a state that suspends and
resumes, and temporary threads run with `reset-and-call`. Between the two resumes of the suspended
thread it overwrites every byte the thread had live, so a suspend that did not really copy the
stack out cannot pass. It also checks that `x19`-`x28` and `d8`-`d15` survive a context round trip,
using sentinels installed by `goal_thread_test_arm64.s`. It is registered with CTest.

`jak1-aot-boot-test` is the boot probe. It walks Jak 1's object files in the order
`goal_src/jak1/game.gp` builds them - `goalc-cbackend-sweep` reads that order out of the make
system and emits the C plus `aot_boot_manifest.c` - loads each one into the real global heap and
runs its `top-level` through `call_goal`. It reports how far it got and what stopped it rather
than only passing or failing.

It walks the first **511** of Jak 1's 518 object files and runs all 511 top-levels, ending
with 7900 symbols in the real symbol table. That is the whole GOAL kernel and engine: math and
geometry, DMA, the GS and display layer, the loader, textures, fonts, collision, the camera,
particles, moods, the level and entity systems, all of `engine/`, `levels/`, `pc/` and the game
task and menu code. Processes really spawn: `engine/gfx/mood/time-of-day.gc` and several later
files run `process-spawn` at top level, which reaches `(method new catch-frame)`, `throw` and
`enter-state`.

It stops there because file 512, `pc/hud-classes-pc.gc`, needs game data rather than runtime: its
`top-level` calls `activate-hud-pc`, which spawns a `hud-battle-enemy` whose `init-particles!`
dereferences `*fuelcell-naked-sg*`, an art group that only exists once level art has been loaded.
That dereference is a hard fault in the guard page rather than a reportable failure, so the walk
stops before it; `JAK1_AOT_BOOT_FRONTIER` in `game/CMakeLists.txt` is the number, and raising it is
how you find the next frontier. `jak1-data-boot-test` below loads the art and gets past it.

Machine-layer functions are asked for along the way and reported by
`goal_kernel_core_stub_machine_layer` when they are not implemented. Jak 2 installs portable
`file-stream-*` functions and the PC settings path/basic display queries; broader window, input,
and PC-port functions remain diagnostic stubs. The pad is another exception; see **The
controller** below, and so are the `scf-get-*` readers; see **The boot configuration** below.

```sh
cmake --build build/Release/bin -j 4 --target jak1-data-boot-test
./build/Release/bin/game/jak1-data-boot-test --synthetic
GOALPAD_JAK1_DATA_DIR=/path/to/out/jak1 ./build/Release/bin/game/jak1-data-boot-test
```

`jak1-data-boot-test` boots the game the way the game boots: `KERNEL.CGO`, then `GAME.CGO`, in DGO
order, and then runs the engine's own startup. It is registered with CTest three times.

`--synthetic` writes a DGO archive of its own into a temporary directory and loads it. It needs no
game data, so it always runs, and it checks the archive reader, the code/data rule, and that an
untranslated code object fails the load instead of being skipped.

The default run needs the player's own extracted data, given by `--data-dir` or
`GOALPAD_JAK1_DATA_DIR` - the directory holding `iso/` and `fr3/`, what `goal_src/jak1/game.gp`
calls `$OUT`. With no directory it prints what it needs and reports success, so the suite stays
green for anyone without game data.

With data it loads all 8 objects of `KERNEL.CGO` and all 346 of `GAME.CGO` - 334 code objects from
the AOT path and 20 data objects linked out of the archives, including the texture-page directory,
nine texture pages, and the `eichar`, `sidekick`, `fuel-cell` and `fuelcell-naked` art groups. The
global heap ends at 32.9 MB with 5700 symbols, and `hud-classes-pc`, the file the probe above
cannot reach, loads and runs.

```sh
GOALPAD_JAK1_DATA_DIR=/path/to/out/jak1 \
  ./build/Release/bin/game/jak1-data-boot-test --play --frames 60
```

`--capture-dma <file>` writes one of those frames to a file so a renderer can replay one real frame
of Jak 1 DMA without the rest of the runtime. See **Capturing a frame** below. A capture is derived
from the player's own game data: it goes where the caller says and never into the repository.

`--play` calls the engine's own `play`, which is the last thing upstream's `InitMachineScheme`
does. It allocates the level heaps and drives the whole title-level load itself, through GOAL's
loader rather than the C one: the DGO RPC and `link-begin` below. `--frames N` then calls
`kernel-dispatcher` N times, which is `KernelCheckAndDispatch`'s loop body without the listener
half - the GOAL kernel's own frame, running processes, states and level streaming.

With data, `--play --frames 60` loads `TIT.DGO` (15 objects) and then `VI1.DGO` (55 objects,
including a 7.3 MB BSP) through the RPC, reaches `GAMEPLAY: enter title`, displays the title level,
streams in `village1`, and swaps the visibility data - reading each level's `.VIS` through the
ramdisk RPC and checking, through `update-vis!`'s own check, that every swap decompressed to bits
the BSP allows. The global heap ends at 55.6 MB, and the 60 frames build 60 DMA chains, the largest
640 kB. The frame loop itself is not limited to 60: 10000 consecutive frames run the same way. The
CTest entry uses 60 so the suite stays quick.

## The boot configuration

Upstream, `jak1::goal_main` (`game/kernel/jak1/kboot.cpp`) fills in a `masterConfig` block from the
PS2's system configuration before `InitMachine` runs - the aspect ratio, the console's language, and
the console's master volume, which the PC port fixes at 100. That file is the desktop entry point
and is not part of this library, so `kernel_core.cpp` does the same block itself, and the seven
`scf-get-*` readers GOAL calls to read it are implemented in `desktop_seams.cpp` out of upstream's
`Decode*` in `game/kernel/common/kmachine.cpp`.

They used to be stubs, and a stub returning 0 was not a neutral answer here. `settings.gc` builds
`*setting-control*`'s defaults from `(scf-get-volume)`, and the memory card only carries three of
them back:

| default | from `scf-get-volume` = 100 | from a stub returning 0 |
| --- | --- | --- |
| `sfx-volume`, `music-volume`, `dialog-volume` | overwritten by the memory card (`pckernel.gc`) | same |
| `ambient-volume` | 75 | 0 |
| `sfx-volume-movie`, `ambient-volume-movie` | 55 | 0 |
| `music-volume-movie` | 65 | 0 |
| `dialog-volume-hint` | 80 | 0 |

`apply-settings` then derives the ambient group's volume as
`0.01 * ambient-volume * sfx-volume`, so with a zero there the 989snd ambient group (the group a
sound's `.SBK` entry names, not the one GOAL passes) sat at master volume 0 for the whole run: in
Sandover Village that is `water-lap`, `bird-2` and `welding-loop`, the entire environmental bed.
Worse, `ambient.gc` applies the `-movie` and `-hint` volumes *as a percentage of the current one*
while ambient speech or a hint is up - `(add-setting! 'sfx-volume 'rel ... )` - so a zero there
multiplied the sfx, music and dialog groups by zero for the duration.

Measured on a 4000-frame scripted gameplay run with the music suppressed, the six seconds of
standing in Sandover Village go from RMS 6-31 (peak 49-148, effectively silence) to RMS 56-165
(peak 447-729), and the walking sections gain 20-40%.

## The controller

There is no input library here, so the host reads a controller and pushes what it read in, once per
frame, before the frame runs. `pad.h` is that seam:

```c
goal_pad_install();                       /* implements cpad-open and cpad-get-data */
goal_pad_state pad;
goal_pad_state_neutral(&pad);             /* connected, nothing held, sticks centered */
pad.buttons |= GOAL_PAD_START;
goal_pad_set_state(0, &pad);              /* before each frame */
goal_pad_get_rumble(0, &large, &small);   /* what GOAL asked the motors to do */
```

`goal_pad_install` replaces the two machine-layer stubs with `CPadOpen` and `CPadGetData`, copied
from `game/kernel/common/kmachine.cpp` - see the note at the top of `pad.cpp` for why they are
copied rather than compiled - over an EE pad library written against the pushed state instead of
against SDL. GOAL sees the PS2 `cpad-info` structure exactly as `engine/ps2/pad.gc` expects it, so
`service-cpads`, `cpad-pressed?` and the analog sticks work with no GOAL change. Nothing in the
library knows about SDL, GameController, a keyboard, or a window. `--pad-seam` checks that contract
against the real Jak 1 structure, and `jak2-pad-seam-test` checks registration plus the shared
132-byte controller prefix through the Jak 2 symbol table while preserving Jak 2's eight-byte tail.
Neither test needs game data; both are registered with CTest.

The boot test's own host is a script on the command line:

```sh
GOALPAD_JAK1_DATA_DIR=/path/to/out/jak1 ./build/Release/bin/game/jak1-data-boot-test \
  --play --frames 2400 --report-state \
  --press start@target-title-wait+60 --press x@target-title-wait+160 \
  --press down@target-title-wait+240 --press down@target-title-wait+260 \
  --press down@target-title-wait+280 --press down@target-title-wait+300 \
  --press x@target-title-wait+340 \
  --stick 127,0@target-stance+600:400 --press x@target-walk+700
```

`--press <buttons>@<when>[:<frames>]` and `--stick <x>,<y>@<when>[:<frames>]` hold something for a
few frames; buttons are the `pad-buttons` names joined with `+`, and a stick axis is a byte with
127 centered. `<when>` is either a frame number or the name of a target state plus an offset, which
is what the example uses: the game's own loading decides when the title sequence ends, so a script
that says "sixty frames after the title starts waiting" runs the same on a slow machine as a fast
one. `--report-state` prints `*master-mode*`, the state `*target*` is in, where it is standing, and
the progress menu's screen and selected option, whenever any of them changes.

That script is what the `jak1-gameplay-test` CTest entry runs, and it is the game being played:
Start opens the title menu (`progress-screen title`), X on *new game* moves to `save-game-title`,
four downs reach *do not save*, and X there runs `(initialize! *game-info* 'game #f "intro-start")`
and returns to `'game`. The title level is discarded, `village1` becomes the displayed level, and
`target` respawns in Sandover Village at (40.1 3.7 827.8) metres. The stick then walks it, and X
jumps. `--expect-state <name>` fails the run if the target never entered a state, and
`--expect-travel <metres>` fails it if the target never moved, so the entry is a check rather than
a log.

A longer run with the stick pointed in eighteen directions in turn reaches, in this order:
`target-continue`, `target-title`, `target-title-play`, `target-title-wait`, `target-stance`,
`target-clone-anim`, `target-walk`, `target-falling`, `target-jump`, `target-hit-ground`,
`target-duck-stance`, `target-duck-walk`, `target-wade-walk`, `target-swim-stance`,
`target-swim-walk`, `target-swim-jump`, `target-swim-jump-jump` and `target-death` - and after the
drowning, `target-continue` again and back to walking. 14000 frames, 6.1 km travelled, no crash.

Two things in that run are reported and are not the runtime's doing. The spooled animation
`sage-intro-sequence-b` asks for `SAISB.STR`, which no Jak 1 extraction contains - `copy-strs` in
`goal_src/jak1/game.gp` does not name it - so the STR RPC reports a miss and the sequence carries
on. And `sage-intro-sequence-a`, which does load, prints `could not find a master slot to link`
from `link-art!`: the spooled animation arrives before its master art group is in a loaded level,
so the intro conversation does not animate. Neither stops the game.

### Level transitions

Jak 1 is a connected world, and the two things that move a player between its levels are a
continue point and a load boundary. Both are scriptable, in this test and in `goalpad-play`:

- `--warp <continue>@<when>` runs `(start 'play (get-continue-by-name *game-info* <name>))`, the
  same call a warp gate makes (`kernel/core/continue_warp.h`). `target-continue` then wants the
  point's two levels, waits for them to reach `'active`, and puts the target there. The names are
  the ones in `engine/level/level-info.gc` - `village1-hut`, `beach-start` and so on. Warping to a
  continue point without the `intro` flag also turns the border machinery on through that state's
  own exit, which starting a new game does not: `"intro-start"` carries `intro`, and the intro
  cutscene that would normally hand over to a plain continue point does not play here.
- `--walk-to <x>,<z>@<when>[:<frames>]` walks the target to a place, in metres
  (`kernel/core/scripted_walk.h`). The stick is camera-relative and nothing here reads the camera,
  so the autopilot measures what the camera contributes - the angle between the stick it pushed
  and the way the target actually moved - and aims with the running mean of it. Legs run in order,
  each until it arrives or its `:<frames>` limit runs out, so a route is a list of places.
- `--report-levels` prints the level system whenever it changes: what `*load-state*` wants, each
  level slot's name, status, display flag and heap use, and whether the target is on a border.
- `--report-boundaries` (this test only) dumps `*load-boundary-list*`, the rings of vertices the
  loaded levels define with a command for each crossing direction - which is how a route gets
  aimed at the game's own boundary rather than guessed at.

`jak1-level-boundary-test` walks a new game from the village hut across the village1/beach display
boundary and fails unless `beach` reaches `'active`. `jak1-level-tour-test` warps through ten
continue points - beach to training - and fails if any of their levels never reaches `'active` or
the global heap grows more than 256 kB, since level code and data must live and die with the level
heaps. Both run under CTest with `GOALPAD_JAK1_DATA_DIR` set.

The same route runs under the renderer: `goalpad-play` with the new-game presses, a warp to
`village1-hut`, and walk legs down the village stream, through the culvert at the west end, onto
the Sentinel Beach sand and back, shows `beach` wanted, loaded, displayed and entered - and
undisplayed again on the way home. The GOAL side discards what it leaves behind (the level slot
that held `misty` is reused for `beach`); the Metal host currently keeps every level's art
resident, which is bounded by the levels visited, not by the crossings made.

## The renderer

`gfx_host.h` is the drawing seam, and it is the same shape as the controller seam above: a table of
plain C function pointers that a host fills in. Nothing in this library knows about Metal, SDL, a
window or a display link.

```c
goal_gfx_host host = {0};
host.send_chain        = ...;  /* __send-gfx-dma-chain: the frame's DMA chain */
host.vsync             = ...;  /* syncv: block until the renderer presented   */
host.sync_path         = ...;  /* sync-path                                   */
host.texture_upload_now = ...; /* __pc-texture-upload-now                     */
host.texture_relocate  = ...;  /* __pc-texture-relocate                       */
host.set_levels        = ...;  /* __pc-set-levels: desired/loading levels     */
host.set_pmode_alp     = ...;  /* put-display-env's blackout alpha            */
host.set_active_levels = ...;  /* __pc-set-active-levels: Jak 2 active levels */
goal_gfx_host_install(&host);
```

`goal_gfx_host_install` also implements the PS2 graphics calls that have nothing left to do once
the hardware is gone - `reset-path`, `reset-graph`, `dma-sync`, the GS IMR pair, and
`gs-store-image` - returning 0, which is what upstream's desktop port does. The shared portable
machine seam owns the `flush-cache` no-op. Every entry may be left NULL, in which case that
function keeps the machine layer's reporting stub.

The level callbacks are more than forwards. Jak 1 passes its two desired level names directly to
`__pc-set-levels`. Jak 2 passes a pointer to its six-entry desired-level array and separately calls
`__pc-set-active-levels` with its six-entry active-level array. The game adapters decode those
different layouts, and the shared seam drops `"none"` placeholders before borrowing the remaining
names to the host. This is how the renderer learns which levels' art to have on the GPU without
anything being hardcoded.

The signatures the host sees are the renderer's, not GOAL's: `send_chain` takes EE main memory and
the chain's GOAL pointer, which are exactly `GfxRendererModule::send_chain`'s two arguments.
`game/goalpad_play.cpp` is the desktop host that fills this in from the Metal renderer; an iPadOS
bridge fills in the same callbacks from a `CAMetalLayer`.

DMA diagnostics do not need to replace that host. `goal_gfx_dma_reset()` clears the measurement
window, and a renderer or validation host may call `goal_gfx_dma_observe_chain()` inside its own
`send_chain` callback before consuming or dropping the same chain. `goal_gfx_dma_install()` remains
the compatibility helper for headless Jak 1 probes that intentionally let measurement own the
GOAL symbol directly.

## Capturing a frame

`__send-gfx-dma-chain` is where a frame's work leaves GOAL. `dma_capture.cpp` follows the chain
with the same `FixedChunkDmaCopier` the renderer uses, then walks the copy again. For Jak 1 that
second walk follows the renderer's bucket dispatch and reports what each bucket was given. For the
headless Jak 2 frontier it follows the exact 327-entry direct bucket array, verifies terminal
completion and the copier's tag and payload totals, and records each bucket without drawing it.
`jak2-dma-boundary-test` covers an empty array and one carrying a PC-port texture upload. Jak 2
capture files and replay remain outside this frontier because the current GPDMACAP format
describes the Jak 1 renderer inputs.
`--dma-frame-report` prints the Jak 1 bucket table, one line per frame.

Two numbers matter and they are not the same. *Payload* is what the chain's tags transfer, which is
what says whether a frame drew anything. *Copied* is chunk-granular - how far apart in EE memory
the chain was spread - and it moves for reasons that have nothing to do with content.

```sh
GOALPAD_JAK1_DATA_DIR=/path/to/out/jak1 ./build/Release/bin/game/jak1-data-boot-test \
  --play --frames 1000 --capture-dma-dir /tmp/goalpad-dma-capture \
  --capture-dma-frames 100 --capture-dma-min-payload 150000 --capture-dma-count 3
```

- `--capture-dma <file>` with `--capture-dma-frame N` writes frame N (1-based; the default is 1).
- `--capture-dma-dir <dir>` with `--capture-dma-frames a,b,c` writes each frame as
  `dma-frame-<N>.gpdma`.
- `--capture-dma-min-payload <bytes>` with `--capture-dma-count <k>` writes the next `k` frames
  whose payload reaches that size, into the same directory. Which frame the game draws a given
  thing on is not fixed - the level loader runs off the wall clock, so the frame numbers move
  between runs - so asking by content is how a caller gets the frame it meant.

A frame that was asked for and never arrived fails the run rather than leaving no file and saying
nothing.

The file format is version 2, `GPDMACAP`, and is documented byte for byte at the top of
`dma_capture.cpp`. In outline: an 80-byte header (frame number, chunk size, EE memory size, `s7`,
the EE base GOAL address, and the offset and length of each section), then the chain exactly as
version 1 wrote it (`FixedChunkDmaCopier::serialize_last_result`), then a snapshot of EE main
memory as the same fixed chunks with the all-zero ones left out and each of the rest compressed on
its own with LZO.

The EE snapshot is there because the chain alone cannot be replayed. The PC port's texture-upload
packets in it carry EE addresses of GOAL `texture-page` structures, which live in the level and
global heaps; `TexturePool::handle_upload_now` reads them, and the texture names, out of EE memory.
`s7` is in the header for the same reason: that upload path does GOAL-pointer arithmetic against
it. The capture checks that every texture-page address the frame names is inside a chunk it kept.

### What the frames hold

Measured with the player's own data, `--play --frames 1000`:

| frame | payload | what is in it |
| --- | --- | --- |
| 1 | 48 bytes | GS state and nothing else - the first chain after boot draws nothing |
| ~100 | 14 kB | title level only: the logo and Jak and Daxter in the merc buckets, "Press Start" in the sprite bucket |
| ~840 | 154 kB | the title screen with Sandover Village behind it: ocean near/mid/far, tie, tfrag, shrub, sky, sprite, merc |

The title sequence is driven by the spooled animations `ndi-intro` and `logo-intro`
(`levels/title/title-obs.gc`), whose `command-list` brings `village1` in behind the logo. It
streams through the STR RPC and it runs: by frame ~520 the background is being drawn every frame,
and the target reaches `target-title-wait`, the state that waits on `(cpad-pressed? 0 start)`.
Pressing Start there leaves the title screen; see **The controller** below.

The bucket walk reports 72 buckets for Jak 1. Seventy of them are buckets; the walk cannot know
that number, so it keeps going to the end of the chain and the last two entries are the chain's
ending data, which the renderer's own bucket dispatch also walks past.

## Code and data in a DGO

A DGO holds two kinds of object file and the linker already tells them apart
(`is_opengoal_object` in `game/kernel/common/klink.cpp`). **v3 objects are code**: x86-64 machine
code emitted by goalc, which nothing here can execute. **v2 and v4 objects are data**: art groups,
texture pages, the game count - no machine code at all, only structures and relocations, so the
real linker's output is the same on any architecture.

So: **code comes from the AOT path, data comes from the DGO.** For a v3 object the bytes read out
of the archive are discarded and the AOT translation unit registered under the same name
(`goal_aot_register_object`) is loaded in its place, with its top-level run where the linker would
have run the object's. For a v2/v4 object the bytes are copied into the heap and linked by
`link_and_exec` exactly as upstream does, ending in the object type's own GOAL `login` method,
which is itself AOT code. A v3 object with no registered translation fails the load; it is never
skipped.

## The two ways a DGO gets loaded

`goal_dgo_load` is the C-driven load, and it is what the boot uses: it reads a whole archive and
returns. GOAL's own level loader does not use it. `engine/load/load-dgo.gc` and
`engine/level/level.gc` drive a DGO one object per frame through the overlord's RPC (`rpc-call` /
`rpc-busy?`) and link each object with `link-begin` / `link-resume`.

`goal_dgo_install_goal_loader` answers that RPC out of the same reader. There is no IOP, so the
RPC is answered where it is sent: `rpc-call` does the work and returns, and `rpc-busy?` is
therefore always 0. A whole object file arrives inside one `rpc-call` instead of streaming while
the EE runs, but that is a difference in *when* the bytes arrive, not in what GOAL sees - GOAL
still gets one object per frame, still toggles between its two load buffers, still gets the last
object at the heap top, and still sees `more` until the archive is done. The same call installs a
`link-begin` that applies the code/data rule above, so both ways in agree about what an object
file is.

Channel 4, the STR RPC, is answered too: it loads a whole file, or one chunk of a chunked one,
straight into GOAL memory. The game uses it for the text and subtitle banks and for spooled art -
the animations in `engine/load/loader.gc`, which link whatever comes back, which is why it is
implemented rather than stubbed.

`link-begin` puts an object file's code in the heap the caller named, which is what upstream's
linker does: the boot names the global heap and a level DGO names the level's own heap
(`dgo-load-link` in `engine/load/load-dgo.gc`), so `(method unload! level)` frees a level's code
along with its data. A file in the global heap is loaded once and reused; a file in a level heap is
relinked every time the level loads, because the heap it was in has been reset under it.

```sh
GOALPAD_JAK1_DATA_DIR=/path/to/out/jak1 ./build/Release/bin/game/jak1-data-boot-test \
  --play --frames 200 --level-frames 600 --levels title,title+village1,title
```

`--levels` names the levels the load state should want, one step at a time (`a`, `a+b`, or `none`),
running `--level-frames` frames between steps, and reports the global heap after each. It is how
the paragraph above is measured rather than asserted: cycling `village1` in and out four times
links 598320 bytes of level code and leaves the global heap at the same 55483634 bytes it started
at. It is registered with CTest.

Channel 2, the ramdisk RPC, is answered as well. Upstream's overlord keeps one whole file in the
IOP's spare RAM and hands the EE 2 kB windows of it; only visibility uses it. `vis-load` in
`engine/level/level.gc` asks for `<nickname>.VIS`, and `(method update-vis! level)` in
`engine/load/decomp.gc` then asks for the compressed vis string of the camera's current BSP leaf.
Here the file lives in host memory instead of the IOP's. A vis string near the end of the file is
shorter than the 2 kB GOAL always asks for - `decomp.gc` calls that "a worst case if the string
can't be compressed" - so a short read is normal and the rest of the window is zeroed rather than
being filled with whatever follows.

## Sound

Channels 0 and 1 are the sound RPC, and `sound_rpc.cpp` answers them the same way: where they are
sent. Upstream they reach two IOP threads in `game/overlord/jak1/srpc.cpp` - `RPC_Player`, which
starts and positions sounds, and `RPC_Loader`, which loads banks and music - and those two
functions are what that file re-implements. Everything they call that is not the IOP is compiled in
unchanged and used as-is: the 64 sound slots with their volume, pan and falloff math
(`game/overlord/common/ssound.cpp`), the 6 bank slots (`sbank.cpp`), and 989snd itself
(`game/sound/**`) - the sequencer, the SPU voice model and the mixer, which are portable C++ with no
threads, no SIMD and no OS calls.

What is different is where the files and the samples come from. A bank is not requested from a file
server and waited on: `<name>.SBK` and `<name>.MUS` are read out of `<data directory>/iso/` and
handed straight to 989snd, which is what `FS_LoadSoundBank` does once upstream's ISO thread has
scheduled it. There is no vblank interrupt, so `goal_sound_frame()` stands in for `VBlank_Handler`:
it runs the music fade and writes the `sound-iop-info` block back to the EE.

989snd's own output backend is cubeb, which needs a desktop audio device. This library has none, so
it is compiled with `GOALPAD_SND_NO_CUBEB` and the seam is a pull instead:

```c
int goal_sound_sample_rate(void);                      /* 48000 */
int goal_sound_pull_audio(int16_t* out, int frames);   /* interleaved stereo 16-bit */
```

A sound's group is the one its `.SBK` entry names, and each group has a master volume the game
sets. Those volumes come out of `*setting-control*`, which is built from the boot configuration -
see **The boot configuration** above for why a stubbed `scf-get-volume` muted the ambient group and
every group under ambient speech.

`goal_sound_pull_audio` calls the same `snd::Player::Tick` the desktop port's audio callback calls,
through `snd_PullAudio` in `game/sound/sndshim.cpp`. A CoreAudio or AVAudioEngine render callback
can call it directly - 989snd takes its own lock, so the caller may be the device's thread - and the
boot test calls it once per game frame. 48 kHz is not a preference: 989snd's 240 Hz sequencer tick,
its note-pitch conversion and its envelope timing are all derived from the output rate, so a host
that cannot open a 48 kHz device has to resample rather than ask for another rate.

**What plays.** Everything sequenced. Sound effects come out of the `.SBK` banks the game loads
(`common`, `empty1`, `empty2`, then the level's own), and music comes out of the level's `.MUS`
bank, which 989snd drives through its MIDI/AME handlers. Music is not started by a command: as
upstream does, the player channel restarts the loaded music bank's sound 0 under the internal id 666
whenever it is not already running, and re-fades it in.

**Streamed audio.** The spooled cutscene and dialogue audio in `VAGWAD.<lang>` is answered by
`vag_stream.cpp`: `VAGDIR.AYB` is the directory of streams, and one of 989snd's raw SPU voices plays
out of a 0xC000-byte double buffer that `goal_sound_frame` refills. It is the same state machine as
upstream's ISO thread VAG cases plus `game/overlord/jak1/stream.cpp`, with the thread taken out.
`sound_rpc.cpp` drives it from the `spool-` prefix on the player channel, from channel 5, and from
the pause/stop/volume commands that address the stream by sound id. What is still absent is the
'STRV' plugin, the path where a music sequence itself queues a stream.

A run reports what it could not play rather than being quieter than it should be: a spool request
naming a stream `VAGDIR.AYB` does not have, a play request naming a sound no loaded bank has, and an
RPC command this implementation does not handle.

```sh
GOALPAD_JAK1_DATA_DIR=/path/to/out/jak1 ./build/Release/bin/game/jak1-data-boot-test \
  --play --frames 4000 --capture-audio /tmp/jak1-title.wav
```

`--capture-audio` drives the seam and writes an ordinary 48 kHz stereo 16-bit WAV. There is no audio
device and nothing waits on one, so the sample clock is the game's: one 60th of a second is pulled
after each `kernel-dispatcher` frame, and the file is exactly as long as the frames the run
executed. Like a DMA capture it is derived from the player's own game data, so it goes where the
caller says and never into the repository.

With data, `--play --frames 4000` loads 4 sound banks and `VILLAGE1.MUS` - the title level's
`music-bank` in `engine/level/level-info.gc` - and the WAV holds 66 seconds of the title screen.
It is silent for the first 8: `target-title` zeroes the music volume while the intro plays, and
`target-title-wait` puts it back. After that the sequenced Sandover Village theme plays continuously
to the end of the run. It is quiet - peak 3416 of 32767 - and that is the game's own volume, not an
attenuation added here: the PC port's default music volume is 40 of 100
(`memcard-volume-music` in `pc/pckernel-impl.gc`), so the music group ends at 351 of 1024, and 989snd's
synth master is 0x3FFF of 0x8000 on top of that. The run reports the group volume it ended at.

`jak1-title-audio-test` is the same run at 1400 frames with `--require-audio`, which measures the
mixed audio without writing it and fails on silence. It is registered with CTest and, like the rest,
reports that it was skipped when there is no data directory.

The audio capture and the scripted controller of **The controller** above are the same run: one
frame loop drives the pad, runs the frame, and pulls that frame's audio. The `jak1-gameplay-test`
script at 4000 frames with `--capture-audio` produces 66.67 seconds that follow what the game did.
It is silent through `target-title` (the intro zeroes the music volume), the title theme comes in at
`target-title-wait` around frame 480 with the progress menu's own sounds over it, it goes quiet
again while the title level is discarded and `village1` is displayed, and from `target-stance` in
Sandover Village to the end of the run the village music plays continuously with the target's
footsteps and jumps in it. The same script run with `--no-sound` enters the same states on the same
frames and travels the same distance, so the audio is being pulled alongside the simulation rather
than changing it.

`--no-sound` boots without starting 989snd at all, which puts the sound channels back on the
machine-layer stub path.

## Playing it

`goalpad-play` (`game/goalpad_play.cpp`) is this library and the Metal renderer in one process: the
game boots, runs its own frame loop on its own thread, and the frames it builds are drawn in a
window. See **The renderer** above for the seam, and `docs/metal-renderer-path.md` for the
threading and the results.

```sh
cmake --build build/Release/bin -j 4 --target goalpad-play
./build/Release/bin/game/goalpad-play --data-dir /path/to/out/jak1
```

Return or the controller's Start leaves the title screen. The keyboard layout is the PC port's own
default (`game/system/hid/input_bindings.cpp`): WASD and the arrows, Space for X, E/F/R for
circle/square/triangle, Q/O and 1/P for the shoulders. Any SDL-recognised controller works, and
`goal_pad_get_rumble` drives its motors.

Standalone static library for a device build:

```sh
cmake -S . -B build/ios-kernel-core -G Ninja \
  -DOPENGOAL_BUILD_JAK1_KERNEL_CORE_ONLY=ON -DBUILD_TESTING=OFF \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/ios-kernel-core -j 4
```

Use `-DCMAKE_OSX_SYSROOT=iphonesimulator` for the simulator. The resulting archive links against
only `libc++` and `libSystem` - 989snd is in it and brings no audio backend with it, so nothing has
to be linked for sound either. What the iPad still needs is a device: an `AVAudioEngine` source node
or an `AudioUnit` render callback that asks `goal_sound_pull_audio` for `inNumberFrames` at 48 kHz.

The same AOT proofs can be built for a device or the simulator. The generated C comes from the host
build above; only the compiler target changes. `jak1-aot-boot-test` and `jak1-thread-switch-test`
build the same way from `build/Release/bin/game/aot-boot` and `.../aot-thread-switch` - one
`clang -c` per generated `.c`, then the driver, then a link against the archive.

Jak 2 has a CMake-managed iPhoneOS final-link proof for its complete externally generated corpus.
First build `jak2-data-boot-test` on the ARM64 host to generate
`build/Release/bin/game/aot-boot-jak2`, then configure the device-only build:

```sh
cmake -S . -B build/ios-jak2-aot-link -G Ninja \
  -DOPENGOAL_BUILD_JAK2_IPHONEOS_AOT_LINK_ONLY=ON \
  -DOPENGOAL_JAK2_AOT_DIR="$PWD/build/Release/bin/game/aot-boot-jak2" \
  -DBUILD_TESTING=OFF -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/ios-jak2-aot-link -j 4 \
  --target jak1-kernel-core jak2-kernel-core \
  jak2-iphoneos-aot-corpus jak2-iphoneos-full-aot-link
```

Configuration and every build validate the manifest, its declared count, and the exact corpus of
840 generated C/header pairs. `jak2-iphoneos-aot-corpus` compiles those 840 units plus
`aot_boot_manifest.c` with `-O3 -fno-strict-aliasing` into
`libopengoal-jak2-aot-corpus.a`; consumers receive the generated manifest include path and the
`jak2-kernel-core` dependency through the CMake target. The final executable links that reusable
archive and retains the complete manifest. It must not link the Jak 1 AOT product in the same
executable because the generated games export overlapping symbols. The Ninja build remains
unsigned. This is a device-SDK archive/link proof, not the shipping GOALPad application or a
runtime result.

The explicit `-O3` is part of the AOT stack contract, not a performance-only preference. This
iPhoneOS-only CMake path returns before OpenGOAL's global compiler defaults; without a target-local
option, Xcode compiled the corpus at `-O0`, and the first Jak 2 time-of-day process consumed 976
bytes at suspension against its valid 896-byte converted backup buffer. The optimized function
uses the same stack model as the host Release build and passes that boundary without changing the
game-authored stack request.

The same target can be generated as a normally signable iPhoneOS app bundle without embedding a
personal team or product identity in the project. Pass a unique bundle identifier and an Apple
development team to opt into Xcode automatic signing:

```sh
cmake -S . -B build/ios-jak2-aot-signed -G Xcode \
  -DOPENGOAL_BUILD_JAK2_IPHONEOS_AOT_LINK_ONLY=ON \
  -DOPENGOAL_JAK2_AOT_DIR="$PWD/build/Release/bin/game/aot-boot-jak2" \
  -DOPENGOAL_JAK2_IPHONEOS_BUNDLE_IDENTIFIER=org.example.Jak2AOTLinkProof \
  -DOPENGOAL_JAK2_IPHONEOS_DEVELOPMENT_TEAM=TEAMID1234 \
  -DBUILD_TESTING=OFF -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0
xcodebuild -project build/ios-jak2-aot-signed/jak.xcodeproj \
  -scheme jak2-iphoneos-full-aot-link -configuration Release -sdk iphoneos \
  -destination 'generic/platform=iOS' -allowProvisioningUpdates build
```

An empty development-team setting keeps Xcode code signing disabled. A successful signed generic
iOS build proves only bundle formation, final linking, and signing. It does not prove launch,
device execution, gameplay, renderer integration, or the shipping application's runtime bridge.

The generated Xcode project also contains an excluded-from-all development target named
`jak2-iphoneos-display-tick-proof`. It is a programmatic UIKit diagnostic that starts the portable
Jak 2 runtime on a background queue and offers each main-run-loop `CADisplayLink` callback to the
portable display-tick coordinator. Each accepted callback synchronously runs exactly one kernel
dispatcher frame. Inactive and background callbacks are paused rather than accumulated, and a
return to the foreground never runs catch-up frames.

The app links the excluded `jak2-iphoneos-runtime` static library and a narrow Objective-C++ Metal
host. The runtime driver links the validated AOT corpus as a dependency instead of becoming
another member of the generated-code archive, preserving the manifest-plus-840-translation-unit
corpus boundary. The Metal product archive selects no kernel; this target links it with
`jak2-kernel-core` only.

The target reads the player's prepared data from the app's `Documents/Jak2` directory, which is
visible through Files and Finder file sharing. A development launch may override that path with
the `GOALPAD_JAK2_DATA_DIR` environment variable. Saves remain local in
`Application Support/OpenGOAL/jak2/saves`. Configure a distinct bundle identifier and build the
target explicitly:

```sh
cmake -S . -B build/ios-jak2-display-tick -G Xcode \
  -DOPENGOAL_BUILD_JAK2_IPHONEOS_AOT_LINK_ONLY=ON \
  -DOPENGOAL_JAK2_AOT_DIR="$PWD/build/Release/bin/game/aot-boot-jak2" \
  -DOPENGOAL_JAK2_IPHONEOS_DISPLAY_TICK_BUNDLE_IDENTIFIER=org.example.Jak2DisplayTickProof \
  -DOPENGOAL_JAK2_IPHONEOS_DEVELOPMENT_TEAM=TEAMID1234 \
  -DBUILD_TESTING=OFF -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0
xcodebuild -project build/ios-jak2-display-tick/jak.xcodeproj \
  -scheme jak2-iphoneos-display-tick-proof -configuration Release -sdk iphoneos \
  -destination 'generic/platform=iOS' -allowProvisioningUpdates build
```

The same development target can be generated for the arm64 iOS Simulator by replacing the SDK
settings in that configure command with `CMAKE_OSX_SYSROOT=iphonesimulator` and then building with
`-sdk iphonesimulator -destination 'generic/platform=iOS Simulator'`. The simulator result does not
replace physical-device validation.

Without an opt-in environment variable, the external host copies each real Jak 2 DMA chain and
dispatches its 327 audited DeferredSkip,
StrictEmpty, or audited Direct buckets synchronously through `MetalRenderer`. It deliberately
supplies no `CAMetalLayer`, so command-buffer commits, drawables, submissions, and presentations
must all remain zero. The separate synthetic submit/readback proof covers offscreen command-buffer
completion and pixels. The on-screen development proof stops after a bounded interval when the
title DGO, the completed policy dispatch, and the graphics synchronization calls have all been
observed. It still has no presented surface, audio output, or input UI. A successful run proves
only the signed display-clock-to-Metal-policy boundary, not a rendered title screen, gameplay, or
physical-device compatibility.

**Experimental:** setting `GOALPAD_JAK2_CAMETAL_LAYER_PROOF=1` selects a data-independent Metal
mode in this development target. It creates a real app-owned `CAMetalLayer`, consumes exactly one
`CADisplayLink` callback, and submits the public synthetic 327-bucket Jak 2 chain through the
existing product dispatcher. The synthetic chain contains no game data and encodes no draw. The
display link is paused immediately after submission, and PASS is emitted only after bounded GPU
completion with exactly one drawable, command-buffer commit, completion, and submission and no
miss, late submission, command-buffer error, presentation drop, or ordering error. iOS Simulator
does not expose the retained physical drawable callback, so that counter must remain zero there
and command-buffer completion is the simulator gate.

The synthetic Metal mode does not consume the game's DMA, display a title screen, prove gameplay,
or establish physical-device compatibility. Future real-DMA presentation should extend the
external product host rather than treating this bounded synthetic presenter as a second runtime
renderer.

```sh
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
GEN=build/Release/bin/game/aot-generated
TARGET=arm64-apple-ios18.0            # or arm64-apple-ios18.0-simulator with the simulator SDK

clang   -c -O2 -fno-strict-aliasing -std=c11   -target $TARGET -isysroot "$SDK" \
        -I . -I $GEN -o gcommon.o $GEN/gcommon.c
clang   -c -O2 -fno-strict-aliasing -std=c11   -target $TARGET -isysroot "$SDK" \
        -I . -I $GEN -o gstring.o $GEN/gstring.c
clang++ -c -O2 -std=c++20 -target $TARGET -isysroot "$SDK" -DFMT_HEADER_ONLY=1 \
        -I . -I third-party -I third-party/fmt/include -I third-party/SDL/include -I $GEN \
        -o aot_execution_test.o game/kernel/core/aot_execution_test.cpp
clang++ -target $TARGET -isysroot "$SDK" -o jak1-aot-execution-test-ios \
        aot_execution_test.o gcommon.o gstring.o build/ios-kernel-core/libjak1-kernel-core.a
codesign -s - -f jak1-aot-execution-test-ios
```

The AOT GOAL functions land in `__TEXT,__text` (`initprot r-x`); the binary has no
writable-executable segment and signs normally. The simulator build runs with
`xcrun simctl spawn <device> ./jak1-aot-execution-test-ios`.

## What is not in this library

Every upstream translation unit that is left out, and the loudly-failing stub that stands in for
it, is listed at the top of `desktop_seams.cpp`. Summary:

| Left out | Why |
| --- | --- |
| `game/kernel/{common,jak1}/kmachine.cpp` | IOP boot, video, pads, PC-port functions: SDL, OpenGL, Discord, sqlite |
| `game/kernel/{common,jak1}/ksound.cpp` | its `InitSoundScheme` installs the desktop `rpc-call`; `core/sound_rpc.cpp` answers the sound channels instead |
| `game/kernel/jak1/kboot.cpp` | desktop boot path and the GOAL kernel dispatch loop |
| `game/sce/sif_ee.cpp` | the EE↔IOP bridge; file I/O is implemented in `desktop_seams.cpp` |
| `game/sce/deci2.cpp`, `game/system/**` | DECI2 debugger transport and sockets |
| `game/mips2c/mips2c_table.cpp` | names all four games; `core/mips2c_seam.cpp` registers Jak 1's |
| `game/runtime.cpp` | desktop process entry point; `g_ee_main_mem` is defined in `kernel_core.cpp` instead |

`game/kernel/jak1/kdgo.cpp` is replaced by `core/dgo_loader.cpp`, which defines the same jak1 entry
points but reads the archive from a file instead of through the IOP RPC. `game/overlord/jak1/srpc.cpp`
is replaced by `core/sound_rpc.cpp` the same way - the same two handlers, answered without an IOP
thread. The rest of `game/overlord/` is the IOP: its threads, mailboxes, file server and VAG stream
machinery. Only the four files that are not (`common/{sbank,soundcommon,srpc,ssound}.cpp`) are here,
unchanged.

`game/kernel/asm_funcs_arm64.s` *is* included: it holds the ARM64 GOAL calling-convention
trampolines, and it is the seam the compiler/AOT track needs. So are
`game/kernel/core/goal_thread_arm64.s` and `goal_native_kernel.cpp`, the native implementations of
the GOAL kernel routines that switch stacks.

`Mips2C::ExecutionContext::jalr` - a hand-translated PS2 function calling back into GOAL - has an
ARM64 case too, in `game/mips2c/mips2c_private.h`. It loads the function object's 64-bit native
entry point the way `call_goal` does, sets `g_goal_current_process` the way the trampolines do, and
calls through `_call_goal8_asm_arm64`. Without it the call returned whatever was in `v0`:
`ocean-generate-verts` uses it on every frame that draws water, which is every frame of the title
screen, and collision uses it throughout `collide_func.cpp` and `collide_edge_grab.cpp`. A platform
with no case now fails to compile rather than returning garbage.

## Known limitations

- **No executable GOAL heap.** The portable kernel core maps EE main memory read/write only by
  policy; it never requests `PROT_EXEC`. `goal_kernel_core_state::main_memory_executable` is
  therefore always 0. On ARM64 a function object stores the 64-bit native entry point of signed AOT
  or mips2c code instead of machine instructions, and `call_goal` loads that pointer. The AOT
  execution test also inspects the live Apple VM region before and after its calls and requires
  `rw-`. Nothing is executed out of the GOAL heap.
- **`pp` and stack-argument kernel functions.** A native pointer cannot also say "pass the current
  process in argument 3", so `copy_basic`, `new_basic` and `alloc_heap_object` get explicit
  ARM64 shims that read `g_goal_current_process` - which is where ARM64 keeps what x86-64 keeps in
  `r13` - and `_format`, `link` and `link-begin` get a shim that rebuilds GOAL's 8-register
  argument array from the C arguments.
- **Jak 2 remains a headless kernel probe.** Its kernel and GAME.CGO objects load through the AOT
  path, its translated mips2c functions are registered, and its sound loader accepts the IRX 4.0
  version handshake, ordinary STR files, validated SBlk banks retained by 989snd, and ordinary
  named-SFX PLAY/update commands. Its portable pad seam accepts host-pushed controller state.
  Its graphics machine boundary now has per-game desired/active-level adapters, host callbacks,
  and pre-`syncv` vblank dispatch, but the kernel probe installs only a counting/discarding host
  and has no renderer or drawn output. Music, streaming and info-frame updates are not
  implemented; neither are its broader machine seams. Jak 3 and Jak X still use the
  desktop/x86-oriented paths and remain non-functional in this ARM64 kernel core.
- **Little machine layer.** Almost nothing from `kmachine.cpp` is here.
  `goal_kernel_core_stub_machine_layer` puts a loudly-failing GOAL function object in each of its
  117 symbols so a call says which function was wanted instead of faulting in the guard page. Only
  the ones that are not machine-specific at all are implemented in `desktop_seams.cpp`:
  `__mem-move` (the PC port's `ultimate-memcpy` is a call to it, so a stub there means every data
  object in a DGO links against zeroes), `__read-ee-timer`, `__pc-get-mips2c`, and the seven
  `scf-get-*` readers of the PS2 system configuration (see **The boot configuration**), plus the
  process-lifetime `pc-rand` generator and the host `flush-cache` no-op. Jak 2 additionally installs
  GOAL's six `file-stream-*` functions and the path/basic display queries needed to initialize PC
  settings, forwards `pc-prof` to the existing global profiler, and reports an explicitly inactive
  mouse when no pointer provider exists. The loader
  half of the machine layer - the DGO and STR RPCs - is implemented in `dgo_loader.cpp`, and the
  pad in `pad.cpp`. `install-handler` retains the vblank and VIF1 GOAL function references; an
  installed graphics host dispatches the Jak 2 vblank handler before host pacing. As that host's
  `send_chain` observer, `dma_capture.cpp` validates and measures completed graphics-DMA chains
  before the validation host drops them. A graphics host also turns hardware-only calls such as `reset-graph` into the
  portable no-ops they are upstream. Broader window, input, and desktop-integration functions
  remain diagnostics. Such a frame can execute real game and graphics-boundary work, but without a
  renderer what it would have shown is not validated.
- **Without a host renderer, a frame is simulation only.** When no host installs itself through
  `gfx_host.h` (see **The renderer** above), `reset-graph`, `syncv`, `sync-path`,
  `put-display-env`, `dma-sync`, `__pc-texture-upload-now`, `__pc-texture-relocate`,
  `__pc-set-levels`, and `__pc-set-active-levels` all report and return 0. Jak 2's
  `__send-gfx-dma-chain` also remains a diagnostic stub unless a host installs a graphics seam:
  `--play-dma` installs the complete counting/discarding host with DMA observation composed behind
  it, while `--play-gfx-host` isolates that host without DMA parsing. Jak 1's existing boot and gameplay probes continue
  to route the seam to `dma_capture.cpp`, which measures the chain and drops it. Those headless
  modes establish what a frame *computes*, not what it shows.
- **File access is sandboxed.** `ee::sceOpen` and friends are real POSIX file descriptors.
  Relative names are resolved under the configured data directory
  (`goal_kernel_core_resolve_data_path`), while absolute Application Support paths are passed
  through. Jak 2's installed `file-stream-*` wrappers use that same boundary, allowing PC settings
  to remain local to the app sandbox without broadening access outside paths the host supplied.
- **Streamed VAG audio is experimental.** It plays - see **Sound** above - but the only path that
  is missing, the 'STRV' plugin, is the one where a music sequence queues a stream itself. Because
  `str-is-playing?` now reports a real stream position, cutscenes that used to be skipped instantly
  play out, which moves every later event in a scripted run. `--no-sound` puts the sound channels
  back on
  the machine-layer stub path, where `gsound.gc` prints `IRX version 0.0` and
  `ERROR: IRX is the wrong version - need 2.0` once during boot.
- `game/kernel/common/kmachine.h` transitively includes `<SDL3/SDL.h>` through
  `game/graphics/gfx.h`, so the vendored SDL headers are on the include path. No SDL code is
  compiled and no SDL library is linked; removing that include from the header is a follow-up.
