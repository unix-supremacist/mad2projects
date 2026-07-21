package main

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
)

// ModPackage is one discovered installable package under a packages root --
// see deploy-plan.md §1-2 for the package layouts this is meant to cover
// (mad2modloader, mad2effects, mad2speedrunning, mad2rando_*, chaosmod,
// etc.), though that doc is still being updated so this deliberately doesn't
// hardcode a package name list -- see DiscoverPackages.
type ModPackage struct {
	Name string // subdirectory name under the packages root, e.g. "mad2effects"
	Dir  string // absolute path to the package directory
}

// packagesPlatform mirrors distCandidate's own platform selection in
// build_native.go -- kept separate since distCandidate needs a pkgName/
// relPath to build one file's path, but here we want the bare directory a
// whole packaging run drops packages under.
func packagesPlatform() string {
	if runtime.GOOS == "windows" {
		return "windows"
	}
	return "linux"
}

// ResolvePackagesRoot finds the directory holding installable mod packages,
// checked in priority order: an explicit user override (LauncherSettings.
// PackagesDir, wired up in ui_modinstaller.go's folder picker -- honored
// even if it doesn't currently exist, so the UI can say "nothing found
// here" rather than silently falling back to a location the user didn't
// choose), this launcher's own bundled packages/ directory (exeDir()/
// packages -- what a downloaded release ships, see deploy-plan.md §3.1),
// and finally repoRoot/dist/<platform>/ (the packaging script's staged
// output, mirroring distCandidate in build_native.go -- useful when testing
// packaging locally). Returns "" if none apply.
func ResolvePackagesRoot(repoRoot, override string) string {
	if override != "" {
		return override
	}
	if dir := exeDir(); dir != "" {
		if cand := filepath.Join(dir, "packages"); dirExists(cand) {
			return cand
		}
	}
	if cand := filepath.Join(repoRoot, "dist", packagesPlatform()); dirExists(cand) {
		return cand
	}
	return ""
}

// DiscoverPackages lists every subdirectory of root that looks like an
// installable package. "Looks like" is deliberately loose (has its own
// mods/ folder, or just has any content at all) rather than matching a
// hardcoded package name list, since deploy-plan.md's package set is still
// being finalized -- a new package directory dropped under root shows up
// here without this file needing an update. mad2launcher itself is skipped
// in case a packaging script ever nests the launcher's own output inside
// its own packages/ tree by accident.
func DiscoverPackages(root string) []ModPackage {
	if root == "" {
		return nil
	}
	entries, err := os.ReadDir(root)
	if err != nil {
		return nil
	}
	var pkgs []ModPackage
	for _, e := range entries {
		if !e.IsDir() || strings.EqualFold(e.Name(), "mad2launcher") {
			continue
		}
		dir := filepath.Join(root, e.Name())
		if looksLikePackage(dir) {
			pkgs = append(pkgs, ModPackage{Name: e.Name(), Dir: dir})
		}
	}
	sort.Slice(pkgs, func(i, j int) bool { return pkgs[i].Name < pkgs[j].Name })
	return pkgs
}

func looksLikePackage(dir string) bool {
	if dirExists(filepath.Join(dir, "mods")) {
		return true
	}
	entries, err := os.ReadDir(dir)
	return err == nil && len(entries) > 0
}

// InstallResult is one package's outcome from InstallPackages, for
// per-package success/failure reporting in the UI.
type InstallResult struct {
	Package string
	Err     error
}

// InstallPackages installs each pkg into installDir in turn, continuing
// past a failed package rather than aborting the whole batch -- one bad or
// partial package (e.g. a permissions error on a single file) shouldn't
// block installing the rest of the user's selection.
func InstallPackages(pkgs []ModPackage, installDir string) []InstallResult {
	results := make([]InstallResult, 0, len(pkgs))
	for _, pkg := range pkgs {
		results = append(results, InstallResult{Package: pkg.Name, Err: InstallPackage(pkg, installDir)})
	}
	return results
}

// InstallPackage copies one package's contents into installDir, the shape
// described in deploy-plan.md §3.1's Mod Installer section:
//   - <pkg>/mods/* merges into <installDir>/mods/, overwriting same-named
//     files -- this is the direct replacement for `just deploy-mad2`'s own
//     "copy mods/*.dll" step (see CLAUDE.md's Deploy layout), not filtered
//     to *.dll specifically since anything a package actually ships under
//     mods/ is trusted content.
//   - <pkg>/config.cfg, if present, is only ever copied in as a first-run
//     seed -- never overwriting a destination that already has one, since
//     that file holds live settings mad2config.dll (and this launcher's own
//     other pages) read/write at runtime, not static package content.
//   - Every other top-level entry (coordinates.yaml, emotes/, ...) merges
//     into installDir itself, recursively for directories, overwriting
//     files but never deleting anything already present in the
//     destination that the package doesn't ship -- installs are additive,
//     not a mirror sync, so a previous install's extras (or a user's own
//     hand-dropped mods) survive.
func InstallPackage(pkg ModPackage, installDir string) error {
	entries, err := os.ReadDir(pkg.Dir)
	if err != nil {
		return fmt.Errorf("reading package %s: %w", pkg.Name, err)
	}

	for _, e := range entries {
		name := e.Name()
		src := filepath.Join(pkg.Dir, name)

		switch {
		case name == "mods" && e.IsDir():
			if err := copyDirMerge(src, filepath.Join(installDir, "mods")); err != nil {
				return fmt.Errorf("package %s: %w", pkg.Name, err)
			}
		case name == "config.cfg":
			dst := filepath.Join(installDir, "config.cfg")
			if fileExists(dst) {
				continue // never clobber a live install's own settings
			}
			if err := copyFile(src, dst); err != nil {
				return fmt.Errorf("package %s: %w", pkg.Name, err)
			}
		case e.IsDir():
			if err := copyDirMerge(src, filepath.Join(installDir, name)); err != nil {
				return fmt.Errorf("package %s: %w", pkg.Name, err)
			}
		default:
			if err := copyFile(src, filepath.Join(installDir, name)); err != nil {
				return fmt.Errorf("package %s: %w", pkg.Name, err)
			}
		}
	}
	return nil
}

// copyDirMerge recursively copies src's contents into dst, creating
// directories as needed and overwriting existing files, but never removing
// anything already in dst that src doesn't have -- see InstallPackage's own
// comment for why (merge, not mirror sync).
func copyDirMerge(src, dst string) error {
	entries, err := os.ReadDir(src)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(dst, 0755); err != nil {
		return err
	}
	for _, e := range entries {
		s := filepath.Join(src, e.Name())
		d := filepath.Join(dst, e.Name())
		if e.IsDir() {
			if err := copyDirMerge(s, d); err != nil {
				return err
			}
			continue
		}
		if err := copyFile(s, d); err != nil {
			return err
		}
	}
	return nil
}

// copyFile overwrites dst with src's contents, preserving src's permission
// bits -- packages ship native binaries alongside DLLs/assets (e.g.
// mad2rando_linux/mad2rando5), and os.Create's default mode would silently
// strip the executable bit otherwise.
func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()

	if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
		return err
	}
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()

	if _, err := io.Copy(out, in); err != nil {
		return err
	}
	if info, err := os.Stat(src); err == nil {
		_ = out.Chmod(info.Mode().Perm())
	}
	return nil
}
