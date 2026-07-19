// mad2tool is a high-level orchestration script for setting up an automated
// ROM hacking workflow for Madagascar 2 PC files.
//
// Commands:
//
//	extract   Extracts all archives from mad2/Content/Streams/win/ to mad2raw/
//	          Additionally dumps nested .pak files to self-contained .pak.json meta files.
//	rebuild   Composes files from mad2raw/ and compiles them back to game-ready archives
//	          in mad2repacked/ (calculating hashes to only repack updated directories).
//	testmod   Executes the "Murder Mort" patch and IslandFever audio substitution.
package main

import (
	"crypto/sha256"
	"encoding/json"
	"flag"
	"fmt"
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
		fmt.Fprintf(os.Stderr, "  mad2tool extract    Dump game assets to mad2raw/\n")
		fmt.Fprintf(os.Stderr, "  mad2tool rebuild    Rebuild modified assets to mad2repacked/\n")
		fmt.Fprintf(os.Stderr, "  mad2tool testmod    Apply testing swaps & translations on raw dumps\n")
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
		return nil
	})
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

		// Verify accuracy and debloat: Compare SHA-256 of repacked archive with the original.
		// If they match, delete from repackedDir to keep only modified archives.
		originalPath := filepath.Join(srcDir, outName)
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
