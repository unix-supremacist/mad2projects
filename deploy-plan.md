# Deployment and Packaging Plan: Madagascar 2 Mod Loader & Chaos Suite

This document defines the deployment layout, packaging structure, and cross-platform compilation plan for the **Madagascar 2 Mod Loader & Chaos Suite** project.

Outputs are packaged as separate, modular archives/directories rather than a single monolithic delivery. Every package below is also bundled inside the `mad2launcher` package (see §3.1), so a user who just wants the GUI experience only needs one download; the standalone packages exist for CLI/scripted use and for people who only want one piece (e.g. just the speedrunning overlays).

Some of what follows describes work that isn't built yet — those sections are marked **(Planned)**. §5 collects every such item in one place as an explicit TODO list; see it before assuming anything below already works.

---

## 1. Split Package Directory Layouts

### 1.1 `mad2modloader` Package (Core Suite)
* **Purpose:** Core mod loader and critical fixes. Essential dependency for all other mods.
* **Linux (Wine/Proton) & Windows (Native) Layout:**
```text
mad2modloader/
├── version.dll                # Proxy DLL loaded by AWL.dll
├── config.cfg                 # Seeded default configuration file
└── mods/
    ├── mad2assetloader.dll    # Asset path redirection
    ├── mad2config.dll         # Shared config file API
    ├── mad2saveloader.dll     # Save redirection
    ├── mad2xinput.dll         # Layered control overrides
    └── shadowfix.dll          # D3D9 shadow renderer fix
```

---

### 1.1a `mad2cheatapply` Package **(New — undocumented until now)**
* **Purpose:** Title-screen cheat-apply mod, ported from `../mad2mod`'s `enable_all_cheats`/`load_cheats_config`. Waits for the title screen (via the same current-level detection `mad2gameeffectsmod` uses — see CLAUDE.md and `improvement-plan.md` for the duplication that implies) and, on a configured trigger, applies the saved cheat toggles. Was missing from this document entirely; also currently missing from the CMake deploy target — see §5.
```text
mad2cheatapply/
└── mods/
    └── mad2cheatapply.dll     # Applies saved cheat toggles at the title screen
```

---

### 1.2 `mad2effects` Package (Chaos Mod Core System)
* **Purpose:** Core registry and effects with no external binary/process dependency. (SM64/Jak & Daxter and podcast playback need a companion process each and so ship in their own standalone packages — see §2.2/§2.3 — even though they register into this same effect registry at runtime.)
* **Linux (Wine/Proton) & Windows (Native) Layout:**
```text
mad2effects/
├── coordinates.yaml           # Position database (copied to game root folder)
└── mods/
    ├── mad2effects.dll        # Shared chaos effect registry API
    ├── mad2inputeffectsmod.dll# Input manipulation effects
    ├── zz_mad2graphicseffectmod.dll  # Graphics screen post-process effects
    ├── mad2gameeffectsmod.dll # Game-state mutation effects (ObjectShuffle, MangoMutate, PlayerTeleport)
    └── mad2effectshud.dll     # HUD overlay for active effects
```

---

### 1.3 `mad2speedrunning` Package (Overlays & Timers)
* **Purpose:** HUD indicators, timers, and streaming overlays.
* **Linux (Wine/Proton) & Windows (Native) Layout:**
```text
mad2speedrunning/
├── LICENSE_KENNEY.txt         # Kenney CC0 1.0 Universal license text
├── CREDITS_KENNEY.txt         # Attribution pointing to Kenney.nl
├── emotes/                    # Emote images folder for twitchchat.dll
│   └── (Twitch chat PNG/GIF emotes)
└── mods/
    ├── igttimer.dll           # In-game/RTA timer overlay
    ├── inputdisplay.dll       # Controller visualizer overlay (uses Kenney assets)
    └── twitchchat.dll         # Anonymous IRC Twitch chat overlay
```
`mad2playercoords` (§2.5) and `mad2musichud` (§2.3) are architecturally the same kind of D3D9 overlay as everything above, but each ships in its own package instead — see those sections for why.

---

### 1.4 `mad2rando` Package (Game Randomizer)
* **Purpose:** Level-logic randomization + the interception mod that consumes it. `mad2rando5` computes a `Slot -> Level` mapping and writes `level_redirects.txt`; it never repacks or touches any archive itself, so it has no dependency on the archive tools in §1.5. Ships two ways: as this standalone package (for CLI/scripted use) and bundled inside the `mad2launcher` package, where its "Mad2 Randomizer" tab drives the same binary (see §3.1). `mad2rando5/` is an in-repo Go module (migrated from a formerly-external sibling checkout) built via `just build-mad2rando5` — no external checkout required anymore.
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

### 1.5 `mad2assettools` Package (Archive & Save Editing) **(New)**
* **Purpose:** general-purpose IGA archive extraction/repacking and save-file editing, unrelated to the randomizer (a different formerly-external sibling, `mad2assetextractor`, migrated in-repo the same way §1.4 was — see CLAUDE.md's "What this is"). `mad2iga` is the shared archive-format library `mad2arc`/`mad2repack`/`mad2tool` import via a local `go.mod` `replace` directive; it's not shipped as its own binary. `mad2savegui` overlaps with `mad2launcher`'s own built-in save/cheat editor (a from-scratch reimplementation, not a caller of this) — bundled here mainly for CLI/no-GUI-launcher use. Built via `just build-assettools`.
* **Linux/Windows Layout** (same tools, `.exe` suffix on Windows):
```text
mad2assettools/
├── mad2arc                    # IGA archive extract/list tool
├── mad2repack                 # IGA archive repack/replace tool
├── mad2tool                   # Misc archive helper tool
├── mad2save                   # CLI save-file editor/checksummer/PC<->Wii converter
├── mad2savegui                # Fyne GUI save-file editor (standalone; not called by mad2launcher)
└── mad2transitions             # Level-transition data tool (see mad2gameeffectsmod's "Level Transition system" note in CLAUDE.md)
```

---

## 2. Standalone Mods
Each of these modules is distributed in its own isolated standalone package directory (and also bundled inside `mad2launcher`, see §3.1).

### 2.1 `chaosmod` Package
```text
chaosmod/
└── mods/
    └── mad2chaosmod.dll       # Interval timer and chaos director loop
```

### 2.2 `sm64jak1mod` Package **(Planned expansion — see §5)**
* **Purpose:** `mad2sm64mod.dll` now registers two chaos effects out of the same DLL — `SM64Challenge` (unchanged) and the new `JakChallenge` (Jak & Daxter via a `gk` OpenGOAL runtime process). Today `JakChallenge` only works on this dev machine: it points at `~/src/jak1nolauncher`, an un-vendored sibling checkout containing a prebuilt `gk` binary and a copyrighted game ISO, with no build recipe in this repo. The plan is to fully migrate that project's source in (parallel to how `extern/sm64ex-alo` is already vendored) and give both games a single, user-supplied ROM/ISO drop folder that one small tool builds from — package renamed accordingly.
* **Linux Layout (Planned):**
```text
sm64jak1mod_linux/
├── roms/                      # User-supplied, NOT redistributed: baserom.us.z64 (SM64), jak1.iso (Jak & Daxter)
├── sm64/
│   └── sm64.us.f3dex2e        # Built by tools/buildroms (or `just build-roms`) from roms/baserom.us.z64
├── jak1/
│   ├── gk                     # Built the same way, from roms/jak1.iso
│   ├── goalc
│   └── data/                  # Extracted/compiled GOAL assets
└── mods/
    └── mad2sm64mod.dll        # Handles both SM64Challenge and JakChallenge
```
* **Windows Layout (Planned, SM64 only):** `JakChallenge` is Wine-only per its own source comment (no Windows OpenGOAL build path exists) — the Windows package therefore only carries the SM64 half:
```text
sm64jak1mod_windows/
├── roms/
│   └── baserom.us.z64
├── sm64/
│   └── sm64.exe                # Best-effort mingw cross-build, see §4
└── mods/
    └── mad2sm64mod.dll
```

### 2.3 `mad2music` Package (Background Music & Podcast) **(New)**
* **Purpose:** Bundles the whole music/podcast subsystem together, since none of its pieces are individually useful without the others: the native playback engine, the chaos effect that triggers a podcast clip, and the HUD that shows what's currently playing. Linux-only today — no Windows build of `mad2music` exists or is planned (`mad2relauncher`, which supervises it, is itself the thing gluing this into a Wine session; see CLAUDE.md's "Native Linux companion processes"). The ~1.9GB audio pool is deliberately **not** part of this package — see §5.
```text
mad2music_linux/
├── mad2music                  # Native Linux SDL3 playback engine
├── downloader/
│   └── download_music          # Fetches the audio pool into audio/ post-install (yt-dlp based)
├── audio/                      # Empty on install; populated by running downloader/download_music
└── mods/
    ├── mad2podcastmod.dll      # "PodcastBreak" chaos effect -- triggers a clip via loopback UDP
    └── mad2musichud.dll        # Now-playing overlay, listens for mad2music's status broadcasts
```

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
* **Purpose:** GUI config editor, save/cheat editor, game launcher, randomizer front-end, and (planned) one-stop mod installer. Ships every package from §1–§2 alongside the launcher binary so it works as a complete, single-download release.
* **Linux Layout:**
```text
mad2launcher_linux/
├── mad2launcher               # Native GUI config editor / game launcher
├── mad2launcher_settings.json # Per-install settings (wine prefix, Proton path, randomizer config, etc.)
└── packages/                  # (Planned) copies of every §1/§2 package, for the Mod Installer to install from
    ├── mad2modloader/
    ├── mad2effects/
    ├── mad2speedrunning/
    ├── mad2rando_linux/
    ├── mad2assettools/
    ├── chaosmod/
    ├── sm64jak1mod_linux/
    ├── mad2music_linux/
    ├── mad2twitchcontrols/
    └── playercoords/
```
* **Windows Layout:**
```text
mad2launcher_windows/
├── mad2launcher.exe            # GUI configuration & launch editor
├── mad2launcher_settings.json
└── packages/                   # (Planned) same idea, Windows-flavored packages where they exist
    ├── mad2modloader/
    ├── mad2effects/
    ├── mad2speedrunning/
    ├── mad2rando_windows/
    ├── mad2assettools/
    ├── chaosmod/
    ├── sm64jak1mod_windows/
    ├── mad2twitchcontrols/
    └── playercoords/
```
* **Mod Installer (Planned, not implemented yet):** a new launcher panel — pick a discovered/added install, pick which package(s) from `packages/` to install or update, and the launcher copies each package's `mods/*.dll` (merging into the install's existing `mods/` folder) plus any non-DLL assets (`coordinates.yaml`, `config.cfg` seed, `emotes/`, `sm64/`/`jak1/`/`roms/`) into place. This is meant to replace `just deploy-mad2` as the end-user-facing install path — `just deploy-*` remains the dev-build path.
* **Randomizer tab:** already implemented (`randomizer.go`/`ui_randomizer.go`). `LocateMad2Rando5Dir` prefers the in-repo `mad2rando5/` directory (falling back to a legacy `../mad2rando5` sibling only if that's not found), and `ensureRandomizerBuilt` only re-`go build`s when `main.go` is newer than the existing binary, reusing it otherwise. The remaining gap (see §5) is that this still assumes a Go toolchain is available at runtime for that first build/rebuild — a packaged release has no such guarantee, so it should instead just run the binary already sitting in `packages/mad2rando_*` (§1.4) and never invoke `go build` at all.

### 3.2 `mad2relauncher` Package
* **Purpose:** Supervisor shim — wraps `umu-run`, crash-relaunches the game, and orchestrates `SM64Challenge`, `JakChallenge`, and the `mad2music` background process (see CLAUDE.md's "Native Linux companion processes"). Takes each companion binary's location as a CLI flag (`-sm64-path`, `-jak1-dir`, `-music-path`, `-music-audio-dir`); once `sm64jak1mod`'s roms/tools migration (§2.2) lands, `-jak1-dir` should point at that package's `jak1/` folder instead of a `~/src/jak1nolauncher` dev checkout.
* **Linux Layout:**
```text
mad2relauncher_linux/
└── mad2relauncher             # Supervisor shim (invokes umu-run / manages window focus / SM64+Jak1+music orchestration)
```
* **Windows Layout:** **(Planned/unverified — see §5.)** `mad2relauncher` currently only exists as a Linux tool; it wraps `umu-run`/gamescope/X11, none of which exist on native Windows, so a Windows build would need its crash-relaunch and window-focus logic reworked around native Win32 APIs rather than being a drop-in cross-compile. Not attempted yet.
```text
mad2relauncher_windows/
└── mad2relauncher.exe         # Windows relauncher shim
```

### 3.3 ROM-to-build tool **(Planned, does not exist yet)**
* **Purpose:** the "small Go script" `sm64jak1mod`'s `roms/` folder needs (§2.2): given a user-supplied `roms/baserom.us.z64` and `roms/jak1.iso`, drive `extern/sm64ex-alo`'s existing `make`-based asset extraction/build and `jak1nolauncher`'s existing `extractor/`+`goalc` build steps to produce the `sm64/` and `jak1/` output folders. Wraps the same steps `just build-sm64-linux`/`build-sm64-windows` already do for SM64 (see §4) plus the not-yet-written Jak & Daxter equivalent, as one tool `mad2launcher` can also shell out to from a "Build Games" action, instead of requiring the CLI.

---

## 4. Cross-Platform Compilation Commands

### 4.1 Windows 32-bit DLL Mods (using MinGW cross-toolchain)
```bash
# Configure Ninja build for 32-bit Windows target
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.toolchain.cmake

# Build all DLL mods
cmake --build build

# Deploy into the local runtime folders (dev workflow -- see §3.1 for the planned end-user path)
just deploy-mad2
```

### 4.2 Go Launcher & Relauncher Tools
```bash
# Native build (what justfile's build-launcher/build-relauncher actually do today)
cd mad2launcher && go build -o ../build/mad2launcher .
cd ../mad2relauncher && go build -o ../build/mad2relauncher .
```
Cross-compiling `mad2launcher` for Windows from Linux (`GOOS=windows GOARCH=386 go build`) is **untested** and likely won't be a plain drop-in: Fyne (the GUI toolkit `mad2launcher` uses) pulls in cgo/OpenGL bindings on most backends, which means a real Windows cross-build would need `CGO_ENABLED=1` plus an `i686-w64-mingw32` C cross-compiler (the same toolchain already used for the DLL mods in §4.1), not just `GOOS`/`GOARCH`. `mad2relauncher` is pure Go with no cgo, so its own cross-build is more likely to just work — but per §3.2, a Windows `mad2relauncher` still needs its umu-run/gamescope/X11-specific logic reworked before the binary would do anything useful there.

### 4.3 Randomizer & Asset Tools (`mad2rando`, `mad2assettools`)
```bash
# Native build (what justfile's build-mad2rando5/build-assettools actually do today)
just build-mad2rando5      # -> build/mad2rando5
just build-assettools      # -> build/{mad2arc,mad2repack,mad2tool,mad2save,mad2savegui,mad2transitions}
```
Cross-compiling for the other OS, e.g.:
```bash
cd mad2rando5 && GOOS=windows GOARCH=amd64 go build -o ../build/windows/mad2rando5.exe .
```
`mad2rando5`, `mad2arc`, `mad2repack`, `mad2tool`, `mad2save`, and `mad2transitions` are pure Go with no cgo, so this is expected to just work. `mad2savegui` uses Fyne like `mad2launcher` — the same §4.2 caveat about cgo/OpenGL and needing a real C cross-compiler for a Windows target applies to it, not the others. None of this is wired into `justfile` as cross-compile targets yet (see §5) — the commands above are the same shape `build-sm64-windows` already uses elsewhere in this repo, just not yet written for these tools.

### 4.4 SM64 (existing) and Jak & Daxter (planned) Native Builds
```bash
# SM64, native Linux -- needs roms/baserom.us.z64 (or extern/sm64ex-alo/baserom.us.z64 today)
just build-sm64-linux

# SM64, best-effort Windows cross-build (i686-w64-mingw32, see justfile's own comments
# on the SDL2-mingw fetch and D3D11/DXGI renderer choice)
just build-sm64-windows
```
There is no `just build-jak1` yet, and no vendored `extern/jak1nolauncher` to build from — see §3.3/§5.

---

## 5. Known Gaps & Open TODOs

Collected from everything marked **(Planned)** above, plus one bug found while writing this plan:

1. ~~`mad2musichud.dll` is built but never deployed.~~ **Fixed** — `add_deploy_target()` in the root `CMakeLists.txt` now copies it and lists it in `DEPENDS`. (Confirmed while cross-checking this doc against `improvement-plan.md`; leaving this line struck through rather than deleted so it's clear the same bug class below was caught by comparison against this exact item.)
1a. **`mad2cheatapply.dll` has the identical bug right now.** `CMakeLists.txt` has `add_subdirectory(mad2cheatapply)` (and it's documented in §1.1a above), but `add_deploy_target()`'s `COMMAND`/`DEPENDS` lists don't mention it — `just deploy-mad2`/`deploy-mad2russia`/`deploy-mad2demo` all silently skip it today. Same fix shape as the old item 1. See `improvement-plan.md` for a proposed structural fix (an `add_mad2_mod()`-style helper so "which mods exist" has one source of truth instead of three hand-maintained lists — `add_subdirectory` calls, this deploy `COMMAND` list, and this deploy `DEPENDS` list — that can silently drift apart, as just happened twice).
1b. **`mad2cheatapply` and `mad2gameeffectsmod` duplicate ~40 lines of current-level detection.** `mad2cheatapply/src/cheatapply.cpp`'s `UpdateCurrentLevel`/`UpdateCurrentLevelFromScriptInfo` (its own comment admits this) copy `mad2gameeffectsmod`'s functions of the same name verbatim — both try `mad2levelredirectmod`'s `Mad2LevelRedirect_GetCurrentLevel` first, then fall back to reading `ScriptInfoLib.dll`'s `_levelLoadRequest` heuristic directly. See `improvement-plan.md` for the extraction options being considered (and why it isn't a trivial "just call the DLL" fix — the fallback path exists specifically for when `mad2levelredirectmod` *isn't* loaded).
2. **`mad2launcher`'s Randomizer tab can still invoke `go build` at runtime** (`randomizer.go`'s `ensureRandomizerBuilt`, only when the binary is missing or stale) instead of exclusively running a prebuilt binary from a `packages/mad2rando_*` download. Harmless on this dev machine (Go toolchain always present), but a packaged release shouldn't assume that. Needs to change per §1.4/§3.1.
3. ~~`tools/patch_global_music.sh` still points at `../mad2assetextractor`~~ **Fixed** — the bash script was replaced outright with `mad2musicpatch/` (a Go tool, `just build-mad2musicpatch`), which takes `-mad2arc`/`-mad2repack` binary paths as flags (defaulting to `build/`) instead of hardcoding the old external-sibling layout. `mad2launcher`'s Music category (`music.go`/`ui_music.go`) builds and drives it directly for the deploy/remove-blanked-`global.arc` buttons.
4. **No cross-compile `just` targets exist yet for `mad2rando5`/`mad2assettools`** — §4.3's commands are hand-written illustrations of what's needed, not things `justfile` actually runs today (unlike `build-sm64-windows`, which is a real recipe).
5. **Jak & Daxter support isn't actually migrated yet.** `JakChallenge` (`mad2sm64mod`'s `jak1mod.cpp`) currently hard-depends on `~/src/jak1nolauncher`, an un-vendored sibling checkout with a prebuilt `gk` binary and a copyrighted game ISO — there is no build recipe, vendored source, or `roms/`-folder convention for it in this repo yet. §2.2/§3.3 describe the target shape; none of it exists today beyond the mod-side C++/UDP-protocol code and `mad2relauncher`'s `jak1.go` orchestration.
6. **No ROM-to-build tool exists** (§3.3) — today, building SM64 is a `just` recipe with inline bash; there's no equivalent for Jak & Daxter, and no unified `roms/` folder convention either game reads from.
7. **`mad2music` has no Windows build.** Background music/podcast playback is Linux-only; there's no plan yet for whether/how that should work on native Windows.
8. **The `mad2launcher` `packages/` bundling and Mod Installer panel don't exist.** Nothing currently copies §1/§2 package contents into a launcher build, and there's no UI for installing them into a game directory — this entire feature is still just this plan.
9. **No Windows build of `mad2relauncher` exists**, and a naive cross-compile wouldn't work as-is (§3.2/§4.2) since it wraps Linux-only tooling (`umu-run`, gamescope, X11).
