# Dark Eco — Apple engine fork

**Jak on iPhone, iPad, Mac, and Apple Vision Pro—with an app built for Apple
devices.** Dark Eco combines native Apple game ports with a Swift/SwiftUI app
for choosing games, importing your own game files, managing controls and
settings, and syncing saves and settings through optional iCloud sync.

| Apple device | Controls |
| --- | --- |
| **iPhone and iPad** | **Touch controls or a gamepad.** |
| **Mac (Apple silicon)** | **Gamepad or keyboard and mouse.** |
| **Apple Vision Pro** | **Gamepad or PSVR2 Sense controllers.** Sense motion gestures are currently a Jak 1 feature. |

Vision Pro also has experimental immersive third-person play with Jak visible.
Availability and remaining work are listed below.

This repository contains Dark Eco's engine: an OpenGOAL fork with native ARM64
ahead-of-time game code, a Metal renderer, and Apple-platform integration.
The complete app, library, import UI, settings, and cloud sync live in the
**[Dark Eco application repository](https://github.com/zschwendi/GOALPad/tree/v2.0)**.
Start there to install or build Dark Eco for your Apple device.

OpenGOAL supplies the reconstructed game code, compiler, and runtime foundation;
Dark Eco adds the Apple ports and app experience described above.

## Game support

| Game | Dark Eco status |
| --- | --- |
| **Jak and Daxter: The Precursor Legacy** | Playable in ordinary windowed mode. Vision Pro third-person immersion is **mostly finished, but incomplete**, with remaining sky, stereo-effect, and HUD issues. |
| **Jak II** | Ordinary play is **almost finished, but still incomplete**. Immersive support **has not started** and follows the remaining Jak 1 immersive fixes. |
| **Jak 3** | **Planned; work has not started**, including immersion. |

These are Dark Eco's Apple-port milestones, not the upstream PC project's
status. Immersive availability depends on the specific app build and source
lane. Future Jak II and Jak 3 immersion will use a normal DualSense controller.

## I want to play

Start with **[Dark Eco: install and play](https://github.com/zschwendi/GOALPad/blob/v2.0/README.md#install-and-play)**.
It covers getting an Apple build, importing your own supported game data,
controls, and saves. See **[Dark Eco in action](https://github.com/zschwendi/GOALPad/blob/v2.0/README.md#see-dark-eco)**
for Zach's own Vision Pro footage.

Dark Eco does not include or download the games. Supply compatible data from
your own legally obtained PlayStation 2 copy. The app executes normally signed,
ahead-of-time code; it requires no PS2 emulation, JIT, or jailbreak.

## I want to build or contribute

Clone the **[application repository](https://github.com/zschwendi/GOALPad/tree/v2.0)**
on an Apple silicon Mac, including its pinned submodules. Follow its
**[build-from-source steps](https://github.com/zschwendi/GOALPad/blob/v2.0/README.md#build-from-source)**
and **[build guide](https://github.com/zschwendi/GOALPad/blob/v2.0/docs/BUILDING.md)**.
Build the `GOALPad` Xcode scheme for the intended Apple destination. Building
this engine repository alone does not produce the Dark Eco app or its setup UI.

### Coding agents

Read the app repository's **[AGENTS.md](https://github.com/zschwendi/GOALPad/blob/v2.0/AGENTS.md)**,
**[Project Overview](https://github.com/zschwendi/GOALPad/blob/v2.0/docs/PROJECT_OVERVIEW.md)**,
**[HANDOFF](https://github.com/zschwendi/GOALPad/blob/v2.0/HANDOFF.md)**, and
**[ROADMAP](https://github.com/zschwendi/GOALPad/blob/v2.0/ROADMAP.md)** before working.
When this is checked out as `OpenGOAL/`, those documents are in the parent
repository. Also read this repository's `AGENTS.md`; its upstream PC background
describes OpenGOAL, while the parent overview governs Dark Eco's Apple scope.

Inspect both repositories' current branches, pins, and dirty state. The unified
product uses `v2.0` in the app and engine repositories. Preserve the separate
experimental immersive lane and per-game data/saves. Run the smallest relevant
checks, leave exact evidence, and follow current device authorization. Do not
commit retail game data, saves, signing material, or secrets.

## Engine layout

| Location | Purpose |
| --- | --- |
| `goalc/` | GOAL compiler and ahead-of-time backend |
| `goal_src/` | OpenGOAL's reconstructed game and engine logic |
| `game/` | Native runtime, Apple platform seams, Metal rendering, audio, input |
| `decompiler/` | Data extraction and preparation tools |
| `test/` | Compiler/runtime tests and original fixtures |
| `docs/` | Engine documentation; some retained material describes upstream desktop development |

## Credits and license

Dark Eco is by Zach Schwendiman / Zephyr Studios and builds on
**[OpenGOAL](https://github.com/open-goal/jak-project)**. Credit for the GOAL
tools, reconstructed game code, original runtime, and formats belongs to the
OpenGOAL contributors. This fork adds the Apple-platform port; OpenGOAL's
[ISC license](LICENSE) and third-party notices remain in place.

Dark Eco is an unofficial community project, unaffiliated with Sony Interactive
Entertainment, Naughty Dog, or the OpenGOAL team.
