Drop your own legally-owned ROM/ISO dumps here, named exactly:

- `baserom.us.z64` — Super Mario 64 (US), needed for `SM64Challenge`
- `jak1.iso` — Jak & Daxter: The Precursor Legacy, needed for `JakChallenge`
- `smb1.nes` — Super Mario Bros. (any NES dump libretro's FCEUmm core accepts), needed for `NesChallenge`
- `smb2j.fds` — Super Mario Bros. 2 / The Lost Levels (Japan, Famicom Disk System dump), needed for `NesLostLevelsChallenge`
- `disksys.rom` — the Famicom Disk System BIOS dump, also needed for `NesLostLevelsChallenge` (FDS games can't boot in emulation without it, same as a real Famicom needs the physical RAM Adapter's own boot ROM) -- NOT needed for `NesChallenge` (SMB1 is an ordinary cartridge ROM)
- `sm64.z64` — Super Mario 64 (US), needed for `N64Sm64Challenge`. **Not the same file as `baserom.us.z64` above** even though it's the same game: `baserom.us.z64` is only ever read offline by `sm64ex-alo`'s own asset extractor (see `build-sm64-linux`/`build-sm64-windows`), never actually executed as a ROM, so its exact byte layout doesn't matter to it; `sm64.z64` is loaded directly by an N64 emulator core (mupen64plus_next) and does need to be a normal, valid, big-endian `.z64` N64 ROM dump. In practice a `baserom.us.z64` you already have almost always works unmodified as `sm64.z64` too (just copy or symlink it) — they're kept as separate files here mainly so a future byte-order/header quirk in one path doesn't silently break the other.

None of these files are redistributed with this project (see `.gitignore` —
`*.z64`/`*.iso`/`*.nes`/`*.fds`/`*.rom` are never tracked). `just build-sm64-linux`/`build-sm64-windows`/
`build-jak1-linux` (and `mad2launcher`'s "Build Games" page, which drives the same
steps via `mad2buildgames`) symlink whichever file they need from here into the right
`extern/*` subproject automatically — you only need one copy of each, dropped
in this one folder, not one per subproject. `smb1.nes`/`smb2j.fds`/`disksys.rom`/`sm64.z64` are
different: unlike SM64/Jak1 (native engines built from source), `mad2nesmod.dll`'s
effects run prebuilt libretro cores directly inside the Mad2 process, so these are
copied (not symlinked) straight into each deployed install's `nescore/`/`n64core/` folders
by `just deploy-mad2`/`deploy-mad2russia`/`deploy-mad2demo` (see `justfile`'s
`_deploy-nes`/`_deploy-n64` recipes and `mad2nesmod/src/nesmod.cpp`) — no subproject build step,
no ISO/ROM extraction, no `extern/*` involved at all.
