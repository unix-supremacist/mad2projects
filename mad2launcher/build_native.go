package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

// goBinaryName appends ".exe" on Windows, mirroring randomizerBinaryName's
// own convention in randomizer.go.
func goBinaryName(base string) string {
	if runtime.GOOS == "windows" {
		return base + ".exe"
	}
	return base
}

// This launcher never shells out to `go build`, `cmake`, or any other
// compiler for any of the companion tools it drives (mad2relauncher,
// mad2rando5, mad2arc/mad2repack, mad2musicpatch, mad2musicdownloader) --
// every one of those is expected to already exist as a prebuilt binary,
// either from a dev `just build-*` run or bundled inside a downloaded
// release's packages/ directory (see deploy-plan.md §3.1's Mod Installer
// and CLAUDE.md's Build commands). A packaged release has no guarantee a Go
// toolchain (or mingw, or anything else used to build this repo) is present
// at runtime. The one deliberate exception is mad2buildgames (see
// buildgames.go), which itself wraps *compiling SM64/Jak1 from a
// user-supplied ROM/ISO* -- that's a real, opt-in "build a game" action the
// user triggers, not this launcher silently recompiling its own tooling.

// exeDir returns the directory containing this launcher's own running
// executable, resolving symlinks -- a packaged release's packages/
// directory (see the Mod Installer feature) is expected to sit right
// alongside it. Returns "" if it can't be determined, in which case
// packageCandidate below simply contributes no candidates.
func exeDir() string {
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	if resolved, err := filepath.EvalSymlinks(exe); err == nil {
		exe = resolved
	}
	return filepath.Dir(exe)
}

// findPrebuilt returns the first candidate path that exists on disk, or an
// error listing every path checked (blank candidates, e.g. from exeDir()
// failing, are silently skipped). Callers pass candidates in priority
// order -- see the packageCandidate/distCandidate/buildCandidate helpers
// below for the three layouts checked (bundled release, packaging script
// output, dev build output).
func findPrebuilt(candidates []string) (string, error) {
	var checked []string
	for _, c := range candidates {
		if c == "" {
			continue
		}
		checked = append(checked, c)
		if fileExists(c) {
			return c, nil
		}
	}
	return "", fmt.Errorf("not found; checked:\n  %s", strings.Join(checked, "\n  "))
}

// packageCandidate builds a candidate path under this launcher's own
// bundled packages/ directory: <exeDir>/packages/<pkgName>/<relPath...>.
// This is where a downloaded release's Mod Installer packages live (see
// deploy-plan.md §3.1) -- a packaged mad2launcher ships every other
// standalone package's contents right alongside itself.
func packageCandidate(pkgName string, relPath ...string) string {
	dir := exeDir()
	if dir == "" {
		return ""
	}
	parts := append([]string{dir, "packages", pkgName}, relPath...)
	return filepath.Join(parts...)
}

// distCandidate builds a candidate path under repoRoot/dist/<platform>/ --
// the packaging script's own staged (pre-archive) output (see
// tools/package.sh / justfile's package-* recipes), useful when testing
// packaging locally without installing next to the launcher binary yet.
func distCandidate(repoRoot, pkgName string, relPath ...string) string {
	platform := "linux"
	if runtime.GOOS == "windows" {
		platform = "windows"
	}
	parts := append([]string{repoRoot, "dist", platform, pkgName}, relPath...)
	return filepath.Join(parts...)
}

// buildCandidate builds a candidate path under repoRoot/build (or
// repoRoot/build/windows when running on Windows) -- plain `just build-*`
// dev output, checked first since it's the fastest-changing/most-trusted
// location on a dev machine.
func buildCandidate(repoRoot string, relPath ...string) string {
	base := filepath.Join(repoRoot, "build")
	if runtime.GOOS == "windows" {
		base = filepath.Join(base, "windows")
	}
	parts := append([]string{base}, relPath...)
	return filepath.Join(parts...)
}

// ResolveMad2Relauncher finds a prebuilt mad2relauncher binary. Linux-only:
// there is no Windows build of mad2relauncher (it wraps umu-run/gamescope/
// X11 -- see deploy-plan.md §3.2/§5), so on Windows this always fails and
// LaunchGame's Windows branch never calls it anyway.
func ResolveMad2Relauncher(repoRoot string) (string, error) {
	return findPrebuilt([]string{
		buildCandidate(repoRoot, "mad2relauncher"),
		packageCandidate("mad2relauncher_linux", "mad2relauncher"),
		distCandidate(repoRoot, "mad2relauncher_linux", "mad2relauncher"),
	})
}

// ResolveMad2AssetTool finds a prebuilt binary (mad2arc/mad2repack/...) from
// the mad2assettools package (deploy-plan.md §1.5).
func ResolveMad2AssetTool(repoRoot, name string) (string, error) {
	bin := goBinaryName(name)
	return findPrebuilt([]string{
		buildCandidate(repoRoot, bin),
		packageCandidate("mad2assettools", bin),
		distCandidate(repoRoot, "mad2assettools", bin),
	})
}

// ResolveMad2MusicPatch finds a prebuilt mad2musicpatch binary. Not part of
// any end-user package (deploy-plan.md never lists it) -- it's a local
// content-prep tool this launcher's Music page drives directly, so a
// packaged release ships it right next to the launcher binary itself
// instead of inside packages/.
func ResolveMad2MusicPatch(repoRoot string) (string, error) {
	bin := goBinaryName("mad2musicpatch")
	dir := exeDir()
	var beside string
	if dir != "" {
		beside = filepath.Join(dir, bin)
	}
	return findPrebuilt([]string{
		buildCandidate(repoRoot, bin),
		beside,
		distCandidate(repoRoot, "mad2launcher", bin),
	})
}

// ResolveMad2MusicDownloader finds a prebuilt mad2musicdownloader binary --
// shipped as part of the mad2music_linux package (deploy-plan.md §2.3's
// downloader/ subfolder).
func ResolveMad2MusicDownloader(repoRoot string) (string, error) {
	bin := goBinaryName("mad2musicdownloader")
	return findPrebuilt([]string{
		buildCandidate(repoRoot, bin),
		packageCandidate("mad2music_linux", "downloader", bin),
		distCandidate(repoRoot, "mad2music_linux", "downloader", bin),
	})
}

// ResolveMad2Rando5 finds a prebuilt mad2rando5 binary from the
// mad2rando_linux/mad2rando_windows package (deploy-plan.md §1.4).
func ResolveMad2Rando5(repoRoot string) (string, error) {
	pkgName := "mad2rando_linux"
	if runtime.GOOS == "windows" {
		pkgName = "mad2rando_windows"
	}
	bin := goBinaryName("mad2rando5")
	return findPrebuilt([]string{
		buildCandidate(repoRoot, bin),
		packageCandidate(pkgName, bin),
		distCandidate(repoRoot, pkgName, bin),
	})
}

// ResolveMad2BuildGames finds a prebuilt mad2buildgames binary (see
// buildgames.go) -- the tool the "Build Games" page shells out to in order
// to compile SM64/Jak1 from a user-supplied ROM/ISO. Native-host-only, no
// package of its own -- ships next to the launcher binary like
// mad2musicpatch above.
func ResolveMad2BuildGames(repoRoot string) (string, error) {
	bin := goBinaryName("mad2buildgames")
	dir := exeDir()
	var beside string
	if dir != "" {
		beside = filepath.Join(dir, bin)
	}
	return findPrebuilt([]string{
		buildCandidate(repoRoot, bin),
		beside,
		distCandidate(repoRoot, "mad2launcher", bin),
	})
}

// GamesRoot returns the directory containing extern/ (SM64/Jak & Daxter
// source) and roms/ (the user-supplied ROM/ISO drop folder both read from --
// see mad2buildgames/main.go's ensureRomLinked) for "Build Games" (§3.1/§3.3
// in deploy-plan.md) to operate against. Checked in order: this launcher's
// own bundled copy (exeDir()/extern -- shipped inside the mad2launcher
// package itself, see tools/package.sh, so "Build Games" works from just a
// launcher download, no full repo checkout needed), falling back to
// repoRoot/extern (this repo's own dev-checkout layout).
func GamesRoot(repoRoot string) string {
	if dir := exeDir(); dir != "" {
		if dirExists(filepath.Join(dir, "extern")) {
			return dir
		}
	}
	return repoRoot
}

// musicBinPath returns the primary (dev build) location this launcher
// expects mad2music (see ../mad2music) to already have been built to --
// <repoRoot>/build/mad2music, the same output path `just build-music-linux`
// produces. mad2music is a native Linux SDL3 executable with its own CMake
// project, not something `go build` can produce, so this launcher never
// builds it -- only locates it. Kept as a single-path helper (rather than
// going through findPrebuilt) since callers want the exact expected path
// even when it's missing, to show in a "not found: <path>" status message.
func musicBinPath(repoRoot string) string {
	return buildCandidate(repoRoot, "mad2music")
}

// ResolveMad2Music finds a prebuilt mad2music binary, checking the packaged
// mad2music_linux package location too (deploy-plan.md §2.3) in addition to
// the dev build path musicBinPath returns.
func ResolveMad2Music(repoRoot string) (string, error) {
	return findPrebuilt([]string{
		musicBinPath(repoRoot),
		packageCandidate("mad2music_linux", "mad2music"),
		distCandidate(repoRoot, "mad2music_linux", "mad2music"),
	})
}
