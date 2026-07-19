package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

// gameInstallDirNames are this repo's own three installs -- the same set
// mad2musicpatch's gameDirs covers, since the patched global.arc is derived
// once and distributed to all of them.
var gameInstallDirNames = []string{"mad2", "mad2russia", "mad2demo"}

// globalArcModPath is where a blanked-music global.arc lands for inst --
// mad2assetloader's CreateFileA/W hook prefers this over the real
// Content/Streams/win/global.arc if present (see mad2musicpatch/main.go's
// header comment), so deploying/removing this one file is the entire toggle.
func globalArcModPath(inst Install) string {
	return filepath.Join(inst.Dir, "Content", "Streams", "mod", "global.arc")
}

// DeployBlankGlobalArc builds mad2arc/mad2repack/mad2musicpatch if needed
// and runs mad2musicpatch, which extracts global.arc, replaces every
// background-music track with 1s of silence, feeds the originals into
// mad2music's pool, and writes the patched archive to every install found
// under repoRoot. Returns the tool's combined output for display.
func DeployBlankGlobalArc(repoRoot string) (string, error) {
	if _, err := exec.LookPath("ffmpeg"); err != nil {
		return "", fmt.Errorf("ffmpeg not found on PATH (needed to generate the blank replacement audio): %w", err)
	}

	mad2arcPath, err := ensureGoBinaryBuilt(filepath.Join(repoRoot, "mad2arc"), "main.go", goBinaryName("mad2arc"), false)
	if err != nil {
		return "", err
	}
	mad2repackPath, err := ensureGoBinaryBuilt(filepath.Join(repoRoot, "mad2repack"), "main.go", goBinaryName("mad2repack"), false)
	if err != nil {
		return "", err
	}
	mad2musicpatchPath, err := ensureGoBinaryBuilt(filepath.Join(repoRoot, "mad2musicpatch"), "main.go", goBinaryName("mad2musicpatch"), false)
	if err != nil {
		return "", err
	}

	cmd := exec.Command(mad2musicpatchPath,
		"-repo-root", repoRoot,
		"-mad2arc", mad2arcPath,
		"-mad2repack", mad2repackPath,
	)
	cmd.Dir = repoRoot
	out, err := cmd.CombinedOutput()
	if err != nil {
		return string(out), fmt.Errorf("mad2musicpatch failed: %w", err)
	}
	return string(out), nil
}

// RemoveBlankGlobalArc deletes the deployed Content/Streams/mod/global.arc
// from every install under repoRoot -- reverting to the original archive is
// exactly this, since mad2musicpatch never touches
// Content/Streams/win/global.arc itself.
func RemoveBlankGlobalArc(repoRoot string) error {
	for _, name := range gameInstallDirNames {
		p := filepath.Join(repoRoot, name, "Content", "Streams", "mod", "global.arc")
		if fileExists(p) {
			if err := os.Remove(p); err != nil {
				return err
			}
		}
	}
	return nil
}

// AnyBlankGlobalArcDeployed reports whether any install under repoRoot
// currently has the blanked archive active.
func AnyBlankGlobalArcDeployed(repoRoot string) bool {
	for _, name := range gameInstallDirNames {
		if fileExists(filepath.Join(repoRoot, name, "Content", "Streams", "mod", "global.arc")) {
			return true
		}
	}
	return false
}

// RunMusicDownloader builds (if needed) and runs
// mad2music/downloader/download_music.go, the yt-dlp-backed tool that syncs
// mad2music/audio/music/*.txt's link lists into that same directory tree
// (skipping anything already downloaded -- see download_music.go's own
// --download-archive use). Run with cwd set to mad2music/ itself, since
// that's what the downloader's own relative paths ("audio/music/...",
// "logs/music_download.log") are relative to. Its own stdout/stderr (mostly
// yt-dlp's per-track progress) already goes to that log file, not returned
// here -- see ui_music.go's download button, which just reports success/
// failure.
func RunMusicDownloader(repoRoot string) error {
	downloaderDir := filepath.Join(repoRoot, "mad2music", "downloader")
	binPath, err := ensureGoBinaryBuilt(downloaderDir, "download_music.go", goBinaryName("mad2musicdownloader"), false)
	if err != nil {
		return err
	}

	cmd := exec.Command(binPath)
	cmd.Dir = filepath.Join(repoRoot, "mad2music")
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("mad2musicdownloader failed: %w\n%s", err, out)
	}
	return nil
}
