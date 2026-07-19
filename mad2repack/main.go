// mad2repack is a command-line tool for repacking IGA archives.
// It supports round-trip verification (extract → repack → hash compare)
// and file replacement within archives.
//
// Usage:
//
//	mad2repack repack <archive> [-o output]
//	mad2repack verify <archive>
//	mad2repack replace <archive> -o <output> -swap <name1>=<name2>
//	mad2repack replace <archive> -o <output> -file <name>=<path>
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"
	"time"

	"mad2iga"
)

func main() {
	if len(os.Args) < 2 {
		printUsage()
		os.Exit(1)
	}

	cmd := os.Args[1]
	args := os.Args[2:]

	switch cmd {
	case "verify":
		if len(args) == 0 {
			fmt.Fprintf(os.Stderr, "Usage: mad2repack verify <archive> [more...]\n")
			os.Exit(1)
		}
		ok := true
		for _, path := range args {
			if err := cmdVerify(path); err != nil {
				fmt.Fprintf(os.Stderr, "FAIL %s: %v\n", path, err)
				ok = false
			}
		}
		if !ok {
			os.Exit(1)
		}

	case "verify-all":
		if len(args) == 0 {
			fmt.Fprintf(os.Stderr, "Usage: mad2repack verify-all <directory>\n")
			os.Exit(1)
		}
		if err := cmdVerifyAll(args[0]); err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}

	case "repack":
		if len(args) < 1 {
			fmt.Fprintf(os.Stderr, "Usage: mad2repack repack <archive> [-o output]\n")
			os.Exit(1)
		}
		srcPath := args[0]
		outPath := ""
		for i := 1; i < len(args); i++ {
			if args[i] == "-o" && i+1 < len(args) {
				outPath = args[i+1]
				i++
			}
		}
		if outPath == "" {
			ext := filepath.Ext(srcPath)
			base := strings.TrimSuffix(srcPath, ext)
			outPath = base + "_repacked" + ext
		}
		if err := cmdRepack(srcPath, outPath); err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}

	case "replace":
		if len(args) < 1 {
			fmt.Fprintf(os.Stderr, "Usage: mad2repack replace <archive> -o <output> [-swap name1=name2] [-file name=path]\n")
			os.Exit(1)
		}
		srcPath := args[0]
		outPath := ""
		var swaps [][2]string
		var fileReplacements [][2]string

		for i := 1; i < len(args); i++ {
			switch args[i] {
			case "-o":
				if i+1 < len(args) {
					outPath = args[i+1]
					i++
				}
			case "-swap":
				if i+1 < len(args) {
					parts := strings.SplitN(args[i+1], "=", 2)
					if len(parts) == 2 {
						swaps = append(swaps, [2]string{parts[0], parts[1]})
					}
					i++
				}
			case "-swap-list":
				if i+1 < len(args) {
					listFile := args[i+1]
					i++
					content, err := os.ReadFile(listFile)
					if err != nil {
						fmt.Fprintf(os.Stderr, "Error reading swap list: %v\n", err)
						os.Exit(1)
					}
					lines := strings.Split(string(content), "\n")
					for _, line := range lines {
						line = strings.TrimSpace(line)
						if line == "" || strings.HasPrefix(line, "#") {
							continue
						}
						parts := strings.SplitN(line, "=", 2)
						if len(parts) == 2 {
							swaps = append(swaps, [2]string{parts[0], parts[1]})
						}
					}
				}
			case "-file":
				if i+1 < len(args) {
					parts := strings.SplitN(args[i+1], "=", 2)
					if len(parts) == 2 {
						fileReplacements = append(fileReplacements, [2]string{parts[0], parts[1]})
					}
					i++
				}
			}
		}

		if outPath == "" {
			fmt.Fprintf(os.Stderr, "Error: -o output path is required\n")
			os.Exit(1)
		}

		if err := cmdReplace(srcPath, outPath, swaps, fileReplacements); err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}

	case "unpack":
		if len(args) < 1 {
			fmt.Fprintf(os.Stderr, "Usage: mad2repack unpack <archive> [-o directory]\n")
			os.Exit(1)
		}
		srcPath := args[0]
		outDir := ""
		for i := 1; i < len(args); i++ {
			if args[i] == "-o" && i+1 < len(args) {
				outDir = args[i+1]
				i++
			}
		}
		if outDir == "" {
			ext := filepath.Ext(srcPath)
			outDir = strings.TrimSuffix(srcPath, ext) + "_unpacked"
		}
		if err := cmdUnpack(srcPath, outDir); err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}

	case "pack":
		if len(args) < 1 {
			fmt.Fprintf(os.Stderr, "Usage: mad2repack pack <directory> -o <output_archive>\n")
			os.Exit(1)
		}
		srcDir := args[0]
		outPath := ""
		version := uint32(0x02)
		memPool := uint32(0)

		for i := 1; i < len(args); i++ {
			switch args[i] {
			case "-o":
				if i+1 < len(args) {
					outPath = args[i+1]
					i++
				}
			case "-version":
				if i+1 < len(args) {
					fmt.Sscanf(args[i+1], "0x%x", &version)
					if version == 0 {
						fmt.Sscanf(args[i+1], "%d", &version)
					}
					i++
				}
			case "-mempool":
				if i+1 < len(args) {
					fmt.Sscanf(args[i+1], "%d", &memPool)
					i++
				}
			}
		}
		if outPath == "" {
			fmt.Fprintf(os.Stderr, "Error: -o output path is required\n")
			os.Exit(1)
		}
		if err := cmdPack(srcDir, outPath, version, memPool); err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}

	case "pak-dump":
		if len(args) < 2 {
			fmt.Fprintf(os.Stderr, "Usage: mad2repack pak-dump <file.pak> <output.json>\n")
			os.Exit(1)
		}
		if err := cmdPakDump(args[0], args[1]); err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}

	case "pak-compile":
		if len(args) < 2 {
			fmt.Fprintf(os.Stderr, "Usage: mad2repack pak-compile <input.json> <output.pak>\n")
			os.Exit(1)
		}
		if err := cmdPakCompile(args[0], args[1]); err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}

	case "level-dump":
		if len(args) < 2 {
			fmt.Fprintf(os.Stderr, "Usage: mad2repack level-dump <level.bld> <output.json>\n")
			os.Exit(1)
		}
		if err := iga.DumpLevelCoords(args[0], args[1]); err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}

	case "level-compile":
		if len(args) < 3 {
			fmt.Fprintf(os.Stderr, "Usage: mad2repack level-compile <input.json> <orig_level.bld> <output_level.bld>\n")
			os.Exit(1)
		}
		if err := iga.CompileLevelCoords(args[0], args[1], args[2]); err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}

	case "objects-dump":
		if len(args) < 2 {
			fmt.Fprintf(os.Stderr, "Usage: mad2repack objects-dump <level.bld> <output.json>\n")
			os.Exit(1)
		}
		if err := cmdObjectsDump(args[0], args[1]); err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}

	case "verify-binary":
		if len(args) < 2 {
			fmt.Fprintf(os.Stderr, "Usage: mad2repack verify-binary <original.pak/iga> <repacked.pak/iga>\n")
			os.Exit(1)
		}
		if err := cmdVerifyBinary(args[0], args[1]); err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}

	default:
		fmt.Fprintf(os.Stderr, "Unknown command: %s\n\n", cmd)
		printUsage()
		os.Exit(1)
	}
}

func printUsage() {
	fmt.Fprintf(os.Stderr, "mad2repack — Madagascar 2 IGA Archive Repacker\n\n")
	fmt.Fprintf(os.Stderr, "Commands:\n")
	fmt.Fprintf(os.Stderr, "  verify <archive>           Extract and repack, verify content hash matches\n")
	fmt.Fprintf(os.Stderr, "  verify-all <directory>     Verify all .arc files in directory\n")
	fmt.Fprintf(os.Stderr, "  verify-binary <orig> <rep> Perform deep side-by-side structure diff of original vs repacked binaries\n")
	fmt.Fprintf(os.Stderr, "  unpack <archive> [-o dir]  Dump all archive files to disk with metadata\n")
	fmt.Fprintf(os.Stderr, "  pack <dir> -o <archive>    Rebuild an archive from an unpacked disk directory\n")
	fmt.Fprintf(os.Stderr, "  repack <archive> [-o out]  Repack an archive (all files stored uncompressed)\n")
	fmt.Fprintf(os.Stderr, "  replace <archive> -o <out> Replace files in an archive\n")
	fmt.Fprintf(os.Stderr, "    -swap name1=name2        Swap two files' data within the archive\n")
	fmt.Fprintf(os.Stderr, "    -swap-list file          Read swap pairs (name1=name2) from a text file\n")
	fmt.Fprintf(os.Stderr, "    -file name=path          Replace a file with data from disk\n")
	fmt.Fprintf(os.Stderr, "  pak-dump <pak> <out.json>  Dump PAK metadata and UTF-16 strings to JSON\n")
	fmt.Fprintf(os.Stderr, "  pak-compile <js> <out.pak> Compile new PAK file from JSON metadata\n")
	fmt.Fprintf(os.Stderr, "  level-dump <bld> <out.json> Dump level float32 coordinates to JSON\n")
	fmt.Fprintf(os.Stderr, "  level-compile <js> <orig.bld> <out.bld> Compile level coordinates from JSON metadata\n")
	fmt.Fprintf(os.Stderr, "  objects-dump <bld> <out.json> Dump the real object graph (class + known fields), not a byte-pattern guess\n")
}

// cmdVerify extracts all files from an archive, repacks them, and verifies
// that extracting from the repacked archive yields identical content.
func cmdVerify(srcPath string) error {
	startTime := time.Now()
	baseName := filepath.Base(srcPath)
	fmt.Printf("=== Verifying %s ===\n", baseName)

	// Open source archive
	arc, err := iga.Open(srcPath)
	if err != nil {
		return err
	}
	defer arc.Close()

	// Extract all files
	entries := make([]iga.RepackEntry, len(arc.Entries))
	originalHashes := make([]string, len(arc.Entries))

	for i, fe := range arc.Entries {
		entries[i].Name = fe.Name
		entries[i].CRC = fe.CRC

		var buf bytes.Buffer
		if err := arc.ExtractFile(i, &buf); err != nil {
			return fmt.Errorf("extract file %d (%s): %w", i, fe.Name, err)
		}
		entries[i].Data = buf.Bytes()
		originalHashes[i] = fmt.Sprintf("%x", sha256.Sum256(entries[i].Data))
	}

	fmt.Printf("  Extracted %d files\n", len(entries))

	// Repack to temp file
	tmpPath := srcPath + ".repack_test"
	defer os.Remove(tmpPath)

	opts := iga.RepackOptions{
		Version:         arc.Header.Version,
		MemoryPoolIndex: arc.Header.MemoryPoolIndex,
	}
	arc.Close()

	if err := iga.Repack(tmpPath, entries, opts); err != nil {
		return fmt.Errorf("repack: %w", err)
	}

	// Open repacked archive and verify
	arc2, err := iga.Open(tmpPath)
	if err != nil {
		return fmt.Errorf("open repacked: %w", err)
	}
	defer arc2.Close()

	if int(arc2.Header.FileCount) != len(entries) {
		return fmt.Errorf("file count mismatch: original=%d repacked=%d", len(entries), arc2.Header.FileCount)
	}

	// Build a map from CRC to original hash for lookup
	crcToHash := make(map[uint32]string)
	crcToName := make(map[uint32]string)
	for i, e := range entries {
		crcToHash[e.CRC] = originalHashes[i]
		crcToName[e.CRC] = e.Name
	}

	mismatches := 0
	for i, fe := range arc2.Entries {
		var buf bytes.Buffer
		if err := arc2.ExtractFile(i, &buf); err != nil {
			return fmt.Errorf("extract repacked file %d (%s): %w", i, fe.Name, err)
		}
		repackedHash := fmt.Sprintf("%x", sha256.Sum256(buf.Bytes()))

		origHash, ok := crcToHash[fe.CRC]
		if !ok {
			fmt.Printf("  WARNING: CRC 0x%08X not found in original\n", fe.CRC)
			mismatches++
			continue
		}

		if repackedHash != origHash {
			name := crcToName[fe.CRC]
			fmt.Printf("  MISMATCH: %s (orig=%d bytes, repacked=%d bytes)\n",
				name, len(entries[i].Data), buf.Len())
			mismatches++
		}
	}

	elapsed := time.Since(startTime)
	if mismatches == 0 {
		fmt.Printf("  ✓ PASS — %d files verified in %.2fs\n", len(entries), elapsed.Seconds())
	} else {
		fmt.Printf("  ✗ FAIL — %d/%d mismatches\n", mismatches, len(entries))
	}

	return nil
}

// cmdVerifyAll verifies all .arc files in a directory.
func cmdVerifyAll(dir string) error {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return err
	}

	startTime := time.Now()
	total := 0
	failed := 0

	for _, e := range entries {
		ext := strings.ToLower(filepath.Ext(e.Name()))
		if ext != ".arc" && ext != ".bld" {
			continue
		}
		total++
		path := filepath.Join(dir, e.Name())
		if err := cmdVerify(path); err != nil {
			fmt.Fprintf(os.Stderr, "  FAIL: %v\n", err)
			failed++
		}
	}

	elapsed := time.Since(startTime)
	fmt.Printf("\n=== Summary: %d/%d passed in %.2fs ===\n", total-failed, total, elapsed.Seconds())

	if failed > 0 {
		return fmt.Errorf("%d archives failed verification", failed)
	}
	return nil
}

// cmdRepack extracts and repacks an archive to the output path.
func cmdRepack(srcPath, outPath string) error {
	fmt.Printf("Repacking %s → %s\n", filepath.Base(srcPath), outPath)

	if err := iga.RepackFromArchive(srcPath, outPath, nil); err != nil {
		return err
	}

	// Print size comparison
	srcInfo, _ := os.Stat(srcPath)
	dstInfo, _ := os.Stat(outPath)
	if srcInfo != nil && dstInfo != nil {
		fmt.Printf("  Original: %.2f MB\n", float64(srcInfo.Size())/(1024*1024))
		fmt.Printf("  Repacked: %.2f MB\n", float64(dstInfo.Size())/(1024*1024))
	}

	fmt.Println("  Done")
	return nil
}

// cmdReplace repacks an archive with file replacements.
func cmdReplace(srcPath, outPath string, swaps [][2]string, fileReplacements [][2]string) error {
	fmt.Printf("Replacing in %s → %s\n", filepath.Base(srcPath), outPath)

	// Open source to read swap data
	arc, err := iga.Open(srcPath)
	if err != nil {
		return err
	}

	replacements := make(map[string][]byte)

	// Process swaps — extract both files and swap their data
	for _, swap := range swaps {
		name1, name2 := swap[0], swap[1]
		var data1, data2 []byte

		for i, fe := range arc.Entries {
			if fe.Name == name1 {
				var buf bytes.Buffer
				if err := arc.ExtractFile(i, &buf); err != nil {
					arc.Close()
					return fmt.Errorf("extract %s: %w", name1, err)
				}
				data1 = buf.Bytes()
			}
			if fe.Name == name2 {
				var buf bytes.Buffer
				if err := arc.ExtractFile(i, &buf); err != nil {
					arc.Close()
					return fmt.Errorf("extract %s: %w", name2, err)
				}
				data2 = buf.Bytes()
			}
		}

		if data1 == nil {
			arc.Close()
			return fmt.Errorf("file not found: %s", name1)
		}
		if data2 == nil {
			arc.Close()
			return fmt.Errorf("file not found: %s", name2)
		}

		replacements[name1] = data2
		replacements[name2] = data1
		fmt.Printf("  Swapping: %s ↔ %s\n", name1, name2)
	}

	// Process file replacements from disk
	for _, fr := range fileReplacements {
		name, path := fr[0], fr[1]
		data, err := os.ReadFile(path)
		if err != nil {
			arc.Close()
			return fmt.Errorf("read replacement file %s: %w", path, err)
		}
		replacements[name] = data
		fmt.Printf("  Replacing: %s with %s (%d bytes)\n", name, path, len(data))
	}

	arc.Close()

	if err := iga.RepackFromArchive(srcPath, outPath, replacements); err != nil {
		return err
	}

	// Print size comparison
	srcInfo, _ := os.Stat(srcPath)
	dstInfo, _ := os.Stat(outPath)
	if srcInfo != nil && dstInfo != nil {
		fmt.Printf("  Original: %.2f MB\n", float64(srcInfo.Size())/(1024*1024))
		fmt.Printf("  Repacked: %.2f MB\n", float64(dstInfo.Size())/(1024*1024))
	}

	fmt.Println("  Done")
	return nil
}

type ArchiveMeta struct {
	Version          uint32            `json:"version"`
	MemoryPoolIndex  uint32            `json:"memory_pool_index"`
	OriginalReserved []uint32          `json:"original_reserved"`
	Files            []ArchiveFileMeta `json:"files"`
}

type ArchiveFileMeta struct {
	Name   string `json:"name"`
	CRC    uint32 `json:"crc"`
	Path   string `json:"path"`
	Offset uint32 `json:"offset"`
}

// cmdUnpack dumps all files to disk and writes a metadata.json describing the archive.
func cmdUnpack(srcPath, outDir string) error {
	fmt.Printf("Unpacking %s to %s...\n", filepath.Base(srcPath), outDir)

	arc, err := iga.Open(srcPath)
	if err != nil {
		return err
	}
	defer arc.Close()

	if err := os.MkdirAll(outDir, 0755); err != nil {
		return fmt.Errorf("create output directory: %w", err)
	}

	meta := ArchiveMeta{
		Version:          arc.Header.Version,
		MemoryPoolIndex:  arc.Header.MemoryPoolIndex,
		OriginalReserved: arc.Header.Reserved[:],
		Files:            make([]ArchiveFileMeta, len(arc.Entries)),
	}

	for i, fe := range arc.Entries {
		sanitized := iga.SanitizePath(fe.Name)
		diskPath := filepath.Join(outDir, sanitized)

		dir := filepath.Dir(diskPath)
		if err := os.MkdirAll(dir, 0755); err != nil {
			return fmt.Errorf("create subdirectory: %w", err)
		}

		f, err := os.Create(diskPath)
		if err != nil {
			return fmt.Errorf("create file %s: %w", diskPath, err)
		}

		if err := arc.ExtractFile(i, f); err != nil {
			f.Close()
			return fmt.Errorf("extract %s: %w", fe.Name, err)
		}
		f.Close()

		meta.Files[i] = ArchiveFileMeta{
			Name:   fe.Name,
			CRC:    fe.CRC,
			Path:   sanitized,
			Offset: fe.Descriptor.DataOffset,
		}
	}

	// Write metadata.json
	metaPath := filepath.Join(outDir, "metadata.json")
	metaFile, err := os.Create(metaPath)
	if err != nil {
		return fmt.Errorf("create metadata.json: %w", err)
	}
	defer metaFile.Close()

	// Write with indentation
	encoder := json.NewEncoder(metaFile)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(meta); err != nil {
		return fmt.Errorf("write metadata.json: %w", err)
	}

	fmt.Printf("  Extracted %d files and wrote metadata.json\n", len(arc.Entries))
	return nil
}

// cmdPack compiles an archive from a directory using metadata.json.
func cmdPack(srcDir, outPath string, version, memPool uint32) error {
	metaPath := filepath.Join(srcDir, "metadata.json")
	metaFile, err := os.Open(metaPath)
	if err != nil {
		return fmt.Errorf("open metadata.json (make sure you unpacked first or created metadata.json): %w", err)
	}
	defer metaFile.Close()

	var meta ArchiveMeta
	if err := json.NewDecoder(metaFile).Decode(&meta); err != nil {
		return fmt.Errorf("parse metadata.json: %w", err)
	}

	// Use values from CLI if customized, otherwise use metadata.json values
	repackVersion := meta.Version
	if version != 0x02 {
		repackVersion = version
	}
	repackMemPool := meta.MemoryPoolIndex
	if memPool != 0 {
		repackMemPool = memPool
	}

	fmt.Printf("Packing %s into %s...\n", srcDir, outPath)

	entries := make([]iga.RepackEntry, len(meta.Files))
	for i, fMeta := range meta.Files {
		diskPath := filepath.Join(srcDir, fMeta.Path)
		data, err := os.ReadFile(diskPath)
		if err != nil {
			return fmt.Errorf("read file %s: %w", diskPath, err)
		}

		entries[i] = iga.RepackEntry{
			Name:   fMeta.Name,
			CRC:    fMeta.CRC,
			Data:   data,
			Offset: fMeta.Offset,
		}
	}

	var origReserved [4]uint32
	if len(meta.OriginalReserved) >= 4 {
		copy(origReserved[:], meta.OriginalReserved[:4])
	}

	opts := iga.RepackOptions{
		Version:          repackVersion,
		MemoryPoolIndex:  repackMemPool,
		OriginalReserved: origReserved,
	}

	if err := iga.Repack(outPath, entries, opts); err != nil {
		return err
	}

	fmt.Println("  Successfully packed archive.")
	return nil
}

// cmdPakDump dumps PAK/IGZ metadata and strings to JSON.
func cmdPakDump(pakPath, jsonPath string) error {
	f, err := os.Create(jsonPath)
	if err != nil {
		return fmt.Errorf("create json output: %w", err)
	}
	defer f.Close()

	if err := iga.DumpPak(pakPath, f); err != nil {
		return err
	}

	fmt.Printf("  Dumped %s strings/metadata to %s\n", filepath.Base(pakPath), jsonPath)
	return nil
}

// cmdPakCompile compiles a new PAK/IGZ file from JSON metadata.
func cmdPakCompile(jsonPath, pakPath string) error {
	jf, err := os.Open(jsonPath)
	if err != nil {
		return fmt.Errorf("open json input: %w", err)
	}
	defer jf.Close()

	pf, err := os.Create(pakPath)
	if err != nil {
		return fmt.Errorf("create pak output: %w", err)
	}
	defer pf.Close()

	if err := iga.CompilePak(jf, pf); err != nil {
		return err
	}

	fmt.Printf("  Compiled %s from %s\n", filepath.Base(pakPath), jsonPath)
	return nil
}

// cmdVerifyBinary performs a deep side-by-side comparison of two binary files.
// It detects whether the files are IGA archives (.arc/.bld) or PAK files (.pak/IGZ).
// It prints a detailed structural breakdown comparing the two, followed by a byte diff if they differ.
func cmdVerifyBinary(origPath, repPath string) error {
	origBytes, err := os.ReadFile(origPath)
	if err != nil {
		return fmt.Errorf("read original file: %w", err)
	}
	repBytes, err := os.ReadFile(repPath)
	if err != nil {
		return fmt.Errorf("read repacked file: %w", err)
	}

	fmt.Printf("Comparing Original (%s) vs Repacked (%s)\n", origPath, repPath)
	fmt.Printf("  Original Size: %d bytes\n", len(origBytes))
	fmt.Printf("  Repacked Size: %d bytes\n", len(repBytes))

	// Detect File Types
	isIGA := len(origBytes) >= 4 && binary.LittleEndian.Uint32(origBytes[0:4]) == 0x1A414749
	isPAK := len(origBytes) >= 4 && binary.LittleEndian.Uint32(origBytes[0:4]) == 0x49475A01

	if isIGA {
		fmt.Println("  Detected File Type: IGA Archive (.arc/.bld)")
		if len(origBytes) < 48 || len(repBytes) < 48 {
			return fmt.Errorf("file too small for IGA header comparison")
		}
		// Parse headers side-by-side
		origVer := binary.LittleEndian.Uint32(origBytes[4:8])
		repVer := binary.LittleEndian.Uint32(repBytes[4:8])
		origTableSize := binary.LittleEndian.Uint32(origBytes[8:12])
		repTableSize := binary.LittleEndian.Uint32(repBytes[8:12])
		origCount := binary.LittleEndian.Uint32(origBytes[12:16])
		repCount := binary.LittleEndian.Uint32(repBytes[12:16])
		origPool := binary.LittleEndian.Uint32(origBytes[20:24])
		repPool := binary.LittleEndian.Uint32(repBytes[20:24])
		origNameOff := binary.LittleEndian.Uint32(origBytes[24:28])
		repNameOff := binary.LittleEndian.Uint32(repBytes[24:28])

		fmt.Println("\n--- Header Comparison ---")
		fmt.Printf("  %-20s | %-12s | %-12s\n", "Field", "Original", "Repacked")
		fmt.Printf("  %-20s | %-12s | %-12s\n", "--------------------", "------------", "------------")
		fmt.Printf("  %-20s | 0x%08X   | 0x%08X\n", "Magic", binary.LittleEndian.Uint32(origBytes[0:4]), binary.LittleEndian.Uint32(repBytes[0:4]))
		fmt.Printf("  %-20s | %-12d | %-12d\n", "Version", origVer, repVer)
		fmt.Printf("  %-20s | %-12d | %-12d\n", "FileTableSize", origTableSize, repTableSize)
		fmt.Printf("  %-20s | %-12d | %-12d\n", "FileCount", origCount, repCount)
		fmt.Printf("  %-20s | %-12d | %-12d\n", "MemoryPoolIndex", origPool, repPool)
		fmt.Printf("  %-20s | 0x%08X   | 0x%08X\n", "NametableOffset", origNameOff, repNameOff)

	} else if isPAK {
		fmt.Println("  Detected File Type: PAK / IGZ v4 String Container")
		if len(origBytes) < 48 || len(repBytes) < 48 {
			return fmt.Errorf("file too small for PAK header comparison")
		}
		origVer := binary.LittleEndian.Uint32(origBytes[4:8])
		repVer := binary.LittleEndian.Uint32(repBytes[4:8])
		origSec0Off := binary.LittleEndian.Uint32(origBytes[16:20])
		repSec0Off := binary.LittleEndian.Uint32(repBytes[16:20])
		origSec0Size := binary.LittleEndian.Uint32(origBytes[20:24])
		repSec0Size := binary.LittleEndian.Uint32(repBytes[20:24])
		origSec2Off := binary.LittleEndian.Uint32(origBytes[32:36])
		repSec2Off := binary.LittleEndian.Uint32(repBytes[32:36])
		origSec2Size := binary.LittleEndian.Uint32(origBytes[36:40])
		repSec2Size := binary.LittleEndian.Uint32(repBytes[36:40])

		fmt.Println("\n--- Header Comparison ---")
		fmt.Printf("  %-20s | %-12s | %-12s\n", "Field", "Original", "Repacked")
		fmt.Printf("  %-20s | %-12s | %-12s\n", "--------------------", "------------", "------------")
		fmt.Printf("  %-20s | 0x%08X   | 0x%08X\n", "Magic", binary.LittleEndian.Uint32(origBytes[0:4]), binary.LittleEndian.Uint32(repBytes[0:4]))
		fmt.Printf("  %-20s | %-12d | %-12d\n", "Version", origVer, repVer)
		fmt.Printf("  %-20s | 0x%08X   | 0x%08X\n", "Sec0Offset", origSec0Off, repSec0Off)
		fmt.Printf("  %-20s | %-12d | %-12d\n", "Sec0Size", origSec0Size, repSec0Size)
		fmt.Printf("  %-20s | 0x%08X   | 0x%08X\n", "Sec2Offset", origSec2Off, repSec2Off)
		fmt.Printf("  %-20s | %-12d | %-12d\n", "Sec2Size", origSec2Size, repSec2Size)
	} else {
		fmt.Println("  Detected File Type: Generic Binary File")
	}

	// Side-by-side Byte Diff
	diffCount := 0
	maxDiffs := 20
	fmt.Println("\n--- Byte Differences (First 20) ---")
	fmt.Printf("  %-12s | %-8s | %-8s\n", "Offset (Hex)", "Original", "Repacked")
	fmt.Printf("  %-12s | %-8s | %-8s\n", "------------", "--------", "--------")

	minLen := len(origBytes)
	if len(repBytes) < minLen {
		minLen = len(repBytes)
	}

	for i := 0; i < minLen; i++ {
		if origBytes[i] != repBytes[i] {
			diffCount++
			if diffCount <= maxDiffs {
				fmt.Printf("  0x%08X   |  0x%02X    |  0x%02X\n", i, origBytes[i], repBytes[i])
			}
		}
	}

	if len(origBytes) != len(repBytes) {
		diffCount += int(math.Abs(float64(len(origBytes) - len(repBytes))))
		fmt.Printf("  File sizes differ by %d bytes.\n", int(math.Abs(float64(len(origBytes)-len(repBytes)))))
	}

	if diffCount == 0 {
		fmt.Println("  Status: Binary Match! Files are identical.")
	} else {
		fmt.Printf("  Status: Binary Dismatch! Found %d byte differences.\n", diffCount)
	}

	return nil
}
