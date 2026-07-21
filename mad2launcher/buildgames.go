package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"time"
)

// sm64RomPath/jak1IsoPath are the final per-project ROM/ISO locations
// mad2buildgames (and the justfile's build-sm64-linux/build-jak1-linux
// recipes it mirrors) actually build against. gamesRoot is the directory
// GamesRoot resolves (extern/ + roms/), not necessarily repoRoot -- see its
// own doc comment in build_native.go.
func sm64RomPath(gamesRoot string) string {
	return filepath.Join(gamesRoot, "extern", "sm64ex-alo", "baserom.us.z64")
}

func jak1IsoPath(gamesRoot string) string {
	return filepath.Join(gamesRoot, "extern", "jak-project", "jak1.iso")
}

// sharedRomPath is the single drop location for a ROM/ISO under gamesRoot's
// roms/ folder (see mad2buildgames/main.go's ensureRomLinked) -- the user
// only needs one copy here, not one per extern/* subproject.
func sharedRomPath(gamesRoot, romName string) string {
	return filepath.Join(gamesRoot, "roms", romName)
}

// romAvailable reports whether building against romName can proceed right
// now -- either the per-project location is already a real file/symlink, or
// a copy sits in the shared roms/ drop folder ready for mad2buildgames to
// symlink in on the next build. Used to enable/disable "Build Games"'s
// per-target buttons without requiring the symlink to already exist.
func romAvailable(gamesRoot, romName, finalPath string) bool {
	return fileExists(finalPath) || fileExists(sharedRomPath(gamesRoot, romName))
}

// sm64LinuxBinPath/sm64WindowsBinPath/jak1LinuxBinPath mirror the exact
// output paths launch.go already checks (sm64LinuxBinPath/jak1LinuxBinPath)
// or that sm64ex-alo's own Makefile produces for a Windows cross-build --
// see its WINDOWS_BUILD branch: BUILD_DIR_BASE=build-windows,
// BUILD_DIR=build-windows/us_pc, EXE=$(BUILD_DIR)/$(TARGET).exe where
// TARGET=sm64.us.f3dex2e.
func sm64LinuxBinPath(gamesRoot string) string {
	return filepath.Join(gamesRoot, "extern", "sm64ex-alo", "build", "us_pc", "sm64.us.f3dex2e")
}

func sm64WindowsBinPath(gamesRoot string) string {
	return filepath.Join(gamesRoot, "extern", "sm64ex-alo", "build-windows", "us_pc", "sm64.us.f3dex2e.exe")
}

func jak1LinuxBinPath(gamesRoot string) string {
	return filepath.Join(gamesRoot, "extern", "jak-project", "build", "game", "gk")
}

// buildGamesLogPath is where mad2buildgames' combined output for one run
// lands -- appended to, not truncated, so a failed-then-retried build keeps
// its history. Lives alongside extern/roms/ (gamesRoot), not any single
// install dir.
func buildGamesLogPath(gamesRoot string) string {
	return filepath.Join(gamesRoot, "buildgames.log")
}

// RunBuildGames resolves the prebuilt mad2buildgames binary (see
// ResolveMad2BuildGames -- never built on demand by this launcher) and runs
// it for the given target ("sm64-linux", "sm64-windows", "jak1-linux")
// against gamesRoot (see GamesRoot in build_native.go), appending its
// combined output to buildgames.log there. Building SM64/Jak1 from source is
// the one deliberate exception to "never compile anything at runtime" (see
// build_native.go's file header) -- it's a real, user-triggered, opt-in
// action against a ROM/ISO the user supplies, not this launcher recompiling
// its own tooling.
func RunBuildGames(repoRoot, target string) error {
	gamesRoot := GamesRoot(repoRoot)
	binPath, err := ResolveMad2BuildGames(repoRoot)
	if err != nil {
		return fmt.Errorf("mad2buildgames %w (run `just build-buildgames` first)", err)
	}

	cmd := exec.Command(binPath, "-target", target, "-games-root", gamesRoot)
	cmd.Dir = gamesRoot
	out, runErr := cmd.CombinedOutput()

	logPath := buildGamesLogPath(gamesRoot)
	if f, ferr := os.OpenFile(logPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644); ferr == nil {
		fmt.Fprintf(f, "\n=== %s (target=%s) ===\n", time.Now().Format(time.RFC3339), target)
		f.Write(out)
		if runErr != nil {
			fmt.Fprintf(f, "error: %v\n", runErr)
		}
		f.Close()
	}

	if runErr != nil {
		return fmt.Errorf("building %s failed: %w (see %s)", target, runErr, logPath)
	}
	return nil
}
