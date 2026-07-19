// mad2arc is a command-line tool for extracting files from IGA archives
// (.arc and .bld files) used by Madagascar: Escape 2 Africa and other
// games built on the Vicarious Visions Alchemy engine.
//
// Usage:
//
//	mad2arc [flags] <archive.arc|archive.bld> [more archives...]
//
// Flags:
//
//	-o <dir>    Output directory (default: current directory)
//	-l          List files without extracting
//	-v          Verbose output
//	-flat       Extract all files into a single flat directory
//	-all        Process all .arc and .bld files in the given directory
package main

import (
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	"mad2iga"
)

func main() {
	outDir := flag.String("o", ".", "Output directory for extracted files")
	listOnly := flag.Bool("l", false, "List archive contents without extracting")
	verbose := flag.Bool("v", false, "Verbose output")
	flat := flag.Bool("flat", false, "Extract files into a flat directory (no subdirectories)")
	all := flag.Bool("all", false, "Process all .arc and .bld files in the given directory")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "mad2arc — Madagascar 2 IGA Archive Extractor\n")
		fmt.Fprintf(os.Stderr, "Based on the Alchemy engine IGA format (version 0x02)\n\n")
		fmt.Fprintf(os.Stderr, "Usage:\n")
		fmt.Fprintf(os.Stderr, "  mad2arc [flags] <archive.arc|archive.bld> [more...]\n")
		fmt.Fprintf(os.Stderr, "  mad2arc [flags] -all <directory>\n\n")
		fmt.Fprintf(os.Stderr, "Flags:\n")
		flag.PrintDefaults()
		fmt.Fprintf(os.Stderr, "\nExamples:\n")
		fmt.Fprintf(os.Stderr, "  mad2arc -o extracted/ global.arc\n")
		fmt.Fprintf(os.Stderr, "  mad2arc -l title.bld\n")
		fmt.Fprintf(os.Stderr, "  mad2arc -all -o extracted/ Content/Streams/win/\n")
	}
	flag.Parse()

	args := flag.Args()
	if len(args) == 0 {
		flag.Usage()
		os.Exit(1)
	}

	// Gather archive file paths
	var archivePaths []string
	if *all {
		for _, dir := range args {
			entries, err := os.ReadDir(dir)
			if err != nil {
				fmt.Fprintf(os.Stderr, "Error reading directory %s: %v\n", dir, err)
				os.Exit(1)
			}
			for _, e := range entries {
				ext := strings.ToLower(filepath.Ext(e.Name()))
				if ext == ".arc" || ext == ".bld" {
					archivePaths = append(archivePaths, filepath.Join(dir, e.Name()))
				}
			}
		}
	} else {
		archivePaths = args
	}

	if len(archivePaths) == 0 {
		fmt.Fprintf(os.Stderr, "No archive files found.\n")
		os.Exit(1)
	}

	totalFiles := 0
	totalBytes := int64(0)
	startTime := time.Now()

	for _, path := range archivePaths {
		n, b, err := processArchive(path, *outDir, *listOnly, *verbose, *flat)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error processing %s: %v\n", path, err)
			continue
		}
		totalFiles += n
		totalBytes += b
	}

	if !*listOnly {
		elapsed := time.Since(startTime)
		fmt.Printf("\nDone: %d files extracted (%.2f MB) in %.2fs\n",
			totalFiles, float64(totalBytes)/(1024*1024), elapsed.Seconds())
	}
}

func processArchive(path, outDir string, listOnly, verbose, flat bool) (int, int64, error) {
	arc, err := iga.Open(path)
	if err != nil {
		return 0, 0, err
	}
	defer arc.Close()

	archiveName := filepath.Base(path)
	archiveBase := strings.TrimSuffix(archiveName, filepath.Ext(archiveName))

	fmt.Printf("=== %s ===\n", archiveName)
	fmt.Printf("  Version:    0x%02X\n", arc.Header.Version)
	fmt.Printf("  Files:      %d\n", arc.Header.FileCount)
	fmt.Printf("  Nametable:  0x%08X (%d bytes)\n", arc.Header.NametableOffset, arc.Header.NametableSize)

	if arc.BigEndian {
		fmt.Printf("  Endianness: Big-Endian\n")
	} else {
		fmt.Printf("  Endianness: Little-Endian\n")
	}
	fmt.Println()

	if listOnly {
		printFileList(arc)
		return 0, 0, nil
	}

	extractedCount := 0
	var totalBytes int64

	for i, entry := range arc.Entries {
		outputPath := buildOutputPath(outDir, archiveBase, entry, flat)

		if verbose {
			compStr := "raw"
			if entry.IsCompressed() {
				compStr = "zlib"
			}
			fmt.Printf("  [%d/%d] %s (%s, %d bytes)\n",
				i+1, len(arc.Entries), entry.Name, compStr, entry.Descriptor.DecompressedSize)
		}

		if err := extractEntry(arc, i, outputPath); err != nil {
			fmt.Fprintf(os.Stderr, "  ERROR extracting %s: %v\n", entry.Name, err)
			continue
		}

		totalBytes += int64(entry.Descriptor.DecompressedSize)
		extractedCount++
	}

	fmt.Printf("  Extracted %d/%d files\n", extractedCount, len(arc.Entries))
	return extractedCount, totalBytes, nil
}

func printFileList(arc *iga.Archive) {
	fmt.Printf("  %-6s  %-12s  %-10s  %-8s  %s\n",
		"Index", "Offset", "Size", "Mode", "Name")
	fmt.Printf("  %-6s  %-12s  %-10s  %-8s  %s\n",
		"-----", "------", "----", "----", "----")

	for i, entry := range arc.Entries {
		modeStr := "raw"
		if entry.IsCompressed() {
			modeStr = fmt.Sprintf("0x%02X", entry.Descriptor.Mode)
		}
		fmt.Printf("  %-6d  0x%08X    %-10d  %-8s  %s\n",
			i,
			entry.Descriptor.DataOffset,
			entry.Descriptor.DecompressedSize,
			modeStr,
			entry.Name,
		)
	}
	fmt.Println()
}

func buildOutputPath(outDir, archiveBase string, entry iga.FileEntry, flat bool) string {
	sanitized := iga.SanitizePath(entry.Name)

	if flat {
		sanitized = filepath.Base(sanitized)
	}

	return filepath.Join(outDir, archiveBase, sanitized)
}

func extractEntry(arc *iga.Archive, index int, outputPath string) error {
	// Create parent directories
	dir := filepath.Dir(outputPath)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return fmt.Errorf("create directory %s: %w", dir, err)
	}

	f, err := os.Create(outputPath)
	if err != nil {
		return fmt.Errorf("create file %s: %w", outputPath, err)
	}
	defer f.Close()

	// Use a buffered writer for better performance
	bw := &bufferedWriter{w: f, buf: make([]byte, 0, 256*1024)}

	if err := arc.ExtractFile(index, bw); err != nil {
		os.Remove(outputPath)
		return err
	}

	if err := bw.Flush(); err != nil {
		os.Remove(outputPath)
		return err
	}

	return nil
}

// bufferedWriter is a simple buffered writer to reduce syscall overhead.
type bufferedWriter struct {
	w   io.Writer
	buf []byte
}

func (bw *bufferedWriter) Write(p []byte) (int, error) {
	if len(bw.buf)+len(p) > cap(bw.buf) {
		if err := bw.Flush(); err != nil {
			return 0, err
		}
		if len(p) >= cap(bw.buf) {
			return bw.w.Write(p)
		}
	}
	bw.buf = append(bw.buf, p...)
	return len(p), nil
}

func (bw *bufferedWriter) Flush() error {
	if len(bw.buf) > 0 {
		_, err := bw.w.Write(bw.buf)
		bw.buf = bw.buf[:0]
		return err
	}
	return nil
}
