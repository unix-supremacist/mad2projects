build_dir := "build"
nproc := `nproc 2>/dev/null || echo 4`

# Pinned SDL2 release used for the sm64ex-alo Windows cross-build (no
# mingw-w64 SDL2 package exists for this host) -- see build-sm64-windows.
sdl2_mingw_version := "2.32.10"

# Pinned SDL3 release used for the mad2music Windows cross-build, same
# "no mingw-w64 package for this host" reasoning as sdl2_mingw_version --
# see build-music-windows.
sdl3_mingw_version := "3.4.12"

# Pinned umu-launcher release used to bundle a portable umu-run -- see
# fetch-umu-run.
umu_launcher_version := "1.4.1"

# Proton installation to use for all games (falls back to searching
# compatibilitytools.d if this path doesn't exist).
protonpath := env_var_or_default("PROTONPATH", home_directory() / ".local/share/Steam/compatibilitytools.d/GE-Proton10-34")
gameid := env_var_or_default("GAMEID", "0")

# Mad2mod ships a version.dll proxy next to each game's exe as its mod
# loader. Tell Wine to prefer that native DLL over its own builtin
# version.dll, otherwise the builtin would win and the mod loader would
# never load.
dll_overrides := env_var_or_default("WINEDLLOVERRIDES", "version=n,b")

# Configure the cross-compiling build (Ninja via samu, ccache-accelerated).
configure:
    cmake -S . -B {{build_dir}} \
        -G Ninja \
        -DCMAKE_MAKE_PROGRAM=samu \
        -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.toolchain.cmake \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Build version.dll + mods (configures first if needed).
build:
    [ -d {{build_dir}} ] || just configure
    cmake --build {{build_dir}}

# Stages the fetched FCEUmm core + roms/{smb1.nes,smb2j.fds,disksys.rom}
# into <dir>/nescore/ for mad2nesmod.dll's NesChallenge/
# NesLostLevelsChallenge effects (see mad2nesmod/src/nesmod.cpp).
# Best-effort: warns and leaves whichever piece is missing unstaged rather
# than failing the whole deploy, same "degrade gracefully" precedent as
# _play's sm64_bin/jak1_dir checks -- each effect just logs a load failure
# and never activates if its own ROM (or, for Lost Levels, the FDS BIOS)
# isn't there yet.
_deploy-nes dir:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "{{dir}}/nescore"
    core="{{justfile_directory()}}/extern/.libretro-cores/fceumm_libretro.dll"
    if [ -f "$core" ]; then
        cp "$core" "{{dir}}/nescore/fceumm_libretro.dll"
    else
        echo "Warning: $core not found (run 'just fetch-libretro-fceumm' first) -- NES effects' core won't load this session" >&2
    fi
    rom="{{justfile_directory()}}/roms/smb1.nes"
    if [ -f "$rom" ]; then
        cp "$rom" "{{dir}}/nescore/smb1.nes"
    else
        echo "Warning: $rom not found (see roms/README.md) -- NesChallenge's ROM won't load this session" >&2
    fi
    rom2="{{justfile_directory()}}/roms/smb2j.fds"
    if [ -f "$rom2" ]; then
        cp "$rom2" "{{dir}}/nescore/smb2j.fds"
    else
        echo "Warning: $rom2 not found (see roms/README.md) -- NesLostLevelsChallenge's ROM won't load this session" >&2
    fi
    fdsbios="{{justfile_directory()}}/roms/disksys.rom"
    if [ -f "$fdsbios" ]; then
        cp "$fdsbios" "{{dir}}/nescore/disksys.rom"
    else
        echo "Warning: $fdsbios not found (see roms/README.md) -- NesLostLevelsChallenge (FDS) won't boot without it this session" >&2
    fi

# Same shape as _deploy-nes above, staging the mupen64plus_next core +
# roms/sm64.z64 into <dir>/n64core/ for mad2nesmod.dll's N64Sm64Challenge
# effect (see mad2nesmod/src/nesmod.cpp). Best-effort, same "warn and
# degrade" precedent.
_deploy-n64 dir:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "{{dir}}/n64core"
    core="{{justfile_directory()}}/extern/.libretro-cores/mupen64plus_next_libretro.dll"
    if [ -f "$core" ]; then
        cp "$core" "{{dir}}/n64core/mupen64plus_next_libretro.dll"
    else
        echo "Warning: $core not found (run 'just fetch-libretro-mupen64plus' first) -- N64Sm64Challenge's core won't load this session" >&2
    fi
    rom="{{justfile_directory()}}/roms/sm64.z64"
    if [ -f "$rom" ]; then
        cp "$rom" "{{dir}}/n64core/sm64.z64"
    else
        echo "Warning: $rom not found (see roms/README.md) -- N64Sm64Challenge's ROM won't load this session" >&2
    fi

# Build and copy version.dll + mods/ into each game directory.
deploy-mad2: build
    cmake --build {{build_dir}} --target deploy-mad2
    just _deploy-nes mad2
    just _deploy-n64 mad2

deploy-mad2russia: build
    cmake --build {{build_dir}} --target deploy-mad2russia
    just _deploy-nes mad2russia
    just _deploy-n64 mad2russia

deploy-mad2demo: build
    cmake --build {{build_dir}} --target deploy-mad2demo
    just _deploy-nes mad2demo
    just _deploy-n64 mad2demo

# Resolve a working Proton path: use protonpath if it exists, else fall back
# to the first GE-Proton*/Proton* found under any known Steam install
# location -- NOT just ~/.local/share/Steam. That was the only location
# ever checked here until a user reported Proton not being found at all
# despite having it installed and working fine via Steam itself (used by
# the older ../mad2mod tool, which launches through Steam directly and so
# never needed to independently rediscover Proton's location the way this
# repo's umu-run-based launch does): they simply had no
# ~/.local/share/Steam directory -- Steam was installed via Flatpak. A
# second user hit the same class of bug via Debian's own `steam` package,
# whose real data directory is ~/.steam/debian-installation -- confirmed
# directly from their machine that neither ~/.steam/root nor ~/.steam/steam
# resolved to it as a symlink on their setup. ~/.steam/root and
# ~/.steam/steam are symlinks Steam itself maintains pointing at wherever
# its real data directory actually is -- normally authoritative regardless
# of native/custom-prefix/relocated installs, kept first since they're
# right far more often than not, but evidently not guaranteed on every
# distro-packaged install; the Flatpak/Snap paths cover the two most common
# non-native packaging cases on Linux.
_resolve_proton:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -d "{{protonpath}}" ]; then
        echo "{{protonpath}}"
        exit 0
    fi
    for steam_root in \
        "$HOME/.steam/root" \
        "$HOME/.steam/steam" \
        "$HOME/.steam/debian-installation" \
        "$HOME/.local/share/Steam" \
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" \
        "$HOME/snap/steam/common/.local/share/Steam"
    do
        fallback=$(find "$steam_root/compatibilitytools.d" "$steam_root/steamapps/common" -maxdepth 1 \( -name "GE-Proton*" -o -name "Proton*" \) 2>/dev/null | head -n 1)
        if [ -n "$fallback" ]; then
            echo "Warning: Default Proton path not found. Falling back to: $fallback" >&2
            echo "$fallback"
            exit 0
        fi
    done
    echo "Error: Proton path not found at '{{protonpath}}' and no fallback was found under any known Steam location." >&2
    echo "Please install Proton via Steam or set PROTONPATH environment variable." >&2
    exit 1

# Launch `exe` (inside `dir`, relative to the repo root) via mad2relauncher
# inside gamescope, using a wineprefix scoped to `prefix_name` under
# ./wineprefixes/. mad2relauncher wraps `umu-run` itself (crash-relaunch --
# see mad2relauncher/main.go) instead of gamescope invoking umu-run
# directly, and also orchestrates the SM64Challenge, JakChallenge, and
# MarioLegacyChallenge chaos effects (native Linux extern/sm64ex-alo /
# extern/jak-project's gk / extern/mario-legacy's gk, forked on request from
# mad2companionmod.dll inside the Wine process).
_play dir exe prefix_name:
    #!/usr/bin/env bash
    set -euo pipefail
    export WINEPREFIX="{{justfile_directory()}}/wineprefixes/{{prefix_name}}"
    export PROTONPATH="$(just _resolve_proton)"
    export GAMEID="{{gameid}}"
    export WINEDLLOVERRIDES="{{dll_overrides}}"
    mkdir -p "$WINEPREFIX"
    echo "Cleaning up any old Wine/game instances..."
    killall -9 "{{exe}}" wineserver winedevice.exe gamescopereaper mad2relauncher sm64.us.f3dex2e gk mad2music 2>/dev/null || true
    sleep 1
    echo "Starting {{exe}} inside gamescope (prefix: $WINEPREFIX)..."
    cd "{{justfile_directory()}}/{{dir}}"
    export UMU_LOG=debug
    export PROTON_LOG=1
    relauncher="{{justfile_directory()}}/{{build_dir}}/mad2relauncher"
    sm64_bin="{{justfile_directory()}}/extern/sm64ex-alo/build/us_pc/sm64.us.f3dex2e"
    jak1_dir="{{justfile_directory()}}/extern/jak-project"
    music_bin="{{justfile_directory()}}/{{build_dir}}/mad2music"
    music_audio_dir="{{justfile_directory()}}/mad2music/audio"
    sm64_flag=()
    if [ -x "$sm64_bin" ]; then
        sm64_flag=(-sm64-path "$sm64_bin")
    else
        echo "Warning: $sm64_bin not found (run 'just build-sm64-linux' first) -- SM64Challenge disabled this session" >&2
    fi
    jak1_flag=()
    if [ -x "$jak1_dir/build/game/gk" ]; then
        jak1_flag=(-jak1-dir "$jak1_dir")
    else
        echo "Warning: $jak1_dir/build/game/gk not found (run 'just build-jak1-linux' first) -- JakChallenge disabled this session" >&2
    fi
    mariolegacy_dir="{{justfile_directory()}}/extern/mario-legacy"
    mariolegacy_flag=()
    if [ -x "$mariolegacy_dir/gk" ]; then
        mariolegacy_flag=(-mariolegacy-dir "$mariolegacy_dir")
    else
        echo "Warning: $mariolegacy_dir/gk not found (see 'just setup-mario-legacy' first) -- MarioLegacyChallenge disabled this session" >&2
    fi
    # Deliberately NOT passing --keep-alive: it was tried as a fix for
    # gamescope tearing down its whole process tree too eagerly (see below),
    # but its actual effect ("keep gamescope alive even when the primary
    # process has died") is to leave gamescope's window open showing
    # nothing once its primary process is gone -- observed as a hang/freeze
    # on Alt+F4. The real fix for the original problem is
    # mad2relauncher/parentwatch.go's armParentDeathSignal: mad2relauncher
    # now detects its own parent (gamescope) dying via PR_SET_PDEATHSIG and
    # cleanly tears its children down itself, regardless of how or when
    # gamescope actually exits -- so gamescope is left free to exit
    # normally (Alt+F4 works as expected) without racing mad2relauncher's
    # own cleanup.
    #
    # --backend sdl + SDL_VIDEODRIVER=x11 (for gamescope's OWN outer window,
    # not its nested children): without an explicit --backend, gamescope
    # auto-detects and picks its newer native-Wayland-nested mode when
    # WAYLAND_DISPLAY is set (as it is here, running under a Wayland
    # session) -- which doesn't create an X11 window on the outer display
    # at all (confirmed: absent from an XQueryTree walk of the outer
    # session), and, more importantly, doesn't cleanly honor a WM's window-
    # close request (Alt+F4) -- observed as the whole thing hanging/
    # freezing instead of exiting. Forcing the SDL backend makes gamescope
    # present as a conventional window that responds to a real
    # WM_DELETE_WINDOW the normal way (confirmed via a direct test: sending
    # WM_DELETE_WINDOW to a --backend sdl gamescope window closed it and
    # its entire child tree cleanly, with no freeze and no orphans).
    SDL_VIDEODRIVER=x11 gamescope --backend sdl -w 1280 -h 720 -b -- "$relauncher" "${sm64_flag[@]}" "${jak1_flag[@]}" "${mariolegacy_flag[@]}" -music-path "$music_bin" -music-audio-dir "$music_audio_dir" "{{exe}}"

# Open Registry Editor in a game's prefix.
_regedit dir prefix_name:
    #!/usr/bin/env bash
    set -euo pipefail
    export WINEPREFIX="{{justfile_directory()}}/wineprefixes/{{prefix_name}}"
    export PROTONPATH="$(just _resolve_proton)"
    export GAMEID="{{gameid}}"
    mkdir -p "$WINEPREFIX"
    cd "{{justfile_directory()}}/{{dir}}"
    umu-run regedit

# Open a bash shell inside a game's Proton environment.
_shell dir prefix_name:
    #!/usr/bin/env bash
    set -euo pipefail
    export WINEPREFIX="{{justfile_directory()}}/wineprefixes/{{prefix_name}}"
    export PROTONPATH="$(just _resolve_proton)"
    export GAMEID="{{gameid}}"
    mkdir -p "$WINEPREFIX"
    cd "{{justfile_directory()}}/{{dir}}"
    umu-run bash

# Build the Go relauncher shim (mad2relauncher/) — see its own file header
# for what it does.
build-relauncher:
    cd mad2relauncher && go build -o "{{justfile_directory()}}/{{build_dir}}/mad2relauncher" .

# Fetches umu-launcher's official portable "zipapp" release (a self-
# contained Python zip archive, still needs a system python3 but nothing
# else -- no umu-launcher package install required) and stages it as
# build/umu-run, sitting right next to build/mad2relauncher. mad2relauncher
# itself (gameloop.go's resolveUmuRun) prefers a copy found beside its own
# binary over one on PATH, so this is what lets `just run` (and a packaged
# mad2launcher release, via tools/package.sh staging the same file next to
# mad2relauncher there too) work without the user separately installing
# umu-launcher system-wide. Same "fetch a pinned upstream release once,
# reuse it" shape as build-sm64-windows's SDL2 fetch.
fetch-umu-run:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "{{build_dir}}"
    if [ -x "{{build_dir}}/umu-run" ]; then
        exit 0
    fi
    echo "[just] Fetching umu-launcher {{umu_launcher_version}} portable zipapp..."
    curl -sL "https://github.com/Open-Wine-Components/umu-launcher/releases/download/{{umu_launcher_version}}/umu-launcher-{{umu_launcher_version}}-zipapp.tar" \
        | tar -x -C "{{build_dir}}" --strip-components=1 umu/umu-run
    chmod +x "{{build_dir}}/umu-run"

# Fetches the official libretro buildbot's latest 32-bit Windows FCEUmm
# core (fceumm_libretro.dll) -- mad2nesmod's NesChallenge effect
# LoadLibraryA's this directly inside the Mad2 process (no RetroArch, no
# separate frontend process -- see mad2nesmod/src/nesmod.cpp). 32-bit
# because mad2 mods are themselves 32-bit (i686 MinGW) DLLs loaded into
# Mad2.exe, a 32-bit process. Cached into a gitignored extern/ subdirectory
# (same "fetch a prebuilt release once, reuse it" shape as fetch-umu-run/
# build-music-windows's SDL3 fetch) since libretro cores have no versioned
# releases, only a rolling "latest" nightly -- re-run this manually if you
# want to pick up a newer core build.
fetch-libretro-fceumm:
    #!/usr/bin/env bash
    set -euo pipefail
    core_dir="extern/.libretro-cores"
    mkdir -p "$core_dir"
    if [ -f "$core_dir/fceumm_libretro.dll" ]; then
        exit 0
    fi
    echo "[just] Fetching FCEUmm libretro core (Windows x86)..."
    mkdir -p "{{build_dir}}"
    curl -sL "https://buildbot.libretro.com/nightly/windows/x86/latest/fceumm_libretro.dll.zip" -o "{{build_dir}}/fceumm_libretro.dll.zip"
    unzip -o -q "{{build_dir}}/fceumm_libretro.dll.zip" -d "$core_dir"
    rm -f "{{build_dir}}/fceumm_libretro.dll.zip"

# Same shape as fetch-libretro-fceumm above, but for mupen64plus_next
# (mupen64plus_next_libretro.dll) -- mad2nesmod's N64Sm64Challenge effect.
# Windows builds of this core always compile in the Angrylion pure-software
# RDP plugin (confirmed against the core's own Makefile: HAVE_THR_AL=1 is
# set unconditionally in its "else" / Windows branch, not gated behind any
# platform check) alongside the default GLideN64 (OpenGL) one -- nesmod.cpp
# forces the core to use Angrylion via a core-options override so it never
# tries to open a GL context this mod's D3D9-only hook can't provide. See
# mad2nesmod/src/nesmod.cpp's file header for the full picture.
fetch-libretro-mupen64plus:
    #!/usr/bin/env bash
    set -euo pipefail
    core_dir="extern/.libretro-cores"
    mkdir -p "$core_dir"
    if [ -f "$core_dir/mupen64plus_next_libretro.dll" ]; then
        exit 0
    fi
    echo "[just] Fetching mupen64plus_next libretro core (Windows x86)..."
    mkdir -p "{{build_dir}}"
    curl -sL "https://buildbot.libretro.com/nightly/windows/x86/latest/mupen64plus_next_libretro.dll.zip" -o "{{build_dir}}/mupen64plus_next_libretro.dll.zip"
    unzip -o -q "{{build_dir}}/mupen64plus_next_libretro.dll.zip" -d "$core_dir"
    rm -f "{{build_dir}}/mupen64plus_next_libretro.dll.zip"

# Build the native Linux music/podcast playback engine (mad2music/), used by
# mad2relauncher (music.go) which runs it continuously for the whole play
# session, and mad2podcastmod.dll (inside the Wine process) which triggers
# its podcast interruption over loopback UDP -- see mad2music/src/main.cpp.
# Own host-native CMake project, not part of the root MinGW cross-build.
build-music-linux:
    cmake -S mad2music -B mad2music/build -G Ninja -DCMAKE_MAKE_PROGRAM=samu
    cmake --build mad2music/build
    mkdir -p "{{build_dir}}"
    cp mad2music/build/mad2music "{{build_dir}}/mad2music"

# Build the offline patch-generation tool for mad2xdeltamod.dll's
# *.mad2xdelta files (see mad2xdeltagen/src/main.cpp and
# mad2xdeltamod/src/xdeltamod.cpp for the full picture): takes an original
# and a modified IGZ section's decompressed bytes and produces a tiny
# xdelta3/VCDIFF-based patch mad2xdeltamod applies at runtime, instead of
# ever redistributing a whole patched .bld/.pak. Own host-native CMake
# project, not part of the root MinGW cross-build.
build-xdeltagen:
    cmake -S mad2xdeltagen -B mad2xdeltagen/build -G Ninja -DCMAKE_MAKE_PROGRAM=samu
    cmake --build mad2xdeltagen/build
    mkdir -p "{{build_dir}}"
    cp mad2xdeltagen/build/dist/mad2xdeltagen "{{build_dir}}/mad2xdeltagen"

# Best-effort i686-w64-mingw32 cross-build of mad2music for native-Windows
# Mad2 installs, where there is no mad2relauncher process to fork/supervise
# it -- see mad2podcastmod/src/podcastmod.cpp's native-Windows
# CreateProcess-if-not-running branch, and mad2music/src/platform_compat.h
# for the actual porting shim. Same "fetch a prebuilt mingw devel package,
# no distro package for this host" shape as build-sm64-windows's SDL2 dance,
# just SDL3 (fetched as a dynamic/shared devel package -- SDL3's official
# mingw release ships no static lib, so SDL3.dll must be copied alongside
# mad2music.exe at deploy time, done below). Produces
# build/windows/mad2music.exe + build/windows/SDL3.dll.
build-music-windows:
    #!/usr/bin/env bash
    set -euo pipefail
    sdl3_dir="extern/.sdl3-mingw"
    if [ ! -d "$sdl3_dir/i686-w64-mingw32" ]; then
        echo "[just] Fetching SDL3 {{sdl3_mingw_version}} mingw devel package..."
        mkdir -p "$sdl3_dir"
        curl -sL "https://github.com/libsdl-org/SDL/releases/download/release-{{sdl3_mingw_version}}/SDL3-devel-{{sdl3_mingw_version}}-mingw.tar.gz" \
            | tar -xz -C "$sdl3_dir" --strip-components=1 "SDL3-{{sdl3_mingw_version}}/i686-w64-mingw32"
    fi
    sdl3_root="$(pwd)/$sdl3_dir/i686-w64-mingw32"
    echo "[just] Cross-compiling mad2music for Windows (i686-w64-mingw32)..."
    # PKG_CONFIG_LIBDIR (not just _PATH) is set so pkg-config only sees this
    # one .pc file, not any host-native sdl3.pc it might otherwise also find
    # -- sdl3.pc's prefix is self-relative (${pcfiledir}/../..) so no other
    # sysroot wiring is needed for this to resolve correctly.
    PKG_CONFIG_LIBDIR="$sdl3_root/lib/pkgconfig" PKG_CONFIG_PATH="" \
        cmake -S mad2music -B mad2music/build-windows -G Ninja \
        -DCMAKE_MAKE_PROGRAM=samu \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_SYSTEM_PROCESSOR=x86 \
        -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc \
        -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++ \
        -DCMAKE_RC_COMPILER=i686-w64-mingw32-windres \
        -DCMAKE_FIND_ROOT_PATH=/usr/i686-w64-mingw32 \
        -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build mad2music/build-windows
    mkdir -p "{{build_dir}}/windows"
    cp mad2music/build-windows/mad2music.exe "{{build_dir}}/windows/mad2music.exe"
    cp "$sdl3_root/bin/SDL3.dll" "{{build_dir}}/windows/SDL3.dll"

# Build the music downloader (mad2music/downloader/) -- a yt-dlp-backed CLI
# that syncs mad2music/audio/music/*.txt's link lists into that same
# directory tree, skipping anything already downloaded (see its own
# --download-archive use). Own tiny Go module, plain `go build`, native to
# the host platform. Must be run with cwd == mad2music/ (its own relative
# paths, e.g. "audio/music", are relative to that) -- mad2launcher's Music
# category does this for you via its "Download music now" button.
build-music-downloader:
    cd mad2music/downloader && go build -o "{{justfile_directory()}}/{{build_dir}}/mad2musicdownloader" .

# Build the GUI launcher (mad2launcher/) natively for the host platform.
# Plain `go build`, no mingw/cross-compilation involved — see its own file
# header for why it doesn't need any of that.
build-launcher:
    cd mad2launcher && go build -o "{{justfile_directory()}}/{{build_dir}}/mad2launcher" .

# Build and run the GUI launcher. Recipes run with cwd == this justfile's
# directory, which is exactly where mad2launcher expects to find mad2/,
# mad2russia/, mad2demo/ (one subdirectory deep) to auto-discover them.
run-launcher: build-launcher
    "{{build_dir}}/mad2launcher"

# Build the standalone level.bld object-graph browser/editor (Go+Fyne, native
# Linux, same toolkit as mad2launcher). Browses any level's objects with real
# resolved names and edits the PC-verified writable fields (ActorInfo position/
# destination, etc.); saves a byte-exact level.bld, repacked into the .bld if
# that's what was opened. See mad2leveleditor/.
build-leveleditor:
    cd mad2leveleditor && go build -o "{{justfile_directory()}}/{{build_dir}}/mad2leveleditor" .

# Build and run the level editor. Optionally pass a path to open on launch,
# e.g. `just run-leveleditor mad2/Content/Streams/win/VolcanoRave.bld`.
run-leveleditor path="": build-leveleditor
    "{{build_dir}}/mad2leveleditor" {{path}}

# Best-effort Windows cross-build of mad2launcher (i686-w64-mingw32,
# GOARCH=386 for consistency with every other 32-bit Windows target in this
# repo). Fyne v2's desktop driver needs cgo (OpenGL bindings), so this is
# NOT a plain GOOS=windows go build like the pure-Go tools below -- it needs
# CGO_ENABLED=1 plus a real C cross-compiler, same toolchain as the DLL mods
# in cmake/mingw-i686.toolchain.cmake. Verified: the resulting .exe depends
# on nothing beyond standard Windows system DLLs (GDI32/KERNEL32/OPENGL32/
# SHELL32/USER32 plus the api-ms-win-crt-* UCRT forwarders, present on any
# Windows 10+ install) -- no libgcc/libstdc++/libwinpthread mingw runtime
# DLLs need to ship alongside it, unlike a typical mingw C++ cross-build.
# -H windowsgui marks it as a GUI subsystem binary so Windows doesn't pop a
# console window behind the Fyne window on launch.
build-launcher-windows:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "{{build_dir}}/windows"
    cd mad2launcher
    CGO_ENABLED=1 GOOS=windows GOARCH=386 \
        CC=i686-w64-mingw32-gcc CXX=i686-w64-mingw32-g++ \
        go build -ldflags "-H windowsgui" \
        -o "{{justfile_directory()}}/{{build_dir}}/windows/mad2launcher.exe" .

# Build the level-logic randomizer (mad2rando5/) that drives
# mad2levelredirectmod via level_redirects.txt -- migrated in-repo from the
# formerly-external ../mad2rando5 sibling. Plain `go build`, native to the
# host platform, no mingw involved (this is a standalone CLI, not a DLL).
# mad2launcher's own "Mad2 Randomizer" category builds/runs this
# automatically too; this target is for driving it directly.
build-mad2rando5:
    cd mad2rando5 && go build -o "{{justfile_directory()}}/{{build_dir}}/mad2rando5" .

# Build the IGA archive/save-editing toolset -- migrated in-repo from the
# formerly-external ../mad2assetextractor sibling, split into one Go module
# per tool (mad2iga is the shared archive-format library the other three
# import via a local `replace` directive; mad2save/mad2savegui/
# mad2transitions are fully standalone). All native, plain `go build`.
build-mad2arc:
    cd mad2arc && go build -o "{{justfile_directory()}}/{{build_dir}}/mad2arc" .

build-mad2repack:
    cd mad2repack && go build -o "{{justfile_directory()}}/{{build_dir}}/mad2repack" .

build-mad2tool:
    cd mad2tool && go build -o "{{justfile_directory()}}/{{build_dir}}/mad2tool" .

build-mad2save:
    cd mad2save && go build -o "{{justfile_directory()}}/{{build_dir}}/mad2save" .

build-mad2savegui:
    cd mad2savegui && go build -o "{{justfile_directory()}}/{{build_dir}}/mad2savegui" .

build-mad2transitions:
    cd mad2transitions && go build -o "{{justfile_directory()}}/{{build_dir}}/mad2transitions" .

# Build every migrated asset-extractor tool at once.
build-assettools: build-mad2arc build-mad2repack build-mad2tool build-mad2save build-mad2savegui build-mad2transitions

# Windows (amd64) cross-builds of the pure-Go asset-extractor tools and the
# level-logic randomizer -- see deploy-plan.md §4.3: none of these import
# cgo, so a plain GOOS/GOARCH env var on top of `go build` is enough (no
# separate cross C toolchain or vendored dependency needed, unlike
# build-sm64-windows). CGO_ENABLED=0 is set explicitly rather than relied on
# implicitly, in case the host environment happens to have a C compiler
# configured that `go build` would otherwise try to use. mad2savegui is
# deliberately NOT included here -- it uses Fyne, same as mad2launcher, and
# needs a real i686/x86_64-w64-mingw32 C cross-compiler for its cgo/OpenGL
# bindings (see deploy-plan.md §4.2), not just GOOS/GOARCH.
build-mad2rando5-windows:
    cd mad2rando5 && CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -o "{{justfile_directory()}}/{{build_dir}}/windows/mad2rando5.exe" .

build-mad2arc-windows:
    cd mad2arc && CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -o "{{justfile_directory()}}/{{build_dir}}/windows/mad2arc.exe" .

build-mad2repack-windows:
    cd mad2repack && CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -o "{{justfile_directory()}}/{{build_dir}}/windows/mad2repack.exe" .

build-mad2tool-windows:
    cd mad2tool && CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -o "{{justfile_directory()}}/{{build_dir}}/windows/mad2tool.exe" .

build-mad2save-windows:
    cd mad2save && CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -o "{{justfile_directory()}}/{{build_dir}}/windows/mad2save.exe" .

build-mad2transitions-windows:
    cd mad2transitions && CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -o "{{justfile_directory()}}/{{build_dir}}/windows/mad2transitions.exe" .

# Build every Windows cross-compiled asset-extractor tool at once (mirrors
# build-assettools' Linux-native grouping; mad2savegui excluded, see the
# comment above build-mad2rando5-windows).
build-assettools-windows: build-mad2arc-windows build-mad2repack-windows build-mad2tool-windows build-mad2save-windows build-mad2transitions-windows

# Build mad2musicpatch, the one-shot tool that blanks global.arc's own
# background music (feeding the originals into mad2music's pool instead) --
# see mad2musicpatch/main.go. Shells out to mad2arc/mad2repack (built above)
# and ffmpeg at runtime, same Unix-style approach as its bash predecessor.
build-mad2musicpatch:
    cd mad2musicpatch && go build -o "{{justfile_directory()}}/{{build_dir}}/mad2musicpatch" .

# Build mad2buildgames -- the tool mad2launcher's "Build Games" page shells
# out to in order to compile SM64/Jak1 from a user-supplied ROM/ISO,
# wrapping the exact same steps as build-sm64-linux/build-sm64-windows/
# build-jak1-linux below (own standalone Go module, plain `go build`,
# native to the host platform -- see mad2buildgames/main.go).
build-buildgames:
    cd mad2buildgames && go build -o "{{justfile_directory()}}/{{build_dir}}/mad2buildgames" .

# All three game installs share a single wineprefix (./wineprefixes/mad2) —
# they're just different installs of the same underlying game.

# Neither build-sm64-linux nor build-jak1-linux are dependencies of `run`
# below (deliberately, matching build-jak1-linux's own precedent -- see its
# comment): SM64Challenge/JakChallenge need a legally-owned ROM/ISO neither
# this repo nor a deployed release can ship, so both are end-user, opt-in,
# build-it-yourself-whenever steps, not something Mad2 itself should ever
# require to launch. _play degrades gracefully (warns, disables the
# effect) if either hasn't been built yet -- see its own sm64_bin/jak1_dir
# checks below. NesChallenge/N64Sm64Challenge (both mad2nesmod.dll) are the
# same story split in two: fetch-libretro-fceumm/fetch-libretro-mupen64plus's
# cores ARE `run` dependencies below (freely redistributable, same as
# umu-run -- see those recipes), but roms/smb1.nes and roms/sm64.z64
# themselves are still user-supplied, opt-in drop-ins (see roms/README.md);
# _deploy-nes/_deploy-n64 degrade gracefully if either is missing.

# Build, deploy, and launch the base game.
run: fetch-libretro-fceumm fetch-libretro-mupen64plus deploy-mad2 build-relauncher fetch-umu-run build-music-linux (_play "mad2" "Mad2.exe" "mad2")

# Build, deploy, and launch the Russian localization install.
run-mad2russia: fetch-libretro-fceumm fetch-libretro-mupen64plus deploy-mad2russia build-relauncher fetch-umu-run build-music-linux (_play "mad2russia" "Mad2.exe" "mad2")

# Build, deploy, and launch the demo install.
run-mad2demo: fetch-libretro-fceumm fetch-libretro-mupen64plus deploy-mad2demo build-relauncher fetch-umu-run build-music-linux (_play "mad2demo" "Mad2Demo.exe" "mad2")

regedit: (_regedit "mad2" "mad2")
regedit-mad2russia: (_regedit "mad2russia" "mad2")
regedit-mad2demo: (_regedit "mad2demo" "mad2")

shell: (_shell "mad2" "mad2")
shell-mad2russia: (_shell "mad2russia" "mad2")
shell-mad2demo: (_shell "mad2demo" "mad2")

# Remove build artifacts.
clean:
    rm -rf {{build_dir}}

# Build and package every distributable piece of this repo (DLL mods, Go
# CLI tools native + Windows cross-build where feasible, mad2music,
# mad2launcher) into dist/linux/<pkg>/ and dist/windows/<pkg>/ per
# deploy-plan.md §1-3, then archives each one (tar.gz linux, zip windows)
# without deleting the staged directories -- see tools/package.sh for the
# full package manifest and exactly what's included/excluded (SM64/Jak &
# Daxter/Mario Legacy binaries are deliberately never packaged here, see its
# header comment).
package:
    ./tools/package.sh

# Build the native Linux SM64 sub-game binary (extern/sm64ex-alo), used by
# mad2relauncher when the SM64Challenge chaos effect fires under Wine. Runs
# inside the "mad2-debian" distrobox if present -- same precedent as
# ../mad2mod's own build-sm64 recipe, whose asset-extraction/build tooling
# has proven more reliable there than directly on an Arch-based host --
# falling back to building directly on the host otherwise. Needs
# roms/baserom.us.z64 (a user-supplied ROM dump, not part of this repo,
# symlinked into extern/sm64ex-alo/ below) to extract assets from -- see
# roms/README.md. One shared roms/ folder for both SM64 and Jak1 (and
# Mario Legacy) instead of dropping a copy into each extern/* subproject.
build-sm64-linux:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ ! -e extern/sm64ex-alo/baserom.us.z64 ] && [ -e roms/baserom.us.z64 ]; then
        ln -s "$(pwd)/roms/baserom.us.z64" extern/sm64ex-alo/baserom.us.z64
    fi
    if [ ! -e extern/sm64ex-alo/baserom.us.z64 ]; then
        echo "Error: baserom.us.z64 not found (drop a US SM64 ROM dump at roms/baserom.us.z64)." >&2
        exit 1
    fi
    if distrobox list 2>/dev/null | grep -q "mad2-debian"; then
        echo "[just] Compiling sm64ex-alo (Linux) inside mad2-debian distrobox..."
        distrobox enter mad2-debian -- make -C extern/sm64ex-alo -j{{nproc}}
    else
        echo "[just] distrobox mad2-debian not found, compiling directly on host..."
        make -C extern/sm64ex-alo -j{{nproc}}
    fi

# Build the native Linux Jak & Daxter runtime (extern/jak-project, vendored
# from https://github.com/open-goal/jak-project at the tag matching
# mad2companionmod's JakChallenge assumptions) from source, and extract assets
# from a user-supplied ISO -- same distrobox-first, host-fallback precedent
# as build-sm64-linux, and same "not a dependency of `run`" reasoning as
# build-sm64-linux is NOT (see that target's own dependents): Mad2 doesn't
# require Jak1's ISO to play, JakChallenge is just disabled for the session
# if this hasn't been run yet (see _play's jak1_flag check). Needs
# roms/jak1.iso (a user-supplied ISO dump, not part of this repo, symlinked
# into extern/jak-project/ below -- see roms/README.md) to extract assets
# from. The `task` runner (go-task) and, inside mad2-debian,
# clang/lld/nasm/ninja-build/libssl-dev/libgl1-mesa-dev must already be
# present -- see docs/setup/system/linux.md in that tree for the full
# package list per distro.
build-jak1-linux:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ ! -e extern/jak-project/jak1.iso ] && [ -e roms/jak1.iso ]; then
        ln -s "$(pwd)/roms/jak1.iso" extern/jak-project/jak1.iso
    fi
    if [ ! -e extern/jak-project/jak1.iso ]; then
        echo "Error: jak1.iso not found (drop a Jak & Daxter: The Precursor Legacy ISO dump at roms/jak1.iso)." >&2
        exit 1
    fi
    cd extern/jak-project
    if distrobox list 2>/dev/null | grep -q "mad2-debian"; then
        echo "[just] Building jak-project (Linux) inside mad2-debian distrobox..."
        RUN="distrobox enter mad2-debian --"
    else
        echo "[just] distrobox mad2-debian not found, building directly on host..."
        RUN=""
    fi
    $RUN task set-game-jak1
    $RUN task set-decomp-ntscv1
    if [ ! -d build ]; then
        $RUN task gen-cmake-release
    fi
    $RUN task build-release
    # The GOAL compiler doesn't create its own output directory tree --
    # nothing in Taskfile.yml does either, so a truly fresh checkout (no
    # prior manual run) fails outright with "Error: failed to write
    # .../out/jak1/iso/JUB.DGO" the first time (mi) tries to write there.
    # This repo's own extern/jak-project only ever avoided the bug because
    # this tree was created once by hand a long time ago and just carried
    # forward since (confirmed by testing an actually-fresh checkout).
    mkdir -p out/jak1/fr3 out/jak1/iso out/jak1/obj
    # Extraction (unlike the CMake build above) isn't incremental and takes
    # ~30s -- skip it once decompiler_out/ already exists, matching the
    # marker-file check the old external jak1nolauncher/run.go used.
    #
    # $RUN (not bare ./build/...) matters here: these binaries' RUNPATH was
    # baked in from inside mad2-debian if that's what built them above, and
    # distrobox only bind-mounts $HOME at the same path inside the
    # container -- anywhere else appears under /run/host/<path>. Running
    # extractor/goalc outside the container again after a distrobox build
    # can fail with "libcompiler.so: cannot open shared object file" for
    # exactly that reason (confirmed) unless run back inside it.
    if [ ! -f decompiler_out/jak1/textures/tpage-dir.txt ]; then
        $RUN ./build/decompiler/extractor -e -d -g jak1 jak1.iso
    fi
    $RUN ./build/goalc/goalc -c "(mi)" -g jak1

# Set up the vendored extern/mario-legacy (OpenGOAL-Mods/Super-Mario-Legacy
# v0.0.28, https://github.com/OpenGOAL-Mods/Super-Mario-Legacy) used by
# mad2companionmod's MarioLegacyChallenge -- a *complete separate*
# jak-project fork (own prebuilt gk/goalc/extractor, libsm64 statically
# linked in), not something layered onto extern/jak-project. Unlike
# build-jak1-linux there is no C++ build step at all (the release tarball
# ships prebuilt Linux binaries) -- this recipe only extracts assets from
# the same Jak & Daxter ISO extern/jak-project already uses (symlinked, not
# copied -- see jak1.iso below) and recompiles the vendored GOAL sources
# (which carry mad2companionmod's own controller-injection patch, already
# applied to data/goal_src/jak1/engine/ps2/pad.gc -- see progress.md).
# Idempotent: skips extraction if decompiler_out/ already exists, same
# marker-file convention as build-jak1-linux.
#
# Needs the same Jak & Daxter ISO build-jak1-linux does, and a US Super
# Mario 64 .z64 ROM dump for libsm64 -- both pulled from the single shared
# roms/ folder (roms/jak1.iso, roms/baserom.us.z64 -- see roms/README.md),
# falling back to whatever extern/jak-project/jak1.iso or
# extern/sm64ex-alo/baserom.us.z64 already point at for anyone who set those
# up before roms/ existed. libsm64 auto-detects a ROM either next to the
# `gk` binary or in `iso_data/mario` (confirmed via `strings gk`), and "next
# to gk" is what these symlinks satisfy since gk lives at the repo root here.
setup-mario-legacy:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ ! -e extern/mario-legacy/gk ]; then
        echo "Error: extern/mario-legacy/gk not found -- download+extract the v0.0.28 Linux release tarball from" >&2
        echo "https://github.com/OpenGOAL-Mods/Super-Mario-Legacy/releases/download/v0.0.28/linux-v0.0.28.tar.gz into extern/mario-legacy/ first." >&2
        exit 1
    fi
    if [ ! -e extern/mario-legacy/jak1.iso ]; then
        if [ -e roms/jak1.iso ]; then
            ln -s "$(pwd)/roms/jak1.iso" extern/mario-legacy/jak1.iso
        elif [ -e extern/jak-project/jak1.iso ]; then
            ln -s "$(pwd)/extern/jak-project/jak1.iso" extern/mario-legacy/jak1.iso
        fi
    fi
    if [ ! -e extern/mario-legacy/jak1.iso ]; then
        echo "Error: jak1.iso not found (drop a Jak & Daxter: The Precursor Legacy ISO dump at roms/jak1.iso)." >&2
        exit 1
    fi
    if [ ! -e extern/mario-legacy/baserom.us.z64 ]; then
        if [ -e roms/baserom.us.z64 ]; then
            ln -s "$(pwd)/roms/baserom.us.z64" extern/mario-legacy/baserom.us.z64
        elif [ -e extern/sm64ex-alo/baserom.us.z64 ]; then
            ln -s "$(pwd)/extern/sm64ex-alo/baserom.us.z64" extern/mario-legacy/baserom.us.z64
        fi
    fi
    if [ ! -e extern/mario-legacy/baserom.us.z64 ]; then
        echo "Error: baserom.us.z64 not found (drop a US SM64 ROM dump at roms/baserom.us.z64)." >&2
        exit 1
    fi
    cd extern/mario-legacy
    if [ ! -f data/decompiler_out/jak1/textures/tpage-dir.txt ]; then
        ./extractor -e -d -g jak1 jak1.iso
    fi
    ./goalc -c "(mi)" -g jak1

# Best-effort Windows cross-build of SM64 (WINDOWS_BUILD=1, i686-w64-mingw32
# cross prefix matching this repo's own toolchain), used by mad2companionmod's
# native-Windows branch. sm64ex-alo's Makefile discovers SDL2 via
# sdl2-config, which isn't packaged for a mingw target on this host -- so
# this recipe fetches the official SDL2 mingw devel release once into
# extern/.sdl2-mingw/ and points SDLCONFIG at its bundled sdl2-config
# script. Best-effort per this project's own scoping decision: the
# resulting sm64.exe's real target is native Windows and can only be
# smoke-tested here via `umu-run`; if this cross-build doesn't succeed on a
# given host, the native-Windows branch simply ships untested.
build-sm64-windows:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ ! -e extern/sm64ex-alo/baserom.us.z64 ] && [ -e roms/baserom.us.z64 ]; then
        ln -s "$(pwd)/roms/baserom.us.z64" extern/sm64ex-alo/baserom.us.z64
    fi
    if [ ! -e extern/sm64ex-alo/baserom.us.z64 ]; then
        echo "Error: baserom.us.z64 not found (drop a US SM64 ROM dump at roms/baserom.us.z64)." >&2
        exit 1
    fi
    sdl2_dir="extern/.sdl2-mingw"
    if [ ! -d "$sdl2_dir/i686-w64-mingw32" ]; then
        echo "[just] Fetching SDL2 {{sdl2_mingw_version}} mingw devel package..."
        mkdir -p "$sdl2_dir"
        curl -sL "https://github.com/libsdl-org/SDL/releases/download/release-{{sdl2_mingw_version}}/SDL2-devel-{{sdl2_mingw_version}}-mingw.tar.gz" \
            | tar -xz -C "$sdl2_dir" --strip-components=1 "SDL2-{{sdl2_mingw_version}}/i686-w64-mingw32"
    fi
    sdl2_root="$(pwd)/$sdl2_dir/i686-w64-mingw32"
    # sdl2-config --cflags points at .../include/SDL2 (so plain "SDL.h"
    # resolves), but this codebase's own sources -- controller_udp.c
    # included -- consistently write #include <SDL2/SDL.h>, needing the
    # parent .../include on the search path instead (the Makefile only
    # patches this mismatch for OSX_BUILD). Fixed via a self-referential
    # symlink rather than an extra -I flag on the command line: passing
    # BACKEND_CFLAGS on `make`'s command line would make GNU Make silently
    # ignore every one of the Makefile's own later "BACKEND_CFLAGS += ..."
    # lines for the rest of the build (CAPI_SDL2/HAVE_SDL2/DXGI defines,
    # etc.) -- command-line-set variables reject further modification by
    # the makefile, `+=` included, unless the makefile itself uses
    # `override`. Only SDLCONFIG is set on the command line below, which is
    # safe since the Makefile only ever reads it, never assigns to it.
    [ -L "$sdl2_root/include/SDL2/SDL2" ] || ln -s . "$sdl2_root/include/SDL2/SDL2"
    echo "[just] Cross-compiling sm64ex-alo for Windows (i686-w64-mingw32)..."
    # CC/CXX must be passed explicitly (not just CROSS) -- Make's built-in
    # implicit rules already bind CC=cc/CXX=g++ before the Makefile's own
    # "CC ?= $(CROSS)gcc" line runs, and ?= never overrides an
    # already-bound variable (including make's own built-in defaults), so
    # without this the final link silently falls back to the host g++.
    # BUILD_DIR_BASE must differ from build-sm64-linux's default ("build")
    # too -- upstream's Makefile reuses the same build/us_pc/ output dir for
    # both native Linux and WINDOWS_BUILD ("us_win (to be renamed)", per its
    # own comment), so building both from one tree leaves stale ELF .o
    # files that the Windows link step then fails to read as PE/COFF.
    # RENDER_API=D3D11 (which forces WINDOW_API=DXGI) sidesteps a second
    # missing dependency: the default GL renderer needs glew32, which --
    # unlike SDL2 -- has no readily-fetchable prebuilt mingw-w64 release
    # and isn't packaged for this host either. D3D11/DXGI need nothing
    # beyond what mingw-w64-headers/crt already ships, and it's the more
    # natural renderer for a native-Windows build anyway.
    # TARGET_BITS=32 must also be passed explicitly: the Makefile only
    # infers it from CROSS for the MXE-style "i686-w64-mingw32.static-"
    # prefix, not our plain "i686-w64-mingw32-" one, and crash_handler.c's
    # StackWalk64 use needs the -ldbghelp link that TARGET_BITS=32 gates.
    make -C extern/sm64ex-alo -j{{nproc}} WINDOWS_BUILD=1 CROSS=i686-w64-mingw32- \
        CC=i686-w64-mingw32-gcc CXX=i686-w64-mingw32-g++ TARGET_BITS=32 \
        BUILD_DIR_BASE=build-windows RENDER_API=D3D11 \
        SDLCONFIG="$sdl2_root/bin/sdl2-config"
