# jak1-kernel-core

The real OpenGOAL Jak 1 kernel subset, built as a static library for platforms that have no
desktop windowing, no IOP/sound emulation, no DECI2 listener transport, and no runtime code
generation. It is the library an iPadOS application links through an Objective-C++ bridge.

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
`goal_kernel_core_stub_machine_layer` rather than implemented - the `scf-get-*` settings readers,
the `file-stream-*` and `pc-*` PC-port functions. Those files' top-levels ran to completion, but
with those calls returning 0, so they are known to load rather than known to work. The pad is the
exception; see **The controller** below.

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
against the real structure and needs no game data; it is registered with CTest.

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

## Capturing a frame

`__send-gfx-dma-chain` is where a frame's work leaves GOAL. `dma_capture.cpp` follows the chain
with the same `FixedChunkDmaCopier` the renderer uses, then walks the copy again the way the
renderer's bucket dispatch does, so every frame is reported as what each bucket was actually given
rather than only as a size. `--dma-frame-report` prints that table, one line per frame.

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

**What does not.** Streamed VAG audio - the spooled cutscene and dialogue audio in `VAGWAD.<lang>`,
reached by the `spool-` prefix on the player channel and by channel 5. Upstream that is a second
subsystem: the ISO thread's VAG state machine, `game/overlord/jak1/stream.cpp`, and a plugin feeding
raw SPU voices, none of which is here. Requests for it are counted and named rather than dropped
quietly, so a run reports what it could not play instead of just being quieter than it should be.
The same goes for a play request naming a sound no loaded bank has, and for an RPC command this
implementation does not handle.

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

- **No executable GOAL heap.** `mmap` refuses `PROT_EXEC` for anonymous memory on ARM64 macOS, the
  iOS simulator, and iOS, so `goal_kernel_core_state::main_memory_executable` is 0. On ARM64 the
  kernel therefore stores the 64-bit native entry point in a function object instead of machine
  code, and `call_goal` loads it. Nothing is executed out of the GOAL heap.
- **`pp` and stack-argument kernel functions.** A native pointer cannot also say "pass the current
  process in argument 3", so `copy_basic`, `new_basic` and `alloc_heap_object` get explicit
  ARM64 shims that read `g_goal_current_process` - which is where ARM64 keeps what x86-64 keeps in
  `r13` - and `_format`, `link` and `link-begin` get a shim that rebuilds GOAL's 8-register
  argument array from the C arguments.
- **Only Jak 1 was converted.** `game/kernel/{jak2,jak3,jakx}/kscheme.cpp` still write x86-64
  trampolines, so those kernels remain non-functional on ARM64.
- **Little machine layer.** Almost nothing from `kmachine.cpp` is here.
  `goal_kernel_core_stub_machine_layer` puts a loudly-failing GOAL function object in each of its
  117 symbols so a call says which function was wanted instead of faulting in the guard page. Only
  the three that are not machine-specific at all are implemented in `desktop_seams.cpp`:
  `__mem-move` (the PC port's `ultimate-memcpy` is a call to it, so a stub there means every data
  object in a DGO links against zeroes), `__read-ee-timer`, and `__pc-get-mips2c`. The loader half
  of the machine layer - the DGO and STR RPCs - is implemented in `dgo_loader.cpp`, and the pad in
  `pad.cpp`; everything else - `file-stream-open`, `reset-graph`, the `scf-get-*` readers - is a
  diagnostic and not an implementation. A frame runs with the display and DMA functions returning
  0, so what a frame *computes* is real and what it would have *shown* is not.
- **The renderer is not here, so a frame is simulation only.** `reset-graph`, `syncv`, `sync-path`,
  `put-display-env`, `dma-sync`, `flush-cache`, `__pc-texture-upload-now` and `__pc-texture-relocate`
  are the machine functions a frame calls and they all report and return 0. `__send-gfx-dma-chain`
  is the exception: `dma_capture.cpp` follows the chain with the same `FixedChunkDmaCopier` the
  renderer uses, measures it bucket by bucket, writes chosen frames to a file, and drops the rest.
  Nothing is drawn. `__pc-set-levels`, the call that would tell a renderer's loader which levels'
  `fr3` art to have ready, is a stub too, so a replayed capture has to decide that for itself.
- **File access is data-directory-relative only.** `ee::sceOpen` and friends are real POSIX file
  descriptors, but every name is resolved under the configured data directory
  (`goal_kernel_core_resolve_data_path`), and an absolute name is passed through. GOAL's own file
  names - what `file-stream-open` would be given - are not translated yet, because nothing calls
  `file-stream-open` here.
- **No streamed VAG audio.** Sequenced sound - effects out of `.SBK` banks and music out of `.MUS`
  banks - plays. The spooled cutscene and dialogue audio in `VAGWAD.<lang>` does not; see **Sound**
  above. It is counted and named, not dropped quietly. `--no-sound` puts the sound channels back on
  the machine-layer stub path, where `gsound.gc` prints `IRX version 0.0` and
  `ERROR: IRX is the wrong version - need 2.0` once during boot.
- `game/kernel/common/kmachine.h` transitively includes `<SDL3/SDL.h>` through
  `game/graphics/gfx.h`, so the vendored SDL headers are on the include path. No SDL code is
  compiled and no SDL library is linked; removing that include from the header is a follow-up.
