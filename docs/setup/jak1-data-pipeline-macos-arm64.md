# Jak 1 Data Pipeline on macOS ARM64 (GOALPad)

**Status: Implemented.** This procedure was executed end to end on 2026-07-31 on an
Apple M4 Pro (12 logical cores, macOS 26.5) against a legally obtained retail copy of
*Jak and Daxter: The Precursor Legacy*. Every command and duration below is the
observed result of that run, not an estimate.

This document describes how a user turns their own legally obtained PlayStation 2 copy
of Jak 1 into the prepared data OpenGOAL loads at runtime, using only the host tools in
this repository built natively for macOS ARM64.

## Legal and hygiene boundary

- You must supply your own legally obtained copy of the game. This repository contains
  no game assets and no way to obtain them.
- All extracted and generated game data must stay **outside** the git checkout. The
  commands below place everything under a user-chosen directory (shown as
  `$JAK1DATA`). Never point `--extract-path` or `--proj-path` inside the repository,
  and never commit extracted or generated game data.
- The repository's `.gitignore` does ignore the pipeline's in-repo default output
  locations (`iso_data/` via its own `.gitignore`, `/decompiler_out*`, `out/`, `/log`,
  `build/`) as a safety net, but keeping the data outside the checkout entirely is the
  supported GOALPad workflow.

## Supported input

The extractor validates the input against its built-in revision database
(`decompiler/extractor/extractor_util.cpp`, `extractor_iso_database()`). For Jak 1 the
supported revisions are:

| Serial | Release | Region | Decompiler config |
| --- | --- | --- | --- |
| SCUS-97124 | Black Label | NTSC-U | `ntsc_v1` |
| SCUS-97124 | Greatest Hits | NTSC-U | `ntsc_v2` |
| SCES-50361 | PAL | PAL | `pal` |
| SCPS-15021 | NTSC-J | NTSC-J | `jp` |
| SCPS-56003 | Korean | NTSC | `ntsc_v1` |

Identification is by the serial and hash of the boot ELF inside the disc image, plus a
file count and whole-contents hash check. Unknown or corrupt images are rejected with
an error; with the `-v` flag validation failures abort the pipeline.

The validated run documented here used the NTSC-U Black Label release
(`SCUS_971.24`, ELF hash `7280758013604870207`, 337 files).

## Prerequisites

- Xcode command line tools (Apple clang).
- Homebrew packages: `cmake` (4.4.1 used), `ninja` (1.13.2), `openssl@3`.
  `nasm` was **not** required for the ARM64 build in this run.
- Disk space: about 5.5 GB for the tool build plus roughly 5 GB of game data
  (ISO + extracted ISO + decompiler output + built output).

## Step 1 — Build the host tools (once)

From the repository root:

```bash
export OPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
cmake -B build --preset=Release-macos-arm64-clang        # 51 s
cmake --build build --parallel 4 --target extractor decompiler   # 441 steps, 2 m 11 s
```

Both binaries build cleanly from this branch with no source changes (warnings only)
and are native `arm64` Mach-O executables:

- `build/decompiler/extractor` — ISO extraction + validation + decompile + build driver
- `build/decompiler/decompiler` — standalone decompiler (not needed for the standard
  flow; the extractor drives decompilation itself)

The build directory was 838 MB after this targeted build.

## Step 2 — Choose an outside-the-repo data root

```bash
export JAK1DATA="$HOME/path/to/Jak1"    # must NOT be inside any git checkout
mkdir -p "$JAK1DATA"
```

The extractor resolves its inputs (decompiler configs, `goal_src`, game assets) and its
outputs (`decompiler_out/`, `out/`, `log/`) relative to a "project data" directory.
Passing `--proj-path` moves all of that outside the repository. Populate it exactly the
way official OpenGOAL releases package their `data/` directory
(see `.github/scripts/releases/extract_build_unix.sh`):

```bash
DEST="$JAK1DATA/projdata"; SRC=/path/to/this/repo
mkdir -p "$DEST/decompiler" "$DEST/game" "$DEST/log"
cp -R "$SRC/decompiler/config" "$DEST/decompiler/"
cp -R "$SRC/goal_src"          "$DEST/"
cp -R "$SRC/game/assets"       "$DEST/game/"
cp -R "$SRC/custom_assets"     "$DEST/"
```

This copies about 145 MB of repository (non-retail) input data.

## Step 3 — Extract and validate the user's disc image

If the copy is inside an archive, unpack the `.iso` first (a 7-Zip archive can be
unpacked with `bsdtar -xf archive`). Then:

```bash
build/decompiler/extractor "/path/to/your Jak 1 disc image.iso" \
  -e -v \
  --extract-path "$JAK1DATA/iso_data" \
  --proj-path    "$JAK1DATA/projdata"
```

Observed: 5.3 s. Output: `$JAK1DATA/iso_data/jak1/` — 338 files, 1.4 GB (the 337 disc
files plus a generated `buildinfo.json` recording the serial and ELF hash). The tool
prints the detected release, e.g.:

```
Detected - Jak & Daxter™: The Precursor Legacy (Black Label)
Region - NTSC-U
Serial - SCUS-97124
Uses Decompiler Config Version - ntsc_v1
```

## Step 4 — Decompile (asset extraction)

```bash
build/decompiler/extractor "$JAK1DATA/iso_data/jak1" \
  -f -d \
  --proj-path "$JAK1DATA/projdata"
```

Observed: about 2 s wall (the extractor runs the decompiler in its minimal
asset-extraction mode — no IR2 source-level analysis). Output:

- `$JAK1DATA/projdata/decompiler_out/jak1/` — 206 MB:
  `raw_obj/` (1017 raw GOAL object files), `entities/` (48 level entity JSON dumps),
  `assets/` (`game_text.txt`, `game_count.txt`), `textures/` (tpage directory +
  remap tables), `all-syms.gc`, `dgo.txt`, `obj.txt`
- `$JAK1DATA/projdata/out/jak1/fr3/` — 25 retail level `.fr3` precomputed graphics
  archives plus `GAME.fr3` (201 MB total with the test level added in step 5)

## Step 5 — Build the runtime data (goalc)

```bash
build/decompiler/extractor "$JAK1DATA/iso_data/jak1" \
  -f -c \
  --proj-path "$JAK1DATA/projdata"
```

Observed: all 1317 build targets in 8.8 s (goalc's build system is parallel; much of
the work is copying streaming audio/animation straight from `iso_data`). Output under
`$JAK1DATA/projdata/out/jak1/`:

- `iso/` — 1.3 GB, 321 files. This is what the runtime loads: 28 `.CGO`/`.DGO`
  archives (`KERNEL.CGO`, `GAME.CGO`, `ENGINE.CGO`, and 25 level DGOs including the
  `TSZ.DGO` test zone), text banks (`*COMMON.TXT`, `*SUBTIT.TXT`), streaming
  animation (`.STR`), sound banks (`.SBK`), music (`.MUS`), and streaming audio
  (`VAGWAD.*` per language, `VAGDIR.AYB`).
- `fr3/` — 26 `.fr3` level graphics archives (including `test-zone.fr3` built from
  `custom_assets`).
- `obj/` — 1039 intermediate compiled GOAL objects (not needed at runtime).

Per `goal_src/jak1/game.gp`, the game runtime loads from `out/jak1/iso` and
`out/jak1/fr3`; `iso_data/` and `decompiler_out/` are pipeline inputs/intermediates
that are only needed to rebuild.

Quirk: the compile step also creates an empty `out/jak1/obj/` directory tree relative
to the process working directory. It contains no files and can be deleted.

## Instruction-set note (important for GOALPad)

The extractor's compile step and the goalc CLI both construct the compiler with
`emitter::InstructionSet::X86` (`decompiler/extractor/main.cpp`, `goalc/main.cpp`).
The compiled GOAL code inside the produced `.DGO`/`.CGO` files and `out/jak1/obj` is
therefore **x86-64** machine code — identical to upstream desktop OpenGOAL data. The
asset side of the output (`fr3`, `.STR`, `.SBK`, `.MUS`, `VAGWAD`, text banks) is
architecture-independent. Producing ARM64 GOAL object code from this branch's ARM64
emitter is not yet wired into the extractor or the goalc CLI; that remains a separate,
pending piece of the GOALPad platform work.

## Observed totals

| Phase | Wall time | Output size |
| --- | --- | --- |
| CMake configure | 51 s | — |
| Build `extractor` + `decompiler` (`-j 4`) | 2 m 11 s | 838 MB build dir |
| Unpack ISO from user archive | ~1.5 min | 1.4 GB |
| Extract + validate (`-e -v`) | 5.3 s | 1.4 GB `iso_data/jak1` |
| Decompile (`-d`) | ~2 s | 206 MB `decompiler_out`, 200 MB `fr3` |
| Build (`-c`) | 8.8 s | 1.3 GB `iso/`, 204 MB `obj/` |

Total prepared-data footprint (excluding the tool build and the original archive):
about 4.8 GB, all outside the repository.

## What this does not cover

- Booting the game with the produced data (`gk`) was not part of this validated run;
  no claim is made here that the runtime loads this data on macOS ARM64 or iPadOS.
- Jak II and Jak 3 inputs are out of scope for GOALPad at this stage.
