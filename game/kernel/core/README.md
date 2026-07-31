# jak1-kernel-core

The real OpenGOAL Jak 1 kernel subset, built as a static library for platforms that have no
desktop windowing, no IOP/sound emulation, no DECI2 listener transport, and no runtime code
generation. It is the library an iPadOS application links through an Objective-C++ bridge.

**Status: Experimental.** It initializes real kernel state. It does not execute GOAL code and does
not load game data.

## What it does

`goal_kernel_core_initialize()` (see `kernel_core.h`):

1. maps 128 MiB of EE main memory and traps the low 512 kB,
2. runs the upstream kernel `*_init_globals` functions,
3. initializes the real global and debug `kheapinfo` heaps at the upstream memory layout,
4. allocates the GOAL print buffer through `init_output()`,
5. runs `jak1::InitSymbolAndTypes()` — the real symbol table allocation, `s7` setup, fundamental
   type bootstrap, and C-function symbol export from `game/kernel/jak1/kscheme.cpp`.

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

## What is not in this library

Every upstream translation unit that is left out, and the loudly-failing stub that stands in for
it, is listed at the top of `desktop_seams.cpp`. Summary:

| Left out | Why |
| --- | --- |
| `game/kernel/{common,jak1}/kmachine.cpp` | IOP boot, video, pads, PC-port functions: SDL, OpenGL, Discord, sqlite |
| `game/kernel/{common,jak1}/ksound.cpp` | 989snd / overlord sound |
| `game/kernel/jak1/kboot.cpp` | desktop boot path and the GOAL kernel dispatch loop |
| `game/sce/sif_ee.cpp` | host file I/O and the EE↔IOP bridge; needs the IOP thread emulation |
| `game/sce/deci2.cpp`, `game/system/**` | DECI2 debugger transport and sockets |
| `game/mips2c/**` | hand-translated PS2 VU/asm renderer and collision functions |
| `game/runtime.cpp` | desktop process entry point; `g_ee_main_mem` is defined in `kernel_core.cpp` instead |

`game/kernel/asm_funcs_arm64.s` *is* included: it holds the ARM64 GOAL calling-convention
trampolines, and it is the seam the compiler/AOT track needs.

## Known limitations

- **No executable GOAL heap.** `mmap` refuses `PROT_EXEC` for anonymous memory on both ARM64 macOS
  and iOS, so `goal_kernel_core_state::main_memory_executable` is 0 and the x86-64 trampolines
  that `make_function_from_c` writes into the heap are dead bytes on ARM64 regardless. Executing
  GOAL code needs the separate AOT track.
- **No file access.** `ee::sceOpen` and friends are stubs, so `FileLoad`, `load`, and DGO loading
  abort. An iPadOS file-path strategy is required before they can be implemented.
- `game/kernel/common/kmachine.h` transitively includes `<SDL3/SDL.h>` through
  `game/graphics/gfx.h`, so the vendored SDL headers are on the include path. No SDL code is
  compiled and no SDL library is linked; removing that include from the header is a follow-up.
