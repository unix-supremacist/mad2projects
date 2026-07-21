# Deployment and Packaging Plan: Madagascar 2 Mod Loader & Chaos Suite

This document defines the deployment layout, packaging structure, and cross-platform compilation plan for the **Madagascar 2 Mod Loader & Chaos Suite** project.

Outputs are packaged as separate, modular archives/directories rather than a single monolithic delivery. Every package below is also bundled inside the `mad2launcher` package (see §3.1), so a user who just wants the GUI experience only needs one download; the standalone packages exist for CLI/scripted use and for people who only want one piece (e.g. just the speedrunning overlays).

**This is now implemented, not just planned.** `just package` (`tools/package.sh`) builds everything and stages/archives every package below into `dist/linux/` and `dist/windows/`; `mad2launcher`'s Mod Installer page (§3.1) installs any of them into a chosen game directory. Two ground rules the whole packaging design follows:
1. **Everything below is distributed as a prebuilt binary — except SM64 and Jak & Daxter.** `mad2launcher` never invokes a compiler/toolchain at runtime for any of its own companion tools (`mad2relauncher`, `mad2rando5`, `mad2arc`/`mad2repack`, `mad2musicpatch`, `mad2musicdownloader`) — see `mad2launcher/build_native.go`'s `findPrebuilt`/`Resolve*` family. The one deliberate exception is SM64/Jak & Daxter, which need a user-supplied, legally-owned ROM/ISO and are therefore built locally via the launcher's "Build Games" page (§3.1) or `just build-sm64-linux`/`build-jak1-linux`, never prebuilt or redistributed.
2. **Mario Legacy (`extern/mario-legacy`, `MarioLegacyChallenge`) is deliberately excluded from all packaging and from the "Build Games" page for now** — it exists in-repo and works via the CLI (`just setup-mario-legacy`), but isn't part of the packaged/GUI-driven release story yet.

Some of what follows still describes work that isn't built — those sections stay marked **(Planned)**. §5 collects every remaining such item.

---

## 1. Split Package Directory Layouts

### 1.1 `mad2modloader` Package (Core Suite)
* **Purpose:** Core mod loader, shared hooking/rendering primitives, and critical fixes. Essential dependency for all other mods. All of `mad2modloader`'s DLLs (and every other mod DLL in every package below) are built by one unified `just build` — a single CMake project, not one target per package — and staged flat into `build/dist/{version.dll,mods/*.dll}` by `cmake/add_mad2_mod.cmake`'s shared `RUNTIME_OUTPUT_DIRECTORY`. The "package" boundaries in this document are a *packaging-time* grouping `tools/package.sh` sorts those already-built DLLs into, not separate build targets.
* **Linux (Wine/Proton) & Windows (Native) Layout:**
```text
mad2modloader/
├── version.dll                # Proxy DLL loaded by AWL.dll
└── mods/
    ├── aa_mad2hookutil.dll    # Shared IAT/vtable hooking primitives (aa_ prefix: must load/hook first)
    ├── mad2textrenderer.dll   # Shared GDI text-to-texture + quad-drawing overlay helpers
    ├── mad2assetloader.dll    # Asset path redirection
    ├── mad2config.dll         # Shared config file API
    ├── mad2saveloader.dll     # Save redirection
    ├── mad2xinput.dll         # Layered control overrides
    └── shadowfix.dll          # D3D9 shadow renderer fix
```
No `config.cfg` seed ships in the package — it's generated on first run by `mad2config.dll` (see CLAUDE.md's "Shared-API mods"), so there's nothing static to bundle.

---

### 1.1a `mad2cheatapply` Package
* **Purpose:** Title-screen cheat-apply mod, ported from `../mad2mod`'s `enable_all_cheats`/`load_cheats_config`. Waits for the title screen (via the same current-level detection `mad2gameeffectsmod` uses — the ~40-line duplication `improvement-plan.md` flagged is still unresolved, see its item 12) and, on a configured trigger, applies the saved cheat toggles. **The CMake deploy-target omission is fixed** — `add_deploy_target()` in the root `CMakeLists.txt` now copies and lists it.
```text
mad2cheatapply/
└── mods/
    └── mad2cheatapply.dll     # Applies saved cheat toggles at the title screen
```

---

### 1.2 `mad2effects` Package (Chaos Mod Core System)
* **Purpose:** Core registry and effects, including `mad2companionmod.dll` (registers `SM64Challenge`/`JakChallenge`/`MarioLegacyChallenge` — formerly `mad2sm64mod`, renamed once it grew beyond just SM64). Unlike the old plan, `mad2companionmod.dll` now ships **here**, not in a separate package: since none of its companion game binaries are ever prebuilt/redistributed (see the ground rules at the top of this document — SM64/Jak1 are built locally via the launcher's "Build Games" page, Mario Legacy isn't packaged at all yet), there's no external binary left to bundle it alongside. The effect itself simply no-ops with a log line until its companion game is built locally. (Podcast playback still ships in its own package, §2.3, since it's tightly coupled to the always-running `mad2music` process rather than an on-demand child process.)
* **Linux (Wine/Proton) & Windows (Native) Layout:**
```text
mad2effects/
├── coordinates.yaml           # Position database (copied to game root folder)
└── mods/
    ├── mad2effects.dll        # Shared chaos effect registry API
    ├── mad2inputeffectsmod.dll# Input manipulation effects
    ├── zz_mad2graphicseffectmod.dll  # Graphics screen post-process effects
    ├── mad2gameeffectsmod.dll # Game-state mutation effects (ObjectShuffle, MangoMutate, PlayerTeleport)
    ├── mad2effectshud.dll     # HUD overlay for active effects
    └── mad2companionmod.dll   # SM64Challenge / JakChallenge / MarioLegacyChallenge (no-ops until its game is built locally)
```

---

### 1.3 `mad2speedrunning` Package (Overlays & Timers)
* **Purpose:** HUD indicators, timers, and streaming overlays. `inputdisplay.dll`'s Kenney Input Prompts glyphs are embedded into the DLL at *compile* time (`tools/gen_embedded_assets.py`, from the vendored `kenney_input-prompts_1.5.zip`) — there's no runtime asset file to bundle for it. `tools/package.sh` still generates the license/attribution text files below for CC0 compliance even though nothing else in the package depends on them.
* **Linux (Wine/Proton) & Windows (Native) Layout:**
```text
mad2speedrunning/
├── LICENSE_KENNEY.txt         # Kenney CC0 1.0 Universal license text
├── CREDITS_KENNEY.txt         # Attribution pointing to Kenney.nl
├── emotes/                    # Empty on install; drop twitchchat.dll's PNG/GIF emote files here
└── mods/
    ├── igttimer.dll           # In-game/RTA timer overlay
    ├── inputdisplay.dll       # Controller visualizer overlay (Kenney assets embedded at build time)
    └── twitchchat.dll         # Anonymous IRC Twitch chat overlay
```
`mad2playercoords` (§2.5) and `mad2musichud` (§2.3) are architecturally the same kind of D3D9 overlay as everything above, but each ships in its own package instead — see those sections for why.

---

### 1.4 `mad2rando` Package (Game Randomizer)
* **Purpose:** Level-logic randomization + the interception mod that consumes it. `mad2rando5` computes a `Slot -> Level` mapping and writes `level_redirects.txt`; it never repacks or touches any archive itself, so it has no dependency on the archive tools in §1.5. Ships two ways: as this standalone package (for CLI/scripted use) and bundled inside the `mad2launcher` package, where its "Mad2 Randomizer" tab drives the same binary (see §3.1). `mad2rando5/` is an in-repo Go module (migrated from a formerly-external sibling checkout) built via `just build-mad2rando5` — no external checkout required anymore. **`mad2launcher` never builds this itself** — `ResolveMad2Rando5` (`mad2launcher/build_native.go`) only ever looks for an already-built binary (`build/mad2rando5`, this package once installed, or `dist/<platform>/mad2rando_*`); if none is found it reports an error telling the user to run `just build-mad2rando5` or install the package, rather than silently shelling `go build` (see §5's now-resolved item 2).
* **Linux Layout:**
```text
mad2rando_linux/
├── mad2rando5                 # CLI randomizer utility (Linux 64-bit ELF)
└── mods/
    └── mad2levelredirectmod.dll # NtCreateFile interceptor + current-level tracking (see CLAUDE.md)
```
* **Windows Layout:**
```text
mad2rando_windows/
├── mad2rando5.exe             # CLI randomizer utility (Windows 64-bit PE)
└── mods/
    └── mad2levelredirectmod.dll # NtCreateFile interceptor + current-level tracking (see CLAUDE.md)
```

### 1.5 `mad2assettools` Package (Archive & Save Editing)
* **Purpose:** general-purpose IGA archive extraction/repacking and save-file editing, unrelated to the randomizer (a different formerly-external sibling, `mad2assetextractor`, migrated in-repo the same way §1.4 was — see CLAUDE.md's "What this is"). `mad2iga` is the shared archive-format library `mad2arc`/`mad2repack`/`mad2tool` import via a local `go.mod` `replace` directive; it's not shipped as its own binary. `mad2savegui` overlaps with `mad2launcher`'s own built-in save/cheat editor (a from-scratch reimplementation, not a caller of this) — bundled here mainly for CLI/no-GUI-launcher use. Built via `just build-assettools` (native) / `just build-assettools-windows` (pure-Go cross-build, `mad2savegui` excluded — see §4.2/§4.3). `mad2launcher`'s own Music page resolves `mad2arc`/`mad2repack` from this same package (`ResolveMad2AssetTool`) for its blanked-`global.arc` feature — never builds them itself.
* **Linux/Windows Layout** (same tools, `.exe` suffix on Windows):
```text
mad2assettools/
├── mad2arc                    # IGA archive extract/list tool
├── mad2repack                 # IGA archive repack/replace tool
├── mad2tool                   # Misc archive helper tool
├── mad2save                   # CLI save-file editor/checksummer/PC<->Wii converter
├── mad2savegui                # Fyne GUI save-file editor (Linux only; standalone, not called by mad2launcher)
└── mad2transitions             # Level-transition data tool (see mad2gameeffectsmod's "Level Transition system" note in CLAUDE.md)
```
`mad2musicpatch` (the one-shot tool that blanks `global.arc`'s in-game music, §2.3) is deliberately **not** part of this package — it's a `mad2launcher`-only utility, shipped directly inside the `mad2launcher` package instead (§3.1), not something a CLI/no-launcher user needs standalone.

---

## 2. Standalone Mods
Each of these modules is distributed in its own isolated standalone package directory (and also bundled inside `mad2launcher`, see §3.1).

### 2.1 `chaosmod` Package
```text
chaosmod/
└── mods/
    └── mad2chaosmod.dll       # Interval timer and chaos director loop
```

### 2.1a `mirrormod` Package
* **Purpose:** persistent "Mirror Mode" toggle (`config.cfg` `[Mirror]` `Enabled`, or a live `ToggleKey`) — horizontally flips the screen and inverts the left/right stick X axis to match. Not part of the chaos-effects system (see CLAUDE.md's own section on it) — a full-session on/off mode, not a randomized/timed effect, so it has no dependency on `mad2effects`/`mad2chaosmod`.
```text
mirrormod/
└── mods/
    └── zz_mad2mirrormod.dll   # Mirror Mode (screen flip + stick invert) -- "zz_" is load-bearing, see CLAUDE.md
```

### 2.2 SM64 / Jak & Daxter / Mario Legacy — built locally, never packaged
* **This section used to describe a planned `sm64jak1mod` package bundling prebuilt game binaries. That's no longer the plan** — see the ground rules at the top of this document. `mad2companionmod.dll` (which registers all three chaos effects — `SM64Challenge`, `JakChallenge`, `MarioLegacyChallenge`) ships inside the `mad2effects` package (§1.2). The actual game engines are **never** prebuilt or bundled into any package, since two of the three need a user-supplied, copyrighted ROM/ISO to extract assets from, and shipping a prebuilt binary would either omit those assets (useless) or require redistributing them (not allowed):
  - **SM64** (`extern/sm64ex-alo`) and **Jak & Daxter** (`extern/jak-project`) are built locally by the end user, either via `mad2launcher`'s "Build Games" page (§3.1 — drives the new `mad2buildgames` tool) or directly via `just build-sm64-linux` / `just build-sm64-windows` (best-effort cross-build) / `just build-jak1-linux`. Until built, `SM64Challenge`/`JakChallenge` simply no-op with a log line (`mad2relauncher` degrades the same way — see `_play`'s `sm64_flag`/`jak1_flag` checks in the justfile).
  - **Mario Legacy** (`extern/mario-legacy`, `MarioLegacyChallenge`) is **not** included in the "Build Games" page or any package for now (see the ground rules). It still works via the CLI (`just setup-mario-legacy`) for anyone who sets it up by hand.
* **One shared `roms/` folder**, not one drop location per game. `roms/README.md` (this repo's root, and shipped inside every `mad2launcher_*` package too — see below) is the single place to drop `baserom.us.z64`/`jak1.iso`; `mad2buildgames` (`ensureRomLinked` in `mad2buildgames/main.go`) and the justfile's own `build-sm64-linux`/`build-sm64-windows`/`build-jak1-linux`/`setup-mario-legacy` recipes all symlink whichever file they need from there into the right `extern/*` subproject automatically, rather than requiring a separate copy per game. `*.z64`/`*.iso` stay globally gitignored, so nothing under `roms/` except its `README.md` is ever tracked or redistributed.
* **The `mad2launcher` package bundles SM64/Jak & Daxter's buildable *source*** (`extern/sm64ex-alo` on both platforms, `extern/jak-project` on Linux only, staged by `tools/package.sh`'s `stage_game_source`) so "Build Games" has something to actually compile after downloading just that one package — a full git checkout is no longer required. `mad2launcher/build_native.go`'s `GamesRoot(repoRoot)` resolves where this all lives at runtime: this launcher's own bundled `extern/`+`roms/` if present (the packaged-release case), falling back to `repoRoot/extern` (this repo's own dev checkout). `stage_game_source` copies git-tracked files only (correctly excludes build output and copyrighted ROM-extracted assets, which get regenerated from the user's own ROM/ISO at build time) — **except two paths `stage_jak_extras` copies verbatim alongside it**, both genuinely-needed non-generated content caught by (upstream) `.gitignore` patterns that are broader than intended: `extern/jak-project/third-party/` (~205MB of vendored third-party library source — fmt, curl, SDL, tree-sitter, sqlite3, replxx, cubeb, ...; `CMakeLists.txt` `add_subdirectory()`s a dozen+ libraries straight out of it, a hard non-regenerable dependency, not build output) and `extern/jak-project/goal_src/jak1/build/*.json` (~530KB of decompiler object-list config data, caught by the same non-root-anchored `build/` pattern that's meant to only exclude the real top-level build output directory). Omitting either broke the packaged build outright (CMake configure failure, then a decompiler "couldn't open .../all_objs_jak1_jp.json" failure respectively) — see §5 item 12 for the third bug found alongside these (a distrobox `RUNPATH` issue, also now fixed). Verified end-to-end: built Jak & Daxter from an isolated copy of exactly what a packaged release ships (bundled `extern/`+`roms/jak1.iso`, no access to this repo's own already-built `extern/jak-project`) and confirmed a fresh CMake configure through GOAL compile completes normally. This roughly doubles `mad2launcher_linux`'s packaged size (~730MB unpacked / ~215MB archived, mostly `extern/jak-project`'s asset/script/third-party tree) — a deliberate tradeoff so the feature actually works out of the box, not a size regression to fix.

### 2.3 `mad2music` Package (Background Music & Podcast)
* **Purpose:** Bundles the whole music/podcast subsystem together, since none of its pieces are individually useful without the others: the native playback engine, the chaos effect that triggers a podcast clip, and the HUD that shows what's currently playing. `mad2launcher`'s "Download music now" button (Music page) runs the bundled `downloader/mad2musicdownloader` directly (`ResolveMad2MusicDownloader` — never builds it) against `mad2music/audio/music/*.txt`'s link lists. The ~1.9GB audio pool itself is deliberately **not** part of this package — that's exactly what the downloader is for.
* `mad2music` was Linux-only for a long time; `just build-music-windows` (best-effort i686-w64-mingw32 cross-build, fetches a pinned SDL3 mingw devel release since SDL3 ships no static lib) now exists for native-Windows Mad2 installs, where there's no `mad2relauncher` to supervise it (see `mad2podcastmod`'s native-Windows `CreateProcess`-if-not-running startup branch in CLAUDE.md). `tools/package.sh` stages a `mad2music_windows` package when that build succeeds; it's still best-effort/less exercised than the Linux build.
```text
mad2music_linux/
├── mad2music                  # Native Linux SDL3 playback engine
├── downloader/
│   └── mad2musicdownloader    # Fetches the audio pool into audio/ post-install (yt-dlp based)
├── audio/                      # Empty on install; populated by running downloader/mad2musicdownloader
└── mods/
    ├── mad2podcastmod.dll      # "PodcastBreak" chaos effect -- triggers a clip via loopback UDP
    └── mad2musichud.dll        # Now-playing overlay, listens for mad2music's status broadcasts
```
`mad2music_windows` mirrors this layout, with `mad2music.exe` + the `SDL3.dll` it needs alongside (no static SDL3 mingw release exists) in place of the native Linux binary, and `downloader/mad2musicdownloader.exe` (pure Go, trivially cross-built) in place of the Linux downloader.

### 2.4 `mad2twitchcontrols` Package
```text
mad2twitchcontrols/
└── mods/
    └── mad2twitchcontrolsmod.dll # EventSub WebSocket integration
```

### 2.5 `playercoords` Package
```text
playercoords/
└── mods/
    └── playercoords.dll       # D3D9 overlay showing exact player coordinates
```

---

## 3. Host-Side Integration Utilities

### 3.1 `mad2launcher` Package
* **Purpose:** GUI config editor, save/cheat editor, game launcher, randomizer front-end, game-build front-end, and mod installer. Ships every package from §1–§2 alongside the launcher binary so it works as a complete, single-download release.
* **Linux Layout:**
```text
mad2launcher_linux/
├── mad2launcher               # Native GUI config editor / game launcher
├── mad2buildgames             # "Build Games" page's SM64/Jak1 build-wrapper tool (see below)
├── mad2musicpatch             # Music page's blanked-global.arc tool (see §1.5)
├── mad2launcher_settings.json # Per-install settings (wine prefix, Proton path, randomizer config, etc.)
└── packages/                  # Copies of every §1/§2 (Linux) package, for the Mod Installer to install from
    ├── mad2modloader/
    ├── mad2cheatapply/
    ├── mad2effects/
    ├── mad2speedrunning/
    ├── mad2rando_linux/
    ├── mad2assettools/
    ├── chaosmod/
    ├── mad2music_linux/
    ├── mad2twitchcontrols/
    ├── playercoords/
    └── mad2relauncher_linux/
```
* **Windows Layout:**
```text
mad2launcher_windows/
├── mad2launcher.exe            # GUI configuration & launch editor (best-effort cross-build, see §4.2)
├── mad2buildgames.exe
├── mad2musicpatch.exe
├── mad2launcher_settings.json
└── packages/                   # Same idea, Windows-flavored packages where they exist (no mad2relauncher_windows)
    ├── mad2modloader/
    ├── mad2cheatapply/
    ├── mad2effects/
    ├── mad2speedrunning/
    ├── mad2rando_windows/
    ├── mad2assettools/
    ├── chaosmod/
    ├── mad2music_windows/       # only if `just build-music-windows` succeeded -- best-effort
    ├── mad2twitchcontrols/
    └── playercoords/
```
Both produced by `just package` (`tools/package.sh`) — see that script for the exact manifest per package and how `packages/` gets assembled (a straight copy of every other staged `dist/<platform>/<pkg>/` directory).
* **Mod Installer — implemented** (`mad2launcher/modinstaller.go` + `ui_modinstaller.go`): a launcher category that uses the currently-selected install, discovers packages under `ResolvePackagesRoot` (checked in order: an explicit user-set override, `<launcher exe dir>/packages/`, `<repoRoot>/dist/<platform>/` — the last one lets `just package`'s output be installed straight from a dev checkout without a separate release download), lists them as checkboxes, and on "Install Selected" copies each checked package's `mods/*` into the install's `mods/` (merge, overwrite), seeds `config.cfg` only if the install doesn't already have one, and merges every other top-level file/directory (`coordinates.yaml`, `emotes/`, ...) into the install root — additive, never deletes anything already there. This is the intended end-user-facing install path now; `just deploy-mad2`/`deploy-mad2russia`/`deploy-mad2demo` remain the dev-build path (faster inner loop, no packaging step).
* **Build Games — implemented** (`mad2launcher/buildgames.go` + `ui_buildgames.go`): drives the new `mad2buildgames` tool (§3.3) to compile SM64 (Linux native, Windows best-effort cross-build) and Jak & Daxter (Linux only) from a user-supplied ROM/ISO, right from the GUI. Mario Legacy is deliberately not on this page (see the ground rules at the top of this document). Progress is appended to `buildgames.log` at the repo root; the page shows per-target ROM/ISO-present and already-built status via simple `fileExists` checks (`sm64RomPath`/`jak1IsoPath`/`sm64LinuxBinPath`/`sm64WindowsBinPath`/`jak1LinuxBinPath`).
* **Randomizer tab:** `randomizer.go`/`ui_randomizer.go`. **Gap #2 below is now fully resolved** — `RunRandomizer` calls `ResolveMad2Rando5` (`build_native.go`), which only ever looks for an already-built binary (dev `build/mad2rando5`, a bundled `packages/mad2rando_*`, or `dist/<platform>/mad2rando_*`) and never invokes `go build`. If none is found, the page shows an error telling the user to run `just build-mad2rando5` or install the package, instead of silently compiling one.
* **This same never-compile-at-runtime rule applies to every companion tool the launcher drives** — `mad2relauncher` (`ResolveMad2Relauncher`), `mad2arc`/`mad2repack` (`ResolveMad2AssetTool`), `mad2musicpatch` (`ResolveMad2MusicPatch`), `mad2musicdownloader` (`ResolveMad2MusicDownloader`), `mad2music` (`ResolveMad2Music`), `mad2buildgames` (`ResolveMad2BuildGames`). The sole exception is `mad2buildgames` itself invoking `make`/`task` to build SM64/Jak1 from source — a real, user-triggered, opt-in action against ROM/ISO assets the user supplies, not this launcher recompiling its own tooling.

### 3.2 `mad2relauncher` Package
* **Purpose:** Supervisor shim — wraps `umu-run`, crash-relaunches the game, and orchestrates `SM64Challenge`, `JakChallenge`, `MarioLegacyChallenge` (Wine branch only — see §2.2), and the `mad2music` background process (see CLAUDE.md's "Native Linux companion processes"). Takes each companion binary's location as a CLI flag (`-sm64-path`, `-jak1-dir`, `-mariolegacy-dir`, `-music-path`, `-music-audio-dir`) pointing straight at the relevant `extern/` checkout — `mad2launcher/launch.go` passes these the same way the justfile's `_play` recipe does, resolved via `sm64LinuxBinPath`/`jak1LinuxBinPath` (`mad2launcher/buildgames.go`).
* **`umu-run` ships bundled, not required system-wide.** `umu-launcher`'s own distribution is normally a system package (installs a thin script pointing at a Python package in site-packages) — this repo instead fetches umu-launcher's official portable "zipapp" release (`just fetch-umu-run`, pinned version, a self-contained Python zip archive that only needs a system `python3`, nothing else) into `build/umu-run`, right beside `build/mad2relauncher`. `mad2relauncher/gameloop.go`'s `resolveUmuRun()` prefers a copy found beside its own binary over one on PATH, so this same mechanism works whether `mad2relauncher` is running from `build/` (dev) or from wherever a package places it (`packages/mad2relauncher_linux/`, `dist/linux/mad2relauncher_linux/`) — `tools/package.sh` stages `umu-run` next to `mad2relauncher` in every one of those locations. `gamescope` remains a real, unbundled system dependency (no portable distribution of it exists) — `mad2launcher/launch.go` still hard-requires it on PATH.
* **Linux Layout:**
```text
mad2relauncher_linux/
├── mad2relauncher             # Supervisor shim (invokes umu-run / manages window focus / SM64+Jak1+Mario Legacy+music orchestration)
└── umu-run                    # Bundled portable umu-launcher zipapp (see above) -- needs system python3 only
```
* **Windows Layout:** **(Not planned — see §5 item 9.)** `mad2relauncher` currently only exists as a Linux tool; it wraps `umu-run`/gamescope/X11, none of which exist on native Windows, so a Windows build would need its crash-relaunch and window-focus logic reworked around native Win32 APIs rather than being a drop-in cross-compile. Not attempted.

### 3.3 `mad2buildgames` — ROM-to-build tool — **implemented**
* **Purpose:** a standalone Go module (`mad2buildgames/`, own `go.mod`, native `go build` only — never cross-compiled since it just orchestrates other native tools) that wraps the exact steps `just build-sm64-linux`/`build-sm64-windows`/`build-jak1-linux` already run (same `make`/`task`/`curl`/`distrobox` invocations, ported line-for-line — see `mad2buildgames/main.go`), so `mad2launcher`'s "Build Games" page (§3.1) can shell out to one prebuilt binary instead of requiring `just`/bash on the end user's machine. Takes `-target sm64-linux|sm64-windows|jak1-linux` and `-games-root <path>` (a directory containing `extern/` + `roms/` — see the shared `roms/` folder note in §2.2, resolved via `mad2launcher/build_native.go`'s `GamesRoot`); Mario Legacy is deliberately not a supported target (see the ground rules at the top of this document). Built via `just build-buildgames`.

---

## 4. Cross-Platform Compilation Commands

### 4.1 Windows 32-bit DLL Mods (using MinGW cross-toolchain)
```bash
# Configure Ninja build for 32-bit Windows target
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.toolchain.cmake

# Build all DLL mods
cmake --build build

# Deploy into the local runtime folders (dev workflow -- see §3.1 for the end-user-facing
# Mod Installer path instead)
just deploy-mad2
```

### 4.2 Go Launcher & Relauncher Tools
```bash
# Native build (what justfile's build-launcher/build-relauncher actually do today)
cd mad2launcher && go build -o ../build/mad2launcher .
cd ../mad2relauncher && go build -o ../build/mad2relauncher .
```
`just build-launcher-windows` cross-compiles `mad2launcher` for Windows — **confirmed working**, not just best-effort-attempted: `CGO_ENABLED=1 GOOS=windows GOARCH=386` with `CC=i686-w64-mingw32-gcc`/`CXX=i686-w64-mingw32-g++` (Fyne's desktop driver needs cgo for its OpenGL bindings, so plain `GOOS=windows go build` alone doesn't work) plus `-ldflags "-H windowsgui"` so it doesn't pop a console window behind the GUI. The resulting `mad2launcher.exe` depends on nothing beyond standard Windows system DLLs (`GDI32`/`KERNEL32`/`OPENGL32`/`SHELL32`/`USER32` plus the `api-ms-win-crt-*` UCRT forwarders, present on any Windows 10+ install) — verified via `objdump -p`, no `libgcc`/`libstdc++`/`libwinpthread` mingw runtime DLLs need to ship alongside it. `mad2relauncher` is pure Go with no cgo, so its own cross-build would be more likely to just work too — but per §3.2, a Windows `mad2relauncher` still needs its umu-run/gamescope/X11-specific logic reworked before the binary would do anything useful there, so it isn't attempted.

### 4.3 Randomizer & Asset Tools (`mad2rando`, `mad2assettools`)
```bash
# Native build (what justfile's build-mad2rando5/build-assettools actually do today)
just build-mad2rando5      # -> build/mad2rando5
just build-assettools      # -> build/{mad2arc,mad2repack,mad2tool,mad2save,mad2savegui,mad2transitions}

# Windows cross-build (pure Go, no cgo -- these are real justfile recipes, not illustrations)
just build-mad2rando5-windows      # -> build/windows/mad2rando5.exe
just build-assettools-windows      # -> build/windows/{mad2arc,mad2repack,mad2tool,mad2save,mad2transitions}.exe
```
`mad2rando5`, `mad2arc`, `mad2repack`, `mad2tool`, `mad2save`, and `mad2transitions` are pure Go with no cgo, so `GOOS`/`GOARCH` alone is enough. `mad2savegui` uses Fyne like `mad2launcher` — the same §4.2 caveat about cgo/OpenGL and needing a real C cross-compiler for a Windows target applies to it, not the others, so it's excluded from `build-assettools-windows`. `just package` (`tools/package.sh`) also cross-builds `mad2musicpatch`/`mad2buildgames`/`mad2musicdownloader` for Windows the same trivial way (no dedicated named recipes for those three since they're single `go build` invocations the script runs inline).

### 4.4 SM64 and Jak & Daxter Native Builds
```bash
# SM64, native Linux -- needs roms/baserom.us.z64 (symlinked into extern/sm64ex-alo/ automatically)
just build-sm64-linux

# SM64, best-effort Windows cross-build (i686-w64-mingw32, see justfile's own comments
# on the SDL2-mingw fetch and D3D11/DXGI renderer choice)
just build-sm64-windows

# Jak & Daxter, native Linux only -- needs roms/jak1.iso (symlinked into extern/jak-project/ automatically)
just build-jak1-linux
```
All three are also drivable from `mad2launcher`'s "Build Games" page (§3.1/§3.3) via the same `mad2buildgames` tool, without needing `just`/bash. There is no Jak & Daxter Windows build (upstream `open-goal/jak-project` has no MinGW path — MSVC/Windows-clang only — see CLAUDE.md's `mad2companionmod` section for the investigation). Mario Legacy (`just setup-mario-legacy`) is CLI-only, not wired into `mad2buildgames` or any package — see the ground rules at the top of this document.

---

## 5. Known Gaps & Open TODOs

Most of the items this section used to track are now resolved — kept struck through (rather than deleted) so the history of what was fixed stays visible:

1. ~~`mad2musichud.dll` is built but never deployed.~~ **Fixed.**
1a. ~~`mad2cheatapply.dll` has the identical bug right now.~~ **Fixed** — `add_mad2_mod()` (`cmake/add_mad2_mod.cmake`) plus its shared flat `build/dist/{version.dll,mods/*.dll}` staging means every mod's build output is discoverable by name; `add_deploy_target()` in the root `CMakeLists.txt` copies and lists it.
1b. **`mad2cheatapply` and `mad2gameeffectsmod` still duplicate ~40 lines of current-level detection.** Still open — `mad2leveldetect/mad2leveldetect.h` exists in-repo (a header, no `CMakeLists.txt`/build target of its own) but isn't wired into either mod yet. See `improvement-plan.md` item 12 for the extraction options being considered.
2. ~~`mad2launcher`'s Randomizer tab can still invoke `go build` at runtime.~~ **Fixed, and generalized.** Every companion tool the launcher drives (`mad2relauncher`, `mad2rando5`, `mad2arc`/`mad2repack`, `mad2musicpatch`, `mad2musicdownloader`, `mad2music`, `mad2buildgames`) now goes through a strict prebuilt-binary resolver (`mad2launcher/build_native.go`'s `findPrebuilt`/`Resolve*` family) that never shells `go build`/`cmake`/anything else — see §3.1.
3. ~~`tools/patch_global_music.sh` still points at `../mad2assetextractor`~~ **Fixed** (unchanged from before — `mad2musicpatch/`).
4. ~~No cross-compile `just` targets exist yet for `mad2rando5`/`mad2assettools`.~~ **Fixed** — `build-mad2rando5-windows`/`build-assettools-windows` are real recipes now (see §4.3).
5. ~~Jak & Daxter support isn't actually migrated yet.~~ **Fixed** — `extern/jak-project` is vendored in-repo, built via `just build-jak1-linux`, orchestrated by `mad2relauncher/jak1.go`. (A third companion game, Mario Legacy / `extern/mario-legacy`, has since been added too — `just setup-mario-legacy` — but is deliberately excluded from packaging/the "Build Games" page, see the ground rules at the top of this document.)
6. ~~No ROM-to-build tool exists.~~ **Fixed** — `mad2buildgames` (§3.3), driven from the launcher's "Build Games" page (§3.1). Covers SM64 (Linux + Windows) and Jak & Daxter (Linux); does not cover Mario Legacy (deliberately).
7. **`mad2music` Windows build status: improved, still best-effort.** `just build-music-windows` now exists and works well enough that `tools/package.sh` stages a `mad2music_windows` package when it succeeds — but it's newer and less exercised than the Linux build, so treat it as best-effort rather than fully proven.
8. ~~The `mad2launcher` `packages/` bundling and Mod Installer panel don't exist.~~ **Fixed** — see §3.1's Mod Installer description and `tools/package.sh` / `just package`.
9. **No Windows build of `mad2relauncher` exists**, and a naive cross-compile wouldn't work as-is (§3.2/§4.2) since it wraps Linux-only tooling (`umu-run`, gamescope, X11). Still open, still not attempted — this is the one piece of "every package should be Linux+Windows where the underlying thing makes sense" that's structurally Linux-only.
10. **Mario Legacy (`extern/mario-legacy`, `MarioLegacyChallenge`) isn't part of any package or the "Build Games" page.** Deliberate scope cut for now (see the ground rules at the top of this document), not a bug — `mad2companionmod.dll` still registers the effect (it just no-ops until the game is set up by hand via `just setup-mario-legacy`), and the CLI path keeps working. Revisit if/when it's meant to be a first-class packaged/GUI-driven target like SM64/Jak1.
11. **`mad2launcher`'s own Windows build (`build-launcher-windows`) is unsigned and untested end-to-end on real Windows.** Cross-compiles cleanly and depends on nothing beyond standard Windows 10+ system DLLs (verified via `objdump -p`), but has only been smoke-checked as a well-formed PE32 GUI binary in this dev environment — nobody has actually run it on Windows yet. Flag this to whoever does a first real Windows release.
12. ~~"jak1 fails to build" (from a packaged release, or any fresh build under the `mad2-debian` distrobox outside `$HOME`).~~ **Fixed — three distinct bugs, found by actually building from an isolated copy of exactly what a packaged release ships (not this repo's own already-built `extern/jak-project`):**
    - `tools/package.sh` omitted `extern/jak-project/third-party/` (see item under §3.1/§2.2 above) — CMake configure failed outright.
    - `goal_src/jak1/build/*.json` (decompiler object-list config data, ~530KB) is also unintentionally caught by the same non-root-anchored `build/` pattern in `extern/jak-project`'s own (upstream) `.gitignore` — without it the decompiler fails with `couldn't open .../all_objs_jak1_jp.json`. `tools/package.sh`'s `stage_jak_extras` now copies this path explicitly too, alongside `third-party/`, rather than editing the vendored `.gitignore` itself.
    - Independent of packaging: `task build-release` runs inside the `mad2-debian` distrobox (when present), which bakes a container-internal `RUNPATH` (`/run/host/<path>/...`) into `gk`/`goalc`/`extractor` for anything built **outside `$HOME`** — distrobox only bind-mounts `$HOME` at the same path inside the container, so this specific bug was invisible in this dev repo (which lives under `$HOME`) but bites both a packaged release unpacked elsewhere *and* `just build-jak1-linux` itself if ever run from outside `$HOME`. Fixed two ways: `mad2buildgames`'s post-build `extractor`/`goalc` invocations (and the justfile's `build-jak1-linux` recipe's own copies of the same two lines) now run back inside the same distrobox container that built them, when one was used; `mad2relauncher/jak1.go`'s runtime launch of `gk` (a separate call site, can't reasonably route actual gameplay through distrobox) instead sets `LD_LIBRARY_PATH` explicitly (searched before a binary's own `RUNPATH`) by walking `build/` for every directory containing a `.so`.
    - The GOAL compiler doesn't create its own output directory tree (`out/jak1/{fr3,iso,obj}/`), and nothing in `Taskfile.yml` does either — a truly fresh checkout fails outright the first time `(mi)` tries to write a `.DGO` file there (`Error: failed to write .../out/jak1/iso/JUB.DGO`). This repo's own `extern/jak-project` only ever avoided the bug because that directory tree was created once by hand a long time ago and just carried forward since. `mad2buildgames` and the justfile's `build-jak1-linux` recipe now `mkdir -p` it explicitly before the GOAL compile step.
    - Verified end-to-end (twice — the first attempt's late-stage `.DGO` write failures turned out to be `tmpfs`-space exhaustion in the sandboxed test environment itself, not a code bug; re-run against disk-backed storage with adequate free space completed cleanly): a from-scratch build (fresh CMake configure through full GOAL asset/level compile, "Successfully built all 1317 targets") using only what `tools/package.sh` now stages, with no access to this repo's own pre-built `extern/jak-project`.
13. ~~"sm64 fails to build" — `error reading from file 'build/us_pc/charmap.txt': Success` / `Error 1: build/us_pc/include/text_options_strings.h`.~~ **Fixed — a genuine upstream Makefile bug in `extern/sm64ex-alo`**, reported from a real packaged-release user's `buildgames.log`. `Makefile`'s `text_options_strings.h`/`text_cheats_strings.h`/`text_debug_strings.h` rules all reference `$(BUILD_DIR)/$(CHARMAP)` (`build/us_pc/charmap.txt`) in their recipe but never declared it as a prerequisite — unlike the correctly-written `text_strings.h` rule right above them, which does. Under parallel `make -j` (this repo always builds with `-j$(nproc)`), the encode step could start reading `charmap.txt` before another job finished (re)writing it. Fixed by adding the missing prerequisite to all three rules, matching `text_strings.h`'s existing pattern — verified with `make -n` that the dependency graph now correctly orders charmap regeneration before the encode step. Higher core counts hit this far more often, which is likely why it wasn't caught in earlier testing on this dev machine.
14. ~~"Proton not found" despite Proton being installed and working (via Steam itself) for the exact same user on the older `../mad2mod` tool.~~ **Fixed.** `ResolveProtonPath` (`mad2launcher/launch.go`) and the justfile's `_resolve_proton` recipe only ever checked `~/.local/share/Steam` — `../mad2mod` never had this problem since it launches games through Steam directly rather than invoking `umu-run` itself, so it never needed to independently rediscover Proton's on-disk location at all. Two real users hit two different variants: one had Steam installed via Flatpak (no `~/.local/share/Steam` at all), the other via Debian's own `steam` package (real data directory `~/.steam/debian-installation`, and on their machine neither of the `~/.steam/root`/`~/.steam/steam` symlinks Steam normally maintains resolved to it either). Both implementations now search a `steamRootCandidates` list covering `~/.steam/root`, `~/.steam/steam`, `~/.steam/debian-installation`, `~/.local/share/Steam`, the Flatpak path, and the Snap path, in that order, stopping at the first root that actually has a `GE-Proton*`/`Proton*` install under its `compatibilitytools.d`/`steamapps/common`. `mad2launcher/settings.go`'s `defaultProtonPathSetting` (the Settings page's first-run pre-fill) now calls the same resolver instead of hardcoding a single guessed path. Verified with a simulated `$HOME` containing only a `.steam/debian-installation` install (no other Steam path present at all) — resolves correctly.
