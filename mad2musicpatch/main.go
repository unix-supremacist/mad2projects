// mad2musicpatch is a one-shot asset-pipeline tool: blanks the game's own
// background/level music inside global.arc (so it doesn't compete with
// mad2music's external player) and feeds the original tracks back into
// mad2music's pool so nothing is lost. Go port of the former
// tools/patch_global_music.sh -- same pipeline, same Unix-style approach of
// shelling out to mad2arc/mad2repack/ffmpeg rather than linking against
// mad2iga directly.
//
// Non-destructive by design: the patched archive is written to
// Content/Streams/mod/global.arc, NOT over Content/Streams/win/global.arc.
// mad2assetloader's existing CreateFileA/CreateFileW hook
// (mad2assetloader/src/file_redirect.cpp) already redirects any open of a
// path containing "Streams/win/" to check "Streams/mod/" first, preferring
// it if present and transparently falling back to the original otherwise.
// Reverting is just deleting the file under Streams/mod/ -- the original
// archive is never touched.
//
// Requires mad2arc/mad2repack already built (just build-mad2arc
// build-mad2repack, or pass -mad2arc/-mad2repack explicitly) and ffmpeg on
// PATH.
package main

import (
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// stingers are short one-shot fanfares/cues that stay native in global.arc
// (not pooled, not blanked) -- everything else under builddata/win/*.snds is
// treated as looping/background music. Filename-based, not ear-verified.
var stingers = map[string]bool{
	"goalcompletefanfareredo.snds":      true,
	"penguin_fail_v1.snds":              true,
	"goal_presentation_mad_v1.snds":     true,
	"goal_presentation_africa_v1.snds":  true,
	"prepare_to_launch_plane.snds":      true,
	"prepare_to_launch.snds":            true,
	"africa_fail_v1.snds":               true,
	"fail_mad_v1.snds":                  true,
	"goal_presentation_penguin_v1.snds": true,
	"comingage_switchoff_cine.snds":     true,
	"demo_music_volume.snds":            true,
	"video_loop_12.snds":                true,
}

// copyrightTracks are confirmed real-world licensed songs -> copyright pool.
// Everything else pooled goes to no_copyright (TFB's own game score, lower
// Content-ID risk than the licensed songs).
var copyrightTracks = map[string]bool{
	"dancelikeanafrican_crop.snds": true,
	"iko-iko_cropcrop.snds":        true,
	"hulelam.snds":                 true,
	"aceyalone_shango.snds":        true,
	"angel-inst.snds":              true,
}

// gameDirs are this repo's own three installs, each carrying its own
// Content/Streams/win/global.arc that the patched archive gets distributed
// to -- the source is only ever read from mad2/'s copy.
var gameDirs = []string{"mad2", "mad2russia", "mad2demo"}

func main() {
	repoRootFlag := flag.String("repo-root", ".", "path to the mad2modloader repo root")
	mad2arcFlag := flag.String("mad2arc", "", "path to the mad2arc binary (default: <repo-root>/build/mad2arc)")
	mad2repackFlag := flag.String("mad2repack", "", "path to the mad2repack binary (default: <repo-root>/build/mad2repack)")
	flag.Parse()

	repoRoot, err := filepath.Abs(*repoRootFlag)
	fatalOn(err)

	mad2arc := *mad2arcFlag
	if mad2arc == "" {
		mad2arc = filepath.Join(repoRoot, "build", "mad2arc")
	}
	mad2repack := *mad2repackFlag
	if mad2repack == "" {
		mad2repack = filepath.Join(repoRoot, "build", "mad2repack")
	}
	if !executable(mad2arc) || !executable(mad2repack) {
		fatal(fmt.Errorf("mad2arc/mad2repack not found/executable at %s / %s\nBuild them first: just build-mad2arc build-mad2repack", mad2arc, mad2repack))
	}
	if _, err := exec.LookPath("ffmpeg"); err != nil {
		fatal(fmt.Errorf("ffmpeg not found on PATH (needed to generate the blank replacement audio): %w", err))
	}

	scratch, err := os.MkdirTemp("", "mad2musicpatch")
	fatalOn(err)
	defer os.RemoveAll(scratch)

	musicPoolDir := filepath.Join(repoRoot, "mad2music", "audio", "music")
	copyrightDir := filepath.Join(musicPoolDir, "copyright")
	noCopyrightDir := filepath.Join(musicPoolDir, "no_copyright")
	fatalOn(os.MkdirAll(copyrightDir, 0755))
	fatalOn(os.MkdirAll(noCopyrightDir, 0755))

	srcGlobalArc := filepath.Join(repoRoot, "mad2", "Content", "Streams", "win", "global.arc")
	if !fileExists(srcGlobalArc) {
		fatal(fmt.Errorf("%s not found", srcGlobalArc))
	}

	fmt.Println("[1/6] Extracting global.arc...")
	extractDir := filepath.Join(scratch, "globalARC")
	runQuiet(mad2arc, "-o", extractDir, srcGlobalArc)
	// mad2arc nests output under a subdirectory named after the archive stem
	// (global.arc -> global/), confirmed by inspection -- not documented.
	sndsDir := filepath.Join(extractDir, "global", "builddata", "win")

	fmt.Println("[2/6] Generating blank replacement audio...")
	blankOgg := filepath.Join(scratch, "blank.ogg")
	run("ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi", "-i", "anullsrc=r=44100:cl=stereo", "-t", "1", "-c:a", "libvorbis", blankOgg)

	fmt.Println("[3/6] Sorting tracks into mad2music's pool...")
	entries, err := os.ReadDir(sndsDir)
	fatalOn(err)
	var replaceArgs []string
	blankedCount, keptCount := 0, 0
	for _, e := range entries {
		base := e.Name()
		if !strings.HasSuffix(strings.ToLower(base), ".snds") {
			continue
		}
		if stingers[base] {
			keptCount++
			continue
		}

		stem := strings.TrimSuffix(base, filepath.Ext(base))
		destDir := noCopyrightDir
		if copyrightTracks[base] {
			destDir = copyrightDir
		}
		fatalOn(copyFile(filepath.Join(sndsDir, base), filepath.Join(destDir, stem+".ogg")))

		replaceArgs = append(replaceArgs, "-file", "c:/tfb/content/builddata/win/"+base+"="+blankOgg)
		blankedCount++
	}
	fmt.Printf("    %d tracks pooled + blanked, %d stingers kept native\n", blankedCount, keptCount)

	// Bonus: Samba de Janeiro lives in VolcanoRave.arc, not global.arc --
	// pooled only, VolcanoRave.arc itself is untouched (out of scope).
	volcanoArc := filepath.Join(repoRoot, "mad2", "Content", "Streams", "win", "VolcanoRave.arc")
	if fileExists(volcanoArc) {
		fmt.Println("[4/6] Extracting Samba de Janeiro from VolcanoRave.arc...")
		volcanoExtractDir := filepath.Join(scratch, "volcanoARC")
		runQuiet(mad2arc, "-o", volcanoExtractDir, volcanoArc)
		volcanoSndsDir := filepath.Join(volcanoExtractDir, "VolcanoRave", "builddata", "win")
		for _, name := range []string{"sambadejaniero", "sambadejaniero_fill_1"} {
			src := filepath.Join(volcanoSndsDir, name+".snds")
			if fileExists(src) {
				fatalOn(copyFile(src, filepath.Join(copyrightDir, name+".ogg")))
			}
		}
	} else {
		fmt.Println("[4/6] VolcanoRave.arc not found, skipping Samba de Janeiro extraction")
	}

	fmt.Println("[5/6] Repacking patched global.arc...")
	patchedArc := filepath.Join(scratch, "global_patched.arc")
	replaceCmdArgs := append([]string{"replace", srcGlobalArc, "-o", patchedArc}, replaceArgs...)
	runQuiet(mad2repack, replaceCmdArgs...)

	fmt.Println("[6/6] Placing patched archive under Streams/mod/ (non-destructive)...")
	for _, dir := range gameDirs {
		winArc := filepath.Join(repoRoot, dir, "Content", "Streams", "win", "global.arc")
		if !fileExists(winArc) {
			continue
		}
		modDir := filepath.Join(repoRoot, dir, "Content", "Streams", "mod")
		fatalOn(os.MkdirAll(modDir, 0755))
		dst := filepath.Join(modDir, "global.arc")
		fatalOn(copyFile(patchedArc, dst))
		fmt.Printf("    %s\n", dst)
	}

	fmt.Println("Done. Original archives untouched -- delete Content/Streams/mod/global.arc in any install to revert.")
}

func run(name string, args ...string) {
	cmd := exec.Command(name, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	fatalOn(cmd.Run())
}

func runQuiet(name string, args ...string) {
	cmd := exec.Command(name, args...)
	cmd.Stderr = os.Stderr
	fatalOn(cmd.Run())
}

func executable(path string) bool {
	st, err := os.Stat(path)
	return err == nil && !st.IsDir() && st.Mode()&0111 != 0
}

func fileExists(path string) bool {
	st, err := os.Stat(path)
	return err == nil && !st.IsDir()
}

func copyFile(src, dst string) error {
	data, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	return os.WriteFile(dst, data, 0644)
}

func fatalOn(err error) {
	if err != nil {
		fatal(err)
	}
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "Error:", err)
	os.Exit(1)
}
