package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
)

// LaunchGame starts inst under the given settings. On Windows this is just
// "run the exe" -- there's no Proton/Wine layer to configure, and gamescope
// doesn't exist there, so none of the Linux-only machinery below applies.
// On Linux it reproduces the justfile's `_play` recipe: a shared Proton
// wineprefix, WINEDLLOVERRIDES=version=n,b (so Wine prefers *our*
// version.dll mod loader over its builtin one -- see CLAUDE.md's "Load
// chain" section), and gamescope wrapping mad2relauncher (which itself
// wraps umu-run) for presentation options (resolution/scaling/FPS cap) plus
// SM64/Jak1 orchestration and background music -- see buildInnerLaunchCommand.
// Returns non-fatal warnings (e.g. "mad2music couldn't be built, launching
// without music") alongside any fatal error.
func LaunchGame(inst Install, s InstallSettings) ([]string, error) {
	if runtime.GOOS == "windows" {
		cmd := exec.Command(filepath.Join(inst.Dir, inst.Exe))
		cmd.Dir = inst.Dir
		return nil, cmd.Start()
	}
	return launchLinux(inst, s)
}

func launchLinux(inst Install, s InstallSettings) ([]string, error) {
	gamescopePath, err := exec.LookPath("gamescope")
	if err != nil {
		return nil, fmt.Errorf("gamescope not found on PATH: %w", err)
	}
	if _, err := exec.LookPath("umu-run"); err != nil {
		return nil, fmt.Errorf("umu-run not found on PATH: %w", err)
	}

	protonPath, err := ResolveProtonPath(s.ProtonPath)
	if err != nil {
		return nil, err
	}

	winePrefix := s.WinePrefix
	if winePrefix == "" {
		winePrefix = defaultWinePrefix(inst)
	}
	if err := os.MkdirAll(winePrefix, 0755); err != nil {
		return nil, fmt.Errorf("creating wineprefix: %w", err)
	}

	// Best-effort cleanup of stale instances from a previous crashed run,
	// same processes the justfile's _play recipe kills before launching.
	_ = exec.Command("killall", "-9", inst.Exe, "wineserver", "winedevice.exe", "gamescopereaper",
		"mad2relauncher", "sm64.us.f3dex2e", "gk", "mad2music").Run()

	inW, inH := splitRes(s.InputRes)
	outW, outH := splitRes(s.OutputRes)

	// --backend sdl: without it, gamescope auto-detects its newer native-
	// Wayland-nested mode whenever WAYLAND_DISPLAY is set (as it is under a
	// Wayland desktop session), which doesn't create a real X11 window on
	// the outer display at all and, worse, doesn't cleanly honor a window
	// manager's close request (Alt+F4) -- observed and confirmed as a
	// hang/freeze (with mad2music surviving into an already-gone session)
	// instead of a clean exit. Forcing the SDL backend (paired with
	// SDL_VIDEODRIVER=x11 in cmd.Env below, for gamescope's own outer
	// window -- its nested children get their own copy via jak1.go/sm64.go)
	// makes gamescope present as a conventional window that responds to a
	// real WM_DELETE_WINDOW the normal way -- verified directly: sending
	// one to a --backend sdl gamescope window closed its entire child tree
	// cleanly, no freeze, no orphans.
	args := []string{"--backend", "sdl", "-w", inW, "-h", inH, "-W", outW, "-H", outH}
	if fps, err := strconv.Atoi(s.FpsCap); err == nil && fps > 0 {
		args = append(args, "-r", strconv.Itoa(fps), "--framerate-limit", strconv.Itoa(fps))
	}
	switch s.Scaling {
	case "FSR":
		args = append(args, "-F", "fsr")
		if s.FsrSharpness != "" {
			args = append(args, "--fsr-sharpness", s.FsrSharpness)
		}
	case "NIS":
		args = append(args, "-F", "nis")
	case "Nearest":
		args = append(args, "-F", "nearest")
	}

	innerCmd, innerArgs, warnings := buildInnerLaunchCommand(filepath.Dir(inst.Dir), inst, s)
	args = append(args, "-b", "--", innerCmd)
	args = append(args, innerArgs...)

	cmd := exec.Command(gamescopePath, args...)
	cmd.Dir = inst.Dir
	gameID := s.GameID
	if gameID == "" {
		gameID = "0"
	}
	cmd.Env = append(os.Environ(),
		"WINEPREFIX="+winePrefix,
		"PROTONPATH="+protonPath,
		"GAMEID="+gameID,
		"WINEDLLOVERRIDES=version=n,b",
		"SDL_VIDEODRIVER=x11",
	)
	return warnings, cmd.Start()
}

// buildInnerLaunchCommand resolves what gamescope should run inside itself,
// mirroring the justfile's _play recipe: mad2relauncher (built on demand via
// `go build`, same as mad2rando5 -- see build_native.go), which wraps
// umu-run itself and additionally handles crash-relaunch, SM64/Jak1
// orchestration, and background music/podcast supervision (mad2music --
// see CLAUDE.md's "Native Linux companion processes" section). Falls back
// to running `umu-run <exe>` directly (the launcher's old behavior -- no
// crash-relaunch, no SM64/Jak1, no music) if mad2relauncher can't be built,
// so "Launch Game" still works without a Go toolchain on PATH; that failure
// and any music-build failure are returned as non-fatal warnings rather than
// blocking the launch.
func buildInnerLaunchCommand(repoRoot string, inst Install, s InstallSettings) (string, []string, []string) {
	var warnings []string

	relauncherPath, err := ensureGoBinaryBuilt(filepath.Join(repoRoot, "mad2relauncher"), "main.go", goBinaryName("mad2relauncher"), false)
	if err != nil {
		warnings = append(warnings, fmt.Sprintf("Couldn't build mad2relauncher, launching without crash-relaunch/SM64/Jak1/music support: %v", err))
		return "umu-run", []string{inst.Exe}, warnings
	}

	var relArgs []string
	sm64Path := filepath.Join(repoRoot, "extern", "sm64ex-alo", "build", "us_pc", "sm64.us.f3dex2e")
	if fileExists(sm64Path) {
		relArgs = append(relArgs, "-sm64-path", sm64Path)
	} else {
		warnings = append(warnings, fmt.Sprintf("SM64Challenge disabled: %s not found (run `just build-sm64-linux`, or the equivalent from this launcher, with your own ROM first)", sm64Path))
	}

	// extern/jak-project, NOT the old external ~/jak1nolauncher sibling --
	// see extern/jak-project's own vendoring (fetched fresh from
	// github.com/open-goal/jak-project since no local jak-project C++
	// source ever existed on disk, unlike sm64ex-alo). gk lives under
	// build/game/ (CMake's own output layout), built via `just
	// build-jak1-linux` from a user-supplied ISO.
	jak1Dir := filepath.Join(repoRoot, "extern", "jak-project")
	if fileExists(filepath.Join(jak1Dir, "build", "game", "gk")) {
		relArgs = append(relArgs, "-jak1-dir", jak1Dir)
	} else {
		warnings = append(warnings, fmt.Sprintf("JakChallenge disabled: %s not found (run `just build-jak1-linux` with your own ISO first)", filepath.Join(jak1Dir, "build", "game", "gk")))
	}

	if !s.MusicDisabled {
		audioDir := s.MusicAudioDir
		if audioDir == "" {
			audioDir = filepath.Join(repoRoot, "mad2music", "audio")
		}
		musicPath := musicBinPath(repoRoot)
		if fileExists(musicPath) {
			relArgs = append(relArgs, "-music-path", musicPath, "-music-audio-dir", audioDir)
		} else {
			warnings = append(warnings, fmt.Sprintf("Background music disabled: %s not found (run `just build-music-linux` first)", musicPath))
		}
	}

	relArgs = append(relArgs, inst.Exe)
	return relauncherPath, relArgs, warnings
}

func splitRes(res string) (w, h string) {
	parts := strings.Split(res, "x")
	if len(parts) == 2 && parts[0] != "" && parts[1] != "" {
		return parts[0], parts[1]
	}
	return "1280", "720"
}

// ResolveProtonPath mirrors the justfile's own _resolve_proton recipe:
// use configured if it exists, else search Steam's compatibilitytools.d /
// steamapps/common for the first GE-Proton*/Proton* install.
func ResolveProtonPath(configured string) (string, error) {
	if configured != "" && dirExists(configured) {
		return configured, nil
	}

	home, err := os.UserHomeDir()
	if err != nil {
		return "", fmt.Errorf("Proton path %q not found and could not determine home directory for fallback search", configured)
	}

	var candidates []string
	for _, searchDir := range []string{
		filepath.Join(home, ".local/share/Steam/compatibilitytools.d"),
		filepath.Join(home, ".local/share/Steam/steamapps/common"),
	} {
		entries, err := os.ReadDir(searchDir)
		if err != nil {
			continue
		}
		for _, e := range entries {
			if !e.IsDir() {
				continue
			}
			name := e.Name()
			if strings.HasPrefix(name, "GE-Proton") || strings.HasPrefix(name, "Proton") {
				candidates = append(candidates, filepath.Join(searchDir, name))
			}
		}
	}
	if len(candidates) == 0 {
		return "", fmt.Errorf("Proton path %q not found, and no GE-Proton*/Proton* install found under Steam's compatibilitytools.d or steamapps/common", configured)
	}
	sort.Strings(candidates)
	return candidates[0], nil
}
