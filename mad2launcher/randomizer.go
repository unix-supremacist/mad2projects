package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

func fileExists(path string) bool {
	st, err := os.Stat(path)
	return err == nil && !st.IsDir()
}

// RunRandomizer resolves a prebuilt mad2rando5 binary (see
// ResolveMad2Rando5 in build_native.go -- this launcher never builds it
// itself) and runs it with flags derived from cfg, writing its
// level_redirects.txt straight into outDir (an install directory -- see
// mad2levelredirectmod/src/levelredirect.cpp, which reads that file from
// the game exe's own cwd). The tool's combined stdout/stderr (the
// level/coin mapping table it prints) is deliberately never returned to
// the caller for display -- it's a full spoiler of the run's randomization
// -- and is instead appended to mad2rando5.log in outDir via
// logRandomizerRun. Only success/failure is reported back.
func RunRandomizer(repoRoot string, cfg RandomizerSettings, outDir string) error {
	binPath, err := ResolveMad2Rando5(repoRoot)
	if err != nil {
		return fmt.Errorf("mad2rando5 %w (run `just build-mad2rando5`, or install the mad2rando package via the Mod Installer)", err)
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
	cmd.Dir = repoRoot
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
