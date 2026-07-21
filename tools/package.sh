#!/usr/bin/env bash
# Packages every distributable piece of this repo into dist/linux/<pkg>/ and
# dist/windows/<pkg>/ directories matching deploy-plan.md's §1-3 package
# layouts, then archives each one (tar.gz for linux, zip for windows)
# without deleting the staged directories -- mad2launcher's Mod Installer
# (mad2launcher/modinstaller.go's ResolvePackagesRoot) reads packages
# straight out of dist/<platform>/ during local testing, and a real release
# copies those same directories into mad2launcher_<platform>/packages/ (see
# stage_launcher_packages below).
#
# Deliberately excludes: SM64/Jak & Daxter/Mario Legacy game *binaries*
# (built locally by the end user via the launcher's "Build Games" page or
# `just build-sm64-linux`/`build-jak1-linux` -- never prebuilt/redistributed,
# see CLAUDE.md and mad2buildgames/), mad2savegui's Windows build (Fyne/cgo,
# same reasoning as mad2launcher.exe below), and Mario Legacy entirely (not
# packaged for now, see CLAUDE.md). It DOES bundle SM64/Jak1's buildable
# *source* (git-tracked files only -- see stage_game_source below) into the
# mad2launcher package, so "Build Games" has something to compile against
# after just downloading that one package, not a full git checkout.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$REPO_ROOT/build"
DIST="$REPO_ROOT/dist"

cd "$REPO_ROOT"

log() { echo "[package] $*"; }
warn() { echo "[package] WARNING: $*" >&2; }

# --- 1. Build everything this script might stage. ------------------------
# Every one of these is a plain `just` recipe already exercised by CLAUDE.md/
# the justfile itself -- this script never invokes a compiler directly, it
# only decides what to *do* with the outputs. Non-fatal: a missing optional
# toolchain (e.g. no mingw Fyne cross-build yet) degrades that one package
# to "skipped" rather than aborting the whole run.
log "Building mingw DLL mods (just build)..."
just build || { warn "just build failed -- DLL mod packages will be incomplete"; }

log "Building native Go tools..."
just build-relauncher
just fetch-umu-run || warn "fetching umu-run failed -- mad2relauncher_linux package will need the user's own system umu-launcher install"
just build-mad2rando5
just build-assettools
just build-mad2musicpatch
just build-music-downloader
just build-buildgames
just build-launcher

log "Cross-building Go tools for Windows (pure Go, no cgo)..."
just build-mad2rando5-windows
just build-assettools-windows
( cd mad2musicpatch && GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -o "$BUILD/windows/mad2musicpatch.exe" . ) \
    || warn "mad2musicpatch Windows cross-build failed"
( cd mad2buildgames && GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -o "$BUILD/windows/mad2buildgames.exe" . ) \
    || warn "mad2buildgames Windows cross-build failed"
( cd mad2music/downloader && GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -o "$BUILD/windows/mad2musicdownloader.exe" . ) \
    || warn "mad2musicdownloader Windows cross-build failed"

log "Building mad2music (native Linux; Windows cross-build best-effort)..."
just build-music-linux || warn "mad2music Linux build failed (needs sdl3 via pkg-config)"
just build-music-windows || warn "mad2music Windows cross-build failed (best-effort, see justfile)"

log "Attempting mad2launcher Windows cross-build (best-effort, Fyne/cgo)..."
just build-launcher-windows || warn "mad2launcher Windows build unavailable -- mad2launcher_windows package will be skipped"

# --- 2. Staging helpers. ---------------------------------------------------
rm -rf "$DIST"
mkdir -p "$DIST/linux" "$DIST/windows"

# stage_common copies files/dirs that are IDENTICAL on both platforms (the
# mingw-cross-compiled Windows-PE DLL mods run the same whether loaded
# inside Wine/Proton or on real Windows -- see deploy-plan.md §1.1-§1.3,
# which document these as one combined "Linux (Wine/Proton) & Windows
# (Native)" layout, not two different builds) into both dist/linux/<pkg>
# and dist/windows/<pkg>.
stage_common() {
    local pkg="$1"; shift
    mkdir -p "$DIST/linux/$pkg" "$DIST/windows/$pkg"
    for spec in "$@"; do
        local src="${spec%%::*}" rel="${spec##*::}"
        if [ ! -e "$src" ]; then
            warn "$pkg: missing $src, skipping"
            continue
        fi
        mkdir -p "$(dirname "$DIST/linux/$pkg/$rel")" "$(dirname "$DIST/windows/$pkg/$rel")"
        cp -a "$src" "$DIST/linux/$pkg/$rel"
        cp -a "$src" "$DIST/windows/$pkg/$rel"
    done
}

# stage_platform copies files/dirs that differ per platform (native
# executables) into dist/<platform>/<pkg> only. spec is "src::relpath"; a
# missing src just warns and skips that one file (so e.g. a missing
# mad2savegui.exe on Windows doesn't abort the whole mad2assettools package).
stage_platform() {
    local platform="$1" pkg="$2"; shift 2
    mkdir -p "$DIST/$platform/$pkg"
    for spec in "$@"; do
        local src="${spec%%::*}" rel="${spec##*::}"
        if [ ! -e "$src" ]; then
            warn "$pkg ($platform): missing $src, skipping"
            continue
        fi
        mkdir -p "$(dirname "$DIST/$platform/$pkg/$rel")"
        cp -a "$src" "$DIST/$platform/$pkg/$rel"
    done
}

# --- 3. Packages identical on both platforms (deploy-plan.md §1.1-§1.3, --
# §1.1a, §2.1, §2.4, §2.5). All sourced from build/dist/mods (see
# cmake/add_mad2_mod.cmake's flat RUNTIME_OUTPUT_DIRECTORY staging).
MODS="$BUILD/dist/mods"

log "Staging mad2modloader (core + shared-API dependency mods)..."
# version.dll lands in build/dist/mods/, not build/dist/, despite
# modloader/CMakeLists.txt passing RUNTIME_SUBDIR "" -- cmake_parse_arguments
# doesn't distinguish "keyword given an empty-string value" from "keyword
# omitted" here, so add_mad2_mod.cmake's "mods" default wins. Harmless for
# `deploy-mad2` (its destination path is hardcoded to the game root
# independent of this), but means this script has to source from the actual
# location, not the one add_mad2_mod.cmake's own comment describes.
stage_common mad2modloader \
    "$MODS/version.dll::version.dll" \
    "$MODS/aa_mad2hookutil.dll::mods/aa_mad2hookutil.dll" \
    "$MODS/mad2textrenderer.dll::mods/mad2textrenderer.dll" \
    "$MODS/shadowfix.dll::mods/shadowfix.dll" \
    "$MODS/mad2assetloader.dll::mods/mad2assetloader.dll" \
    "$MODS/mad2saveloader.dll::mods/mad2saveloader.dll" \
    "$MODS/mad2xinput.dll::mods/mad2xinput.dll" \
    "$MODS/mad2config.dll::mods/mad2config.dll"

log "Staging mad2cheatapply..."
stage_common mad2cheatapply \
    "$MODS/mad2cheatapply.dll::mods/mad2cheatapply.dll"

log "Staging mad2effects (chaos-effect registry + effects, no external process)..."
stage_common mad2effects \
    "$MODS/mad2effects.dll::mods/mad2effects.dll" \
    "$MODS/mad2inputeffectsmod.dll::mods/mad2inputeffectsmod.dll" \
    "$MODS/zz_mad2graphicseffectmod.dll::mods/zz_mad2graphicseffectmod.dll" \
    "$MODS/mad2gameeffectsmod.dll::mods/mad2gameeffectsmod.dll" \
    "$MODS/mad2effectshud.dll::mods/mad2effectshud.dll" \
    "$MODS/mad2companionmod.dll::mods/mad2companionmod.dll" \
    "$REPO_ROOT/mad2gameeffectsmod/data/coordinates.yaml::coordinates.yaml"

log "Staging mad2speedrunning (overlays/timers)..."
stage_common mad2speedrunning \
    "$MODS/igttimer.dll::mods/igttimer.dll" \
    "$MODS/inputdisplay.dll::mods/inputdisplay.dll" \
    "$MODS/twitchchat.dll::mods/twitchchat.dll"
mkdir -p "$DIST/linux/mad2speedrunning/emotes" "$DIST/windows/mad2speedrunning/emotes"
cat > "$DIST/linux/mad2speedrunning/CREDITS_KENNEY.txt" <<'EOF'
Controller button/stick glyphs used by mad2inputdisplay are adapted from the
Kenney Input Prompts pack (kenney_input-prompts_1.5.zip, embedded at build
time -- see tools/gen_embedded_assets.py), by Kenney (www.kenney.nl),
released under CC0 1.0 Universal. See LICENSE_KENNEY.txt.
EOF
cp "$DIST/linux/mad2speedrunning/CREDITS_KENNEY.txt" "$DIST/windows/mad2speedrunning/CREDITS_KENNEY.txt"
cat > "$DIST/linux/mad2speedrunning/LICENSE_KENNEY.txt" <<'EOF'
CC0 1.0 Universal

To the extent possible under law, Kenney (www.kenney.nl) has waived all
copyright and related or neighboring rights to the Kenney Input Prompts
asset pack. This work is published from the Netherlands.

Full legal text: https://creativecommons.org/publicdomain/zero/1.0/legalcode
EOF
cp "$DIST/linux/mad2speedrunning/LICENSE_KENNEY.txt" "$DIST/windows/mad2speedrunning/LICENSE_KENNEY.txt"

log "Staging chaosmod (director loop)..."
stage_common chaosmod \
    "$MODS/mad2chaosmod.dll::mods/mad2chaosmod.dll"

log "Staging mirrormod (persistent Mirror Mode toggle)..."
stage_common mirrormod \
    "$MODS/zz_mad2mirrormod.dll::mods/zz_mad2mirrormod.dll"

log "Staging mad2twitchcontrols..."
stage_common mad2twitchcontrols \
    "$MODS/mad2twitchcontrolsmod.dll::mods/mad2twitchcontrolsmod.dll"

log "Staging playercoords..."
stage_common playercoords \
    "$MODS/playercoords.dll::mods/playercoords.dll"

# --- 4. Platform-specific packages (native executables). ------------------

log "Staging mad2rando (level randomizer + interceptor mod)..."
stage_platform linux mad2rando_linux \
    "$BUILD/mad2rando5::mad2rando5" \
    "$MODS/mad2levelredirectmod.dll::mods/mad2levelredirectmod.dll"
stage_platform windows mad2rando_windows \
    "$BUILD/windows/mad2rando5.exe::mad2rando5.exe" \
    "$MODS/mad2levelredirectmod.dll::mods/mad2levelredirectmod.dll"

log "Staging mad2assettools (archive/save editing CLI tools)..."
stage_platform linux mad2assettools \
    "$BUILD/mad2arc::mad2arc" \
    "$BUILD/mad2repack::mad2repack" \
    "$BUILD/mad2tool::mad2tool" \
    "$BUILD/mad2save::mad2save" \
    "$BUILD/mad2savegui::mad2savegui" \
    "$BUILD/mad2transitions::mad2transitions"
stage_platform windows mad2assettools \
    "$BUILD/windows/mad2arc.exe::mad2arc.exe" \
    "$BUILD/windows/mad2repack.exe::mad2repack.exe" \
    "$BUILD/windows/mad2tool.exe::mad2tool.exe" \
    "$BUILD/windows/mad2save.exe::mad2save.exe" \
    "$BUILD/windows/mad2transitions.exe::mad2transitions.exe"
# mad2savegui has no Windows build (Fyne/cgo -- same reasoning as
# mad2launcher.exe, see build-launcher-windows and deploy-plan.md §4.2).

log "Staging mad2music (Linux-native playback + best-effort Windows build)..."
stage_platform linux mad2music_linux \
    "$BUILD/mad2music::mad2music" \
    "$BUILD/mad2musicdownloader::downloader/mad2musicdownloader" \
    "$MODS/mad2podcastmod.dll::mods/mad2podcastmod.dll" \
    "$MODS/mad2musichud.dll::mods/mad2musichud.dll"
mkdir -p "$DIST/linux/mad2music_linux/audio"
if [ -f "$BUILD/windows/mad2music.exe" ]; then
    stage_platform windows mad2music_windows \
        "$BUILD/windows/mad2music.exe::mad2music.exe" \
        "$BUILD/windows/SDL3.dll::SDL3.dll" \
        "$BUILD/windows/mad2musicdownloader.exe::downloader/mad2musicdownloader.exe" \
        "$MODS/mad2podcastmod.dll::mods/mad2podcastmod.dll" \
        "$MODS/mad2musichud.dll::mods/mad2musichud.dll"
    mkdir -p "$DIST/windows/mad2music_windows/audio"
else
    warn "mad2music.exe not built -- mad2music_windows package skipped (mad2music is Linux-only per CLAUDE.md unless build-music-windows succeeded)"
fi

log "Staging mad2relauncher (Linux-only, wraps umu-run/gamescope/X11)..."
# umu-run ships right beside mad2relauncher -- gameloop.go's resolveUmuRun
# prefers a copy found beside its own binary over one on PATH, so this is
# what lets a downloaded release work without the user separately
# installing umu-launcher system-wide (gamescope is still a system
# dependency -- no portable distribution of that exists to bundle).
stage_platform linux mad2relauncher_linux \
    "$BUILD/mad2relauncher::mad2relauncher" \
    "$BUILD/umu-run::umu-run"

log "Staging mad2launcher (GUI, bundles copies of every package above)..."
stage_platform linux mad2launcher_linux \
    "$BUILD/mad2launcher::mad2launcher" \
    "$BUILD/mad2buildgames::mad2buildgames" \
    "$BUILD/mad2musicpatch::mad2musicpatch"
if [ -f "$BUILD/windows/mad2launcher.exe" ]; then
    stage_platform windows mad2launcher_windows \
        "$BUILD/windows/mad2launcher.exe::mad2launcher.exe"
    [ -f "$BUILD/windows/mad2buildgames.exe" ] && cp -a "$BUILD/windows/mad2buildgames.exe" "$DIST/windows/mad2launcher_windows/mad2buildgames.exe"
    [ -f "$BUILD/windows/mad2musicpatch.exe" ] && cp -a "$BUILD/windows/mad2musicpatch.exe" "$DIST/windows/mad2launcher_windows/mad2musicpatch.exe"
else
    warn "mad2launcher.exe not built -- mad2launcher_windows package skipped (run 'just build-launcher-windows')"
fi

# stage_game_source copies pkgname's git-tracked files (respecting its own
# .gitignore exclusions -- no build/ output, no ROM/ISO, no fetched
# third-party deps) from extern/<pkgname> into dest/extern/<pkgname>, so
# mad2buildgames (bundled alongside mad2launcher above) actually has SM64/
# Jak1 source to compile after a user downloads *just* the launcher
# package -- without this, "Build Games" would have nothing to build from
# outside a full git checkout of this repo. `cp --parents` (run with cwd =
# REPO_ROOT) preserves each file's "extern/<pkgname>/..." path verbatim
# under dest, which is exactly the layout GamesRoot/mad2buildgames expect.
stage_game_source() {
    local pkgname="$1" dest="$2"
    ( cd "$REPO_ROOT" && git ls-files -z "extern/$pkgname" | xargs -0 cp --parents -t "$dest" -- )
}

# stage_jak_extras additionally copies specific paths under
# extern/jak-project that its own (upstream) .gitignore excludes for good
# reason in general (build output, copyrighted-ROM-extracted content
# regenerated at build/extract time) but that also, as an unintended side
# effect, swallow a couple of genuinely-needed non-generated paths:
#   - third-party/ (~205MB): a hard, non-regenerable CMake dependency --
#     CMakeLists.txt add_subdirectory()s a dozen+ libraries straight out of
#     it (fmt, curl, SDL, tree-sitter, sqlite3, replxx, cubeb, libco, zstd,
#     ...). Without it CMake's configure step fails outright.
#   - goal_src/jak1/build/*.json (~530KB): decompiler object-list config
#     data (all_objs.json / all_objs_jak1_{jp,pal}.json), caught by the same
#     bare "build/" gitignore pattern that (correctly) excludes the real
#     top-level build/ output directory -- that pattern isn't anchored to
#     the repo root, so it also matches this unrelated nested build/ path.
#     Without these, the decompiler fails outright too ("couldn't open
#     .../all_objs_jak1_jp.json").
# Verified end-to-end: built Jak & Daxter from an isolated copy containing
# only what stage_game_source + this function ship, confirmed it gets past
# both failure points and compiles/extracts normally.
stage_jak_extras() {
    local dest="$1"
    for rel in third-party goal_src/jak1/build; do
        local src="$REPO_ROOT/extern/jak-project/$rel"
        if [ ! -e "$src" ]; then
            warn "extern/jak-project/$rel not found on disk -- packaged Jak & Daxter source will be incomplete. See extern/jak-project's own upstream docs for how to populate it."
            continue
        fi
        mkdir -p "$dest/extern/jak-project/$(dirname "$rel")"
        cp -a "$src" "$dest/extern/jak-project/$rel"
    done
}

if [ -d "$DIST/linux/mad2launcher_linux" ]; then
    log "Bundling SM64/Jak1 source into mad2launcher_linux (so Build Games has something to compile without a full git checkout)..."
    stage_game_source sm64ex-alo "$DIST/linux/mad2launcher_linux"
    stage_game_source jak-project "$DIST/linux/mad2launcher_linux"
    stage_jak_extras "$DIST/linux/mad2launcher_linux"
    mkdir -p "$DIST/linux/mad2launcher_linux/roms"
    cp "$REPO_ROOT/roms/README.md" "$DIST/linux/mad2launcher_linux/roms/README.md"
fi
if [ -d "$DIST/windows/mad2launcher_windows" ]; then
    log "Bundling SM64 source into mad2launcher_windows (no Jak & Daxter Windows build exists -- see deploy-plan.md)..."
    stage_game_source sm64ex-alo "$DIST/windows/mad2launcher_windows"
    mkdir -p "$DIST/windows/mad2launcher_windows/roms"
    cp "$REPO_ROOT/roms/README.md" "$DIST/windows/mad2launcher_windows/roms/README.md"
fi

# stage_launcher_packages copies every OTHER staged package for platform
# into mad2launcher_<platform>'s own packages/ subfolder -- this is what
# ResolvePackagesRoot's exeDir()/packages candidate reads once this whole
# mad2launcher_<platform>/ directory is unpacked as a release (see
# mad2launcher/build_native.go and modinstaller.go).
stage_launcher_packages() {
    # Two separate `local` statements, not one `local a=$1 b=$a` -- bash
    # expands every word of a single local/declare command before any of
    # that command's assignments take effect, so a same-statement reference
    # to an earlier variable in the same list is unbound under `set -u`.
    local platform="$1"
    local launcher_pkg="mad2launcher_$platform"
    [ -d "$DIST/$platform/$launcher_pkg" ] || return 0
    local dest="$DIST/$platform/$launcher_pkg/packages"
    mkdir -p "$dest"
    for pkgdir in "$DIST/$platform"/*/; do
        local name
        name="$(basename "$pkgdir")"
        [ "$name" = "$launcher_pkg" ] && continue
        cp -a "$pkgdir" "$dest/$name"
    done
}
log "Bundling packages/ into mad2launcher_linux and mad2launcher_windows..."
stage_launcher_packages linux
stage_launcher_packages windows

# --- 5. Archive every staged package (tar.gz linux, zip windows) without --
# deleting the plain directories, so both forms are available afterward.
log "Archiving..."
for pkgdir in "$DIST/linux"/*/; do
    name="$(basename "$pkgdir")"
    ( cd "$DIST/linux" && tar -czf "${name}.tar.gz" "$name" )
done
for pkgdir in "$DIST/windows"/*/; do
    name="$(basename "$pkgdir")"
    if command -v zip >/dev/null; then
        ( cd "$DIST/windows" && zip -qr "${name}.zip" "$name" )
    else
        warn "zip not found on PATH -- ${name}.zip not created (directory still staged at dist/windows/$name)"
    fi
done

log "Done. See $DIST/linux/*.tar.gz and $DIST/windows/*.zip (plain directories kept alongside)."
