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

// LocateMad2Rando5Dir finds mad2rando5 -- the Go CLI level-logic randomizer
// that drives mad2levelredirectmod via level_redirects.txt (see CLAUDE.md's
// "What this is" section). mad2rando5/ now lives inside this repo
// (migrated from the formerly-external ../mad2rando5 sibling), so the
// primary candidates are anchored off already-discovered installs' repo
// root (repoRoot + "mad2rando5") and cwd-relative guesses the same way
// defaultScanRoots does for game installs; the old ../mad2rando5 sibling
// path is kept as a last-resort fallback for anyone still on that layout.
func LocateMad2Rando5Dir(installs []Install) (string, bool) {
	var candidates []string
	for _, inst := range installs {
		repoRoot := filepath.Dir(inst.Dir)
		candidates = append(candidates,
			filepath.Join(repoRoot, "mad2rando5"),               // in-repo (current layout)
			filepath.Join(filepath.Dir(repoRoot), "mad2rando5"), // ../mad2rando5 (legacy sibling)
		)
	}
	if cwd, err := os.Getwd(); err == nil {
		candidates = append(candidates,
			filepath.Join(cwd, "mad2rando5"),
			filepath.Join(cwd, "..", "mad2rando5"),
			filepath.Join(cwd, "..", "..", "mad2rando5"),
		)
	}

	seen := map[string]bool{}
	for _, c := range candidates {
		abs, err := filepath.Abs(c)
		if err != nil || seen[abs] {
			continue
		}
		seen[abs] = true
		if fileExists(filepath.Join(abs, "main.go")) {
			return abs, true
		}
	}
	return "", false
}

func fileExists(path string) bool {
	st, err := os.Stat(path)
	return err == nil && !st.IsDir()
}

func randomizerBinaryName() string {
	if runtime.GOOS == "windows" {
		return "mad2rando5.exe"
	}
	return "mad2rando5"
}

// prebuiltRandomizerCandidates returns, in preference order, paths this
// launcher treats as an "already built, don't touch, never invoke `go
// build` for" mad2rando5 binary -- checked before falling back to
// ensureRandomizerBuilt's own "build in dir, rebuild if source changed"
// behavior. Today this is just build/mad2rando5 (`just build-mad2rando5`'s
// own output, see CLAUDE.md's Build commands section). Deliberately a slice
// rather than a single path: a packaged release resolves mad2rando5 from a
// bundled packages/mad2rando_* download instead (deploy-plan.md §1.4/§3.1/
// §5 item 2) -- that lookup doesn't exist yet (§5 item 8, a much bigger
// packaging feature deferred separately), but when it lands it's meant to
// slot in here as another candidate ahead of build/mad2rando5, not require
// touching ensureRandomizerBuilt's own logic again.
func prebuiltRandomizerCandidates(repoRoot string) []string {
	return []string{
		filepath.Join(repoRoot, "build", randomizerBinaryName()),
	}
}

// ensureRandomizerBuilt resolves a runnable mad2rando5 binary for dir (the
// mad2rando5 source directory -- see LocateMad2Rando5Dir). It first checks
// prebuiltRandomizerCandidates and returns the first one found as-is, with
// no `go build` invocation at all -- a packaged release can't assume a Go
// toolchain is present at runtime (deploy-plan.md §5 item 2). Only if none
// of those exist does it fall back to the original dev-machine behavior:
// (re)building mad2rando5's binary in dir if it's missing or older than
// main.go, mirroring the "build once, reuse after" pattern build-launcher/
// build-relauncher already use.
func ensureRandomizerBuilt(dir string) (string, error) {
	repoRoot := filepath.Dir(dir)
	for _, candidate := range prebuiltRandomizerCandidates(repoRoot) {
		if fileExists(candidate) {
			return candidate, nil
		}
	}

	binPath := filepath.Join(dir, randomizerBinaryName())
	srcPath := filepath.Join(dir, "main.go")

	needBuild := true
	if binInfo, err := os.Stat(binPath); err == nil {
		if srcInfo, err2 := os.Stat(srcPath); err2 == nil {
			needBuild = srcInfo.ModTime().After(binInfo.ModTime())
		} else {
			needBuild = false
		}
	}
	if !needBuild {
		return binPath, nil
	}

	cmd := exec.Command("go", "build", "-o", binPath, ".")
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("building mad2rando5: %w\n%s", err, out)
	}
	return binPath, nil
}

// RunRandomizer builds (if needed) and runs mad2rando5 with flags derived
// from cfg, writing its level_redirects.txt straight into outDir (an
// install directory -- see mad2levelredirectmod/src/levelredirect.cpp,
// which reads that file from the game exe's own cwd). The tool's combined
// stdout/stderr (the level/coin mapping table it prints) is deliberately
// never returned to the caller for display -- it's a full spoiler of the
// run's randomization -- and is instead appended to mad2rando5.log in
// outDir via logRandomizerRun. Only success/failure is reported back.
func RunRandomizer(dir string, cfg RandomizerSettings, outDir string) error {
	binPath, err := ensureRandomizerBuilt(dir)
	if err != nil {
		return err
	}

	// Bool flags MUST use "-flag=value" (not "-flag" "value" as two argv
	// entries) -- Go's flag package treats a bare "-flag" as true and takes
	// no argument for bools, silently leaving "value" as an unconsumed
	// positional. Non-bool flags don't have this problem.
	args := []string{
		"-seed", orDefault(cfg.Seed, "0"),
		"-coins", orDefault(cfg.CoinsNeeded, "0"),
		"-maxcoins=" + boolFlag(cfg.MaxCoins),
		"-separate=" + boolFlag(cfg.SeparatePools),
		"-monkeymatch=" + boolFlag(cfg.MonkeyMatch),
		"-coinmatch=" + boolFlag(cfg.CoinMatch),
		"-locked", cfg.LockedLevels,
		"-out", filepath.Join(outDir, "level_redirects.txt"),
	}

	cmd := exec.Command(binPath, args...)
	cmd.Dir = dir
	out, runErr := cmd.CombinedOutput()
	logRandomizerRun(outDir, args, out, runErr)
	if runErr != nil {
		return fmt.Errorf("running mad2rando5: %w (see mad2rando5.log in %s)", runErr, outDir)
	}
	return nil
}

// logRandomizerRun appends one run's full output to mad2rando5.log in
// outDir (the install directory, alongside level_redirects.txt) -- the only
// place this ever gets written, per changes.md: nothing about which
// levels/coins got picked should show up in the Fyne UI itself, so players
// aren't spoiled.
func logRandomizerRun(outDir string, args []string, out []byte, runErr error) {
	f, err := os.OpenFile(filepath.Join(outDir, "mad2rando5.log"), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644)
	if err != nil {
		return
	}
	defer f.Close()
	fmt.Fprintf(f, "\n=== %s ===\nargs: %v\n", time.Now().Format(time.RFC3339), args)
	f.Write(out)
	if runErr != nil {
		fmt.Fprintf(f, "error: %v\n", runErr)
	}
}

func boolFlag(b bool) string {
	if b {
		return "true"
	}
	return "false"
}

func orDefault(s, def string) string {
	if strings.TrimSpace(s) == "" {
		return def
	}
	return s
}
