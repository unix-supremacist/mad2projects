package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

// goBinaryName appends ".exe" on Windows, mirroring randomizerBinaryName's
// own convention in randomizer.go.
func goBinaryName(base string) string {
	if runtime.GOOS == "windows" {
		return base + ".exe"
	}
	return base
}

// ensureGoBinaryBuilt (re)builds a standalone `go build` module in dir --
// used for mad2relauncher/mad2arc/mad2repack, the same "build once, reuse
// after, rebuild if source changed" shape as randomizer.go's own
// ensureRandomizerBuilt (kept separate rather than shared, matching this
// repo's precedent of small self-contained build helpers over one general
// one -- see CLAUDE.md's iat_hook.cpp note). force skips the up-to-date
// check, for an explicit user-triggered rebuild.
//
// mainFile is kept as a parameter for logging/documentation purposes, but
// staleness is judged from EVERY *.go file directly in dir, not just it --
// mad2relauncher in particular is a multi-file package (jak1.go, sm64.go,
// x11.go, ...), and checking only main.go's mtime meant editing any of
// those other files without also touching main.go silently kept using a
// stale binary (observed: real window-focus and window-resize fixes in
// x11.go not taking effect through mad2launcher, despite working when
// built directly).
func ensureGoBinaryBuilt(dir, mainFile, binName string, force bool) (string, error) {
	binPath := filepath.Join(dir, binName)

	if !force {
		if binInfo, err := os.Stat(binPath); err == nil {
			if !anyGoFileNewerThan(dir, binInfo.ModTime()) {
				return binPath, nil
			}
		}
	}

	cmd := exec.Command("go", "build", "-o", binPath, ".")
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("building %s: %w\n%s", binName, err, out)
	}
	return binPath, nil
}

// anyGoFileNewerThan reports whether any *.go file directly in dir (no
// subdirectories -- these are all flat single-package source trees) has a
// modification time after cutoff.
func anyGoFileNewerThan(dir string, cutoff time.Time) bool {
	entries, err := os.ReadDir(dir)
	if err != nil {
		// Can't tell -- err on the side of rebuilding rather than silently
		// running something stale.
		return true
	}
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".go") {
			continue
		}
		info, err := e.Info()
		if err != nil || info.ModTime().After(cutoff) {
			return true
		}
	}
	return false
}

// musicBinPath returns where mad2music (see ../mad2music) is expected to
// already have been built to -- <repoRoot>/build/mad2music, the same output
// path the justfile's build-music-linux recipe produces. mad2launcher no
// longer builds mad2music itself (it's a native Linux SDL3 executable with
// its own CMake project, not something `go build` can produce, and building
// it was pure duplicate logic of that recipe -- see CLAUDE.md's "What this
// is" section and changes.md). It's expected to be built ahead of time via
// `just build-music-linux` and shipped as a binary, same as the
// SM64Challenge/JakChallenge native binaries -- see launch.go's fileExists
// checks for those.
func musicBinPath(repoRoot string) string {
	return filepath.Join(repoRoot, "build", "mad2music")
}
