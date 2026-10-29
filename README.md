# Jak trilogy on Apple platforms

**jak-pad is the OpenGOAL engine fork used by the Dark Eco app.** It is not a
separate game or app. To install, play, or build, go to
**[Dark Eco](https://github.com/zschwendi/dark-eco)**.

Dark Eco is the only application. This repository is one of its components.
Building jak-pad by itself does not produce an installable app. The Dark Eco
repository includes jak-pad automatically as its `OpenGOAL` Git submodule, so
you don't need to install the engine separately.

## Why two repositories?

| Repository | Contains |
| --- | --- |
| **[dark-eco](https://github.com/zschwendi/dark-eco)** | The Apple app: Swift/SwiftUI launcher, game library, import UI, settings, saves and cloud sync, touch controls, Xcode project, and install/build documentation |
| **jak-pad** (this repository) | The engine: an OpenGOAL fork with the GOAL compiler, reconstructed game code, native runtime, and Apple-platform integration including the Metal renderer |

Keeping the engine as a fork preserves OpenGOAL's history, credits, and
license, and keeps the Apple changes separate from the app shell.

## How to get started

- **Players:** start at **[Dark Eco](https://github.com/zschwendi/dark-eco)**.
  Its README covers getting a build, importing your game data, controls, and
  saves.
- **Contributors:** clone the Dark Eco repository on an Apple silicon Mac with
  its pinned submodules (which includes this engine), then follow its
  **[build guide](https://github.com/zschwendi/dark-eco/blob/v2.0/docs/BUILDING.md)**.
  Engine changes are made in this repository, then the app repository updates
  its submodule pin.

Dark Eco does not include or download any game files. You supply compatible
data from your own legally obtained PlayStation 2 copy. Game code is compiled
ahead of time to native ARM64; graphics use Metal. No PS2 emulation is involved.

## Platforms and controls

| Apple device | Controls and status |
| --- | --- |
| **iPhone and iPad** | Touch controls or a gamepad |
| **Apple Vision Pro** | Gamepad or PSVR2 Sense controllers. Sense motion gestures are currently Jak 1 only. |
| **Mac (Apple silicon), experimental** | Earlier Jak 1 import and title-screen boot demonstrated; full gameplay and feature parity not validated |

## Game status

| Game | Status |
| --- | --- |
| **Jak and Daxter: The Precursor Legacy** | Playable in ordinary windowed mode. Vision Pro third-person immersive mode is mostly finished but incomplete; sky, stereo-effect, and HUD issues remain. |
| **Jak II** | Ordinary play is almost finished but still incomplete. Immersive work has not started. |
| **Jak 3** | Planned; work has not started. |

These are Dark Eco's Apple-port milestones, not the status of the upstream PC
project. The trilogy is not complete on Apple platforms yet.

## Engine layout

| Location | Purpose |
| --- | --- |
| `goalc/` | GOAL compiler and ahead-of-time backend |
| `goal_src/` | OpenGOAL's reconstructed game and engine logic |
| `game/` | Native runtime, Apple platform seams, Metal rendering, audio, input |
| `decompiler/` | Data extraction and preparation tools |
| `test/` | Compiler/runtime tests and original fixtures |
| `docs/` | Engine documentation; some retained material describes upstream desktop development |

Coding agents should read the Dark Eco repository's
[AGENTS.md](https://github.com/zschwendi/dark-eco/blob/v2.0/AGENTS.md) and
[Project Overview](https://github.com/zschwendi/dark-eco/blob/v2.0/docs/PROJECT_OVERVIEW.md)
first. This repository's `AGENTS.md` describes upstream OpenGOAL's PC project;
the Dark Eco overview governs Apple-platform scope.

## Credits and license

Dark Eco is by Zach Schwendiman / Zephyr Studios and builds on
**[OpenGOAL](https://github.com/open-goal/jak-project)**. Credit for the GOAL
tools, reconstructed game code, original runtime, and formats belongs to the
OpenGOAL contributors. This fork adds the Apple-platform port; OpenGOAL's
[ISC license](LICENSE) and third-party notices remain in place.

Dark Eco and jak-pad are unofficial community projects, unaffiliated with Sony
Interactive Entertainment, Naughty Dog, or the OpenGOAL team.
