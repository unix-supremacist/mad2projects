# Open items

Written 2026-07-21 as the tail end of a completion audit of `changes.md` and
`improvement-plan.md` (both now deleted — every item in them was either
confirmed done in current source, explicitly deprioritized/skipped by an
earlier decision, or migrated here). `deploy-plan.md` was left as-is — it
already tracks its own open items in its §5 "Known Gaps & Open TODOs" section
in this same style, so nothing from it is duplicated below.

This file is a running list, not a log — delete an entry once it's done
rather than striking it through.

## 1. Music track display names don't use the downloader's `song_names.txt` manifest

**What:** `mad2music/downloader`'s `updateSongManifest` (`mad2music/downloader/download_music.go:99-115`,
`songManifestName = "song_names.txt"`) generates a stable `songNN` name for
every downloaded track per audio-category directory (`catDir/song_names.txt`,
plain `actual filename=songNN` lines) specifically so `mad2musichud`'s
now-playing overlay doesn't show real filenames (yt-dlp video titles —
spoiler-y and often too long for the HUD).

**Why it's not done:** the consumer side was never wired up. `mad2music/src/main.cpp:39-43`'s
`CopyTrackName` (called at lines 66-67 to populate the status-broadcast
packet `mad2musichud` reads over loopback UDP) derives the display name
purely from `std::filesystem::path(path).stem()` — the raw filename stem —
and never reads `song_names.txt` at all.

**Where to pick this up:** `mad2music/src/main.cpp`'s `CopyTrackName` (or its
call sites) needs to load/cache each audio directory's `song_names.txt` and
look up the real filename to get its `songNN` label, falling back to the
stem if no manifest entry exists (e.g. a manually-added track).
`mad2musichud/src/musichud.cpp` just displays whatever string arrives in the
packet — no changes needed there once `main.cpp` resolves the right name.

## 2. No keyboard-equivalent input modifiers

**What:** every controller-input-scrambling chaos effect in
`mad2inputeffectsmod` (`JoystickReversal`, `SwapAB`, `SwapXY`,
`StickDirectionDisable`, the phantom-button-press effects, `StickPercentLimit`)
should also apply an equivalent modification to keyboard input, since Mad2
supports keyboard controls too — right now these effects are invisible to a
keyboard player.

**Status:** not started at all — confirmed via grep across
`mad2inputeffectsmod/src/*.cpp` and `mad2xinput/src/*.cpp`: no keyboard
hook/injection code (`SetWindowsHookEx`, `GetAsyncKeyState` interception,
`keybd_event`, `VkKeyScan`, etc.) exists anywhere in either mod.

**Where to pick this up:** `mad2inputeffectsmod/src/*.cpp` owns all the
effect apply/clear callbacks today. `mad2xinput`'s layered-override system
(`mad2xinput_api.h`) is the existing design precedent for controller state,
but there's no keyboard equivalent shared-API mod yet — this would need a new
hook (likely a low-level keyboard hook, `WH_KEYBOARD_LL`, since Mad2 is
always the foreground app while playing) plus a design decision for how each
effect's controller-side behavior maps to keyboard terms (e.g. which
WASD/arrow keys stand in for which stick axis for `JoystickReversal`).

## 3. Companion-process chaos-effect framework extraction — deliberately deferred

**What:** `mad2companionmod`'s three source files (`sm64mod.cpp`,
`jak1mod.cpp`, `mariolegacymod.cpp`) share a large amount of near-identical
scaffolding — `LoadConfig()`'s shape, `IsRunningUnderWine()`, the
`g_Active`/`g_Seq`/socket-address globals, `ApplyInputBlock`/`RemoveInputBlock`,
`EnsureWinsock`/`SetupSockets`, `SendControl`, `StreamThreadFunc`/
`StartStreaming`/`StopStreaming`, watcher-thread self-clear logic, and
matching `Apply*Challenge`/`Clear*Challenge` callback shapes — a strong
candidate for the same kind of shared-DLL extraction `mad2hookutil`/
`mad2textrenderer` already did for hooking/text-rendering. The
`mad2relauncher` side (`sm64.go`/`jak1.go`/`mariolegacy.go`'s `*Controller`
types) has the same parallel shape.

**Status:** explicitly deferred by the user (2026-07-21) — do not extract
now. `mad2companionmod` is fully stable (all three effects — SM64, Jak1,
Mario Legacy — verified working end-to-end, native-Windows branches added for
two of the three), so the original "revisit once stable" condition from
`improvement-plan.md` has technically been met, but the call was made to
leave this alone for now rather than churn stable code.

**Where to pick this up:** `mad2companionmod/src/{sm64mod,jak1mod,mariolegacymod}.cpp`
and `mad2relauncher/{sm64,jak1,mariolegacy}.go`, whenever it's revisited.

## Investigated, closed with no action taken

- **"Trim unnecessary `just` recipes" (was `changes.md` item 19):**
  cross-referenced every recipe in the justfile against `tools/package.sh`
  (which calls nearly every `build-*` recipe directly), `mad2launcher/build_native.go`'s
  `Resolve*`/`findPrebuilt` candidate paths (which only ever look for these
  recipes' *output* — never shell out to build anything themselves, per the
  earlier tool-building-tool fix), and `CLAUDE.md`'s documented build
  commands. Found no recipe that's dead or purely redundant with something
  `mad2launcher` now does — every recipe is either a real `package.sh` build
  step, the sole producer of a binary some `Resolve*` function looks for, or
  a CLI-only diagnostic (`regedit`/`shell` variants) with no GUI equivalent.
  If anything, the earlier fix (the launcher never builds anything itself)
  makes every `build-*` recipe *more* necessary, not less. No recipes were
  removed — none were found safe to remove under the "verify it's really
  dead before deleting" bar.
