# JakChallenge + jak-project vendoring + SM64 controller fix — done

## Round 10: native-Windows support for mad2music, JakChallenge (investigated,
not tractable), MarioLegacyChallenge (done)

Picked up Round 9's "Not done" item: native-Windows paths for the three
things that currently lean on `mad2relauncher`, which the user does not
want ported (it wraps `umu-run`/gamescope/X11, Linux-only, not worth
reworking). Worked strictly in priority order 1/2/3 as asked; all three
were reached this round.

**1. `mad2music` on native Windows — done, verified via a real cross-build.**
Read every file under `mad2music/src/`: the only Linux-specific surface is
POSIX sockets (`arpa/inet.h`/`netinet/in.h`/`sys/socket.h`/`unistd.h`),
`getpid()`, `localtime_r`, and `popen`/`pclose` (piping `ffmpeg` for PCM
decode) -- a small, well-scoped port, not a rewrite. Isolated the shim in a
new `src/platform_compat.h` (`PlatformSocketInit`/`Cleanup`/`CloseSocket`/
`GetPid`, POSIX vs. `_WIN32` branches) rather than scattering `#ifdef`s
through `main.cpp`; `log.cpp` got a `localtime_s` branch, `audio_engine.cpp`
got `_popen`/`_pclose`. Also had to fix two latent `sendto`/`recvfrom` calls
passing typed pointers directly (`&pkt`) instead of `char*` -- compiles on
Linux (accepts `void*`) but Windows' Winsock prototypes are strict
`char*`/`const char*`; fixed with explicit casts, harmless on both
platforms.

`CMakeLists.txt` needed no structural change -- `pkg_check_modules(SDL3
REQUIRED sdl3)` works unmodified when cross-compiling too, since the
official `SDL3-devel-*-mingw` release's `sdl3.pc` has a self-relative
`prefix` (`${pcfiledir}/../..`); just point `PKG_CONFIG_LIBDIR` at its
`i686-w64-mingw32/lib/pkgconfig` before invoking cmake. Added `ws2_32` link
(Winsock, not pulled in by SDL3's own pkg-config Libs) and `-mwindows`
(no console window -- everything already goes to `mad2music.log`, not
stdout) gated on `WIN32`. New `just build-music-windows` recipe, modeled on
`build-sm64-windows`'s "fetch a prebuilt mingw devel package, no distro
package for this host" shape (`extern/.sdl3-mingw`, pinned
`sdl3_mingw_version := "3.4.12"`) -- **actually ran it**: clean configure,
clean build, produced `build/windows/mad2music.exe` (`file`: "PE32
executable ... (GUI), Intel i386") + copied `SDL3.dll` alongside it (SDL3's
mingw devel package ships no static lib, dynamic-only). Also reconfirmed
the native Linux build (`build-music-linux`) still builds clean after the
shared-file edits.

Since there's no `mad2relauncher` on native Windows to start/supervise
`mad2music` for the session, added the launch step to `mad2podcastmod`
(already has its own init thread, per its existing "no D3D9 hook" shape):
`mad2music/src/main.cpp`'s `main()` now creates a named mutex
(`Mad2Music_SingleInstanceMutex`) on Windows, held for its whole process
lifetime (never closed) -- both enforcing real single-instance behavior
(a second instance would otherwise fight the first for the same UDP
ports) and giving `mad2podcastmod` a cheap `OpenMutexA` check.
`podcastmod.cpp`'s `InitThreadFunc` now calls
`StartMad2MusicIfNeeded_NativeWindows()` (gated on `!IsRunningUnderWine()`,
same platform check duplicated from `sm64mod.cpp`'s precedent) --
`CreateProcess`'s `mad2music.exe` once via a configurable `[Podcast]`
`WindowsMusicPath`/`WindowsAudioDir` (both relative to the game exe dir,
default `mad2music\mad2music.exe` / `mad2music\audio` -- no deploy target
wires this up automatically yet, a manual placement step, same status as
`mad2companionmod`'s existing `WindowsXxxPath` keys) and lets it run
unsupervised for the session (no crash-relaunch -- nothing on native
Windows provides that). Compiled clean via the real root MinGW cross-build
(`cmake --build build --target mad2podcastmod`, then a full `just build`
confirming nothing else broke). **Not verified**: never actually run on a
real Windows machine end-to-end (no such machine available here) -- the
cross-build succeeding and the logic being a direct structural mirror of
`sm64mod.cpp`'s already-proven native-Windows branch is the extent of the
verification possible from this environment.

**2. `JakChallenge` on native Windows — investigated, not tractable here,
left alone.** Checked upstream `open-goal/jak-project`'s actual README and
CI config (not assumed): the README states "At the moment we support
**x86_64** on Windows, Linux and macOS", and `.github/workflows/
build-matrix.yaml` has dedicated `windows-build-clang`/`windows-build-msvc`
jobs. So a Windows build genuinely exists upstream -- but
`docs/setup/system/windows.md` confirms it's Visual-Studio/MSVC-SDK-based
(or Windows-native clang), **no MinGW cross-compile path at all**, unlike
every other Windows target in this repo (`sm64ex-alo`, now `mad2music`,
both cross-compiled from Linux via plain `i686-w64-mingw32`). That's a
fundamentally different toolchain requiring an actual Windows build
machine with Visual Studio installed -- not something this Linux
cross-compile sandbox can attempt, let alone verify with a real build.
Per the task's own "don't force it" standard (same bar as the Mario Legacy
investigation), left `jak1mod.cpp`'s `ApplyJakChallenge` exactly as it was
(explicit no-op + log line on real Windows) and did not touch it further.
Recommendation if picked up later: needs a real Windows machine (or a CI
runner) with MSVC to attempt a build at all; worth revisiting once/if one
becomes available, since the actual C++ integration work
(`CreateProcess`+window-focus+watcher-thread) would otherwise mirror
`sm64mod.cpp`'s existing native branch closely.

**3. `MarioLegacyChallenge` on native Windows — done, integration verified;
full runtime not tested (no Windows machine).** Checked
`OpenGOAL-Mods/Super-Mario-Legacy`'s GitHub releases API directly for tag
v0.0.28 (the exact tag already vendored) -- confirmed it ships a
`windows-v0.0.28.zip` asset alongside `linux-v0.0.28.tar.gz`, containing
`gk.exe`/`goalc.exe`/`extractor.exe` at the top level plus the same `data/`
tree. Verified the `data/` trees are actually identical before assuming it
(not just "probably the same release"): same file count (3798) in both
archives, and a spot-checked file's MD5 matches byte-for-byte across
platforms. So vendoring was genuinely just the three `.exe` files, not a
second `data/` copy -- extracted directly into the existing
`extern/mario-legacy/` alongside the Linux binaries.

Added `mariolegacymod.cpp`'s native-Windows branch mirroring
`sm64mod.cpp`'s shape (`IsRunningUnderWine()` gate, `LaunchWindowsMarioLegacy`
+ `CreateProcessA`, `FocusMarioLegacyWindowThreadFunc` via `FindWindowA`
against gk's own "OpenGOAL" window title, `WatcherThreadFunc_Native` via
`WaitForSingleObject` on the process handle, `ApplyMarioLegacyChallenge`/
`ClearMarioLegacyChallenge` branching on the same `wine` bool) -- new
config key `MarioLegacy_WindowsPath` (default `mario-legacy\gk.exe`). One
real difference from SM64's native branch that needed actual new code, not
just a copy-paste: `gk` (like Jak1's) has no socket primitive at all, so
where SM64's native branch streams UDP straight to the child process, this
one's `StreamThreadFunc` branches -- Wine still sends UDP to
`mad2relauncher`, but the native-Windows path calls a new
`WriteInputFileAtomic`, atomically overwriting `<gk.exe dir>\data\
mad2_input.dat` (write-to-`.tmp`-then-`MoveFileExA`-rename) with the exact
same packed struct bytes `mad2relauncher/mariolegacy.go`'s
`writeInputFileAtomic` writes on the Wine side -- the same GOAL-side
`pad.gc` patch polls this file every frame regardless of which side of the
wire produced it, so no GOAL/goalc recompile was needed for the Windows
path. Deliberately did **not** wire up `-level`/a checkpoint list for the
Windows launch args (`--game jak1 --config-path <dir>\saves_chaos --
-boot -fakeiso`, no `-level`) -- per Round 9's already-documented gap, this
fork has no title-screen-skip patch, so a checkpoint argument would be a
no-op on this platform exactly as it already is on the Wine branch; adding
one would just be dead complexity.

Compiled clean via the real root MinGW cross-build (`cmake --build build
--target mad2companionmod`, then a full `just build` confirming the whole
tree, all three companion effects included, still builds). **Not
verified**: no real Windows machine to actually run `gk.exe`, confirm the
window title match, or confirm controller input reaches the game through
the new file-write path end-to-end -- the verification available from this
environment is the vendored binaries' provenance (release API + checksum
match) and the C++ side compiling clean and structurally mirroring
`sm64mod.cpp`'s already-proven pattern.

Updated `CLAUDE.md` throughout (mad2music's Windows build + single-instance
mutex, mad2podcastmod's native-Windows startup step, JakChallenge's
investigated-and-declined Windows path, MarioLegacyChallenge's new native
branch, extern/mario-legacy's vendored Windows binaries) to reflect all of
the above.

## Round 9: MarioLegacyChallenge added (third companion process, extern/mario-legacy)

Vendored `extern/mario-legacy` (OpenGOAL-Mods/Super-Mario-Legacy release
v0.0.28 -- checked `~/src/jak1nolauncher/mods.json`'s `sm64-1` key first,
confirmed v0.0.28 is still current) as a third companion-process chaos
effect, `MarioLegacyChallenge`, alongside `SM64Challenge`/`JakChallenge` in
`mad2companionmod.dll`. This fork is a **complete separate** jak-project
checkout (own prebuilt `gk`/`goalc`/`extractor`, own `goal_src`, `libsm64`
statically linked into `gk` -- confirmed via `strings gk`) -- confirmed this
session it genuinely cannot be layered onto our own `extern/jak-project`,
it needs its own tree, exactly as a prior investigation this session had
already found.

**Setup, no C++ build step needed**: extracted the release tarball into
`extern/mario-legacy/` (prebuilt Linux binaries, `data/` subdirectory
holding `goal_src`/assets -- a real layout difference from
`extern/jak-project`'s top-level `goal_src`, found live not assumed).
Symlinked in the same two assets this repo's other two forks already need
rather than asking the user for a third copy: `jak1.iso` (same ISO
`extern/jak-project/jak1.iso` points at) and `baserom.us.z64` (same ROM
`extern/sm64ex-alo/baserom.us.z64` points at -- confirmed via `strings gk`
that `libsm64` auto-detects a ROM either next to the `gk` binary or in
`iso_data/mario`, and "next to gk" is satisfied by this symlink since `gk`
sits at this repo's root). Ran this fork's own `extractor -e -d -g jak1
jak1.iso` (32.56s) and `goalc -c "(mi)" -g jak1` (1316 targets, clean) --
new `just setup-mario-legacy` recipe wraps both, idempotent (skips
extraction if `data/decompiler_out/` already exists), mirroring
`build-jak1-linux`'s shape but without any C++ compile step.

**Controller injection -- ported and verified live, not just diffed.**
Diffed this fork's `goal_src/jak1/engine/ps2/pad.gc` against
`extern/jak-project`'s own (which already carries our hand-written
injection patch) and found them **byte-identical apart from that exact
patch** -- Mario's moveset (`mario.gc` reads buttons via
`cpad-hold?`/`cpad-pressed?` against the same `x`/`square`/`circle`/`l1`/
`l2`/`r1`/`l3` symbols, i.e. the same PS2 pad-buttons bitfield every stock
Jak1 target reads) needed no new wire format or C++ translation logic --
confirmed by reading `mario.gc`'s own button-bitmask comment and grepping
its `cpad-*` calls directly. Applied the identical patch shape (new
`mad2-mariolegacy-input-packet` GOAL type + `service-cpads` polling block,
distinct magic `MAD2_MARIOLEGACY_INPUT_MAGIC` from Jak1's) to this fork's
`pad.gc`, recompiled clean (1316 targets, then 552 on the incremental
rebuild), and **verified live end-to-end**: added a temporary
`(format 0 ...)` debug line in the injection branch, crafted a 14-byte
`mad2_input.dat` by hand (`button0=0x4000`, `lx=128 ly=200 rx=128 ry=128`),
placed it at `data/mad2_input.dat`, ran `gk` standalone for 15s -- log showed
842 identical `MAD2-DEBUG-MARIO-INJECT btn=4000 lx=128 ly=200 rx=128
ry=128` lines (one per frame, exact values matching the crafted file).
Removed the debug line and rebuilt clean afterward.

**mad2relauncher**: new `mariolegacy.go`, `MarioLegacyController` --
`Jak1Controller`'s exact twin (same `New*`/`Serve*`/`handleLaunch`/
`handleStop`/`sendExited`/`StopAll` shape), own loopback UDP ports (47068
input / 47069 control -- next free after Jak1's 47066/47067), reusing
`jak1.go`'s own `jak1Checkpoints` list directly (same jak1 level/continue-
point set, confirmed this fork doesn't rename or restructure it). One real,
live-found difference from Jak1Controller: this fork's GOAL file-stream
opens resolve relative to `<repo root>/data/`, not the repo root itself
(confirmed by running `gk` with cwd=repo root and watching it try
`.../mario-legacy/data/mad2_input.dat`, not `.../mario-legacy/mad2_input.dat`)
-- so `inputPath` has an extra `"data"` path component Jak1Controller's
doesn't. Wired into `main.go` (`-mariolegacy-dir`/`-mariolegacy-input-port`/
`-mariolegacy-control-port` flags, `StopAll` on shutdown) and `justfile`'s
`_play` (`mariolegacy_flag`, soft-disabled with a warning if
`extern/mario-legacy/gk` isn't set up yet -- same pattern as SM64/Jak1).

**mad2companionmod**: new `src/mariolegacymod.cpp` + `include/
mad2mariolegacy_protocol.h`, registering `MarioLegacyChallenge` alongside
the other two effects from the same shared init thread
(`sm64mod.cpp`'s `InitThreadFunc` now also calls
`MarioLegacy_RegisterEffect()`; `DLL_PROCESS_DETACH` also calls
`MarioLegacy_Shutdown()` -- both declared in `jak1_init.h` alongside the
existing `Jak1_*` pair). Same "no auto-revert duration, self-clearing
watcher thread" shape, same raw-`Mad2XInput_GetState`-not-
`GetOverriddenState` fix Round 8 found for SM64/Jak1 (applied here from the
start, not re-discovered). `XInputToPs2Pad` is duplicated (not shared)
between `jak1mod.cpp` and `mariolegacymod.cpp` -- the former's copy is
`static`/file-local and the two packet struct types are distinct nominal
types despite identical layout, so a literal shared symbol wasn't an option
without more header surgery than this shim needed; matches this DLL's
existing SM64/Jak1 precedent of duplicating small platform shims rather
than factoring them out. Added `MarioLegacy_*`-prefixed keys to the shared
`[Companion]` config section (`UserIndex`, `BroadcastToAllControllers`,
`Weight`, `InputPort`, `ControlPort`, `TestTriggerKey` default `VK_F12`,
next free after SM64's F10/Jak1's F11). `mad2companionmod.dll` builds clean
(`cmake --build build --target mad2companionmod`, 3 translation units).

**Verified live, full stack, via the real binaries (not synthetic
unit tests)**: built the real `mad2relauncher` binary with `-mariolegacy-dir`,
ran it standalone against a dummy exe, sent a raw UDP LAUNCH packet on port
47069 -- log shows `launched Mario Legacy: .../extern/mario-legacy/gk
(pid=..., checkpoint=firecanyon-start)` followed by the X11 focus/resize
lines (`resized "OpenGOAL" window ... to 1280x720`, `focused "OpenGOAL"
window`). Sent a raw UDP input packet on port 47068 -- confirmed
`data/mad2_input.dat` was written atomically with the exact expected 14
bytes. Sent a STOP control packet on 47069 -- confirmed the real `gk`
process was gone (`pgrep` came up empty), the input file was cleaned up,
and the log shows `Mario Legacy exited: signal: terminated`.

**Known gap, flagged clearly, not fixed this round**: unlike
`extern/jak-project`, this fork's `game-info.gc`/`level.gc`/
`kernel-defs.gc` do **not** carry the title-screen-skip/
`*kernel-boot-level*`/auto-exit-on-power-cell patches Round 6 ported into
`extern/jak-project` -- diffed live, genuinely absent upstream in this
fork (confirmed `kernel-defs.gc` has no `*kernel-boot-level*` symbol at
all). So `mad2relauncher`'s `-level <checkpoint>` flag currently has **no
effect** here: `MarioLegacyChallenge` boots to this fork's own title
screen rather than jumping straight into a checkpoint the way
`JakChallenge` does, and has no auto-exit-on-power-cell behavior either --
it only self-clears when the player manually quits `gk`, like
`SM64Challenge`. Porting the equivalent patch into this fork's
differently-structured `game-info.gc` (it already has its own
libsm64-specific hooks threaded through the same functions -- see its diff
against `extern/jak-project`'s version) is a real follow-up, not attempted
this round; flagging so it isn't mistaken for already having Jak1's
polish.

**Not done**: Priority 2 (native-Windows support for `mad2music`,
`JakChallenge`, `MarioLegacyChallenge`) was not attempted this round --
Priority 1 (Mario Legacy) consumed the full session. See the requesting
task for the concrete specs if picked up later.

Also updated `CLAUDE.md` (`extern/mario-legacy` directory bullet,
`mad2companionmod`'s three-effects description, `mad2relauncher`'s job
list, `just setup-mario-legacy`) to reflect all of the above.

## Round 8: flashing-triangle root cause (window hide, not unmap), input pipeline verified live end-to-end, SM64/Jak1 self-blocking-input bug found, rename to mad2companionmod + config merge

**Flashing triangles -- root cause found and fixed, confirmed via X11 state, not screenshots** (screenshots are off-limits per this repo's own convention, and no working screenshot tool was available anyway in this environment). Reproduced live: launched real Mad2.exe via `just _play`, waited for full boot, then triggered JakChallenge with a synthetic XTest F11 keypress against gamescope's nested display. `x11.go`'s `FocusWindowAndHideMad2`/`RestoreMad2` only ever moved Mad2's window off-screen (`ConfigureWindow` to 10000,10000) -- it was never actually unmapped. Direct X11 `GetWindowAttributes` queries before/after confirmed Mad2's window stayed `Viewable` (just repositioned) the whole time Jak1 ran alongside it: gamescope is a Wayland compositor that composites app *surfaces*, not raw X11 window geometry, and has its own documented history of getting confused about which of several simultaneously-mapped windows to display (ValveSoftware/gamescope#437). Two live, mapped, continuously-rendering windows (Mad2's D3D9 + gk's OpenGL) sitting in one gamescope session is a very plausible match for "flashing triangles" -- confirmed gk's own rendering is clean in isolation (normal OpenGL 4.6/AMD context init, no GL errors, same one-off benign texture-fallback warning as every prior round) even while Mad2 was simultaneously running, ruling out corruption in gk's own pipeline. Fix: `FocusWindowAndHideMad2` now actually `UnmapWindow`s Mad2 (keeping the off-screen move as harmless defense-in-depth), `RestoreMad2` `MapWindow`s it back plus a `sync()` round-trip before `raiseAndFocus` (a `SetInputFocus`-before-map race was observed once). Confirmed live both directions: window genuinely flips `Viewable` -> `Unmapped` on hide and back to `Viewable` on restore. **Not independently re-confirmed that the visual flashing itself is gone** (no screenshot capability here) -- ask the user to confirm in a real session.

**Input pipeline -- the real bug was upstream of everything previously checked, found and fixed, confirmed live end-to-end**: per Round 7's open question, did a full live test this time. Standalone `gk` + a temporary `service-cpads` debug log confirmed the file-read/GOAL-side half of the pipeline was already correct (confirmed `mad2_input.dat`'s path resolution, atomic write/rename, and parsing all work, both at startup and dynamically mid-session). Then drove the *real* `mad2relauncher` binary with synthetic UDP packets matching the exact wire protocol (LAUNCH control packet + input packets) -- it forked the real `gk`, and the input packets correctly reached `mad2_input.dat` and were read every frame. So the file-bridge mechanism was never the problem. The actual bug: `jak1mod.cpp` (and identically, `sm64mod.cpp`'s `SM64Challenge` -- changes.md item 13, "sm64 fails to read controller inputs", same root cause) read `Mad2XInput_GetOverriddenState` to get "post-override" state to stream to the companion game -- but `ApplyInputBlock()` installs its *own* `AddOverride` layer on the same controller (forceBlockButtons=0xFFFF, sticks forced to dead-center) specifically to stop Mad2 itself from reacting to the player's real input, and `GetOverriddenState` returns state with *every* active layer applied, including that self-installed block. So both effects were always reading back their own just-applied zero, every poll, regardless of what the controller was actually doing -- explaining "controller inputs not working" completely, for both SM64Challenge and JakChallenge, and explaining why nothing found in Round 7's static review looked wrong (the file-bridge code genuinely was fine). Fixed both `sm64mod.cpp` and `jak1mod.cpp` to read `Mad2XInput_GetState` (raw, pre-override, immune to anyone's layer) instead. Trade-off, flagged in both files' comments: other mods' XInput effects (JoystickReversal, SwapAB, ...) no longer compose into what SM64/Jak1 see -- recovering that would need `mad2xinput.dll` to grow an "exclude this handle" variant of `GetOverriddenState`, not done here (out of scope, another agent owns that DLL).

**Renamed `mad2sm64mod` -> `mad2companionmod`** (changes.md item 10) now that it registers both SM64Challenge and JakChallenge: directory, CMakeLists.txt target/`OUTPUT_NAME`/deploy filename, root `CMakeLists.txt`'s `add_subdirectory`/deploy-copy/`DEPENDS` list, `justfile` comment references, `CLAUDE.md`. Also merged the two effects' config: both used to have their own `[SM64]`/`[Jak1]` sections in `mad2config.dll`'s `config.cfg`; now share one `[Companion]` section with `SM64_*`/`Jak1_*`-prefixed keys (per the user's explicit ask). Fixed the log-file race flagged in improvement-plan.md item 14: `sm64mod.cpp` and `jak1mod.cpp` each had their own independent `static FILE*`/`Log()` targeting the same filename (one `"w"` truncate, one `"a"` append) -- now one mutex-guarded `FILE*` owned by `sm64mod.cpp`, exposed to `jak1mod.cpp` via a new `Companion_LogV` (declared in `jak1_init.h`). Confirmed compiling clean (`just build`, full tree) and confirmed live: a real session's `mad2companionmod.log` shows both effects' `LoadConfig`/`Register` lines interleaved correctly from the shared file, sourced from the new `[Companion]` section.

**Known remaining hazard in this environment, not this repo's bug**: `build/` is shared across concurrent agents' sessions and got wiped by someone else's `just clean`/reconfigure mid-session at least twice, taking `build/mad2relauncher`/`build/mad2music` down with it (those are plain `cp`/`go build` outputs into the same directory CMake owns). Not fixed here since it's an artifact of this multi-agent environment, not a real bug -- flagging in case future rounds hit the same "file vanished under me" confusion.

## Round 7: mad2music teardown bug found via a real log, Jak1 music muted via the user's own suggested approach

User reported three things after a real play session: mad2music not exiting
when gamescope closes, gamescope occasionally hanging, and (separately)
Jak1 having buggy graphics, non-working controller input, and wanting its
own music muted.

**mad2music/teardown bug -- root cause found, not guessed**: read the
actual `mad2/mad2relauncher.log` + `mad2/mad2music.log` from the user's last
session. Sequence: `19:48:41` `X11 connection closed -- stop-key grab
ending` logged, then **nothing else** from `mad2relauncher` ever again --
but `mad2music.log` shows it starting a *new* track at `19:48:43`, two
seconds later, and process list confirmed nothing was running minutes
after. Root cause: `x11.go`'s `GrabStopKey` already detects the nested X11
connection closing (a strong signal gamescope died or tore down its
display) but the code just logged it and returned -- never called `stopFn`.
So unless gamescope's own process death happened to independently trigger
`armParentDeathSignal`'s `PR_SET_PDEATHSIG` (which apparently didn't fire in
this session, e.g. if gamescope hung rather than truly exiting), nothing
ever told `mad2relauncher` to tear down, so `mad2music` (and the game) kept
running. Fixed: `ev == nil` (connection closed) now calls `stopFn()`, same
as Ctrl+Q.

**Defense-in-depth for the same bug class**: even with the above fix, if
`mad2relauncher` itself is ever killed outright (`SIGKILL`, no chance to run
its own `Stop()`/`StopAll()` cleanup), its children were previously left to
be reparented and keep running. Added `Pdeathsig: syscall.SIGTERM` to every
child's `SysProcAttr` (`gameloop.go`, `music.go`, `sm64.go`, `jak1.go`) --
kernel-level guarantee each child dies the instant `mad2relauncher` does,
independent of any userspace signal handling. Confirmed `go build` clean.

**"gamescope hangs" -- likely the same root cause, not independently
confirmed**: the X11-connection-close fix above directly explains a
session where gamescope's nested display goes away (crash, hang, whatever)
without its own process necessarily dying cleanly enough for
`PR_SET_PDEATHSIG` to fire. Didn't get a separate repro/description of what
"hangs" means beyond that (visually frozen window vs. unkillable process
vs. next launch failing) -- flagged for the user to confirm whether this
round's fix actually covers it or if there's a distinct remaining case.

**Investigated "buggy graphics" and "controller inputs not working"
directly, live**: this environment unexpectedly has a real GPU + X11
display, so ran `extern/jak-project/build/game/gk` standalone (not through
Mad2/Wine) and read its logs rather than guessing:
- Graphics: real hardware path confirmed -- `OpenGL context renderer: AMD
  Radeon RX 6600 (radeonsi, navi23, ACO ...)`, `OpenGL initialized - v4.6
  ... Mesa 26.1.2`, not a software/llvmpipe fallback. Only one anomaly in
  the whole level-load log: `Failed to find texture at 0, using random
  (direct: [ 3] sky-direct)` -- fired exactly once, at level start, which
  reads as a transient/harmless one-off rather than systemic corruption.
  Found nothing else. **Still need the user's own description (or a
  specific level/moment) of what looks wrong** -- nothing here pointed at a
  concrete bug to fix.
- Controller input: `[warn] No active game controllers could be found or
  loaded successfully - inputs will not work!` looked like a promising lead
  at first, but traced it to gk's own native SDL controller-detection path
  (`game/system/hid/input_manager.cpp`), which is unrelated to
  `JakChallenge`'s injection path -- confirmed `*cpad-list*`'s `num-cpads`
  is hardcoded to 4 in `pad.gc` (not gated on real controller count), so the
  `dotimes` loop containing our `mad2_input.dat`-polling patch always runs
  regardless. Also re-verified the wire struct (`mad2-jak1-input-packet`'s
  `:size-assert #xe` = 14 bytes, matching `Mad2Jak1InputPacket` exactly) and
  the injection logic (forces `valid`/`status` unconditionally, not just the
  button/stick fields) both look correct on inspection. Attempted a fully
  live end-to-end verification (write a crafted `mad2_input.dat`, confirm
  in-game reaction via `gk`'s debug nREPL) but `goalc`'s REPL didn't accept
  piped/non-tty stdin in this environment, and the user asked to abandon
  screenshot-based visual verification -- so this is **not fully verified
  live**, only statically. No concrete bug found in the injection pipeline
  itself; if input genuinely doesn't work in the user's real session, the
  next thing to check would be whether `mad2relauncher`'s Jak1 input
  listener is actually receiving packets at all (log a count periodically?)
  or whether `mad2_input.dat`'s path/cwd assumption doesn't hold under the
  user's actual launch method.

**Jak1 music mute -- done, using the user's own suggested approach**: `gk`
only exposes one relevant CLI flag (`-nosound`, mutes everything -- SFX
too), so per the user's explicit choice ("music only, needs more work") we
instead changed the actual *default* the game seeds a fresh settings file
with, per the user's own suggestion to "hijack" it:
`goal_src/jak1/pc/pckernel-impl.gc`'s `reset-misc` used to set
`memcard-volume-music` to `40.0` -- changed to `0.0`. `sfx-volume`/
`dialog-volume` untouched. This is a *default*, which `*setting-control*`'s
`current music-volume` continuously `seek!`s toward each frame
(`settings.gc`) -- so it stays muted even through code elsewhere that
temporarily ducks/restores music via `add-setting!`/`remove-setting!`
(`pov-camera.gc`, `ambient.gc`, `sage.gc`, title/death sequences), unlike a
one-shot imperative override (tried first, reverted -- see below) which
those could have clobbered. Scoped correctly: this vendored
`extern/jak-project` tree's `gk` is *only* ever launched by
`mad2relauncher`'s `JakChallenge` (always via `--config-path` pointing at
the isolated `saves_chaos/` dir), never the user's separate personal
`~/src/jak1nolauncher` install, so this doesn't touch any real playthrough.

Recompiled clean (`goalc -c "(mi)" -g jak1`, all 546 targets, 16.3s).
**Verified live**: launched `gk` against a fresh `--config-path`, confirmed
the newly-written settings file contains `(memcard-volume-music 0.0000)`.
Also fixed the pre-existing `saves_chaos/OpenGOAL/jak1/settings/pc-settings.gc`
(already had `40.0000` persisted from earlier test sessions) to `0.0000` so
the mute takes effect immediately without the user needing to manually
reset settings.

(First attempt, reverted: an imperative `(set-setting! 'music-volume 'abs
0.0 0)` call added to `game-info.gc`'s `*kernel-boot-level*` branch. Reverted
in favor of the default-hijack approach above after the user pointed out
Jak1 already has a real volume-slider default to just change, which is more
robust against other systems that add/remove `'music-volume` settings by
name.)

**Also updated `CLAUDE.md`**, which had never been touched for any of
`extern/jak-project`, `JakChallenge`, or `mad2relauncher`'s Jak1/Pdeathsig
additions across the last several rounds -- added `extern/jak-project` to
the top-level directory list and "Native Linux companion processes",
expanded `mad2sm64mod`'s and `mad2relauncher`'s description to cover both
effects/all four jobs, and corrected two other already-stale facts found
along the way: the command list still listed `build-sm64-linux` as part of
`just run`'s dependency chain (removed back in this session's own Round 3)
and never mentioned `build-jak1-linux`/`build-music-linux` at all.

## Open questions for you
- Does this round's `x11.go`/`Pdeathsig` fix actually resolve "gamescope
  occasionally hangs" for you, or is there a distinct remaining case (e.g.
  gamescope itself visibly frozen mid-gameplay, not at exit)?
- "Controller inputs aren't working" -- couldn't find a static bug in the
  injection pipeline (see above). Worth clarifying: is this Mad2's own
  controller not working, or specifically Jak1/JakChallenge's controller
  input once launched? If the latter, next step would be adding a packet
  counter to `mad2relauncher`'s Jak1 input listener (or `jak1mod.cpp`'s
  send side) to confirm packets are actually flowing end-to-end in a real
  session.
- "Buggy graphics" -- no description yet of what's actually wrong (tearing?
  wrong colors? geometry? missing effects?). The one log anomaly found (a
  single transient texture-load fallback at level start) doesn't obviously
  match "really buggy" on its own.


Three rounds of work this session, verified by actually running things,
not just compiling.

## Round 3: gamescope teardown fix, deploy independence, Jak1 window-visibility fix

Three issues reported after Round 2:

1. **"jak never shows up in the gamescope window."** Root cause:
   `gk`'s vendored SDL3 supports Wayland, and gamescope doesn't reliably
   strip `WAYLAND_DISPLAY` from its child processes' environment -- so
   `gk`'s window could get created on the *outer* Wayland session instead
   of gamescope's own nested X11 display, never appearing inside gamescope
   at all. Fixed by forcing `SDL_VIDEODRIVER=x11` in both `jak1.go`'s and
   (defensively) `sm64.go`'s `cmd.Env` -- gamescope is always a nested X11
   compositor, so this is always correct there regardless of which SDL
   major version a given sub-game vendors.

2. **"relauncher is being a bit broken... when gamescope is killed it's
   relaunching the music player, and if the game exits it doesn't relaunch
   it."** Root cause: gamescope's default behavior is to tear down its
   *entire* process tree the instant its primary process's window closes
   (e.g. Mad2.exe crashing) -- racing ahead of and defeating
   `mad2relauncher`'s whole purpose (crash-relaunch, see `gameloop.go`,
   whose relaunch loop itself was verified correct via live testing back in
   Round 2). That abrupt teardown is also what left `mad2music` orphaned or
   double-launched instead of going through `mad2relauncher`'s own clean
   `Stop()` path. Fix: gamescope has a documented flag for exactly this,
   `--keep-alive` ("Keep Gamescope alive even when the primary process has
   died") -- added to the `_play` recipe's `gamescope` invocation.

3. **"jak and sm64 shouldn't be required for building... the user is going
   to have to build them way post deploy, we'll ship the source code we can
   legally distribute."** `build-sm64-linux` was a *hard* dependency of
   `run`/`run-mad2russia`/`run-mad2demo` (`just run` would `exit 1` if
   `baserom.us.z64` was missing) -- removed from all three. `_play` now
   soft-checks for the built SM64 binary the same way it already did for
   Jak1 (added in Round 2): warns and omits `-sm64-path` if not built yet,
   rather than assuming it exists. Confirmed `just build`/`deploy-mad2`
   (the CMake side) never touch `extern/sm64ex-alo`/`extern/jak-project` at
   all -- only `mad2sm64mod.dll` itself (the mod, not the sub-games) is
   part of the deploy step, so this was already deploy-independent; the fix
   was specifically the `run` targets forcing a build too eagerly.

None of the three fixes above were verified against a real, full
Mad2+Wine+gamescope session (no Mad2 install/wineprefix available in this
environment) -- reasoned from gamescope's own `--help` output and code
inspection, not observed end-to-end live. User reported back: `--keep-alive`
alone didn't fix the relaunch bug, and Jak still didn't show up. See Round 4.

## Round 4: actually reproduced both bugs live and found the real causes

This time everything below was verified by actually running gamescope +
mad2relauncher + gk together and inspecting logs, not just reasoning about
gamescope flags.

**"Jak never shows up"**: the `SDL_VIDEODRIVER=x11` guess from Round 3 was
irrelevant -- a direct `gamescope -- gk` test showed it connecting to
gamescope's nested display fine either way ("Found monitor gamescope").
The real bug: `gk` has no `--width`/`--height`/`--fullscreen` CLI flags
(unlike sm64ex-alo). It hardcodes an initial `640x480` window
(`game/graphics/gfx.cpp`'s `Display::InitMainDisplay(640, 480, ...)`) that
normally gets resized once the player picks a resolution from the title
screen -- which our boot flow skips entirely (`-boot -fakeiso -level ...`
goes straight to gameplay). So the window was real and correctly
positioned, just tiny and easy to lose behind Mad2's window. Confirmed via
log: `Setting to display mode: 2, with window_size: 640,480`.

Fix: `x11.go`'s `FocusWindowAndHideMad2` now takes a `resizeTo [2]uint32`
param and force-resizes the found window via `ConfigureWindow` before
raising it. `jak1.go` passes `{1280, 720}` (matching the justfile's
`gamescope -w 1280 -h 720`); `sm64.go` passes `{0, 0}` (already sized
correctly via its own CLI flags, no-op). Verified live: log shows
`resized "OpenGOAL" window (id=...) to 1280x720` firing, and gk's own log
then reports `Found monitor gamescope, currently set to 1280x720`.

**Relauncher orphaning / mad2music getting relaunched into nowhere**:
`--keep-alive` wasn't sufficient. Root cause, confirmed by direct test: if
gamescope's own teardown doesn't happen to reach `mad2relauncher` first
(crash, a hard `SIGKILL`, however the window actually closes), the process
just gets reparented to init and keeps running forever, completely unaware
its session is gone -- `gameloop.go`'s crash-relaunch loop never gets told
to stop, and if `mad2music` happens to exit around the same moment,
`music.go`'s own crash-relaunch loop dutifully relaunches it into a
gamescope session that no longer exists.

Fix: new `mad2relauncher/parentwatch.go`, `armParentDeathSignal()` --
arms Linux's `PR_SET_PDEATHSIG` (via a raw `prctl` syscall, no new
dependency) so the kernel sends `mad2relauncher` `SIGTERM` the instant its
parent process exits, by *any* means, independent of whatever gamescope's
own teardown behavior happens to do. Called first thing in `main()`, with
`runtime.LockOSThread()` since the property is per-OS-thread.

**Verified live** with a real kill test (not synthetic): spawned
`mad2relauncher` under an intermediate shell, `SIGKILL`ed the shell
(simulating gamescope dying abruptly), and confirmed via
`mad2relauncher.log`: `received signal terminated` → `stop requested --
tearing down` → child `umu-run` process killed → `mad2relauncher exiting`
-- clean shutdown, no orphans, every time. Before this fix the process
would have simply kept running.

Both fixes are in the rebuilt `build/mad2relauncher` binary and
`extern/jak-project`'s vendored source is unaffected (Go/X11-side fixes
only, no GOAL changes this round). Still not tested against the *real*
Mad2 game under Wine -- only synthetic (`nonexistent.exe`) + real `gk`
sessions -- but the underlying mechanisms (X11 `ConfigureWindow`,
`PR_SET_PDEATHSIG`) don't care what the child process actually is.

## Round 5: user launches via mad2launcher, not `just` -- wrong target entirely

User clarified they launch through `mad2launcher` (the Fyne GUI), not
`just run`. **All of Round 3/4's justfile fixes were irrelevant to their
actual workflow** -- `mad2launcher/launch.go` is a separate,
hand-duplicated reimplementation of the justfile's `_play` recipe, not a
wrapper around it. Two real bugs found there, independent of anything
fixed so far:

1. **`launch.go`'s `jak1Dir` still pointed at the old external
   `~/jak1nolauncher`**, never updated when JakChallenge was repointed at
   `extern/jak-project` back in Round 2 (that repoint only touched the
   justfile and `mad2relauncher/jak1.go` itself -- `mad2launcher` was never
   checked). So through this launcher, JakChallenge was either silently
   disabled (if `~/jak1nolauncher/gk` didn't exist) or using the old
   prebuilt binary with none of this session's fixes. Fixed: now points at
   `extern/jak-project`, checks `build/game/gk`. Also added the same
   `fileExists`-gated soft-disable for `-sm64-path` that jak1 already had
   (was being passed unconditionally before, even when not built).

2. **Found why the `mad2relauncher`-side fixes (PDEATHSIG, X11 EWMH focus,
   window resize) weren't taking effect either, even for SM64/paths that
   *were* correct**: `mad2launcher` builds `mad2relauncher` on demand via
   `ensureGoBinaryBuilt` (`build_native.go`), which only checked `main.go`'s
   mtime against the cached binary's -- completely ignoring every other file
   in that multi-file package (`jak1.go`, `sm64.go`, `x11.go`,
   `parentwatch.go`). Confirmed live: a stale binary at
   `mad2relauncher/mad2relauncher` (built 12:04) was older than `x11.go`'s
   actual fix (12:26) but the check only looked at `main.go` (11:55, so
   "not stale") and kept reusing it. Fixed `anyGoFileNewerThan` to check
   every `*.go` file in the directory; deleted the stale cached binary so
   the next launch rebuilds fresh regardless.

**Also reverted `--keep-alive`** (added in Round 3, justfile only): the
user's actual report ("Alt+F4 freezes gamescope, music player restarts")
pointed at `--keep-alive` itself being the culprit, not a missing flag --
its whole documented purpose is "keep gamescope alive even when the
primary process has died," which manifests as exactly a frozen window
showing nothing once its primary process exits. `armParentDeathSignal`'s
`PR_SET_PDEATHSIG` (Round 4) already solves the original problem
(mad2relauncher cleaning itself up when its parent dies) from the other
direction, so `--keep-alive` was actively counterproductive -- removed.

**Jak below Madagascar 2 in z-order, still, after resizing**: on top of the
window-resize fix, `x11.go`'s `raiseAndFocus` was still using raw
`ConfigureWindow(StackModeAbove)` + `SetInputFocus` to try to bring gk's
window forward. Rewrote it to send the standard EWMH `_NET_ACTIVE_WINDOW`
client message to the root window instead -- gamescope is a Wayland
compositor exposing an Xwayland surface, and its own notion of "which app
is currently displayed" is very likely tracked through that EWMH protocol
(the same one every taskbar/pager uses to raise windows in someone else's
client), not raw X11 stacking order, which compositors commonly ignore
from third-party clients by design (focus-stealing prevention). Kept the
old calls alongside it (harmless, and `SetInputFocus` is still needed for
keyboard routing once a window *is* active) rather than replacing them.

**Not yet re-verified live end-to-end after this round's fixes** (the
EWMH-focus rewrite, the jak1Dir fix, and the build-staleness fix) --
ran out of turn budget for another live gamescope+mad2launcher session.
Worth confirming: (a) mad2launcher actually rebuilds mad2relauncher fresh
next launch (stale binary was deleted, so it must), (b) Jak now appears
*and* is on top, (c) Alt+F4 exits cleanly without a freeze and without
mad2music surviving.

## Round 6: freeze root cause found via direct reproduction, and a much bigger bug -- the vendored Jak1 patches were never actually ported

User reported Round 5's fixes didn't resolve the freeze/relaunch issue, but
Jak1 *does* now appear -- with two new problems: it doesn't come to the
front (still behind Mad2), and it boots to the title screen instead of the
intended random checkpoint. Both were tracked down by actually reproducing
them, not reasoning further from logs.

**Freeze root cause, finally nailed down**: built a small throwaway Go/xgb
tool to inspect the *outer* desktop's X11 window tree while gamescope runs.
Without an explicit `--backend`, gamescope auto-detects and picks its
newer native-Wayland-nested mode whenever `WAYLAND_DISPLAY` is set (true
here, this is a Wayland desktop) -- confirmed directly: gamescope had zero
windows anywhere in the outer X11/Xwayland tree despite running. That mode
apparently doesn't cleanly honor a window manager's close request either.
Relaunched with `--backend sdl` + `SDL_VIDEODRIVER=x11` (for gamescope's
*own* outer window, separate from the nested-child copies already in
jak1.go/sm64.go) -- this made gamescope show up as a real, conventional X11
window. Sent it a real `WM_DELETE_WINDOW` message (the actual protocol a
window manager sends for Alt+F4, via another small throwaway tool) and
watched it close cleanly, its entire child tree (mad2relauncher, umu-run)
gone with it -- no freeze, no orphans, confirmed by log and `pgrep`. Applied
`--backend sdl` + `SDL_VIDEODRIVER=x11` to both the justfile's `_play` and
`mad2launcher/launch.go`'s `launchLinux` (its own separate gamescope
invocation, per Round 5's finding that it doesn't go through the justfile
at all).

**Jak not coming to the front**: `x11.go`'s `raiseAndFocus` was still using
raw `ConfigureWindow(StackModeAbove)` + `SetInputFocus`. Rewrote it to send
gamescope the standard EWMH `_NET_ACTIVE_WINDOW` client message instead (the
same protocol every taskbar/pager uses to raise a window in someone else's
client) -- compositors commonly ignore raw stacking requests from
third-party clients by design (focus-stealing prevention), which is
presumably why the old approach never worked even though the resize/found
logging looked fine. Kept the old calls alongside it (harmless, and
`SetInputFocus` still matters for keyboard routing once a window *is*
active).

**Boots to title screen instead of the checkpoint -- the actually big
one**: reproduced directly by running `gk -level lavatube-start` standalone
for 15+ seconds (every previous test in this session had killed the
process within a few seconds, never long enough to notice it wasn't
progressing). Confirmed it sits at `village1`/title indefinitely,
regardless of `-level`. Root cause, found by reading `goal_src/jak1/engine/
level/level.gc`'s actual `play` function: `startup-level` is computed
*purely* from `*kernel-boot-message*`/`*debug-segment*` -- there is no
stock code path that ever looks at `*kernel-boot-level*` to skip the title
screen. The skip-title-and-jump-to-checkpoint behavior documented back in
Round 1 was **the user's own pre-existing hand patch** in
`jak1nolauncher`'s copy of `game-info.gc`/`level.gc`/`kernel-defs.gc` --
and when `extern/jak-project` was vendored fresh from upstream GitHub in
Round 2, only `pad.gc` (the new controller-injection patch written this
session) was ever manually copied over. The three *pre-existing* patches
were never ported at all, so `extern/jak-project` was running with none of
them -- title-screen-skip, speedrunner-mode/skip-movies, and the
auto-exit-on-power-cell behavior confirmed working back in Round 1 were
*all* silently absent this whole time.

Per the user's suggestion, re-diffed every file `jak1nolauncher` had ever
touched (re-ran the mtime scan from Round 1, now including `pad.gc`) against
a fresh checkout, this time actually reading and porting every real diff
instead of dismissing anything as a dead end:
- `kernel-defs.gc`, `engine/level/level.gc`, `engine/game/game-info.gc` --
  the title-screen-skip / speedrunner-mode / auto-exit-on-cell patches
  documented in Round 1, now actually copied over (previously only
  *documented*, never ported).
- `engine/common-obs/process-taskable.gc` -- a **new** find, missed
  entirely in Round 1's dead-end conclusion: a one-line change making a
  cutscene-skip lambda unconditionally abort (`#t`) rather than checking a
  query-response state, directly tied to the `skip-movies?` flag the
  `game-info.gc` patch sets -- without it, spooled cutscenes wouldn't
  actually skip even with speedrunner mode on. Ported.
- `engine/common-obs/collectables.gc`, `levels/title/title-obs.gc` --
  re-confirmed byte-identical to stock upstream, genuinely no patch there
  (Round 1's conclusion for these two specifically was correct).

Recompiled (`goalc -c "(mi)"`, clean, 546 targets) and reproduced the fix
live: `gk --game jak1 -- -boot -fakeiso -level lavatube-start` now logs
`Displaying level lavatube [display]` within seconds -- no title screen,
correct checkpoint, confirmed by direct observation this time, not just
log inspection of a prematurely-killed process.

**Verified this round**: gamescope backend/freeze fix (direct WM_DELETE_WINDOW
test), Jak1 checkpoint-skip fix (direct 15s+ boot observation). **Not
verified**: the EWMH `_NET_ACTIVE_WINDOW` focus fix (no way to visually
confirm z-order from here) and the auto-exit-on-power-cell behavior in the
now-ported `extern/jak-project` copy specifically (only confirmed working
in `jak1nolauncher`'s original copy back in Round 1 -- should work
identically now that the exact same patch is in both places, but not
re-tested end-to-end post-port).

## Round 1: JakChallenge (mad2sm64mod extension)
See prior summary below (unchanged) — `mad2sm64mod` now registers both
`SM64Challenge` and `JakChallenge`, mirrored architecture, GOAL-side
controller-injection patch in `pad.gc`'s `service-cpads`.

## Round 2: vendored jak-project from source + fixed SM64's real controller bug

**Corrected a false premise.** There was no jak-project C++ source anywhere
on this machine — `~/src/jak1nolauncher/gk`/`goalc` are prebuilt binaries
from an OpenGOAL-Launcher release download (no `.git`, no `.cpp`, no
`CMakeLists.txt` anywhere under that directory). Fetched the real thing
fresh from `github.com/open-goal/jak-project` at tag `v0.3.5` (matches
`gk`'s embedded version string exactly) and vendored it into
`extern/jak-project/` (474M), mirroring `extern/sm64ex-alo`'s existing
"build from source + user-supplied ROM" pattern.

**Got it building for real**, inside the existing `mad2-debian` distrobox
(installed `nasm`, `ninja-build`, `go-task`, `clang`, `lld`, `libssl-dev`,
`libgl1-mesa-dev` there — nothing touched on the host):
- `task set-game-jak1 && task set-decomp-ntscv1 && task gen-cmake-release
  && task build-release` → all 1497 C++ targets built clean (`gk`, `goalc`,
  `extractor`, `decompiler`, etc.)
- Verified the output binaries run **directly on the host** (no distrobox
  needed at runtime) — confirmed via `./build/game/gk --version` and a full
  extraction/compile/boot cycle.
- Extracted assets from the user's own legitimately-owned ISO
  (`~/src/jak1nolauncher/Jak and Daxter....iso`, the same disc they already
  had): `./build/decompiler/extractor -e -d -g jak1 <iso>` → 27s.
- Compiled all GOAL code: `./build/goalc/goalc -c "(mi)" -g jak1` → all 1317
  targets, 26s.
- Booted `gk` directly and confirmed it reaches real gameplay
  init (OpenGL 4.6 context, IOP init, display detection) before being
  killed by a timeout.
- Copied the JakChallenge `pad.gc` patch (identical GOAL-source version) into
  the vendored tree and reconfirmed it compiles there too.

**Live end-to-end integration test** (not just unit-level): built the real
`mad2relauncher` binary, ran it with `-jak1-dir extern/jak-project`, sent it
a raw UDP LAUNCH packet — it forked the real source-built `gk` with the
correct args (`--game jak1 --config-path .../saves_chaos -- -boot -fakeiso
-level <random checkpoint>`), confirmed via `gk`'s own log
(`dkernel: level lavatube-start`, isolated save path in use). Sent a raw
UDP input packet — `mad2_input.dat` was written atomically with exactly the
expected bytes. This surfaced one real, since-clarified non-issue (`gk`
exited with a segfault signal during this particular test run — user
confirmed that was just them closing the game window, not a real bug).

**Repointed the integration** away from `~/src/jak1nolauncher` at the
vendored, source-built tree:
- `mad2relauncher/jak1.go`: `gk` path is now `<jak1Dir>/build/game/gk`;
  comments updated; verified `cmd.Dir=jak1Dir` (repo root) is right even
  though the binary lives in a subdirectory — `gk` finds its own
  `goal_src`/`decompiler_out` via its binary path, not cwd (confirmed via
  its own startup log).
- `justfile`'s `_play` recipe: `jak1_dir` now points at
  `extern/jak-project`, checks for `build/game/gk`.
- New `build-jak1-linux` just target, mirroring `build-sm64-linux`'s shape
  exactly (distrobox-first/host-fallback, hard-fails with a clear message
  if `extern/jak-project/jak1.iso` is missing, skips the slow extraction
  step via a marker-file check once already extracted). Ran it for real —
  clean.
- Deliberately **not** added as a dependency of `run`/`run-mad2russia`/
  `run-mad2demo` (unlike `build-sm64-linux`, which *is* currently a hard
  dependency there) — making a second console ROM mandatory just to launch
  Mad2 felt like a bigger UX change than asked for. `_play` already
  degrades gracefully (warns + disables JakChallenge) if `gk` isn't built
  yet. Flagging this asymmetry in case you want it to match SM64's
  all-or-nothing precedent instead.

**Found and fixed a real, independent bug in SM64Challenge** (user reported
"SM64's controls are broken too" mid-session): `sm64mod.cpp` was sending
XInput's raw `wButtons` bitmask completely untranslated, and
`controller_udp.c` copied it straight into SM64's `pad->button` with no
translation either. XInput's bit positions (`DPAD_UP=0x0001`, `A=0x1000`,
`START=0x0010`, ...) don't line up with N64's `PR/os_cont.h` `CONT_*` bits
(`CONT_UP=0x0800`, `CONT_A=0x8000`, `CONT_START=0x1000`, ...) at almost any
bit — every button was effectively scrambled. Added `XInputToN64Buttons` in
`sm64mod.cpp` (translates in C++, same architecture as `jak1mod.cpp`'s
`XInputToPs2Pad`, which got this right from the start for the same
underlying reason): A→A, B→B, X→Z (common modern-pad remap, N64 has no 4th
face button), dpad→dpad, LB/RB→L/R (N64's L/R are shoulder bumpers, not
lower triggers, so this is the natural match — not LT/RT), right stick
thresholded into C-buttons matching `controller_sdl2.c`'s own `0x4000`
threshold and sign convention. Y is intentionally left unbound (no N64
equivalent). Updated `mad2sm64_protocol.h`'s field comment and
`controller_udp.c`'s file header to describe the new (correct) contract.
Rebuilt both `mad2sm64mod.dll` (MinGW) and the real `sm64ex-alo` Linux
binary (via `make -C extern/sm64ex-alo` in `mad2-debian`) — both clean.

## Not done / open questions for you
- `just run`/`run-mad2russia`/`run-mad2demo` do NOT require
  `build-jak1-linux` (see asymmetry note above) — say the word if you want
  it made mandatory like SM64's `baserom.us.z64`.
- No live in-game verification that JakChallenge's controller mapping
  (PS2 pad-buttons via the `pad.gc` patch) actually *feels* right in a full
  play session — only wire-level/file-write verification. Same caveat as
  before for the SM64 button-mapping fix: reasoned from `controller_sdl2.c`
  and `os_cont.h`, not confirmed with a real gamepad in front of a running
  game.
- `extern/jak-project/jak1.iso` in this working tree is currently a symlink
  to `~/src/jak1nolauncher`'s ISO (used for testing, not copied) — you may
  want to replace it with your own copy/symlink convention, or leave it.
- `~/src/jak1nolauncher` itself is untouched and still has its own working
  prebuilt-binary setup — nothing there was deleted, just no longer what
  this repo's build points at.
