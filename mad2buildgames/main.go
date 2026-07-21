// mad2buildgames wraps building the two chaos-effect companion games
// (Super Mario 64 via extern/sm64ex-alo, Jak & Daxter via extern/jak-project)
// from a user-supplied ROM/ISO, so mad2launcher's "Build Games" page can
// shell out to one prebuilt binary instead of requiring `just`/bash on the
// end user's machine. This mirrors the exact steps the justfile's own
// build-sm64-linux/build-sm64-windows/build-jak1-linux recipes run --
// see this repo's CLAUDE.md for why those two games can't be prebuilt and
// shipped directly (copyrighted ROM/ISO assets, extracted locally).
//
// Deliberately does NOT cover extern/mario-legacy (Super Mario Legacy) --
// left out of packaging/Build Games for now, unlike SM64/Jak1.
package main

import (
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

const sdl2MingwVersion = "2.32.10"

func main() {
	target := flag.String("target", "", "sm64-linux | sm64-windows | jak1-linux")
	gamesRoot := flag.String("games-root", "", "directory containing extern/ (SM64/Jak1 source) and roms/ (user-supplied ROM/ISO)")
	flag.Parse()

	if *gamesRoot == "" {
		fmt.Fprintln(os.Stderr, "error: -games-root is required")
		os.Exit(2)
	}
	abs, err := filepath.Abs(*gamesRoot)
	if err != nil {
		fmt.Fprintln(os.Stderr, "error: resolving -games-root:", err)
		os.Exit(2)
	}

	var buildErr error
	switch *target {
	case "sm64-linux":
		buildErr = buildSM64Linux(abs)
	case "sm64-windows":
		buildErr = buildSM64Windows(abs)
	case "jak1-linux":
		buildErr = buildJak1Linux(abs)
	default:
		fmt.Fprintln(os.Stderr, "error: -target must be one of: sm64-linux, sm64-windows, jak1-linux")
		os.Exit(2)
	}
	if buildErr != nil {
		fmt.Fprintln(os.Stderr, "error:", buildErr)
		os.Exit(1)
	}
	fmt.Println("[mad2buildgames] done.")
}

func fileExists(path string) bool {
	st, err := os.Stat(path)
	return err == nil && !st.IsDir()
}

func dirExists(path string) bool {
	st, err := os.Stat(path)
	return err == nil && st.IsDir()
}

// run executes name with args, streaming its stdout/stderr straight through
// to this process's own -- mad2launcher captures this tool's combined
// output to a log file (same pattern as its other companion-tool calls),
// so real-time progress from `make`/`task` reaches the user via that log.
func run(dir, name string, args ...string) error {
	fmt.Printf("[mad2buildgames] + %s %s\n", name, strings.Join(args, " "))
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

// ensureRomLinked makes sure destPath exists, symlinking it from
// <gamesRoot>/roms/<romName> if it doesn't already -- lets the user drop
// one copy of baserom.us.z64/jak1.iso in the single shared roms/ directory
// instead of duplicating it into every extern/* subproject that needs it
// (mirrors what the justfile's own recipes now do -- see build-sm64-linux/
// build-jak1-linux for the bash equivalent of this same symlink dance).
func ensureRomLinked(gamesRoot, romName, destPath string) error {
	if fileExists(destPath) {
		return nil // already a real file or a working symlink
	}
	src := filepath.Join(gamesRoot, "roms", romName)
	if !fileExists(src) {
		return fmt.Errorf("%s not found (drop your own copy at %s)", destPath, src)
	}
	if err := os.MkdirAll(filepath.Dir(destPath), 0755); err != nil {
		return err
	}
	if err := os.Symlink(src, destPath); err != nil {
		// Fall back to a copy if symlinking isn't possible on this
		// filesystem -- rare, but cheap to handle.
		in, openErr := os.Open(src)
		if openErr != nil {
			return openErr
		}
		defer in.Close()
		out, createErr := os.Create(destPath)
		if createErr != nil {
			return createErr
		}
		defer out.Close()
		_, copyErr := io.Copy(out, in)
		return copyErr
	}
	return nil
}

// hasDistrobox reports whether a distrobox container named name exists --
// mirrors the justfile's own `distrobox list 2>/dev/null | grep -q name`
// check, preferred (when present) for build-sm64-linux/build-jak1-linux
// since that container's toolchain has proven more reliable than building
// directly on an Arch-based host for this repo.
func hasDistrobox(name string) bool {
	out, err := exec.Command("distrobox", "list").Output()
	if err != nil {
		return false
	}
	return strings.Contains(string(out), name)
}

// buildSM64Linux mirrors justfile's build-sm64-linux recipe.
func buildSM64Linux(gamesRoot string) error {
	sm64Dir := filepath.Join(gamesRoot, "extern", "sm64ex-alo")
	rom := filepath.Join(sm64Dir, "baserom.us.z64")
	if err := ensureRomLinked(gamesRoot, "baserom.us.z64", rom); err != nil {
		return err
	}

	nproc := fmt.Sprintf("-j%d", runtime.NumCPU())
	if hasDistrobox("mad2-debian") {
		fmt.Println("[mad2buildgames] Compiling sm64ex-alo (Linux) inside mad2-debian distrobox...")
		return run(gamesRoot, "distrobox", "enter", "mad2-debian", "--", "make", "-C", sm64Dir, nproc)
	}
	fmt.Println("[mad2buildgames] distrobox mad2-debian not found, compiling directly on host...")
	return run(gamesRoot, "make", "-C", sm64Dir, nproc)
}

// buildSM64Windows mirrors justfile's build-sm64-windows recipe: fetches a
// pinned SDL2 mingw devel release (no mingw-w64 SDL2 package exists for
// this host), symlinks around the sm64 codebase's #include <SDL2/SDL.h>
// vs. sdl2-config's -I.../include/SDL2 mismatch, then cross-compiles via
// i686-w64-mingw32-{gcc,g++} with RENDER_API=D3D11 (sidesteps needing
// glew32, which has no readily-fetchable mingw release either). See the
// justfile recipe's own extensive comments for why each of these flags is
// required exactly as written -- ported verbatim, not simplified.
func buildSM64Windows(gamesRoot string) error {
	sm64Dir := filepath.Join(gamesRoot, "extern", "sm64ex-alo")
	rom := filepath.Join(sm64Dir, "baserom.us.z64")
	if err := ensureRomLinked(gamesRoot, "baserom.us.z64", rom); err != nil {
		return err
	}

	sdl2Dir := filepath.Join(gamesRoot, "extern", ".sdl2-mingw")
	sdl2Root := filepath.Join(sdl2Dir, "i686-w64-mingw32")
	if !dirExists(sdl2Root) {
		fmt.Printf("[mad2buildgames] Fetching SDL2 %s mingw devel package...\n", sdl2MingwVersion)
		if err := os.MkdirAll(sdl2Dir, 0755); err != nil {
			return err
		}
		url := fmt.Sprintf("https://github.com/libsdl-org/SDL/releases/download/release-%s/SDL2-devel-%s-mingw.tar.gz", sdl2MingwVersion, sdl2MingwVersion)
		innerPath := fmt.Sprintf("SDL2-%s/i686-w64-mingw32", sdl2MingwVersion)
		pipeline := fmt.Sprintf("curl -sL %s | tar -xz -C %s --strip-components=1 %s",
			shellQuote(url), shellQuote(sdl2Dir), shellQuote(innerPath))
		if err := run(gamesRoot, "sh", "-c", pipeline); err != nil {
			return fmt.Errorf("fetching SDL2 mingw devel package: %w", err)
		}
	}

	// [ -L path ] || ln -s . path -- self-referential symlink so plain
	// #include <SDL2/SDL.h> (this codebase's own convention) resolves
	// against sdl2-config's --cflags -I.../include/SDL2.
	symlinkPath := filepath.Join(sdl2Root, "include", "SDL2", "SDL2")
	if fi, err := os.Lstat(symlinkPath); err != nil || fi.Mode()&os.ModeSymlink == 0 {
		if err := os.Symlink(".", symlinkPath); err != nil && !os.IsExist(err) {
			return fmt.Errorf("creating self-referential SDL2 symlink: %w", err)
		}
	}

	fmt.Println("[mad2buildgames] Cross-compiling sm64ex-alo for Windows (i686-w64-mingw32)...")
	nproc := fmt.Sprintf("-j%d", runtime.NumCPU())
	args := []string{
		"-C", sm64Dir, nproc,
		"WINDOWS_BUILD=1",
		"CROSS=i686-w64-mingw32-",
		"CC=i686-w64-mingw32-gcc",
		"CXX=i686-w64-mingw32-g++",
		"TARGET_BITS=32",
		"BUILD_DIR_BASE=build-windows",
		"RENDER_API=D3D11",
		"SDLCONFIG=" + filepath.Join(sdl2Root, "bin", "sdl2-config"),
	}
	return run(gamesRoot, "make", args...)
}

// buildJak1Linux mirrors justfile's build-jak1-linux recipe: the `task`
// runner (go-task) drives extern/jak-project's own CMake+ninja build and
// asset extraction/GOAL compile steps.
func buildJak1Linux(gamesRoot string) error {
	jak1Dir := filepath.Join(gamesRoot, "extern", "jak-project")
	iso := filepath.Join(jak1Dir, "jak1.iso")
	if err := ensureRomLinked(gamesRoot, "jak1.iso", iso); err != nil {
		return err
	}

	useDistrobox := hasDistrobox("mad2-debian")
	if useDistrobox {
		fmt.Println("[mad2buildgames] Building jak-project (Linux) inside mad2-debian distrobox...")
	} else {
		fmt.Println("[mad2buildgames] distrobox mad2-debian not found, building directly on host...")
	}
	runTask := func(args ...string) error {
		if useDistrobox {
			full := append([]string{"enter", "mad2-debian", "--", "task"}, args...)
			return run(jak1Dir, "distrobox", full...)
		}
		return run(jak1Dir, "task", args...)
	}

	if err := runTask("set-game-jak1"); err != nil {
		return err
	}
	if err := runTask("set-decomp-ntscv1"); err != nil {
		return err
	}
	if !dirExists(filepath.Join(jak1Dir, "build")) {
		if err := runTask("gen-cmake-release"); err != nil {
			return err
		}
	}
	if err := runTask("build-release"); err != nil {
		return err
	}

	// The GOAL compiler doesn't create its own output directory tree --
	// nothing in Taskfile.yml does either, so a truly fresh checkout (no
	// prior manual run) fails outright with "Error: failed to write
	// .../out/jak1/iso/JUB.DGO" the first time (mi) tries to write there.
	// This repo's own extern/jak-project only ever avoided the bug because
	// this tree was created once by hand a long time ago and just carried
	// forward since -- confirmed by testing an actually-fresh checkout.
	for _, sub := range []string{"fr3", "iso", "obj"} {
		if err := os.MkdirAll(filepath.Join(jak1Dir, "out", "jak1", sub), 0755); err != nil {
			return fmt.Errorf("creating out/jak1/%s: %w", sub, err)
		}
	}

	// runBin runs a binary task build-release just produced the same way it
	// was built (inside mad2-debian if that's what built it) -- its own
	// RUNPATH is baked in from inside that build environment. distrobox
	// bind-mounts $HOME at the same path inside the container, but anything
	// OUTSIDE $HOME only appears under a /run/host/<path> prefix -- so a
	// binary built under e.g. /tmp/somewhere gets a RUNPATH of
	// /run/host/tmp/somewhere/..., which doesn't exist if you then run that
	// binary directly on the host. Running it back inside the same
	// container sidesteps this entirely. (Confirmed: running extractor
	// directly on the host after a distrobox build failed with "libcompiler
	// .so: cannot open shared object file" for exactly this reason.)
	runBin := func(name string, args ...string) error {
		if useDistrobox {
			full := append([]string{"enter", "mad2-debian", "--", name}, args...)
			return run(jak1Dir, "distrobox", full...)
		}
		return run(jak1Dir, name, args...)
	}

	// Extraction isn't incremental and takes ~30s -- skip it once
	// decompiler_out/ already exists, matching the marker-file check the
	// old external jak1nolauncher/run.go and the justfile recipe both use.
	marker := filepath.Join(jak1Dir, "decompiler_out", "jak1", "textures", "tpage-dir.txt")
	if !fileExists(marker) {
		if err := runBin(filepath.Join(".", "build", "decompiler", "extractor"), "-e", "-d", "-g", "jak1", "jak1.iso"); err != nil {
			return err
		}
	}
	return runBin(filepath.Join(".", "build", "goalc", "goalc"), "-c", "(mi)", "-g", "jak1")
}

func shellQuote(s string) string {
	return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'"
}
