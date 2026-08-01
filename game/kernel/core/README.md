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
`goal_kernel_core_stub_machine_layer` rather than implemented - `cpad-open`, `rpc-call`,
`rpc-busy?`, the `scf-get-*` settings readers, the `file-stream-*` and `pc-*` PC-port functions.
Those files' top-levels ran to completion, but with those calls returning 0, so they are known to
load rather than known to work.

```sh
cmake --build build/Release/bin -j 4 --target jak1-data-boot-test
./build/Release/bin/game/jak1-data-boot-test --synthetic
GOALPAD_JAK1_DATA_DIR=/path/to/out/jak1 ./build/Release/bin/game/jak1-data-boot-test
```

`jak1-data-boot-test` boots the game the way the game boots: `KERNEL.CGO`, then `GAME.CGO`, in DGO
order. It is registered with CTest twice.

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
cannot reach, loads and runs. `--play` and `--frames` go further; see the file's comments for
exactly where each one stops.

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
only `libc++` and `libSystem`.

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
| `game/kernel/{common,jak1}/ksound.cpp` | 989snd / overlord sound |
| `game/kernel/jak1/kboot.cpp` | desktop boot path and the GOAL kernel dispatch loop |
| `game/sce/sif_ee.cpp` | the EE↔IOP bridge; file I/O is implemented in `desktop_seams.cpp` |
| `game/sce/deci2.cpp`, `game/system/**` | DECI2 debugger transport and sockets |
| `game/mips2c/mips2c_table.cpp` | names all four games; `core/mips2c_seam.cpp` registers Jak 1's |
| `game/runtime.cpp` | desktop process entry point; `g_ee_main_mem` is defined in `kernel_core.cpp` instead |

`game/kernel/jak1/kdgo.cpp` is replaced by `core/dgo_loader.cpp`, which defines the same jak1 entry
points but reads the archive from a file instead of through the IOP RPC.

`game/kernel/asm_funcs_arm64.s` *is* included: it holds the ARM64 GOAL calling-convention
trampolines, and it is the seam the compiler/AOT track needs. So are
`game/kernel/core/goal_thread_arm64.s` and `goal_native_kernel.cpp`, the native implementations of
the GOAL kernel routines that switch stacks.

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
- **Almost no machine layer.** Nothing from `kmachine.cpp` is here.
  `goal_kernel_core_stub_machine_layer` puts a loudly-failing GOAL function object in each of its
  117 symbols so a call says which function was wanted instead of faulting in the guard page. Only
  the three that are not machine-specific at all are implemented in `desktop_seams.cpp`:
  `__mem-move` (the PC port's `ultimate-memcpy` is a call to it, so a stub there means every data
  object in a DGO links against zeroes), `__read-ee-timer`, and `__pc-get-mips2c`. Everything else
  - `cpad-open`, `file-stream-open`, `reset-graph`, the `scf-get-*` readers, `rpc-call`,
  `rpc-busy?` - is a diagnostic and not an implementation.
- **No IOP, so no GOAL-driven DGO load.** `goal_dgo_load` reads an archive synchronously, which is
  what `InitHeapAndSymbol` and `InitMachineScheme` need. GOAL's own level loader
  (`engine/load/load-dgo.gc`, `engine/level/level.gc`) does not use it: it drives the DGO RPC with
  `rpc-call` / `rpc-busy?` and links with `link-begin` / `link-resume`. Both are missing, so
  `(play)` spins in `(check-busy rpc-buffer-pair)` waiting for a level that never arrives.
  Level loading needs the DGO RPC answered and the code/data rule moved into `link_begin`, where
  GOAL-driven linking can reach it.
- **Backup stacks are sized for the PS2.** `PROCESS_STACK_SAVE_SIZE` is 256 bytes and an ARM64
  machine context alone is 176, so a process that suspends needs `stack-size-set!` with a larger
  number than the game currently asks for. `thread-suspend` aborts with both numbers rather than
  copying past the end of the thread object. See `docs/aot-stack-model.md`.
- **File access is data-directory-relative only.** `ee::sceOpen` and friends are real POSIX file
  descriptors, but every name is resolved under the configured data directory
  (`goal_kernel_core_resolve_data_path`), and an absolute name is passed through. GOAL's own file
  names - what `file-stream-open` would be given - are not translated yet, because nothing calls
  `file-stream-open` here.
- **mips2c calls back into GOAL do not work on ARM64.** `ExecutionContext::jalr` in
  `game/mips2c/mips2c_private.h` has no ARM64 case. Nothing on the art-group login path uses it.
- `game/kernel/common/kmachine.h` transitively includes `<SDL3/SDL.h>` through
  `game/graphics/gfx.h`, so the vendored SDL headers are on the include path. No SDL code is
  compiled and no SDL library is linked; removing that include from the header is a follow-up.
