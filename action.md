# Roadmap: chaos-effects/companion-process overhaul

## Context

Several parts of the chaos-effects/companion-process stack have accumulated real gaps and duplication that this roadmap closes out:

- **`mad2companionmod`'s `SM64Challenge`** runs a whole separate native process (`extern/sm64ex-alo`) purely so Wine doesn't have to stack two D3D9 layers. But `mad2nesmod` already solved the "run an emulated console inside the Mad2 process" problem for NES/N64 via libretro cores, including a working N64Sm64Challenge running the real ROM via mupen64plus_next. Consolidating onto that path removes an entire companion-process/relauncher/X11-focus-juggling code path (SM64Challenge, `sm64.go`, `extern/sm64ex-alo`'s build targets) and gets SM64 the same first-class RAM-poke effect treatment (`AutoStopOnLevelComplete`, `RandomLevelStart`) the NES games already have — using the PC port's own existing "quit on star, retry same level on death" patch (`extern/sm64ex-alo/src/game/level_update.c:852-877`, `gChallengeLevel`) as the *effect* to replicate, not code to port literally, since the target is now emulated ROM RAM, not a recompiled executable.
- **`mad2companionmod`'s Jak1/Mario Legacy effects** are structurally solid, but Mario Legacy is missing the one patch that makes Jak1 feel like a real chaos effect (power-cell quits back to Mad2, random checkpoint start) because only prebuilt binaries were ever vendored — no source to patch. And neither fork has a Windows build path at all, unlike everything else in this repo.
- **`mad2inputdisplay`** regressed relative to the old external `../mad2mod` tool, which had a GameCube-layout branch for SM64 that never got ported when `mad2inputdisplay` was rewritten in-process.
- **Cheat toggling exists as a working diagnostic** (`mad2climbprobe`'s `ToggleAllCheats`, proven to work mid-level, not just at the title screen) but was never turned into an actual chaos effect.
- **`mad2music`/`mad2relauncher`** are Linux/gamescope-first; getting more of the stack running on native Windows is an explicit goal, and music categorization has outgrown its current copyright/lyrics-only split.
- **The randomizer** currently redirects levels by intercepting file opens at runtime. Now that the pak-recompilation text-resize crash is fully fixed and verified end-to-end (`docs/IGA_FORMAT.md`, Bug 5/6 resolution), it can move to directly rewriting `global.bld`/level paks offline instead — a bigger architectural change that also unlocks a monkey-name randomizer (monkey names are plain strings in `global.bld`'s `ENGLISH.pak`, already edited successfully by hand before — this is NOT the unsolved "instance naming" research problem documented elsewhere, it's ordinary string-pool editing). This is sequenced last, per explicit instruction, since it needs the most testing.

This document is a **roadmap only** — no code changes happen as part of writing it. It's meant to guide implementation across multiple future sessions. Each phase below should get its own session(s)/PR(s); check off sub-items as they land.

---

## Phase 1 — Retire the SM64 PC port, replicate its effect in nesmod's emulated core

**Goal:** `N64Sm64Challenge` (running the real ROM via mupen64plus_next inside `mad2nesmod`) gains the same behavior `SM64Challenge` (the native `extern/sm64ex-alo` process) has today, then the PC port is removed entirely.

1. Clone the SM64 decompilation project (e.g. `n64decomp/sm64`) **as a reference only** — not built or vendored — to identify the RAM addresses/symbols for: current course/level number, current star/act number, the warp-op state machine (analog of `sDelayedWarpOp`/`WARP_OP_STAR_EXIT`/`WARP_OP_CREDITS_START` in `extern/sm64ex-alo/src/game/level_update.c:852-877`), and death/game-over state.
2. Pull per-ROM-hack memory-offset data from `aglab2/StarDisplayLayouts` (companion data repo to `aglab2/SM64StarDisplay`) so the resulting detection generalizes across supported hacks (confirmed target: Star Road) instead of only vanilla SM64 US, mirroring how that tool itself achieves hack compatibility.
3. In `mad2nesmod/src/nesmod.cpp`'s N64 engine path, add the N64 analog of the existing NES `DetectLevelComplete`/`kAddr_*` pattern (lines ~1637-1987 today): poll N64 RAM via `retro_get_memory_data` each frame for the warp-op-equivalent state; when it indicates a star-exit or credits-start event, call `Mad2Effects_ClearByName("N64Sm64Challenge")` (the "quit" signal, replacing the PC port's `exit(0)`). On the death/game-over-equivalent state, replicate the PC port's *retry* behavior (reset health/lives-equivalent RAM, re-warp into the same course) instead of ending the effect — same intent as `gChallengeLevel`'s death handling.
4. Add a random-course-start RAM poke (the N64 analog of `gCLIOpts.LevelNumOverride`/`gChallengeLevel`) applied once per fresh load, matching `RandomLevelStart`'s existing NES pattern.
5. Remove `SM64Challenge` entirely: `mad2companionmod/src/sm64mod.cpp`, `mad2companionmod/include/mad2sm64_protocol.h`, `mad2relauncher/sm64.go` and its wiring in `mad2relauncher/main.go`, `extern/sm64ex-alo` (vendor + build targets `build-sm64-linux`/`build-sm64-windows` in `justfile`), and `roms/README.md`'s `baserom.us.z64` entry (keep `roms/sm64.z64`, the N64 ROM dump, since `N64Sm64Challenge` still needs it).
6. Update `CLAUDE.md`'s architecture writeup to drop `SM64Challenge`/`extern/sm64ex-alo` and reflect `N64Sm64Challenge` as the sole SM64 integration.

**Verification:** build + deploy, launch, trigger `N64Sm64Challenge` via the HUD/chaos mode against vanilla `sm64.z64`, confirm grabbing a star clears the effect and dying retries the same course without ending it; repeat against a Star Road ROM using the layout-derived offsets; check `logs/mad2.log` for the new detection lines.

## Phase 2 — Finish and rename nesmod to an "emulation effects" mod

1. Rename `mad2nesmod` → `mad2emulationeffectsmod` (directory, CMake target, DLL name, deploy target in root `CMakeLists.txt`, `justfile` `_deploy-nes`/`_deploy-n64` steps, `nescore`/`n64core` folder wiring left as-is since they're per-console not per-mod-name).
2. Fold in the Phase 1 N64Sm64Challenge changes as part of the same pass; do a general pass closing out any other rough edges/TODOs left in `nesmod.cpp` (e.g. World 9/Worlds A-D — leave parked per existing notes unless trivially resolved).
3. Update `CLAUDE.md` and any deploy docs for the rename.

**Verification:** clean build + deploy under the new name, all three existing effects (NesChallenge, NesLostLevelsChallenge, N64Sm64Challenge) still trigger and clear correctly.

## Phase 3 — Vendor real Super Mario Legacy source, port the Jak1 patch

1. Replace/augment the prebuilt-binaries-only vendor at `extern/mario-legacy` with an actual checkout of `OpenGOAL-Mods/Super-Mario-Legacy`'s source (matching the currently-vendored v0.0.28 tag, or latest if a newer tag is more appropriate — confirm at execution time).
2. Port the `*kernel-boot-level*`/power-cell-`kernel-shutdown`/random-checkpoint-start patch from `extern/jak-project/goal_src/jak1/engine/game/game-info.gc:182-192,317` into Mario Legacy's own differently-nested `data/goal_src/.../game-info.gc` (confirmed absent upstream, confirmed nested under `data/` unlike jak-project's top-level layout).
3. Wire a real `just build-mario-legacy-linux` (or extend `setup-mario-legacy`) that actually compiles `goal_src` with the patch applied, replacing today's "extract assets only" behavior — mirror `build-jak1-linux`'s shape.
4. Update `mad2companionmod/src/mariolegacymod.cpp` and `mad2relauncher/mariolegacy.go` if the checkpoint-selection/launch-flag wiring needs to change to match Jak1's (`-level <checkpoint>` etc.).
5. Update CLAUDE.md's "Known gap" note for `MarioLegacyChallenge` — should no longer apply.

**Verification:** build from source, deploy, trigger `MarioLegacyChallenge`, confirm it boots straight to a random checkpoint (not its own title screen) and that grabbing a power cell quits back to Mad2, matching `JakChallenge`'s behavior.

## Phase 4 — Native Windows builds for Jak1 and Mario Legacy, automated via the launcher

Confirmed: no MinGW cross-compile path exists for either fork (both need real clang/MSVC toolchain); the intended approach is a **native** compile on an actual Windows machine (not cross-compiled from Linux), automated the same way `just build-jak1-linux`/the new Phase 3 Mario Legacy build automate it on Linux.

1. Add Windows build automation to `mad2launcher` (which already has a native-Windows launch branch, `launch.go`'s `runtime.GOOS == "windows"` case) — a build action that shells out to `task`/clang on the Windows host to build `extern/jak-project` and `extern/mario-legacy` from source, mirroring their existing upstream `windows-build-clang.yaml` CI recipes.
2. This depends on Phase 3's Mario Legacy source vendor being in place; can run in parallel with Phase 3's Linux-side patch work.
3. Wire the resulting `gk.exe`/`goalc.exe` outputs into `mad2companionmod`'s existing native-Windows `CreateProcess` branches (`jak1mod.cpp` currently has none — needs one added, matching `mariolegacymod.cpp`'s existing native-Windows shape).

**Verification:** on an actual Windows install of the game, use `mad2launcher` to build both, then trigger `JakChallenge`/`MarioLegacyChallenge` and confirm they launch and the checkpoint/power-cell behavior matches the Linux/Wine path.

## Phase 5 — `mad2effectsminigame` mod (Pong, Snake, Minesweeper)

1. New CMake target `mad2effectsminigame`, structurally mirroring `mad2emulationeffectsmod`'s pattern (per-game `Engine`-like struct, D3D9 `EndScene` hook via `mad2hookutil`, full-screen takeover, `mad2xinput` input blocking/override) but with **hand-written game logic**, no libretro/no loaded core/ROM.
2. Three effects registered via `mad2effects`: `PongChallenge`, `SnakeChallenge`, `MinesweeperChallenge` — same "no auto-revert duration, stays active until explicitly cleared" contract as the companion-process effects, cleared by an in-minigame quit input (define a simple button combo) rather than a watched child process.
3. Rendering: reuse the fixed-function-quad techniques already established in `mad2graphicseffectmod` (no D3DX) for drawing simple shapes/sprites; text via `mad2textrenderer`.

**Verification:** build + deploy, trigger each effect from the HUD, confirm full input takeover, playable game loop, and clean return to normal gameplay on quit.

## Phase 6 — Per-effect controller layouts in `mad2inputdisplay`

Confirmed mapping: **GCN** for all N64 games (no N64-specific icon set available) and NES/FDS games (no NES-specific icon set available either — treat GCN as the general "non-Xbox Nintendo" fallback); **PS(2)** for both `JakChallenge` and `MarioLegacyChallenge` (Mario Legacy retains PS2 button prompts internally); **Xbox** (today's default) for everything else, including the new minigames.

1. Port the GameCube icon/draw-layout branch from `../mad2mod/src/main.cpp:2775-2874` into `mad2inputdisplay/src/inputdisplay.cpp` as a selectable layout, add a PS2-icon layout alongside it (new icon assets needed — source or hand-draw a minimal PS2 face-button/stick icon set, matching the existing GCN asset style).
2. No new shared API needed: `mad2inputdisplay` already resolves `mad2effects` lazily like every other consumer — add a local name→layout lookup table (`N64Sm64Challenge`, `NesChallenge`, `NesLostLevelsChallenge` → GCN; `JakChallenge`, `MarioLegacyChallenge` → PS2) and poll `GetCount`/`GetInfo(...).isActive` each frame (same pattern `mad2effectshud` already uses) to pick the active layout, falling back to Xbox when none of those match.
3. Config: add a per-layout position/scale override set if needed, following the existing `Config` struct conventions.

**Verification:** trigger each mapped effect in turn, confirm the HUD swaps to the right icon set and swaps back to Xbox when no mapped effect is active.

## Phase 7 — Random cheat-toggle effect in `mad2gameeffectsmod`

1. Add a new effect, e.g. `CheatShuffle`, to `mad2gameeffectsmod/src/gameeffects.cpp`, reusing the cheat-struct pointer chain and 18-entry offset table already duplicated in both `mad2cheatapply/src/cheatapply.cpp:75-95,135-154` and `mad2climbprobe/src/climbprobe.cpp` (climbprobe already proved this works mid-level, not just at the title screen — no title-screen gate needed for the new effect's write).
2. Semantics (confirmed): **fire-and-forget, re-rolled every trigger** — no snapshot/restore. `Apply` picks a random subset of the *allowed* cheats and writes `2`/`0` to each; `Clear` is a no-op (matching `ObjectShuffle`/`MangoMutate`'s existing shape).
3. Config-driven allow-list: one `config.cfg` key per cheat (default derived from `kCheats`' names) under a new `[CheatShuffle]` section, matching the existing `Weight_<EffectName>`-style per-item override convention — default most cheats to allowed (cosmetic/model-swap ones) and a few to disallowed by default (flag at execution time which ones need excluding — e.g. anything that could soft-lock progress).

**Verification:** trigger repeatedly, confirm only allowed cheats change, confirm re-triggering re-rolls rather than accumulating, confirm mid-level (not just title screen).

## Phase 8 — `mad2music` + `mad2relauncher` Windows support, and expanded music categories

**`mad2relauncher` on Windows:**
1. Add a real Windows build target for `mad2relauncher` itself (Go cross-compiles trivially; the work is splitting out platform-specific pieces via build tags): `x11.go`/`parentwatch.go`'s `PR_SET_PDEATHSIG`/X11 window-focus juggling have no Windows equivalent and get stubbed/dropped on that platform; every `syscall.SysProcAttr{Setpgid, Pdeathsig}` process-group usage (`gameloop.go`, `sm64.go` — being removed per Phase 1 —, `jak1.go`, `mariolegacy.go`, `music.go`) needs a Windows-specific `SysProcAttr` (job-object-based teardown, no `Pdeathsig`/`Setpgid` equivalent) split into its own `_windows.go`/`_linux.go` files.
2. The "launch Mad2.exe" step itself is entirely different on Windows (no gamescope/umu-run/WINEPREFIX/Proton — just a direct native launch, same shape `mad2launcher/launch.go`'s existing `runtime.GOOS == "windows"` branch already does) — `mad2relauncher`'s Windows build keeps the crash-relaunch loop and UDP-based orchestration (Jak1, Mario Legacy, music) but swaps this one piece out.
3. `mad2launcher`'s native-Windows branch should shell out to the new Windows `mad2relauncher.exe` instead of doing a bare `CreateProcess`, so music/Jak1/Mario Legacy supervision works there too (today it's genuinely bare).

**`mad2music` categorization:**
4. Expand `mad2music/src/music_pool.cpp`'s current six flat folders (`copyright`, `copyright_lyrics`, `no_copyright`, `no_copyright_lyrics`, `custom`, `custom_lyrics`) to separate the **original Mad2 soundtrack** from **community/custom pool additions**: propose `vanilla_copyright`/`vanilla_no_copyright` (the ripped original game soundtrack, split by whether it's actually copyrighted vs. licensed royalty-free) as new top-level categories alongside the existing `custom`/`custom_lyrics` (renamed if needed for symmetry) — finalize exact folder names/config keys (`[Music]` `AllowVanilla`-style flags) at execution time.

**Verification:** on native Windows, `mad2relauncher.exe` launches Mad2.exe directly, keeps `mad2music.exe` running/relaunched, and Jak1/Mario Legacy UDP orchestration still works; on Linux, no regression to existing behavior. Music: confirm pool scanning picks up the new folders and respects new allow-flags.

## Phase 9 (last, most testing required) — Pak-level randomizer + monkey-name randomizer

Sequenced last per explicit instruction — this is the biggest architectural change and needs the most validation.

1. Replace `mad2levelredirectmod`'s runtime `NtCreateFile`-interception redirect entirely with an offline step: `mad2rando5` computes the same `Slot -> Level` mapping it does today, then a tool (extend `mad2repack`, or a new `mad2rando5`-adjacent step) directly rewrites `global.bld` and/or the relevant level paks so the redirect is baked into the archive itself before the game ever launches — no more runtime file-open interception. Remove `mad2levelredirectmod`'s redirect responsibility (its second responsibility, current-level tracking via `Mad2LevelRedirect_GetCurrentLevel`, still needs a home — likely stays in a slimmed-down version of the same mod, or moves, since `mad2gameeffectsmod` **and now `mad2minigamemodefix`** depend on it via `mad2leveldetect.h`).
2. Monkey-name randomizer: confirmed straightforward — monkey names are plain strings in `global.bld`'s `ENGLISH.pak`, already hand-edited successfully before. Build this as a `mad2repack` string-pool edit (same proven mechanism as the text-resize-fix verification case in `docs/IGA_FORMAT.md`), shuffling/randomizing the monkey name strings in place. No instance-naming research needed — that's a different, unrelated open problem documented in `docs/CLASS_SCHEMA.md`.
3. Given the size of this change, test extensively per `docs/LEVEL_DIFFS.md`'s existing cross-install diffing methodology before/after each pak rewrite, across all three installs (`mad2`, `mad2russia`, `mad2demo`).
4. **`mad2minigamemodefix` compatibility (new mod, landed since this roadmap was written — see `CLAUDE.md`'s Architecture section for the full writeup):** its per-level whitelist (`Minigame_HotDurian`/`RoP_MusicalChairs`/`Soccer`/`HungryHippo`/`DivingLocation_*`, everything else forced to non-minigame) matches against whatever `mad2leveldetect.h` reports as the current level — which today is the real *content* level actually opened, not the requested slot, so it's already correct under the current runtime redirect (confirmed directly: redirecting `Soccer`'s slot to `IslandFever`'s content still correctly forces minigame mode off, matching `IslandFever`'s own content rather than the `Soccer` slot name). Whatever ends up owning current-level tracking after this phase's redirect rewrite (see item 1) needs to keep reporting real content, not the pre-redirect slot name, or this whitelist match breaks. Explicitly re-verify once the offline redirect lands: load each whitelisted minigame through a redirected slot and confirm minigame mode still activates; load a redirected story level and confirm it's still forced off.

**Verification:** full playthroughs with randomization active on all three installs, confirm no load crashes (the exact failure mode the text-resize fix was chasing), confirm redirected levels load correctly, confirm monkey names display correctly and consistently across languages/installs where applicable, confirm `mad2minigamemodefix`'s whitelist still resolves correctly under the new offline redirect (item 4 above).

---

## Open items to confirm at execution time (not blocking this roadmap)

- Exact SM64 decomp repo/branch to reference for RAM symbols (Phase 1).
- Which specific cheats in `kCheats` should default to disallowed in `CheatShuffle` (Phase 7) — needs a pass identifying any progress-breaking ones (e.g. `unlockLevels`, `debugMode`).
- Final folder/config-key names for the expanded `mad2music` categories (Phase 8).
- Whether `mad2levelredirectmod`'s current-level-tracking responsibility (Phase 9) stays in that mod or gets absorbed elsewhere once its redirect responsibility is removed — now with two consumers to keep working (`mad2gameeffectsmod` and `mad2minigamemodefix`), both via `mad2leveldetect.h`, both requiring it to keep reporting the real content level rather than the pre-redirect slot name.
