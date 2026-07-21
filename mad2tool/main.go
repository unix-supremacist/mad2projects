// mad2tool is a high-level orchestration script for setting up an automated
// ROM hacking workflow for Madagascar 2 PC files.
//
// Commands:
//
//	extract         Extracts all archives from mad2/Content/Streams/win/ to mad2raw/
//	                Additionally dumps nested .pak files to self-contained .pak.json meta files.
//	rebuild         Composes files from mad2raw/ and compiles them back to game-ready archives
//	                in mad2repacked/ (calculating hashes to only repack updated directories).
//	testmod         Executes the "Murder Mort" patch and IslandFever audio substitution.
//	unpack-all      Unpacks every archive under <gamedir>/Content/Streams/win/ into its own
//	                directory, each with a hashing manifest.json recording a SHA256 baseline
//	                for every raw/derived (editable) file it produced. See docs/UNPACK_REPACK.md.
//	repack-changed  Walks a directory tree produced by unpack-all, recompiles only the archives
//	                whose derived/editable files changed since their manifest baseline, and
//	                leaves everything else as byte-for-byte passthrough. See docs/UNPACK_REPACK.md.
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"flag"
	"fmt"
	"image/png"
	"io"
	"mad2iga"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

func main() {
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "mad2tool — ROM Hacking Workflow Orchestrator\n\n")
		fmt.Fprintf(os.Stderr, "Usage:\n")
		fmt.Fprintf(os.Stderr, "  mad2tool extract                                Dump game assets to mad2raw/\n")
		fmt.Fprintf(os.Stderr, "  mad2tool rebuild                                Rebuild modified assets to mad2repacked/\n")
		fmt.Fprintf(os.Stderr, "  mad2tool testmod                                Apply testing swaps & translations on raw dumps\n")
		fmt.Fprintf(os.Stderr, "  mad2tool unpack-all <gamedir> [outDir]          Unpack every archive with a hashing manifest\n")
		fmt.Fprintf(os.Stderr, "  mad2tool repack-changed <gamedir> [unpackDir] [outDir]  Recompile only changed archives\n")
	}
	flag.Parse()

	args := flag.Args()
	if len(args) < 1 {
		flag.Usage()
		os.Exit(1)
	}

	switch args[0] {
	case "extract":
		filter := ""
		if len(args) > 1 {
			filter = strings.ToLower(args[1])
		}
		if err := runExtract(filter); err != nil {
			fmt.Fprintf(os.Stderr, "Extraction failed: %v\n", err)
			os.Exit(1)
		}
	case "rebuild":
		filter := ""
		if len(args) > 1 {
			filter = strings.ToLower(args[1])
		}
		if err := runRebuild(filter); err != nil {
			fmt.Fprintf(os.Stderr, "Rebuild failed: %v\n", err)
			os.Exit(1)
		}
	case "testmod":
		if err := runTestMod(); err != nil {
			fmt.Fprintf(os.Stderr, "Testing mod application failed: %v\n", err)
			os.Exit(1)
		}
	case "randomize":
		if len(args) < 2 {
			fmt.Fprintf(os.Stderr, "Usage: mad2tool randomize <level_name>\n")
			os.Exit(1)
		}
		if err := runRandomize(args[1]); err != nil {
			fmt.Fprintf(os.Stderr, "Randomization failed: %v\n", err)
			os.Exit(1)
		}
	case "unpack-all":
		if len(args) < 2 {
			fmt.Fprintf(os.Stderr, "Usage: mad2tool unpack-all <gamedir> [outDir]\n")
			os.Exit(1)
		}
		outDir := ""
		if len(args) > 2 {
			outDir = args[2]
		}
		if err := cmdUnpackAll(args[1], outDir); err != nil {
			fmt.Fprintf(os.Stderr, "unpack-all failed: %v\n", err)
			os.Exit(1)
		}
	case "repack-changed":
		if len(args) < 2 {
			fmt.Fprintf(os.Stderr, "Usage: mad2tool repack-changed <gamedir> [unpackDir] [outDir]\n")
			os.Exit(1)
		}
		unpackDir := ""
		outDir := ""
		if len(args) > 2 {
			unpackDir = args[2]
		}
		if len(args) > 3 {
			outDir = args[3]
		}
		if err := cmdRepackChanged(args[1], unpackDir, outDir); err != nil {
			fmt.Fprintf(os.Stderr, "repack-changed failed: %v\n", err)
			os.Exit(1)
		}
	default:
		flag.Usage()
		os.Exit(1)
	}
}

// runExtract extracts all game archives to mad2raw/
func runExtract(filter string) error {
	srcDir := "mad2/Content/Streams/win"
	rawDir := "mad2raw"

	fmt.Println("=== Starting Extraction Pipeline ===")
	if err := os.MkdirAll(rawDir, 0755); err != nil {
		return err
	}

	entries, err := os.ReadDir(srcDir)
	if err != nil {
		return fmt.Errorf("read game streams: %w", err)
	}

	for _, e := range entries {
		ext := strings.ToLower(filepath.Ext(e.Name()))
		if ext != ".arc" && ext != ".bld" {
			continue
		}

		archivePath := filepath.Join(srcDir, e.Name())
		archiveBase := strings.TrimSuffix(e.Name(), ext)

		// For .bld archives, differentiate name
		outSubDir := archiveBase + "ARC"
		if ext == ".bld" {
			outSubDir = archiveBase + "BLD"
		}

		if filter != "" && !strings.Contains(strings.ToLower(e.Name()), filter) {
			continue
		}
		targetDir := filepath.Join(rawDir, outSubDir)

		fmt.Printf("Unpacking %s to %s...\n", e.Name(), targetDir)
		cmd := exec.Command("./mad2repack", "unpack", archivePath, "-o", targetDir)
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		if err := cmd.Run(); err != nil {
			return fmt.Errorf("unpack error: %w", err)
		}

		// Find all extracted .pak files and dump them to JSON, then remove the raw binaries
		if err := processDumps(targetDir); err != nil {
			return fmt.Errorf("process pak files: %w", err)
		}
	}

	fmt.Println("\nExtraction Pipeline complete!")
	return nil
}

func processDumps(dir string) error {
	return filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}

		if strings.ToLower(filepath.Ext(path)) == ".pak" {
			jsonPath := path + ".json"
			fmt.Printf("  Dumping PAK strings to %s...\n", jsonPath)

			cmd := exec.Command("./mad2repack", "pak-dump", path, jsonPath)
			if err := cmd.Run(); err != nil {
				return fmt.Errorf("fail dumping pak %s: %w", path, err)
			}

			// Remove the raw .pak binary so we build purely from JSON
			if err := os.Remove(path); err != nil {
				return fmt.Errorf("fail removing raw pak %s: %w", path, err)
			}
		}

		if strings.ToLower(filepath.Base(path)) == "level.bld" {
			origPath := path + ".orig"
			jsonPath := path + ".json"
			fmt.Printf("  Dumping level coords to %s...\n", jsonPath)

			// Rename original level.bld to level.bld.orig
			if err := os.Rename(path, origPath); err != nil {
				return fmt.Errorf("fail renaming level.bld: %w", err)
			}

			cmd := exec.Command("./mad2repack", "level-dump", origPath, jsonPath)
			if err := cmd.Run(); err != nil {
				return fmt.Errorf("fail dumping level %s: %w", path, err)
			}
		}

		if strings.ToLower(filepath.Ext(path)) == ".texs" {
			if err := dumpTexs(path); err != nil {
				return fmt.Errorf("fail dumping texs %s: %w", path, err)
			}
		}

		if strings.ToLower(filepath.Ext(path)) == ".snds" {
			if err := dumpSnds(path); err != nil {
				return fmt.Errorf("fail dumping snds %s: %w", path, err)
			}
		}
		return nil
	})
}

// dumpTexs decodes a raw ".texs" file at path to a PNG (path+".png") and, on
// success, REMOVES the raw ".texs" -- unlike unpack-all's "texs" Kind (which
// keeps a ".texs.orig" copy alongside its PNG, since iga.EncodeInsert needs
// the original container bytes as a splice base), extract/rebuild's srcDir
// (mad2/Content/Streams/win/) is always the pristine, unmodified game
// install, so rebuild can just re-read the same entry's original bytes
// fresh from there instead of keeping a redundant on-disk copy here (see
// compileTexs). If decoding fails, the raw .texs is left in place untouched
// -- same "warn and fall back to raw passthrough" behavior unpack-all uses
// for an undecodable texture; dumpSnds (below) follows the exact same
// "editable form fully replaces the raw file" shape for both of .snds's own
// two formats now that mad2iga has a real IMA-ADPCM encoder, not just for
// Ogg-Vorbis passthrough.
func dumpTexs(path string) error {
	rawData, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	img, _, decodeErr := iga.DecodeTexs(rawData)
	if decodeErr != nil {
		fmt.Printf("  Warning: %s: could not decode texture, keeping raw .texs: %v\n", path, decodeErr)
		return nil
	}
	pngPath := path + ".png"
	fmt.Printf("  Decoding texture to %s...\n", pngPath)
	pf, err := os.Create(pngPath)
	if err != nil {
		return err
	}
	pngErr := png.Encode(pf, img)
	pf.Close()
	if pngErr != nil {
		return fmt.Errorf("encode png: %w", pngErr)
	}
	if err := os.Remove(path); err != nil {
		return fmt.Errorf("remove raw texs: %w", err)
	}
	return nil
}

// dumpSnds handles a raw ".snds" file at path per docs/AUDIO_FORMAT.md's two
// distinct formats sharing the extension. Both now have a real way back to
// ".snds" -- mad2iga grew a real IMA-ADPCM encoder (fsb.go's RepackFSB3/
// RepackSnds), so this is no longer the one-way asymmetry it used to be --
// and both branches now DELETE the raw ".snds" on a successful decode,
// matching dumpTexs's own "the editable form fully replaces the raw file;
// rebuild re-reads the pristine original from srcDir instead of keeping a
// redundant on-disk copy" precedent (see compileSnds):
//
//   - Plain Ogg Vorbis (the ~2.4% minority, iga.IsOggVorbis) is ALREADY a
//     standard, directly-editable/reusable container under a nonstandard
//     extension -- iga.ExtractSnds returns it byte-for-byte unchanged with
//     ext=".ogg". Renamed to a clean "<name>.ogg" (the ".snds" infix is
//     DROPPED, not kept as "<name>.snds.ogg" -- a pure naming cleanup with
//     no raw ".snds" duplicate either way); compileSnds's identity copy-back
//     needs no format work at all.
//   - FSB3 (the ~97.6% majority) decodes to "<name>.snds.wav" -- a REAL
//     editable source now, not just a listening convenience. compileSnds
//     re-encodes an edited .wav back through mad2iga's new encoder
//     (iga.RepackSnds) using the matching entry's pristine original bytes
//     (re-read fresh from srcDir, same as compileTexs does for .texs) as
//     the FSB3 header template. An unedited .wav round-trips back to the
//     exact pristine bytes via a re-derived-baseline comparison, same
//     "verify unchanged, use pristine bytes verbatim, don't round-trip
//     through the (lossy) encoder for nothing" discipline compileTexs
//     already uses for DXT1/DXT3.
//
// A decode failure (neither valid FSB3 nor Ogg) leaves the raw .snds
// untouched, same fallback shape as dumpTexs/unpack-all.
func dumpSnds(path string) error {
	rawData, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	out, ext, decodeErr := iga.ExtractSnds(rawData)
	if decodeErr != nil {
		fmt.Printf("  Warning: %s: could not decode audio, keeping raw .snds: %v\n", path, decodeErr)
		return nil
	}
	if ext == ".ogg" {
		oggPath := strings.TrimSuffix(path, filepath.Ext(path)) + ".ogg" // "<name>.snds" -> "<name>.ogg"
		fmt.Printf("  Renaming Ogg Vorbis stream to %s...\n", oggPath)
		if err := os.WriteFile(oggPath, out, 0644); err != nil {
			return err
		}
		if err := os.Remove(path); err != nil {
			return fmt.Errorf("remove raw snds: %w", err)
		}
		return nil
	}
	wavPath := path + ext // ".wav"
	fmt.Printf("  Decoding audio to %s (editable -- re-encoded via mad2iga's IMA-ADPCM encoder on rebuild)...\n", wavPath)
	if err := os.WriteFile(wavPath, out, 0644); err != nil {
		return err
	}
	if err := os.Remove(path); err != nil {
		return fmt.Errorf("remove raw snds: %w", err)
	}
	return nil
}

// runRebuild rebuilds archives into mad2repacked/ based on changes in mad2raw/
func runRebuild(filter string) error {
	srcDir := "mad2/Content/Streams/win"
	rawDir := "mad2raw"
	repackedDir := "mad2repacked"

	fmt.Println("=== Starting Rebuild Pipeline ===")
	if err := os.MkdirAll(repackedDir, 0755); err != nil {
		return err
	}

	entries, err := os.ReadDir(rawDir)
	if err != nil {
		return fmt.Errorf("read raw directory: %w", err)
	}

	for _, e := range entries {
		if !e.IsDir() {
			continue
		}

		rawSubDir := filepath.Join(rawDir, e.Name())

		// Determine output name and filter out based on user request
		var outName string
		if strings.HasSuffix(e.Name(), "ARC") {
			if filter == "bld" {
				continue
			}
			if filter != "" && filter != "arc" && !strings.Contains(strings.ToLower(e.Name()), filter) {
				continue
			}
			outName = strings.TrimSuffix(e.Name(), "ARC") + ".arc"
		} else if strings.HasSuffix(e.Name(), "BLD") {
			if filter == "arc" {
				continue
			}
			if filter != "" && filter != "bld" && !strings.Contains(strings.ToLower(e.Name()), filter) {
				continue
			}
			outName = strings.TrimSuffix(e.Name(), "BLD") + ".bld"
		} else {
			continue
		}

		// Rebuild all .pak.json files in this subdirectory back to binary .pak
		if err := compilePaks(rawSubDir); err != nil {
			return fmt.Errorf("compile pak files: %w", err)
		}

		// Rebuild all level.bld.json files back to level.bld
		if err := compileLevels(rawSubDir); err != nil {
			return fmt.Errorf("compile level files: %w", err)
		}

		// originalPath is this archive's pristine, unmodified copy under
		// srcDir -- used both as compileTexs' base-bytes source (see its
		// doc comment) and, further below, as the final hash-compare/prune
		// baseline.
		originalPath := filepath.Join(srcDir, outName)

		// Re-encode any edited *.texs.png back to *.texs.
		if err := compileTexs(rawSubDir, originalPath); err != nil {
			return fmt.Errorf("compile texs files: %w", err)
		}

		// Restore any *.ogg (renamed at extract time) and re-encode any
		// edited *.snds.wav back to *.snds.
		if err := compileSnds(rawSubDir, originalPath); err != nil {
			return fmt.Errorf("compile snds files: %w", err)
		}

		outPath := filepath.Join(repackedDir, outName)

		// Compare directory state hashes or just pack
		fmt.Printf("Reassembling %s to %s...\n", rawSubDir, outPath)
		cmd := exec.Command("./mad2repack", "pack", rawSubDir, "-o", outPath)
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		if err := cmd.Run(); err != nil {
			return fmt.Errorf("pack error: %w", err)
		}

		// Clean up the temporarily compiled paks so rawDir remains clean JSON-only
		if err := cleanupCompiledPaks(rawSubDir); err != nil {
			return fmt.Errorf("cleanup compiled paks: %w", err)
		}

		// Clean up the temporarily compiled levels
		if err := cleanupCompiledLevels(rawSubDir); err != nil {
			return fmt.Errorf("cleanup compiled levels: %w", err)
		}

		// Clean up the temporarily re-encoded texs (only removes ones that
		// have a .png sibling -- see cleanupCompiledTexs's doc comment for
		// why an undecodable fallback raw .texs must NOT be swept up here)
		if err := cleanupCompiledTexs(rawSubDir); err != nil {
			return fmt.Errorf("cleanup compiled texs: %w", err)
		}

		// Clean up the temporarily restored/re-encoded snds (only removes
		// ones with an .ogg or .snds.wav sibling -- an undecodable-at-extract
		// raw .snds with neither sibling is the permanent on-disk form and
		// must survive, see cleanupCompiledSnds/compileSnds)
		if err := cleanupCompiledSnds(rawSubDir); err != nil {
			return fmt.Errorf("cleanup compiled snds: %w", err)
		}

		// Verify accuracy and debloat: Compare SHA-256 of repacked archive with the original.
		// If they match, delete from repackedDir to keep only modified archives.
		origHash, err := hashFile(originalPath)
		if err != nil {
			fmt.Printf("  Warning: could not hash original file %s: %v\n", originalPath, err)
			continue
		}
		newHash, err := hashFile(outPath)
		if err != nil {
			return fmt.Errorf("failed to hash repacked file %s: %w", outPath, err)
		}

		if origHash == newHash {
			fmt.Printf("  ✓ Identical to original. Pruning %s to debloat repacked output.\n", outName)
			if err := os.Remove(outPath); err != nil {
				return fmt.Errorf("failed to prune unmodified file: %w", err)
			}
		} else {
			fmt.Printf("  ★ Modified! Kept %s in repacked output.\n", outName)
		}
	}

	fmt.Println("\nRebuild Pipeline complete!")
	return nil
}

func hashFile(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()

	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return fmt.Sprintf("%x", h.Sum(nil)), nil
}

func compilePaks(dir string) error {
	return filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}

		if strings.HasSuffix(path, ".pak.json") {
			pakPath := strings.TrimSuffix(path, ".json")
			fmt.Printf("  Compiling PAK from %s...\n", path)
			cmd := exec.Command("./mad2repack", "pak-compile", path, pakPath)
			if err := cmd.Run(); err != nil {
				return fmt.Errorf("compile pak error: %w", err)
			}
		}
		return nil
	})
}

func cleanupCompiledPaks(dir string) error {
	return filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}

		if strings.ToLower(filepath.Ext(path)) == ".pak" {
			if err := os.Remove(path); err != nil {
				return err
			}
		}
		return nil
	})
}

// compileTexs walks dir for every "*.texs.png" (an edited or unedited
// decode produced by dumpTexs) and re-encodes it back to a temporary
// "*.texs" binary at the same path the original had (matching
// metadata.json's Path field, same shape as compilePaks/compileLevels'
// temp files, cleaned up after packing by cleanupCompiledTexs below).
//
// Unlike unpack-all's "texs" Kind, dumpTexs does NOT keep a ".texs.orig"
// copy of the original container bytes on disk -- iga.EncodeInsert needs
// the *original* file's Section 0/1 (and any mip levels beyond 0) as its
// splice base, so instead this re-reads that same entry fresh from
// originalArchivePath, which for extract/rebuild is always
// mad2/Content/Streams/win/<archive>, the pristine, never-mutated game
// install. This makes the on-disk PNG the sole source of truth for the
// base mip level, with no redundant raw copy anywhere under mad2raw/.
//
// Per-file change detection: unlike compilePaks/compileLevels (which
// unconditionally recompile every time -- there's no cheap way to tell if a
// .pak.json/level.bld.json "really" changed without just doing the work),
// a .texs.png IS cheap to check, and DXT1/DXT3 re-encoding is lossy (see
// texture.go), so blindly re-encoding every texture every rebuild would
// needlessly re-compress ones nobody touched. Instead of keeping a hash
// sidecar, the baseline is re-derived in memory: decode the pristine
// original entry the exact same way dumpTexs did at extract time and
// re-encode it to PNG bytes; if that matches the on-disk PNG byte-for-byte,
// nothing changed, so the pristine original bytes are used verbatim (true
// byte-identical passthrough) instead of round-tripping through
// EncodeInsert at all.
//
// If dir has zero "*.texs.png" files, the original archive is never even
// opened.
func compileTexs(dir, originalArchivePath string) error {
	var pngPaths []string
	if err := filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}
		if strings.HasSuffix(strings.ToLower(path), ".texs.png") {
			pngPaths = append(pngPaths, path)
		}
		return nil
	}); err != nil {
		return err
	}
	if len(pngPaths) == 0 {
		return nil
	}

	arc, err := iga.Open(originalArchivePath)
	if err != nil {
		return fmt.Errorf("open original archive %s for texs base bytes: %w", originalArchivePath, err)
	}
	defer arc.Close()

	byPath := make(map[string]int, len(arc.Entries))
	for i, fe := range arc.Entries {
		byPath[iga.SanitizePath(fe.Name)] = i
	}

	for _, pngPath := range pngPaths {
		rel, err := filepath.Rel(dir, pngPath)
		if err != nil {
			return err
		}
		rel = filepath.ToSlash(rel)
		texsRel := strings.TrimSuffix(rel, ".png")

		idx, ok := byPath[texsRel]
		if !ok {
			return fmt.Errorf("%s: no matching entry in original archive %s", texsRel, originalArchivePath)
		}

		var origBuf bytes.Buffer
		if err := arc.ExtractFile(idx, &origBuf); err != nil {
			return fmt.Errorf("read original texs bytes for %s: %w", texsRel, err)
		}
		origData := origBuf.Bytes()

		currentPNG, err := os.ReadFile(pngPath)
		if err != nil {
			return err
		}

		// Re-derive the baseline PNG dumpTexs would have written for this
		// pristine entry and compare byte-for-byte against what's on disk
		// now. Decode/re-encode is deterministic (same codec, same input
		// bytes, same default png.Encode options), so equality here really
		// does mean "unedited since extract".
		unchanged := false
		if baseImg, _, decErr := iga.DecodeTexs(origData); decErr == nil {
			var baseBuf bytes.Buffer
			if encErr := png.Encode(&baseBuf, baseImg); encErr == nil {
				unchanged = bytes.Equal(baseBuf.Bytes(), currentPNG)
			}
		}

		outPath := filepath.Join(dir, filepath.FromSlash(texsRel))
		if unchanged {
			fmt.Printf("  %s unchanged, passing original texs through untouched\n", pngPath)
			if err := os.WriteFile(outPath, origData, 0644); err != nil {
				return err
			}
			continue
		}

		fmt.Printf("  Compiling texture from %s...\n", pngPath)

		img, decErr := png.Decode(bytes.NewReader(currentPNG))
		if decErr != nil {
			return fmt.Errorf("decode png %s: %w", pngPath, decErr)
		}

		compiled, err := iga.EncodeInsert(origData, img)
		if err != nil {
			return fmt.Errorf("encode texs %s: %w", texsRel, err)
		}

		if err := os.WriteFile(outPath, compiled, 0644); err != nil {
			return err
		}
	}
	return nil
}

// cleanupCompiledTexs removes the temporary "*.texs" files compileTexs
// created, leaving only the PNG behind. It only removes a ".texs" file that
// has a ".texs.png" sibling -- a raw ".texs" with NO PNG sibling means
// dumpTexs's decode failed for that entry at extract time (see its doc
// comment) and that raw file IS the permanent, only on-disk form for the
// next rebuild too; sweeping it up here would silently break every
// subsequent rebuild since there is no PNG to regenerate it from.
func cleanupCompiledTexs(dir string) error {
	return filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}
		if strings.ToLower(filepath.Ext(path)) != ".texs" {
			return nil
		}
		if _, statErr := os.Stat(path + ".png"); statErr == nil {
			if err := os.Remove(path); err != nil {
				return err
			}
		}
		return nil
	})
}

// compileSnds walks dir for every ".snds"-derived editable file dumpSnds
// produced and restores/re-encodes it to a temporary "*.snds" at the
// original path (matching metadata.json's Path field, same shape as
// compilePaks/compileLevels/compileTexs' temp files, cleaned up after
// packing by cleanupCompiledSnds below):
//
//   - "*.ogg" (the Ogg-Vorbis minority, renamed from "<name>.snds" by
//     dumpSnds -- see its doc comment for why the ".snds" infix is dropped):
//     restored via a lossless identity copy back to "<name>.snds" -- dumpSnds's
//     Ogg branch never re-encodes anything, it only renames.
//   - "*.snds.wav" (the FSB3 majority): re-read the matching entry's
//     PRISTINE original bytes fresh from originalArchivePath (same
//     "srcDir is always the pristine, never-mutated game install" pattern
//     compileTexs already uses for .texs -- see its own doc comment),
//     re-derive the baseline WAV dumpSnds would have written for those
//     pristine bytes, and compare byte-for-byte against what's on disk now.
//     If unchanged, the pristine original .snds bytes are used verbatim
//     (true byte-identical passthrough, no re-encode round-trip at all --
//     IMA-ADPCM re-encoding is lossy, same reason compileTexs avoids blind
//     unconditional re-encoding of untouched textures). Only a .wav that
//     differs from its own re-derived baseline gets iga.RepackSnds'd.
//
// If dir has zero "*.snds.wav" files AND zero "*.ogg" files that match a
// ".snds" archive entry, the original archive is never even opened.
func compileSnds(dir, originalArchivePath string) error {
	var wavPaths []string
	var oggPaths []string
	if err := filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}
		lower := strings.ToLower(path)
		switch {
		case strings.HasSuffix(lower, ".snds.wav"):
			wavPaths = append(wavPaths, path)
		case strings.HasSuffix(lower, ".ogg"):
			oggPaths = append(oggPaths, path)
		}
		return nil
	}); err != nil {
		return err
	}
	if len(wavPaths) == 0 && len(oggPaths) == 0 {
		return nil
	}

	// Ogg restoration needs no archive access at all -- it's a pure rename
	// back, the same lossless identity transform dumpSnds itself did in
	// reverse.
	for _, oggPath := range oggPaths {
		sndsPath := strings.TrimSuffix(oggPath, ".ogg") + ".snds"
		fmt.Printf("  Restoring Ogg-in-snds container from %s...\n", oggPath)
		data, err := os.ReadFile(oggPath)
		if err != nil {
			return err
		}
		if err := os.WriteFile(sndsPath, data, 0644); err != nil {
			return err
		}
	}

	if len(wavPaths) == 0 {
		return nil
	}

	arc, err := iga.Open(originalArchivePath)
	if err != nil {
		return fmt.Errorf("open original archive %s for snds base bytes: %w", originalArchivePath, err)
	}
	defer arc.Close()

	byPath := make(map[string]int, len(arc.Entries))
	for i, fe := range arc.Entries {
		byPath[iga.SanitizePath(fe.Name)] = i
	}

	for _, wavPath := range wavPaths {
		rel, err := filepath.Rel(dir, wavPath)
		if err != nil {
			return err
		}
		rel = filepath.ToSlash(rel)
		sndsRel := strings.TrimSuffix(rel, ".wav")

		idx, ok := byPath[sndsRel]
		if !ok {
			return fmt.Errorf("%s: no matching entry in original archive %s", sndsRel, originalArchivePath)
		}

		var origBuf bytes.Buffer
		if err := arc.ExtractFile(idx, &origBuf); err != nil {
			return fmt.Errorf("read original snds bytes for %s: %w", sndsRel, err)
		}
		origData := origBuf.Bytes()

		currentWAV, err := os.ReadFile(wavPath)
		if err != nil {
			return err
		}

		// Re-derive the baseline WAV dumpSnds would have written for this
		// pristine entry and compare byte-for-byte against what's on disk
		// now. ExtractSnds is deterministic (same decoder, same input
		// bytes), so equality here really does mean "unedited since
		// extract".
		unchanged := false
		if baseOut, baseExt, decErr := iga.ExtractSnds(origData); decErr == nil && baseExt == ".wav" {
			unchanged = bytes.Equal(baseOut, currentWAV)
		}

		outPath := filepath.Join(dir, filepath.FromSlash(sndsRel))
		if unchanged {
			fmt.Printf("  %s unchanged, passing original snds through untouched\n", wavPath)
			if err := os.WriteFile(outPath, origData, 0644); err != nil {
				return err
			}
			continue
		}

		fmt.Printf("  Re-encoding audio from %s...\n", wavPath)
		compiled, err := iga.RepackSnds(origData, currentWAV)
		if err != nil {
			return fmt.Errorf("encode snds %s: %w", sndsRel, err)
		}
		if err := os.WriteFile(outPath, compiled, 0644); err != nil {
			return err
		}
	}
	return nil
}

// cleanupCompiledSnds removes the temporary "*.snds" files compileSnds
// created/restored -- only ones with an ".ogg" sibling (the Ogg-Vorbis
// minority) or a ".snds.wav" sibling (the FSB3 majority). dumpSnds always
// produces one or the other on a successful decode, so a raw ".snds" with
// NEITHER sibling means dumpSnds's decode failed at extract time, and that
// raw file is the permanent on-disk form for the next rebuild too -- same
// reasoning as cleanupCompiledTexs.
func cleanupCompiledSnds(dir string) error {
	return filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}
		if strings.ToLower(filepath.Ext(path)) != ".snds" {
			return nil
		}
		oggSibling := strings.TrimSuffix(path, ".snds") + ".ogg"
		if _, statErr := os.Stat(oggSibling); statErr == nil {
			return os.Remove(path)
		}
		if _, statErr := os.Stat(path + ".wav"); statErr == nil {
			return os.Remove(path)
		}
		return nil
	})
}

// runTestMod runs string substitutions and links English audio filenames to German audio
func runTestMod() error {
	rawDir := "mad2raw"
	fmt.Println("=== Applying Testing Mod modifications ===")

	// 1. Search and replace "beat mort" (any capitalization) to "fuck mort" in all .pak.json files
	err := filepath.Walk(rawDir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}

		if strings.HasSuffix(path, ".pak.json") {
			if err := patchPakJSON(path); err != nil {
				return fmt.Errorf("patch pak strings: %w", err)
			}
		}
		return nil
	})
	if err != nil {
		return err
	}

	// 2. Point English audio files for IslandFever to German ones
	// This means in the IslandFeverARC metadata.json, we swap the data files.
	if err := swapAudioInMetadata(); err != nil {
		return fmt.Errorf("audio metadata swap: %w", err)
	}

	fmt.Println("Modifications applied successfully!")
	return nil
}

func patchPakJSON(jsonPath string) error {
	data, err := os.ReadFile(jsonPath)
	if err != nil {
		return err
	}

	var meta iga.PakMeta
	if err := json.Unmarshal(data, &meta); err != nil {
		return err
	}

	modified := false
	for i, s := range meta.Strings {
		lower := strings.ToLower(s)
		if strings.Contains(lower, "beat mort") {
			// Find capitalization layout
			orig := s
			// Replace all instances of "beat mort" with "fuck mort" case-sensitively or just direct replace
			// Since case layout can be tricky, let's do a case-insensitive replacement.
			// Let's replace any instance of "beat mort" or similar with "fuck mort"
			rebuilt := replaceCaseInsensitive(s, "beat mort", "fuck mort")
			if rebuilt != orig {
				meta.Strings[i] = rebuilt
				modified = true
				fmt.Printf("  [%s]: Modified string:\n    Old: %q\n    New: %q\n", filepath.Base(jsonPath), orig, rebuilt)
			}
		}
	}

	if modified {
		newData, err := json.MarshalIndent(meta, "", "  ")
		if err != nil {
			return err
		}
		return os.WriteFile(jsonPath, newData, 0644)
	}

	return nil
}

func replaceCaseInsensitive(str, old, new string) string {
	lowerStr := strings.ToLower(str)
	lowerOld := strings.ToLower(old)

	idx := strings.Index(lowerStr, lowerOld)
	if idx == -1 {
		return str
	}

	return str[:idx] + new + replaceCaseInsensitive(str[idx+len(old):], old, new)
}

// ArchiveMeta/ArchiveFileMeta duplicates for JSON decoding
type ArchMeta struct {
	Version         uint32         `json:"version"`
	MemoryPoolIndex uint32         `json:"memory_pool_index"`
	Files           []ArchFileMeta `json:"files"`
}

type ArchFileMeta struct {
	Name string `json:"name"`
	CRC  uint32 `json:"crc"`
	Path string `json:"path"`
}

func swapAudioInMetadata() error {
	metaPath := "mad2raw/IslandFeverARC/metadata.json"
	data, err := os.ReadFile(metaPath)
	if err != nil {
		if os.IsNotExist(err) {
			return fmt.Errorf("metadata.json not found at %s. Did you run extract first?", metaPath)
		}
		return err
	}

	var meta ArchMeta
	if err := json.Unmarshal(data, &meta); err != nil {
		return err
	}

	// Scan through metadata and find English level sound files:
	// English: Name matches "builddata/win/*.snds" (no subfolders)
	// German: Name matches "builddata/win/german/*.snds"
	//
	// Instead of altering file data, we point the metadata paths!
	// E.g., if ENGLISH entry points to "builddata/win/l1_pg_boulderbustingfailure5_julian.snds",
	// we change its disk path to "builddata/win/german/l1_pg_boulderbustingfailure5_julian.snds".

	germanMap := make(map[string]string) // name -> path
	for _, f := range meta.Files {
		if strings.Contains(strings.ToLower(f.Name), "builddata/win/german/") && strings.HasSuffix(f.Name, ".snds") {
			germanMap[f.Name] = f.Path
		}
	}

	modifiedCount := 0
	for i, f := range meta.Files {
		// Is it an English sound file directly under builddata/win/ ?
		if strings.Contains(strings.ToLower(f.Name), "builddata/win/") && strings.HasSuffix(f.Name, ".snds") {
			if !strings.Contains(strings.ToLower(f.Name), "builddata/win/german/") &&
				!strings.Contains(strings.ToLower(f.Name), "builddata/win/spanish/") &&
				!strings.Contains(strings.ToLower(f.Name), "builddata/win/french/") &&
				!strings.Contains(strings.ToLower(f.Name), "builddata/win/italian/") &&
				!strings.Contains(strings.ToLower(f.Name), "builddata/win/dutch/") &&
				!strings.Contains(strings.ToLower(f.Name), "builddata/win/swedish/") {

				// Find corresponding German file
				filename := filepath.Base(f.Name)
				gerName := "c:/tfb/content/builddata/win/german/" + filename

				if gerPath, exists := germanMap[gerName]; exists {
					// Point this English entry to read from the German audio file on disk!
					meta.Files[i].Path = gerPath
					modifiedCount++
				}
			}
		}
	}

	if modifiedCount > 0 {
		fmt.Printf("  Pointed %d English audio streams to German disk files\n", modifiedCount)
		newData, err := json.MarshalIndent(meta, "", "  ")
		if err != nil {
			return err
		}
		return os.WriteFile(metaPath, newData, 0644)
	}

	return nil
}

func compileLevels(dir string) error {
	return filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}

		if strings.HasSuffix(strings.ToLower(path), "level.bld.json") {
			origPath := strings.TrimSuffix(path, ".json") + ".orig"
			outPath := strings.TrimSuffix(path, ".json")
			fmt.Printf("  Compiling Level from %s...\n", path)
			cmd := exec.Command("./mad2repack", "level-compile", path, origPath, outPath)
			if err := cmd.Run(); err != nil {
				return fmt.Errorf("compile level error: %w", err)
			}
		}
		return nil
	})
}

func cleanupCompiledLevels(dir string) error {
	return filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}

		if strings.HasSuffix(strings.ToLower(path), "level.bld") {
			// Remove the temporarily compiled level.bld
			if err := os.Remove(path); err != nil {
				return err
			}
		}
		return nil
	})
}

func runRandomize(levelName string) error {
	rawDir := "mad2raw"
	entries, err := os.ReadDir(rawDir)
	if err != nil {
		return err
	}

	targetSubDir := ""
	expected := strings.ToLower(levelName + "BLD")
	for _, e := range entries {
		if e.IsDir() && strings.ToLower(e.Name()) == expected {
			targetSubDir = e.Name()
			break
		}
	}

	if targetSubDir == "" {
		return fmt.Errorf("could not find raw level directory for name: %s", levelName)
	}

	jsonPath := filepath.Join(rawDir, targetSubDir, "level.bld.json")

	fmt.Printf("=== Randomizing coordinates for level %s ===\n", levelName)
	jsonData, err := os.ReadFile(jsonPath)
	if err != nil {
		return fmt.Errorf("could not read level JSON: %w", err)
	}

	var meta iga.LevelMeta
	if err := json.Unmarshal(jsonData, &meta); err != nil {
		return fmt.Errorf("could not parse level JSON: %w", err)
	}

	rand.Seed(time.Now().UnixNano())
	randomizedCount := 0
	for i := range meta.Coordinates {
		if strings.Contains(meta.Coordinates[i].Label, "item_placement") {
			meta.Coordinates[i].X += (rand.Float32() * 500.0) - 250.0
			meta.Coordinates[i].Y += (rand.Float32() * 500.0) - 250.0
			meta.Coordinates[i].Z += (rand.Float32() * 500.0) - 250.0
			randomizedCount++
		}
	}
	fmt.Printf("Randomized %d dynamic item placements.\n", randomizedCount)

	newJsonData, err := json.MarshalIndent(meta, "", "  ")
	if err != nil {
		return err
	}

	if err := os.WriteFile(jsonPath, newJsonData, 0644); err != nil {
		return err
	}

	fmt.Println("Coordinates randomized. Rebuilding level...")
	return runRebuild(levelName)
}

// ============================================================================
// unpack-all / repack-changed
//
// A change-detection layer on top of mad2iga's existing, solved primitives
// (iga.Open/ExtractFile, iga.RepackFromArchive, iga.DumpPak/CompilePak,
// iga.DumpLevelCoords/CompileLevelCoords). Distinct from extract/rebuild
// above: extract/rebuild always recompiles every .pak.json/level.bld.json
// found under mad2raw/ and only prunes the *output* archive afterward by
// comparing whole-archive hashes (see runRebuild's "Pruning" branch);
// unpack-all/repack-changed instead record a SHA256 baseline for every
// editable file up front and skip recompiling (and, for archives with zero
// changed entries, skip repacking at all) anything whose hash still matches
// that baseline. See docs/UNPACK_REPACK.md for the full design and
// real-file verification evidence.
// ============================================================================

// ManifestFile records one archive entry's on-disk representation and the
// baseline SHA256 hash(es) captured at unpack time, so repack-changed can
// tell whether the *editable* source (a derived JSON, or the raw file
// itself for passthrough-only formats) has been modified since.
type ManifestFile struct {
	Name   string `json:"name"` // original in-archive path (nametable name)
	CRC    uint32 `json:"crc"`
	Offset uint32 `json:"offset"` // original DataOffset (layout hint, mirrors ArchiveFileMeta.Offset)

	// Kind selects which mad2iga compile function (if any) turns the
	// editable on-disk form back into archive-entry bytes:
	//   "raw"   - RawPath's current bytes ARE the archive entry verbatim;
	//             no compile step. Covers everything with no known
	//             dump/compile pair in mad2iga today, as well as any file a
	//             real compiler could be added for later. Also the fallback
	//             for a .texs whose base mip level failed to decode, or a
	//             .snds that's neither valid FSB3 nor Ogg Vorbis, at unpack
	//             time (see "texs"/"snds" below) -- treated exactly like any
	//             other undecodable format.
	//   "pak"   - DerivedPath is pak-dump JSON (iga.PakMeta); iga.CompilePak
	//             rebuilds the binary .pak from it. No raw copy is kept on
	//             disk -- PakMeta.HeaderData is a self-contained base64 blob
	//             of the whole file minus the string pool, so nothing else
	//             is needed to recompile. Mirrors mad2tool's existing
	//             extract/processDumps, which likewise deletes the raw .pak
	//             once its JSON is dumped.
	//   "level" - DerivedPath is level-dump JSON (iga.LevelMeta); unlike
	//             "pak", iga.CompileLevelCoords needs the *original* raw
	//             bytes too (RawPath, kept alongside as "<name>.orig")
	//             since it only overwrites specific float offsets in a copy
	//             of that original file rather than re-serializing the
	//             whole IGZ from scratch.
	//   "texs"  - DerivedPath is a decoded PNG of the base (largest) mip
	//             level (iga.DecodeTexs/DecodeImage); RawPath is the
	//             original ".texs.orig" bytes, kept alongside for the same
	//             reason as "level" -- iga.EncodeInsert only overwrites the
	//             base mip level's byte range in a copy of that original
	//             file (Sections 0/1 and any mip levels beyond 0 are copied
	//             through byte-for-byte), it does not re-serialize the whole
	//             IGZ from scratch. Only used when DecodeTexs succeeds at
	//             unpack time -- see "raw" above for the fallback.
	//   "ogg"   - .snds files that are actually plain Ogg Vorbis reusing the
	//             extension (see docs/AUDIO_FORMAT.md). RawPath is a clean
	//             "<name>.ogg" (the ".snds" infix dropped) holding the exact
	//             original bytes -- already a standard, directly-editable
	//             container, so this is really "raw" under a friendlier
	//             on-disk name; no DerivedPath, no compile step.
	//   "snds"  - FSB3/IMA-ADPCM .snds files (~97.6% of the real corpus).
	//             DerivedPath is a decoded WAV (iga.ExtractSnds); RawPath is
	//             the original ".snds.orig" bytes, kept alongside for the
	//             same reason as "texs"/"level" -- iga.RepackSnds
	//             (mad2iga/fsb.go) needs the pristine original bytes as its
	//             FSB3 header/metadata template (name, loop points, mode,
	//             etc.), it does not reserialize a header from scratch.
	//             Only used when the entry parses as FSB3 -- see "raw" above
	//             for the fallback (should not happen for any real MAD2
	//             sample, but see docs/AUDIO_FORMAT.md's open questions).
	Kind string `json:"kind"`

	RawPath     string `json:"raw_path,omitempty"`     // relative to the archive's unpack directory
	DerivedPath string `json:"derived_path,omitempty"` // relative to the archive's unpack directory

	RawHash     string `json:"raw_hash,omitempty"`     // sha256(RawPath content) at unpack time
	DerivedHash string `json:"derived_hash,omitempty"` // sha256(DerivedPath content) at unpack time
}

// Manifest is written as manifest.json inside every archive's unpack
// directory (one manifest per unpacked archive, plain JSON, diffable).
type Manifest struct {
	ArchiveName      string         `json:"archive_name"` // e.g. "Credits.bld"
	SourcePath       string         `json:"source_path"`  // original archive path at unpack time, used by repack-changed as RepackFromArchive's srcPath
	Version          uint32         `json:"version"`
	MemoryPoolIndex  uint32         `json:"memory_pool_index"`
	OriginalReserved [4]uint32      `json:"original_reserved"`
	UnpackedAt       string         `json:"unpacked_at"` // RFC3339, informational only
	Files            []ManifestFile `json:"files"`
}

func sha256Hex(data []byte) string {
	sum := sha256.Sum256(data)
	return fmt.Sprintf("%x", sum)
}

// cmdUnpackAll unpacks every .arc/.bld archive under
// <gamedir>/Content/Streams/win/ into its own subdirectory of outDir (default
// <gamedir>/Content/Streams/win/unpacked/), each with a manifest.json hashing
// baseline.
func cmdUnpackAll(gamedir, outDir string) error {
	srcDir := filepath.Join(gamedir, "Content", "Streams", "win")
	if outDir == "" {
		outDir = filepath.Join(srcDir, "unpacked")
	}

	entries, err := os.ReadDir(srcDir)
	if err != nil {
		return fmt.Errorf("read game streams %s: %w", srcDir, err)
	}

	fmt.Printf("=== unpack-all: %s -> %s ===\n", srcDir, outDir)

	count := 0
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		ext := strings.ToLower(filepath.Ext(e.Name()))
		if ext != ".arc" && ext != ".bld" {
			continue
		}
		srcPath := filepath.Join(srcDir, e.Name())
		dstDir := filepath.Join(outDir, e.Name())
		if err := unpackOneArchiveWithManifest(srcPath, dstDir); err != nil {
			return fmt.Errorf("unpack %s: %w", e.Name(), err)
		}
		count++
	}

	fmt.Printf("=== unpack-all complete: %d archives ===\n", count)
	return nil
}

// unpackOneArchiveWithManifest extracts every entry of the archive at
// srcPath into dstDir, dumping .pak/level.bld entries to editable JSON via
// mad2iga (everything else is kept as a raw passthrough file), and writes a
// manifest.json hashing baseline for the whole directory.
func unpackOneArchiveWithManifest(srcPath, dstDir string) error {
	arc, err := iga.Open(srcPath)
	if err != nil {
		return err
	}
	defer arc.Close()

	if err := os.MkdirAll(dstDir, 0755); err != nil {
		return fmt.Errorf("create %s: %w", dstDir, err)
	}

	manifest := Manifest{
		ArchiveName:      filepath.Base(srcPath),
		SourcePath:       srcPath,
		Version:          arc.Header.Version,
		MemoryPoolIndex:  arc.Header.MemoryPoolIndex,
		OriginalReserved: arc.Header.Reserved,
		UnpackedAt:       time.Now().UTC().Format(time.RFC3339),
		Files:            make([]ManifestFile, len(arc.Entries)),
	}

	for i, fe := range arc.Entries {
		sanitized := iga.SanitizePath(fe.Name)
		diskPath := filepath.Join(dstDir, sanitized)
		if err := os.MkdirAll(filepath.Dir(diskPath), 0755); err != nil {
			return fmt.Errorf("create subdirectory for %s: %w", fe.Name, err)
		}

		var buf bytes.Buffer
		if err := arc.ExtractFile(i, &buf); err != nil {
			return fmt.Errorf("extract %s: %w", fe.Name, err)
		}
		rawData := buf.Bytes()

		mf := ManifestFile{Name: fe.Name, CRC: fe.CRC, Offset: fe.Descriptor.DataOffset}
		lowerName := strings.ToLower(sanitized)

		switch {
		case strings.HasSuffix(lowerName, ".pak"):
			// DumpPak reads from a path, not a reader, so write the raw pak
			// briefly, dump it, then drop the raw copy: PakMeta.HeaderData
			// is a self-contained base64 blob (see Kind's doc comment
			// above), so nothing else needs it to recompile.
			if err := os.WriteFile(diskPath, rawData, 0644); err != nil {
				return err
			}
			jsonPath := diskPath + ".json"
			jf, err := os.Create(jsonPath)
			if err != nil {
				return err
			}
			dumpErr := iga.DumpPak(diskPath, jf)
			jf.Close()
			if dumpErr != nil {
				return fmt.Errorf("pak-dump %s: %w", fe.Name, dumpErr)
			}
			if err := os.Remove(diskPath); err != nil {
				return err
			}
			jsonData, err := os.ReadFile(jsonPath)
			if err != nil {
				return err
			}
			mf.Kind = "pak"
			mf.DerivedPath = sanitized + ".json"
			mf.DerivedHash = sha256Hex(jsonData)

		case strings.HasSuffix(lowerName, "level.bld"):
			origPath := diskPath + ".orig"
			if err := os.WriteFile(origPath, rawData, 0644); err != nil {
				return err
			}
			jsonPath := diskPath + ".json"
			if err := iga.DumpLevelCoords(origPath, jsonPath); err != nil {
				return fmt.Errorf("level-dump %s: %w", fe.Name, err)
			}
			jsonData, err := os.ReadFile(jsonPath)
			if err != nil {
				return err
			}
			mf.Kind = "level"
			mf.RawPath = sanitized + ".orig"
			mf.DerivedPath = sanitized + ".json"
			mf.RawHash = sha256Hex(rawData)
			mf.DerivedHash = sha256Hex(jsonData)

		case strings.HasSuffix(lowerName, ".texs"):
			// Decode the base mip level to a PNG so it's actually editable
			// in an image editor, same "dump to editable form" treatment
			// .pak/level.bld already get. Keep the original bytes alongside
			// as "<name>.texs.orig" (mirrors "level"'s RawPath) since
			// EncodeInsert needs them as the base to splice new pixel bytes
			// into -- it does not regenerate Sections 0/1 or mip levels
			// beyond 0 from scratch.
			//
			// Not every .texs is guaranteed to decode (see
			// mad2iga/texture.go's package doc comment -- 0/186 real
			// samples failed as of this codec's own verification, but an
			// exotic format variant this repo's corpus doesn't contain is
			// still possible). A decode failure here is NOT treated as
			// fatal to the whole unpack-all run -- warn and fall back to
			// the same raw-passthrough handling every other undecodable
			// format already gets, so an unusual texture doesn't block
			// unpacking everything else.
			img, _, decodeErr := iga.DecodeTexs(rawData)
			if decodeErr != nil {
				fmt.Printf("  Warning: %s: could not decode texture, keeping raw passthrough only: %v\n", fe.Name, decodeErr)
				if err := os.WriteFile(diskPath, rawData, 0644); err != nil {
					return err
				}
				mf.Kind = "raw"
				mf.RawPath = sanitized
				mf.RawHash = sha256Hex(rawData)
				break
			}
			origPath := diskPath + ".orig"
			if err := os.WriteFile(origPath, rawData, 0644); err != nil {
				return err
			}
			pngPath := diskPath + ".png"
			pf, err := os.Create(pngPath)
			if err != nil {
				return err
			}
			pngErr := png.Encode(pf, img)
			pf.Close()
			if pngErr != nil {
				return fmt.Errorf("encode png for %s: %w", fe.Name, pngErr)
			}
			pngData, err := os.ReadFile(pngPath)
			if err != nil {
				return err
			}
			mf.Kind = "texs"
			mf.RawPath = sanitized + ".orig"
			mf.DerivedPath = sanitized + ".png"
			mf.RawHash = sha256Hex(rawData)
			mf.DerivedHash = sha256Hex(pngData)

		case strings.HasSuffix(lowerName, ".snds"):
			// .snds is not one format -- see docs/AUDIO_FORMAT.md. Both of
			// its two real shapes now get "dump to editable form" treatment,
			// same idea as .texs above, now that mad2iga has a real
			// IMA-ADPCM encoder (fsb.go's RepackFSB3/RepackSnds) alongside
			// its existing decoder:
			switch {
			case iga.IsOggVorbis(rawData):
				// Already a standard, directly-editable container reusing
				// the .snds extension -- rename losslessly to a clean
				// "<name>.ogg" (the ".snds" infix dropped entirely, no raw
				// ".snds" duplicate kept -- same naming cleanup
				// extract/rebuild's dumpSnds now does). Nothing to decode:
				// the on-disk .ogg bytes ARE the editable source.
				oggSanitized := strings.TrimSuffix(sanitized, ".snds") + ".ogg"
				oggDiskPath := filepath.Join(dstDir, filepath.FromSlash(oggSanitized))
				if err := os.MkdirAll(filepath.Dir(oggDiskPath), 0755); err != nil {
					return err
				}
				if err := os.WriteFile(oggDiskPath, rawData, 0644); err != nil {
					return err
				}
				mf.Kind = "ogg"
				mf.RawPath = oggSanitized
				mf.RawHash = sha256Hex(rawData)

			case iga.IsFSB3(rawData):
				// FSB3/IMA-ADPCM majority. Decode to a real editable WAV,
				// same "dump to editable form, keep the original bytes
				// alongside as the encoder's header/metadata template"
				// treatment "texs" gets above -- iga.RepackSnds needs the
				// pristine original bytes as its FSB3 header template (name,
				// loop points, mode, etc.), it does not reserialize a header
				// from scratch. A decode failure here (should not happen for
				// any real MAD2 sample, but see docs/AUDIO_FORMAT.md's open
				// questions) is NOT fatal to the run -- same warn-and-fall-
				// back-to-raw-passthrough behavior as an undecodable .texs.
				out, ext, decodeErr := iga.ExtractSnds(rawData)
				if decodeErr != nil || ext != ".wav" {
					fmt.Printf("  Warning: %s: could not decode audio, keeping raw passthrough only: %v\n", fe.Name, decodeErr)
					if err := os.WriteFile(diskPath, rawData, 0644); err != nil {
						return err
					}
					mf.Kind = "raw"
					mf.RawPath = sanitized
					mf.RawHash = sha256Hex(rawData)
					break
				}
				origPath := diskPath + ".orig"
				if err := os.WriteFile(origPath, rawData, 0644); err != nil {
					return err
				}
				wavPath := diskPath + ".wav"
				if err := os.WriteFile(wavPath, out, 0644); err != nil {
					return err
				}
				mf.Kind = "snds"
				mf.RawPath = sanitized + ".orig"
				mf.DerivedPath = sanitized + ".wav"
				mf.RawHash = sha256Hex(rawData)
				mf.DerivedHash = sha256Hex(out)

			default:
				// Neither FSB3 nor Ogg -- unknown .snds variant, same raw
				// passthrough fallback every other undecodable format gets.
				if err := os.WriteFile(diskPath, rawData, 0644); err != nil {
					return err
				}
				mf.Kind = "raw"
				mf.RawPath = sanitized
				mf.RawHash = sha256Hex(rawData)
			}

		default:
			// Raw passthrough: anything else with no known dump/compile
			// pair in mad2iga today. The raw file on disk IS the editable
			// source -- if its hash changes, its new bytes are used as the
			// replacement entry data directly.
			if err := os.WriteFile(diskPath, rawData, 0644); err != nil {
				return err
			}
			mf.Kind = "raw"
			mf.RawPath = sanitized
			mf.RawHash = sha256Hex(rawData)
		}

		manifest.Files[i] = mf
	}

	manifestPath := filepath.Join(dstDir, "manifest.json")
	mfFile, err := os.Create(manifestPath)
	if err != nil {
		return fmt.Errorf("create %s: %w", manifestPath, err)
	}
	enc := json.NewEncoder(mfFile)
	enc.SetIndent("", "  ")
	encErr := enc.Encode(manifest)
	mfFile.Close()
	if encErr != nil {
		return fmt.Errorf("write %s: %w", manifestPath, encErr)
	}

	fmt.Printf("  %s: %d entries -> %s\n", filepath.Base(srcPath), len(arc.Entries), dstDir)
	return nil
}

// cmdRepackChanged walks a directory tree produced by unpack-all (default
// <gamedir>/Content/Streams/win/unpacked/), and for each archive subdirectory
// re-hashes every editable file against its manifest.json baseline. Archives
// with zero changed entries are skipped entirely (no repack call, no output
// file -- a true skip, not a rebuild-then-prune). Archives with at least one
// changed entry get recompiled via the matching mad2iga compile function and
// fed into iga.RepackFromArchive as a replacement map; every entry NOT in
// that map is copied byte-for-byte from the original archive by
// RepackFromArchive itself, so only the changed subset of each archive is
// ever rebuilt.
func cmdRepackChanged(gamedir, unpackDir, outDir string) error {
	srcDir := filepath.Join(gamedir, "Content", "Streams", "win")
	if unpackDir == "" {
		unpackDir = filepath.Join(srcDir, "unpacked")
	}
	if outDir == "" {
		outDir = filepath.Join(srcDir, "repacked_changed")
	}

	entries, err := os.ReadDir(unpackDir)
	if err != nil {
		return fmt.Errorf("read unpack directory %s (run unpack-all first?): %w", unpackDir, err)
	}

	fmt.Printf("=== repack-changed: %s -> %s ===\n", unpackDir, outDir)

	repacked, skipped := 0, 0
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		archDir := filepath.Join(unpackDir, e.Name())
		manifestPath := filepath.Join(archDir, "manifest.json")

		data, err := os.ReadFile(manifestPath)
		if err != nil {
			if os.IsNotExist(err) {
				continue // not an unpack-all output directory
			}
			return fmt.Errorf("read %s: %w", manifestPath, err)
		}
		var manifest Manifest
		if err := json.Unmarshal(data, &manifest); err != nil {
			return fmt.Errorf("parse %s: %w", manifestPath, err)
		}

		replacements := make(map[string][]byte)
		for _, f := range manifest.Files {
			switch f.Kind {
			case "raw":
				cur, err := os.ReadFile(filepath.Join(archDir, f.RawPath))
				if err != nil {
					return fmt.Errorf("%s: read %s: %w", manifest.ArchiveName, f.RawPath, err)
				}
				if sha256Hex(cur) != f.RawHash {
					replacements[f.Name] = cur
				}

			case "pak":
				jsonPath := filepath.Join(archDir, f.DerivedPath)
				jsonData, err := os.ReadFile(jsonPath)
				if err != nil {
					return fmt.Errorf("%s: read %s: %w", manifest.ArchiveName, f.DerivedPath, err)
				}
				if sha256Hex(jsonData) == f.DerivedHash {
					continue // unchanged, don't recompile -- passthrough via RepackFromArchive
				}
				var buf bytes.Buffer
				if err := iga.CompilePak(bytes.NewReader(jsonData), &buf); err != nil {
					return fmt.Errorf("%s: pak-compile %s: %w", manifest.ArchiveName, f.DerivedPath, err)
				}
				replacements[f.Name] = buf.Bytes()

			case "level":
				jsonPath := filepath.Join(archDir, f.DerivedPath)
				jsonData, err := os.ReadFile(jsonPath)
				if err != nil {
					return fmt.Errorf("%s: read %s: %w", manifest.ArchiveName, f.DerivedPath, err)
				}
				if sha256Hex(jsonData) == f.DerivedHash {
					continue // unchanged, don't recompile -- passthrough via RepackFromArchive
				}
				origPath := filepath.Join(archDir, f.RawPath)
				tmpOut := jsonPath + ".compiled.tmp"
				if err := iga.CompileLevelCoords(jsonPath, origPath, tmpOut); err != nil {
					return fmt.Errorf("%s: level-compile %s: %w", manifest.ArchiveName, f.DerivedPath, err)
				}
				compiled, err := os.ReadFile(tmpOut)
				os.Remove(tmpOut)
				if err != nil {
					return err
				}
				replacements[f.Name] = compiled

			case "texs":
				pngPath := filepath.Join(archDir, f.DerivedPath)
				pngData, err := os.ReadFile(pngPath)
				if err != nil {
					return fmt.Errorf("%s: read %s: %w", manifest.ArchiveName, f.DerivedPath, err)
				}
				if sha256Hex(pngData) == f.DerivedHash {
					continue // unchanged, don't recompile -- passthrough via RepackFromArchive
				}
				origPath := filepath.Join(archDir, f.RawPath)
				origData, err := os.ReadFile(origPath)
				if err != nil {
					return fmt.Errorf("%s: read %s: %w", manifest.ArchiveName, f.RawPath, err)
				}
				img, err := png.Decode(bytes.NewReader(pngData))
				if err != nil {
					return fmt.Errorf("%s: decode png %s: %w", manifest.ArchiveName, f.DerivedPath, err)
				}
				compiled, err := iga.EncodeInsert(origData, img)
				if err != nil {
					return fmt.Errorf("%s: tex-insert %s: %w", manifest.ArchiveName, f.DerivedPath, err)
				}
				replacements[f.Name] = compiled

			case "ogg":
				// Plain Ogg Vorbis reusing the .snds extension -- the
				// on-disk .ogg bytes ARE the archive entry content
				// verbatim, same as "raw" above, just under a friendlier
				// on-disk name/extension (see unpackOneArchiveWithManifest).
				cur, err := os.ReadFile(filepath.Join(archDir, f.RawPath))
				if err != nil {
					return fmt.Errorf("%s: read %s: %w", manifest.ArchiveName, f.RawPath, err)
				}
				if sha256Hex(cur) != f.RawHash {
					replacements[f.Name] = cur
				}

			case "snds":
				// FSB3/IMA-ADPCM majority -- re-encode an edited WAV via
				// mad2iga's encoder (fsb.go's RepackFSB3/RepackSnds), using
				// the pristine original .snds bytes (f.RawPath) as the
				// header/metadata template, same "unchanged means
				// byte-identical passthrough, only re-encode what actually
				// changed" discipline as "texs" above.
				wavPath := filepath.Join(archDir, f.DerivedPath)
				wavData, err := os.ReadFile(wavPath)
				if err != nil {
					return fmt.Errorf("%s: read %s: %w", manifest.ArchiveName, f.DerivedPath, err)
				}
				if sha256Hex(wavData) == f.DerivedHash {
					continue // unchanged, don't recompile -- passthrough via RepackFromArchive
				}
				origPath := filepath.Join(archDir, f.RawPath)
				origData, err := os.ReadFile(origPath)
				if err != nil {
					return fmt.Errorf("%s: read %s: %w", manifest.ArchiveName, f.RawPath, err)
				}
				compiled, err := iga.RepackSnds(origData, wavData)
				if err != nil {
					return fmt.Errorf("%s: snds-encode %s: %w", manifest.ArchiveName, f.DerivedPath, err)
				}
				replacements[f.Name] = compiled

			default:
				return fmt.Errorf("%s: manifest entry %q has unknown kind %q", manifest.ArchiveName, f.Name, f.Kind)
			}
		}

		if len(replacements) == 0 {
			fmt.Printf("  %-40s unchanged, skipped (0/%d entries)\n", manifest.ArchiveName, len(manifest.Files))
			skipped++
			continue
		}

		if err := os.MkdirAll(outDir, 0755); err != nil {
			return fmt.Errorf("create %s: %w", outDir, err)
		}
		outPath := filepath.Join(outDir, manifest.ArchiveName)
		if err := iga.RepackFromArchive(manifest.SourcePath, outPath, replacements); err != nil {
			return fmt.Errorf("%s: repack: %w", manifest.ArchiveName, err)
		}
		fmt.Printf("  %-40s repacked (%d/%d entries changed) -> %s\n", manifest.ArchiveName, len(replacements), len(manifest.Files), outPath)
		repacked++
	}

	fmt.Printf("=== repack-changed complete: %d repacked, %d unchanged (skipped) ===\n", repacked, skipped)
	return nil
}
