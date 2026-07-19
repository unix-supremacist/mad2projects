# Improvement Plan: Duplication & Cleanup Survey (v4 — refined)

Status: **planning only, nothing implemented yet.** `mad2cheatapply` is in scope (item 12). `mad2sm64mod`/Jak-Daxter code is now surveyed too (item 13, below) — **but another agent is actively building it right now**, so that section is pure research for awareness/future reference, not a to-do list. Nothing there should be acted on until that work is confirmed stable. The save-editor cheat-*toggle-editing* format work (item 7/8) is deprioritized — see the note below.

`deploy-plan.md` has already been updated directly (with your go-ahead) to: fix its stale claim that `mad2musichud` isn't deployed (it now is), document `mad2cheatapply` for the first time (it was entirely missing from that doc), and record the same "built but not deployed" bug now affecting `mad2cheatapply`, plus the level-detection duplication below. This document is the detailed backing for those entries.

---

## Decisions so far

| # | Topic | Decision |
|---|---|---|
| 1 | `mad2assetloader` missing saveloader's IAT self-chain guard | **Wait** — don't fix yet. |
| 2 | `mad2graphicseffectmod` `D3DPOOL_DEFAULT` targets not freed around `Reset()` | **Approved** — fix this. |
| 3 | IAT-hook DLL-API load-order problem | **Option (a)** — name the new hook-utility mod to sort first alphabetically, mirroring the existing `zz_` trick in reverse. |
| 4 | Naming for the two new libraries | **`mad2hookutil`** (IAT/vtable hooking) and **`mad2textrenderer`** (GDI text-to-texture + quad-drawing overlay helpers). |
| 5 | CMake per-mod boilerplate | **Approved** — add an `add_mad2_mod()` helper function. |
| 6 | Per-mod log files | **At least** a unified `logs/` subdirectory (each mod keeps its own file, just relocated); a single unified log file is the stretch goal if that turns out easy. |
| 7/8 | Go save-format duplication | **Deprioritized entirely, not just the format shape.** You don't care about the exact tool-to-tool format right now — the actual goal is a future full rewrite of the save-file system as plain-text JSON with explained tags, which isn't happening yet. No `dump`/`apply` work should be built against the current struct in the meantime. |
| 9 | Level-name list duplication | **Skip** — not worth doing. |
| 10 | Scope | Look at everything, including `deploy-plan.md`. |
| 11 | Commit granularity | All at once is fine. |
| 12 | `mad2cheatapply`/`mad2gameeffectsmod` level-detection duplication | **Option (b)** — extract the ~40-line detection routine into a small shared piece (preserves working-without-`mad2levelredirectmod` behavior) rather than folding it into `mad2levelredirectmod` itself. |
| 12a | `mad2xinput`'s ordinal-aware IAT variant | **Optional parameter** on `mad2hookutil`'s shared `PatchIat`, not a separate copy kept in `mad2xinput`. |
| 13 | Init-thread dedup (finding 5) / Go arg-parsing inconsistency (finding 9) | **Doesn't matter** — not worth addressing. |
| 14 | `mad2sm64mod`/Jak-Daxter scope | Now surveyed (research only — actively being built by another agent, don't act on findings yet). See item 14 below. |

---

## Item 3/4: `mad2hookutil` and `mad2textrenderer`

- **`mad2hookutil`** exports `PatchIat`/`PatchIatAllModules`/`PatchVTableSlot` as a plain `GetProcAddress`-resolved DLL API, same shape as `mad2xinput_api.h` etc. Since IAT hooking currently runs **synchronously inside `DllMain`** for every consumer (installing the `Direct3DCreate9` IAT hook immediately at load), per decision #3 the deployed filename gets a sorts-first prefix — proposing **`aa_mad2hookutil.dll`**, mirroring `zz_mad2graphicseffectmod.dll`'s sorts-last trick in reverse. I checked today's mods-folder listing: `igttimer.dll` is currently the earliest-sorting hooking mod, so `aa_` (or similar) reliably beats it and everything else, present and future. Consumers (`mad2assetloader`, `mad2saveloader`, `mad2igttimer`, `mad2playercoords`, `mad2inputdisplay`, `mad2twitchchat`, `mad2effectshud`, `mad2graphicseffectmod`, `mad2musichud`, `mad2shadowfix`, `mad2levelredirectmod`) resolve it via `GetModuleHandleA("aa_mad2hookutil")` + `GetProcAddress` directly in their own `DllMain`, same as today just pointed at a DLL instead of a local static function. Per decision #12a, `PatchIat`/`PatchIatAllModules` grow an optional ordinal-match parameter (defaulted off) so `mad2xinput` calls the same shared function instead of keeping its own copy.
- **`mad2textrenderer`** exports the GDI mask/outline text-to-texture rasterizer, `SaveState`/`RestoreState`, and `DrawOverlayQuad`/textured-quad drawing, resolved lazily from each consumer's first `EndScene` call — no load-order concerns, same pattern `mad2xinput` already uses. Consumers: `mad2igttimer`, `mad2playercoords`, `mad2inputdisplay`, `mad2twitchchat`, `mad2effectshud`. `mad2twitchchat`'s multi-run colored-text and `mad2effectshud`'s dynamic-resize behavior are real feature differences (not just copy-paste) — the shared API needs to support per-run coloring and both fixed and dynamic texture sizing, not just the lowest-common-denominator version. `mad2graphicseffectmod`'s post-process pipeline doesn't do GDI text at all, so it's not a consumer of this piece.
- Normal `-Wl,--kill-at` treatment applies to both new DLLs, same as the existing shared-API mods.

## Item 6: logs directory

Each mod keeps writing its own file, just relocated from `<gamedir>/<name>.log` to `<gamedir>/logs/<name>.log`. Mechanical change to ~17 `fopen` call sites plus the shared `modloader`/`mad2assetloader`/`mad2saveloader` logger. Worth sequencing alongside the `mad2textrenderer`/`mad2hookutil` extraction since it touches mostly the same files. The stretch goal (single unified log file) needs the thread-safety fix (only `mad2twitchchat` currently locks its log write) applied everywhere first, since a shared file means genuinely concurrent writers.

## Item 7/8: Go save-format tools — deprioritized

You don't care about the tool-to-tool duplication here for its own sake — the actual end goal is bigger: eventually reimplementing the whole Madagascar 2 save-file format as plain-text JSON with explained/documented tags (replacing the current fixed 13568-byte-struct/CRC32 approach entirely), just not right now due to time. Given that, spending effort designing a `dump`/`apply` subprocess interface against the *current* struct format would likely be thrown away once that rewrite happens. **No action on this item for now.** When you do have time for the bigger rewrite, the Unix-style "one tool owns the format, others call it" shape from the earlier discussion is still probably the right philosophy to apply — worth revisiting this section then, not before.

## Item 9: level-name lists — decided, skipping

Confirmed as 3 in-repo files holding the same list in different shapes for different purposes (rich `mad2rando5/levels.go` metadata, flat `mad2launcher/alchemy.go` dropdown list, normalized `mad2transitions/main.go` lookup map) — not pure duplication, and the game's level roster is effectively frozen, so per your call this isn't worth doing. No further action.

## Item 12: `mad2cheatapply`/`mad2gameeffectsmod` level-detection — decided, option (b)

Confirmed directly by reading the source: `mad2cheatapply/src/cheatapply.cpp`'s `UpdateCurrentLevel()`/`UpdateCurrentLevelFromScriptInfo()` are a verbatim copy of `mad2gameeffectsmod/src/gameeffects.cpp`'s functions of the same name — both try `mad2levelredirectmod`'s `Mad2LevelRedirect_GetCurrentLevel` first, then fall back to reading `ScriptInfoLib.dll`'s `_levelLoadRequest` pointer directly if that's unavailable. `mad2cheatapply`'s own comment (`cheatapply.cpp:22-24,169`) admits this is a duplicate rather than a shared call. ~40 LOC.

Per decision #12, going with **(b)**: extract the ~40-line detection routine (both the primary `mad2levelredirectmod` call and the `ScriptInfoLib` fallback) into a small shared piece that both `mad2gameeffectsmod` and `mad2cheatapply` call, preserving the "still works even if `mad2levelredirectmod` isn't installed" property. Since this routine needs no runtime shared state (each mod keeps its own local `g_CurrentLevel` cache either way — only the *detection logic* is shared), my default recommendation is a **shared header with a small `static inline` function**, compiled directly into both mods, rather than standing up a third DLL — no `GetProcAddress`/load-order concerns at all this way, and it's a much smaller unit than `mad2hookutil`/`mad2textrenderer`. Flagging this as my proposed approach rather than a fully closed decision — say so if you'd rather it be a proper third DLL API instead (e.g. if you want it to compose with `mad2levelredirectmod`'s existing exported-API convention).

---

## Findings (background reference, unchanged from v2 except item 12 above)

### C++ mods

1. **IAT hooking** (`PatchIat`/`PatchIatAllModules`, ~64 LOC) duplicated in 11 files: `mad2assetloader`, `mad2saveloader`, `mad2igttimer`, `mad2playercoords`, `mad2inputdisplay`, `mad2twitchchat`, `mad2effectshud`, `mad2graphicseffectmod`, `mad2musichud`, `mad2shadowfix`, `mad2levelredirectmod` (~700 LOC total), plus a legitimately-different ordinal-aware variant in `mad2xinput`. → `mad2hookutil` (item 3/4).
   - Bug: `mad2saveloader/src/iat_hook.cpp:42-47` has a self-chain guard `mad2assetloader/src/iat_hook.cpp` lacks (deferred per decision #1).
   - Drift: `mad2shadowfix`'s copy has one extra `Log(...)` line the other 9 lack (cosmetic).
2. **D3D9 bootstrap** (`PatchVTableSlot` + `Direct3DCreate9`→`CreateDevice`[16]→`EndScene`[42]) byte-identical across 7 mods: `mad2igttimer`, `mad2playercoords`, `mad2inputdisplay`, `mad2twitchchat`, `mad2effectshud`, `mad2graphicseffectmod`, `mad2musichud` (~210 LOC). → `mad2hookutil` (item 3/4). `mad2shadowfix` is architecturally different (full COM proxy), out of scope for this extraction.
3. **GDI text-to-texture rendering** — near-verbatim in `mad2igttimer`/`mad2playercoords`, adapted-but-same-idiom in `mad2effectshud`/`mad2twitchchat` (~350-400 LOC). **Quad-drawing/state-save boilerplate** duplicated across `mad2igttimer`, `mad2playercoords`, `mad2effectshud`, `mad2twitchchat`, `mad2inputdisplay` (~450-500 LOC). → `mad2textrenderer` (item 3/4).
   - Bug (approved to fix, decision #2): `mad2graphicseffectmod`'s ping-pong/echo/low-fps render targets are `D3DUSAGE_RENDERTARGET | D3DPOOL_DEFAULT` and are only recreated on dimension change, never on `Reset()` itself — plausible cause of alt-tab/resolution-change failures (including the game's own `Reset()` failing with `D3DERR_DEVICENOTRESET`) while an effect is active.
4. **Logging** — `modloader`/`mad2assetloader`/`mad2saveloader` share one identical `log.cpp` (fine, deliberate). ~15 other mods each inline the same ~14-line logger (~210 LOC duplicated). **`mad2twitchchat` is the only one with a mutex around the log write** — the other 14 have a check-then-act race on lazy `fopen` init and no protection against interleaved writes from background threads. → relevant to item 6's stretch goal.
5. **Init-only dependency-wait thread** — `mad2inputeffectsmod`, `mad2gameeffectsmod`, `mad2podcastmod` each inline the same retry loop (~60 LOC total duplicated). **Decided: doesn't matter, not addressing (decision #13).**
6. **CMakeLists.txt** — 21 per-mod files, ~260-280 LOC of identical `add_library`/`set_target_properties`/link-options boilerplate → `add_mad2_mod()` (item 5). This same missing-single-source-of-truth problem is what let both the `mad2musichud` (now fixed) and `mad2cheatapply` (still broken) deploy-target omissions happen — see `deploy-plan.md` §5.
7. **`mad2cheatapply`/`mad2gameeffectsmod` current-level detection** — ~40 LOC, self-admitted duplicate. See item 12 above.

### Go tools

8. **Save-file struct/CRC logic** — real, self-admitted duplication (~170 LOC) across `mad2save`/`mad2savegui`/`mad2launcher`'s `saveeditor.go`. CLAUDE.md's description of `mad2launcher`'s version as an independent reimplementation is inaccurate. **Deprioritized — see item 7/8.**
9. **`mad2arc`/`mad2repack`/`mad2tool`** — `mad2tool`'s `ArchMeta`/`ArchFileMeta` admittedly duplicate `mad2repack`'s `ArchiveMeta`/`ArchiveFileMeta` (~12 LOC); `mad2tool` repeats the same `filepath.Walk` skeleton 4x in one file (~40 LOC); flag-parsing style is inconsistent (stdlib `flag` vs. hand-rolled). **Decided: doesn't matter, not addressing (decision #13).**
10. **Level-name lists** — decided skip, see item 9 above.
11. **Misc** — all 9 Go modules pin the same Go version (no skew); `mad2savegui`/`mad2launcher` have identical Fyne `require` blocks (expected without a `go.work`); `mad2transitions` has a small (~15 LOC) independent reimplementation of a `mad2iga.PakMeta` subset, low severity/deliberate.

---

## Item 14: `mad2sm64mod`/Jak-Daxter + `mad2relauncher` — research only, in-flux

You said I could look at this now, with an explicit caveat that another agent is actively building it — **everything in this section is pure observation, nothing here is proposed for action**, since the code may be mid-change and any dedup work now would likely conflict with that work. Revisit once that effort is confirmed stable.

**A likely-real bug, not just duplication, worth surfacing regardless:** `mad2sm64mod.dll` compiles two source files, `sm64mod.cpp` and `jak1mod.cpp`, and **each has its own independent `static FILE* g_LogFile` + `Log()`, both targeting the same file name `sm64mod.log`** — `sm64mod.cpp:72` opens it `"w"` (truncate), `jak1mod.cpp:70` opens it `"a"` (append), each lazily on first use, with no coordination between the two. Depending on which half logs first, this can truncate output the other half already wrote, and the two independent `FILE*` handles writing the same underlying file from what may be different threads (each effect has its own streaming/watcher threads) is a real interleaving hazard — this is the "two copies of the same duplication colliding within one binary" version of finding 4 above, and unlike the cross-DLL cases, it can actually corrupt output today. Worth a heads-up to whoever's building this now, even though it's out of scope for this plan to fix directly.

**Structural duplication observed** (for future reference once the Jak-Daxter work stabilizes):
- `sm64mod.cpp` (636 lines) and `jak1mod.cpp` (502 lines) share near-identical scaffolding: `Log()`, `LoadConfig()`'s shape, `IsRunningUnderWine()`, the `g_Active`/`g_Seq`/socket-address globals, `ApplyInputBlock`/`RemoveInputBlock`, `EnsureWinsock`/`SetupSockets`, `SendControl`, `ReadEffectiveOverriddenState`, `StreamThreadFunc`/`StartStreaming`/`StopStreaming`, a watcher thread that self-clears the effect on process exit, matching `Apply*Challenge`/`Clear*Challenge` WINAPI callbacks, and a `TestTriggerThreadFunc`. The genuine differences are the wire protocol (`Mad2Sm64` vs `Mad2Jak1` packet types, plus Jak's PS2-pad byte-packing `XInputToPs2Pad`) and platform-launch specifics (SM64 has a native-Windows `CreateProcess` path with window-focus juggling; Jak is Wine-only per its own source, relauncher-only). This is a strong "extract a shared companion-process chaos-effect framework" candidate — essentially the same shape as the `mad2hookutil`/`mad2textrenderer` extractions above, just for this third pattern — but only once there's a second stable consumer to generalize from instead of guessing at the right seams mid-development.
- The same parallel exists on the `mad2relauncher` side: `sm64.go`'s `Sm64Controller` and `jak1.go`'s `Jak1Controller` share the same `New*Controller`/`Serve*`/`handleLaunch`/`handleStop`/`sendExited`/`StopAll` shape (`jak1.go` additionally has `ServeInput`/`writeInputFileAtomic` since OpenGOAL's controller backend reads input from a file rather than a socket — a genuine protocol difference, not just copy-paste). Same "revisit once stable" caveat applies.

---

## Still open

- Whether item 12's level-detection extraction should be a shared header (my recommendation) or a proper third DLL API — say so if you want the DLL route instead.
- The `mad2sm64mod`/`sm64mod.log` truncate-vs-append collision (item 14) — flagging for awareness; not fixing here since that code is actively being worked on elsewhere.
