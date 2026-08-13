# mad2modloader

A native mod loader, a set of gameplay/compatibility/chaos mods, and a
from-scratch asset-format reverse-engineering toolkit for **Madagascar 2:
The Game** (PC), targeting Linux via Proton/Wine.

This is a from-source-up modding project: there's no existing modding
community or tooling for this game to build on, so everything here — the
DLL injection chain, the individual mods, and the asset/save file formats —
was reverse-engineered and built from scratch against the shipped game
binaries (plus, for the asset formats, an unstripped Wii demo build used as
a cross-reference — see `docs/`).

For the full engineering reference (build system internals, mod
architecture, load-order gotchas, logging conventions), see
**[`CLAUDE.md`](CLAUDE.md)** — it's written as a dense reference doc, not
prose, and is kept up to date as the authoritative source. This README is a
lighter-weight orientation on top of it.

## What's here

### The mod loader + mods

`modloader/` builds `version.dll`, which the game loads before its own
entry point runs (via a Wine DLL override) and which in turn loads every
`.dll` in `mods/`. Everything else at the repo root next to `modloader/` —
`mad2xinput`, `mad2config`, `mad2effects`, `mad2shadowfix`,
`mad2graphicseffectmod`, `mad2chaosmod`, `mad2companionmod`, and around twenty
more — is one mod, one CMake target, one DLL (roughly 25 in total, including
two shared hooking/rendering-primitive DLLs, `mad2hookutil`/`mad2textrenderer`,
that other mods resolve internally rather than a player ever interacting with
directly). Roughly:

- **File/IO redirection**: asset loading, save loading, level redirection.
- **Shared APIs**: a controller-input hook, a config-file reader/writer, and
  a chaos-effects registry that other mods resolve by name at runtime.
- **Diagnostics/fixes**: e.g. `mad2shadowfix`, a targeted fix for broken
  shadow rendering under DXVK/wine3d.
- **Overlays**: IGT/RTA timer, player coordinates, controller input display,
  Twitch chat.
- **The chaos-effects system**: ~19 screen-space post-process effects,
  input-scrambling effects, a "director" that triggers them on a timer or
  via Twitch channel-point redemptions, and integrations that launch
  entirely separate games (a vendored Super Mario 64 PC port, two separate
  vendored Jak & Daxter decompilation forks) as chaos effects, orchestrated
  cross-process via `mad2relauncher`.

### Native companion tools (not in-process DLLs)

- **`mad2relauncher`** — crash-relaunches the game, and orchestrates the
  companion-game chaos effects (SM64, two Jak forks) and background music.
- **`mad2launcher`** — a standalone Go+Fyne desktop GUI: install discovery,
  config editing, a level picker, and a save-file cheat-toggle editor. Not
  part of the mod-loading chain — a convenience layer on top of it.
- **`mad2music`** — a native Linux (and cross-built native Windows) SDL3
  audio player for background music + "podcast break" chaos-effect audio.
- **`mad2rando5`** — the level-logic randomizer.

### Asset format reverse engineering (`mad2iga`, `mad2repack`, `docs/`)

A separate, large effort from the mods above: understanding the actual
on-disk game asset formats (the Vicarious Visions "Alchemy" engine's
archive container and serialized-object-graph format) well enough to edit
arbitrary game data — level layouts, object placement, audio, textures,
eventually level *logic* — not just hook the running process.

- **`mad2iga`** — the shared Go library: archive container (solid, verified
  round-trip), a real object-graph walker over the serialized level data,
  a per-class field schema (cross-verified between an unstripped Wii build
  and the real PC binaries), FSB3 audio parsing, and `.texs` texture
  container parsing.
- **`mad2repack`** — the CLI exercising all of it (`unpack`/`pack`/`verify`/
  `objects-dump`/`snd-extract`/`tex-info`/and more).
- **`mad2arc`**/**`mad2tool`**/**`mad2save`**/**`mad2savegui`**/
  **`mad2transitions`** — a read-only archive extractor, a whole-game
  extract/rebuild orchestrator, and save-file editing tools.
- **[`docs/`](docs/README.md)** — the actual reverse-engineering writeup:
  what's solid, what's a working heuristic, what's still an open problem,
  with evidence for every claim. Start at `docs/README.md`.
  `waterhole_objects_example.json` (repo root) is a real, current example
  of what the object-graph tooling can already recover from a shipped level
  file — worth a look to see the current state concretely.

### Vendored external engines (`extern/`)

Three vendored, independently-built projects that some chaos effects launch
as separate processes: a Super Mario 64 PC port, and two separate Jak &
Daxter decompilation forks (one running the original game, one a "Mario
reskin" mod of the same decompilation). None of these are required to build
or run the core mod loader — see `CLAUDE.md` for what each chaos effect
does when its engine isn't built.

## Building and running

Full command reference (and what each one actually does) is in
`CLAUDE.md`'s "Build commands" section. The short version, using
[`just`](https://github.com/casey/just):

```sh
just build            # build the mod loader + all mods
just deploy-mad2       # deploy into mad2/
just run               # deploy, then launch the game (gamescope + umu-run, via mad2relauncher)
just run-launcher      # build and run the standalone desktop GUI
just build-assettools  # build the asset/save-editing CLI toolset
```

`mad2/`, `mad2russia/`, `mad2demo/` are the actual game install directories
(PC retail, Russian localization, demo — not checked into version control)
and are required to build against; they're not included in this repo.

## Requirements

MinGW i686 cross-toolchain (`i686-w64-mingw32-gcc`/`g++`/`windres`), CMake +
`samu` (a Ninja-compatible build tool), Go (for the native Linux tools), and
a Proton install for `just run`. See `CLAUDE.md` for the full toolchain
breakdown, including what's needed for the optional vendored-engine chaos
effects.

## Acknowledgments

The asset-format reverse engineering (`docs/`, `mad2iga`) benefited from
several independent efforts on the same Vicarious Visions "Alchemy" engine:

- **[`MaxStache/madagascar-2-tools`](https://github.com/MaxStache/madagascar-2-tools)**
  — a from-scratch Python reader for Mad2's own `.bld`/IGZ format plus a
  static reflection dumper. Its clean use of the `TSTR`/`RSTR` Section-0
  fixup tables is what pinned down per-instance naming (`ActorInfo._name`,
  `_varName`, …); we independently re-derived and reimplemented that in Go
  (`mad2iga/objgraph.go`), and cross-checked its static reflection dump
  against our own live one. See `docs/CLASS_SCHEMA.md` and `docs/README.md`.
- **[`MaxStache/madagascar-tfbtool`](https://github.com/MaxStache/madagascar-tfbtool)**
  — a decompiler for the original 2005 *Madagascar*'s `.ai` TFBScript
  bytecode, a useful semantic reference for Mad2's `TFBScriptInfo` opcodes.
- **[`NefariousTechSupport/AlchemyMetadataDumper`](https://github.com/NefariousTechSupport/AlchemyMetadataDumper)**
  and **[`bonesinmysoup/igRewrite8`](https://github.com/bonesinmysoup/igRewrite8)**
  — tooling for later Skylanders-era builds of the same engine lineage, used
  as structural cross-references (not byte-compatible with Mad2's IGZ v4).

All Alchemy-engine format work here is our own from-scratch Go/C++
implementation; these are credited as references and corroboration, not as
incorporated code.
