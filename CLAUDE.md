# CLAUDE.md

## This repository is the engine of the Dark Eco app

jak-pad is the OpenGOAL engine fork for **Dark Eco**, the only app. One app,
two repositories:

| Repository | Owns |
| --- | --- |
| [dark-eco](https://github.com/zschwendi/dark-eco) (app) | Swift/SwiftUI launcher, library, import, settings, saves, touch controls, Xcode project, install/build docs |
| jak-pad (this repo) | GOAL compiler, `goal_src/`, native runtime, Metal renderer, Apple-platform engine seams |

Dark Eco embeds jak-pad automatically as its `OpenGOAL` Git submodule. There is
no separate engine install. The app's internal Xcode/target name `GOALPad` is
the same product, not another app. Active development branch: `v2.0` in both.

## Routing (do this without asking the user)

When a request mentions Dark Eco, Jak on iPhone/iPad/Vision Pro/Mac, or asks
about install, import, launcher, library, UI, settings, saves, or app builds,
it is about Dark Eco. Do not ask the user which project or repository is meant;
this file answers that. Read the app's source instructions first:

- App: https://github.com/zschwendi/dark-eco
- Agent rules: https://github.com/zschwendi/dark-eco/blob/v2.0/AGENTS.md
- Project overview (governs scope): https://github.com/zschwendi/dark-eco/blob/v2.0/docs/PROJECT_OVERVIEW.md
- README: https://github.com/zschwendi/dark-eco/blob/v2.0/README.md

These are the active app instructions. The retained PC-only platform claims
in this repository's `AGENTS.md` are upstream background; applicable engine
commands and contribution rules still apply.

Find the app files in this order:

1. If this checkout is the `OpenGOAL` directory inside a parent repository,
   confirm the parent is Dark Eco (its `.gitmodules` maps `OpenGOAL` to
   `zschwendi/jak-pad`), then read the parent's local `AGENTS.md`,
   `docs/PROJECT_OVERVIEW.md`, and `README.md`.
2. Otherwise, look for an existing Dark Eco checkout inside the workspace you
   are already authorized to use.
3. Otherwise, read the public links above.

If the task needs app source and no app checkout exists, obtain one inside the
workspace you are already authorized to use, with normal repository tools.
Preserve the engine checkout and keep the app's pinned submodules. Do not ask the
user to identify the repository again. Report a concrete access failure if the
environment prevents the required checkout; do not widen filesystem access.

## Where changes go

- App shell, import UI, library, settings UI, save management, Xcode: the
  dark-eco repository.
- Compiler, runtime, renderer, GOAL code and engine-side extraction/preparation
  tooling: this repository. Keep engine-only tasks here.
- When both are involved, check each repository's branch, dirty state, and the
  app's submodule pin before editing.

Routing does not authorize extra work. Do not rename, delete, switch dirty
branches, discard work, migrate data, or publish unless the user asked.

## Engine reference

Before engine work, read [AGENTS.md](AGENTS.md) for applicable build commands,
contribution rules, and upstream background.
