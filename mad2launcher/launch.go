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
	// No hard umu-run-on-PATH check here (unlike gamescope, which has no
	// portable distribution to bundle and is always a real system
	// dependency) -- buildInnerLaunchCommand's normal path goes through
	// mad2relauncher, which resolves its own bundled copy first (see
	// mad2relauncher/gameloop.go's resolveUmuRun, and tools/package.sh's
	// mad2relauncher_linux staging); only the "mad2relauncher couldn't be
	// resolved at all" fallback below ever falls back to a bare "umu-run"
	// relying on PATH, and that failing is a natural, clearly-surfaced
	// exec error at that point rather than something worth pre-flighting.

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
// mirroring the justfile's _play recipe: a prebuilt mad2relauncher (see
// ResolveMad2Relauncher in build_native.go -- this launcher never builds it
// itself), which wraps umu-run itself and additionally handles
// crash-relaunch, SM64/Jak1 orchestration, and background music/podcast
// supervision (mad2music -- see CLAUDE.md's "Native Linux companion
// processes" section). Falls back to running `umu-run <exe>` directly (the
// launcher's old behavior -- no crash-relaunch, no SM64/Jak1, no music) if
// no prebuilt mad2relauncher binary can be found, so "Launch Game" still
// works without it; that and any missing-music warning are returned as
// non-fatal warnings rather than blocking the launch.
func buildInnerLaunchCommand(repoRoot string, inst Install, s InstallSettings) (string, []string, []string) {
	var warnings []string

	relauncherPath, err := ResolveMad2Relauncher(repoRoot)
	if err != nil {
		warnings = append(warnings, fmt.Sprintf("mad2relauncher %v, launching without crash-relaunch/SM64/Jak1/music support (run `just build-relauncher`, or install the mad2relauncher package via the Mod Installer)", err))
		return "umu-run", []string{inst.Exe}, warnings
	}

	// Resolved the same way "Build Games" resolves where it builds TO (see
	// GamesRoot in build_native.go) -- a packaged release ships extern/
	// alongside the launcher binary itself, not under repoRoot/extern (which
	// may not exist at all if the game install lives somewhere else
	// entirely).
	gamesRoot := GamesRoot(repoRoot)

	var relArgs []string
	sm64Path := sm64LinuxBinPath(gamesRoot)
	if fileExists(sm64Path) {
		relArgs = append(relArgs, "-sm64-path", sm64Path)
	} else {
		warnings = append(warnings, fmt.Sprintf("SM64Challenge disabled: %s not found (build it from the \"Build Games\" page, or run `just build-sm64-linux`, with your own ROM first)", sm64Path))
	}

	// extern/jak-project, NOT the old external ~/jak1nolauncher sibling --
	// see extern/jak-project's own vendoring (fetched fresh from
	// github.com/open-goal/jak-project since no local jak-project C++
	// source ever existed on disk, unlike sm64ex-alo). gk lives under
	// build/game/ (CMake's own output layout), built via `just
	// build-jak1-linux` from a user-supplied ISO.
	jak1Dir := filepath.Join(gamesRoot, "extern", "jak-project")
	if fileExists(jak1LinuxBinPath(gamesRoot)) {
		relArgs = append(relArgs, "-jak1-dir", jak1Dir)
	} else {
		warnings = append(warnings, fmt.Sprintf("JakChallenge disabled: %s not found (build it from the \"Build Games\" page, or run `just build-jak1-linux`, with your own ISO first)", jak1LinuxBinPath(gamesRoot)))
	}

	if !s.MusicDisabled {
		audioDir := s.MusicAudioDir
		if audioDir == "" {
			audioDir = filepath.Join(repoRoot, "mad2music", "audio")
		}
		if musicPath, err := ResolveMad2Music(repoRoot); err == nil {
			relArgs = append(relArgs, "-music-path", musicPath, "-music-audio-dir", audioDir)
		} else {
			warnings = append(warnings, fmt.Sprintf("Background music disabled: mad2music %v (run `just build-music-linux`, or install the mad2music package via the Mod Installer)", err))
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

// steamRootCandidates returns every location this repo knows Steam might
// actually live, in priority order -- NOT just ~/.local/share/Steam. That
// was the only location ever checked until a user reported Proton not
// being found at all despite having it installed, working, and used by the
// older ../mad2mod tool (which launches games through Steam itself rather
// than invoking umu-run directly, so it never needed to independently
// rediscover Proton's on-disk location the way this repo does): they
// simply didn't have a ~/.local/share/Steam directory at all -- Steam was
// installed via Flatpak instead. ~/.steam/root and ~/.steam/steam are
// symlinks Steam itself maintains pointing at wherever its real data
// directory actually is (the closest thing to an authoritative answer,
// regardless of native/custom-prefix/relocated installs); ~/.local/share/
// Steam is kept as the common-case fallback; the Flatpak and Snap paths
// cover the two most common non-native packaging cases on Linux.
func steamRootCandidates(home string) []string {
	return []string{
		filepath.Join(home, ".steam/root"),
		filepath.Join(home, ".steam/steam"),
		// Debian's own `steam` package's real data directory -- confirmed
		// directly from a user's machine where neither .steam/root nor
		// .steam/steam resolved to it as a symlink.
		filepath.Join(home, ".steam/debian-installation"),
		filepath.Join(home, ".local/share/Steam"),
		filepath.Join(home, ".var/app/com.valvesoftware.Steam/.local/share/Steam"),
		filepath.Join(home, "snap/steam/common/.local/share/Steam"),
	}
}

// ResolveProtonPath mirrors the justfile's own _resolve_proton recipe: use
// configured if it exists, else search every steamRootCandidates root's
// compatibilitytools.d / steamapps/common for the first GE-Proton*/Proton*
// install found.
func ResolveProtonPath(configured string) (string, error) {
	if configured != "" && dirExists(configured) {
		return configured, nil
	}

	home, err := os.UserHomeDir()
	if err != nil {
		return "", fmt.Errorf("Proton path %q not found and could not determine home directory for fallback search", configured)
	}

	var candidates []string
	for _, steamRoot := range steamRootCandidates(home) {
		var found []string
		for _, searchDir := range []string{
			filepath.Join(steamRoot, "compatibilitytools.d"),
			filepath.Join(steamRoot, "steamapps/common"),
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
					found = append(found, filepath.Join(searchDir, name))
				}
			}
		}
		if len(found) > 0 {
			// Stop at the first Steam root that actually has any Proton
			// install -- later roots in the list are lower-priority
			// fallbacks (e.g. Flatpak), not additional places to merge
			// results from.
			candidates = found
			break
		}
	}
	if len(candidates) == 0 {
		return "", fmt.Errorf("Proton path %q not found, and no GE-Proton*/Proton* install found under any known Steam location (~/.steam/root, ~/.steam/steam, ~/.local/share/Steam, or the Flatpak/Snap equivalents)", configured)
	}
	sort.Strings(candidates)
	return candidates[0], nil
}
