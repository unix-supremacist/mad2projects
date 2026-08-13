# gamescope crash — investigation notes (2026-08-04)

Found while debugging unrelated system lag. `gamescope` has been crashing repeatedly
while running this project's launcher. Logging what was found so far for whoever
picks this up.

## Frequency

43 coredumps for `gamescope`/`gamescope-wl` recorded between 2026-08-01 and
2026-08-04 (`coredumpctl list gamescope-wl`), sometimes multiple times within
minutes of each other. This is frequent enough to be a real workflow problem,
not a rare edge case.

## The command that crashes

```
/usr/bin/gamescope --backend sdl -w <W> -h <H> -W <W> -H <H> -r 60 \
  --framerate-limit 60 -F fsr --fsr-sharpness 2 -b -- \
  /home/unix/src/mad2modloader/build/mad2relauncher \
  -sm64-path /home/unix/src/mad2modloader/extern/sm64ex-alo/build/us_pc/sm64.us.f3dex2e \
  -jak1-dir /home/unix/src/mad2modloader/extern/jak-project \
  Mad2.exe
```

Seen at both 1280x960 and 1920x1080 — resolution doesn't matter, same crash both times.

## What's actually happening

Signal 6 (SIGABRT), via an **uncaught C++ exception**: `std::terminate()` →
`abort()`. This is *gamescope's own code* aborting (not the child `mad2relauncher`/
`Mad2.exe` process), per the coredump for `gamescope-wl`.

**The crash is 100% deterministic** — every single coredump checked has the exact
same stack, down to the byte offset, in the gamescope binary:

```
#5  _ZSt9terminatev              (libstdc++.so.6 + 0x9a654)
#6  n/a                          (gamescope + 0x2dd22)
#7  n/a                          (gamescope + 0xc1286)
#8  n/a                          (gamescope + 0xd8508)
#9  n/a                          (gamescope + 0x10e2cf)
```

This matches across at least 4 separate crash instances (pids 1457974, 1464550,
1471528, 1480925), across different days and different resolutions. This is not
a flaky/racy crash — it's the same code path failing the same way every time this
gamescope invocation runs. Should be easy to reproduce on demand.

## What's ruled out

- **Not a GPU/driver issue**: only GPU present is AMD Navi 23 (RX 6600, `amdgpu`
  driver). No nvidia card despite an nvidia.conf blacklist entry existing on the
  system (leftover/generic config, not applicable here). No GPU reset/error
  messages in `dmesg` around any crash timestamp.
- **Not thermal/throttling**: sensors were nowhere near critical at the time
  (checked separately, general system health pass).
- **Not disk/memory pressure**: checked separately as part of a broader system
  audit, all clean.

## What's missing / next steps for whoever debugs this

1. **The actual exception message was not captured.** `std::terminate`'s default
   handler prints `terminate called after throwing an instance of '...' what(): ...`
   to stderr right before aborting — that string would almost certainly identify
   the exception type and message. It didn't make it into the journal because
   gamescope was launched from a terminal (kitty) whose stderr wasn't logged.
   **Next time this is reproduced, run it directly in a terminal you're watching**
   (or redirect stderr to a file: `... 2>gamescope-crash.log`) to capture that line.
2. **Symbols are stripped** — backtraces show raw offsets (`gamescope + 0x2dd22`)
   instead of function names. If gamescope is built locally, building/installing
   with debug info (or installing a `-debug`/`-dbgsym` package if the distro
   provides one) would let `coredumpctl debug <PID>` give a real backtrace with
   symbol names instead of offsets.
3. Given it's 100% reproducible, worth bisecting the command-line flags to see
   which one triggers it — candidates to try removing/varying one at a time:
   `-F fsr --fsr-sharpness 2` (FSR upscaling), `-b` (borderless), `--backend sdl`
   (try a different backend if available). Since it happens at multiple
   resolutions but always the same flags otherwise, the flags themselves (or the
   interaction with `mad2relauncher`'s own window/surface setup) are the prime
   suspect over resolution-specific state.
4. To pull up any of the existing dumps for further inspection:
   `coredumpctl list gamescope-wl` then `coredumpctl info <PID>` or
   `coredumpctl debug <PID>` (needs debug symbols per point 2 to be useful).

## Side effect noticed

Every one of these crashes also leaves behind a stuck `drkonqi-coredump-launcher`
process (KDE's crash handler) that never exits on its own — 16-18 of them had
accumulated in the background over the past few days, collectively costing
noticeable idle CPU. Not gamescope's fault directly, but worth knowing this crash
loop has a compounding side effect beyond just the crash itself.
