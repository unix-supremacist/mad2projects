package main

import (
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Install is one discovered Mad2/Mad2Demo game installation directory.
type Install struct {
	Name string // display name, e.g. "mad2" or "mad2demo"
	Dir  string // absolute path to the install directory
	Exe  string // exe filename inside Dir, e.g. "Mad2.exe" or "Mad2Demo.exe"
}

// exeCandidates are the executable filenames DiscoverInstalls looks for,
// matched case-insensitively -- these are the two exes this repo's own
// justfile launches (Mad2.exe for mad2/mad2russia, Mad2Demo.exe for mad2demo).
var exeCandidates = []string{"mad2.exe", "mad2demo.exe"}

// skipDirNames are directories DiscoverInstalls never descends into --
// build outputs, VCS metadata, and wineprefixes never contain a game install
// and can be large enough to make an unbounded walk slow.
var skipDirNames = map[string]bool{
	".git": true, "build": true, "wineprefixes": true, "extern": true,
	"node_modules": true, ".cache": true,
}

// defaultScanRoots is the launcher's own working directory plus its parent,
// one subdirectory deep -- this repo's own mad2/mad2russia/mad2demo sit
// directly under the repo root, but the launcher binary may itself be
// invoked from a subdirectory (e.g. mad2launcher/ during `go run .`), so
// checking one level up too means the common case never needs the manual
// "+ Add folder..." fallback.
func defaultScanRoots() []string {
	cwd, err := os.Getwd()
	if err != nil {
		return []string{"."}
	}
	return []string{cwd, filepath.Dir(cwd)}
}

// DiscoverInstalls walks each root, one subdirectory deep, looking for
// directories containing one of exeCandidates. roots is normally
// defaultScanRoots() plus any user-added folders from
// LauncherSettings.ExtraRoots (those are searched a little deeper, since
// they're explicitly opted into and may point at a less predictable
// layout, e.g. a Steam library).
func DiscoverInstalls(roots []string) []Install {
	var results []Install
	seen := map[string]bool{}

	for _, root := range roots {
		absRoot, err := filepath.Abs(root)
		if err != nil {
			continue
		}
		walkForExes(absRoot, 1, &results, seen)
	}

	sort.Slice(results, func(i, j int) bool { return results[i].Dir < results[j].Dir })
	return results
}

// DiscoverInstallsDeep is used for user-added extra roots, which may not be
// the repo root itself (e.g. an arbitrary Steam library folder), so a
// slightly deeper bounded walk is worth the extra cost.
func DiscoverInstallsDeep(roots []string) []Install {
	var results []Install
	seen := map[string]bool{}
	for _, root := range roots {
		absRoot, err := filepath.Abs(root)
		if err != nil {
			continue
		}
		walkForExes(absRoot, 4, &results, seen)
	}
	sort.Slice(results, func(i, j int) bool { return results[i].Dir < results[j].Dir })
	return results
}

func walkForExes(root string, maxDepth int, results *[]Install, seen map[string]bool) {
	rootDepth := strings.Count(filepath.Clean(root), string(filepath.Separator))
	_ = filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if d.IsDir() {
			base := d.Name()
			if strings.HasPrefix(base, ".") && path != root {
				return filepath.SkipDir
			}
			if skipDirNames[strings.ToLower(base)] {
				return filepath.SkipDir
			}
			depth := strings.Count(filepath.Clean(path), string(filepath.Separator)) - rootDepth
			if depth > maxDepth {
				return filepath.SkipDir
			}
			return nil
		}
		lower := strings.ToLower(d.Name())
		for _, cand := range exeCandidates {
			if lower == cand {
				dir := filepath.Dir(path)
				if seen[dir] {
					return nil
				}
				seen[dir] = true
				*results = append(*results, Install{
					Name: filepath.Base(dir),
					Dir:  dir,
					Exe:  d.Name(),
				})
				return nil
			}
		}
		return nil
	})
}

// DisplayName renders the dropdown label for an install, e.g. "mad2 (Mad2.exe)".
func (i Install) DisplayName() string {
	return i.Name + " (" + i.Exe + ")"
}

func dirExists(path string) bool {
	st, err := os.Stat(path)
	return err == nil && st.IsDir()
}
