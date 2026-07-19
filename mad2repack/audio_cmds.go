package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	iga "mad2iga"
)

// cmdSndExtract decodes one `.snds` file to a playable file. The output
// extension is chosen automatically (`.wav` for FSB3-decoded samples,
// `.ogg` for the plain-Ogg-Vorbis `.snds` files that exist verbatim in the
// game's assets — see docs/AUDIO_FORMAT.md) and appended to outPath if it
// doesn't already end in the right one.
func cmdSndExtract(inPath, outPath string) error {
	data, err := os.ReadFile(inPath)
	if err != nil {
		return fmt.Errorf("read %s: %w", inPath, err)
	}

	out, ext, err := iga.ExtractSnds(data)
	if err != nil {
		return fmt.Errorf("extract %s: %w", inPath, err)
	}

	if strings.ToLower(filepath.Ext(outPath)) != ext {
		outPath += ext
	}

	if err := os.WriteFile(outPath, out, 0644); err != nil {
		return fmt.Errorf("write %s: %w", outPath, err)
	}

	fmt.Printf("  %s -> %s (%s, %d bytes)\n", inPath, outPath, ext, len(out))
	return nil
}

// cmdSndExtractAll recursively finds every `.snds` file under srcDir and
// decodes each to outDir, mirroring the directory structure.
func cmdSndExtractAll(srcDir, outDir string) error {
	var total, ok, oggCount, failCount int

	err := filepath.Walk(srcDir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() || strings.ToLower(filepath.Ext(path)) != ".snds" {
			return nil
		}
		total++

		rel, err := filepath.Rel(srcDir, path)
		if err != nil {
			rel = filepath.Base(path)
		}

		data, err := os.ReadFile(path)
		if err != nil {
			fmt.Fprintf(os.Stderr, "  FAIL %s: %v\n", rel, err)
			failCount++
			return nil
		}

		out, ext, err := iga.ExtractSnds(data)
		if err != nil {
			fmt.Fprintf(os.Stderr, "  FAIL %s: %v\n", rel, err)
			failCount++
			return nil
		}

		outPath := filepath.Join(outDir, strings.TrimSuffix(rel, filepath.Ext(rel))+ext)
		if err := os.MkdirAll(filepath.Dir(outPath), 0755); err != nil {
			return fmt.Errorf("create output directory for %s: %w", outPath, err)
		}
		if err := os.WriteFile(outPath, out, 0644); err != nil {
			return fmt.Errorf("write %s: %w", outPath, err)
		}

		ok++
		if ext == ".ogg" {
			oggCount++
		}
		return nil
	})
	if err != nil {
		return err
	}

	fmt.Printf("  Extracted %d/%d files (%d Ogg passthrough, %d FSB3-decoded, %d failed) to %s\n",
		ok, total, oggCount, ok-oggCount, failCount, outDir)
	if failCount > 0 {
		return fmt.Errorf("%d file(s) failed to extract", failCount)
	}
	return nil
}

// cmdTexInfo prints what mad2iga can currently determine about a `.texs`
// file — container/class metadata and a best-effort dimension guess. See
// docs/TEXTURE_FORMAT.md for exactly what is and isn't solved for this
// format; this intentionally does not attempt to decode pixel data.
func cmdTexInfo(inPath string) error {
	data, err := os.ReadFile(inPath)
	if err != nil {
		return fmt.Errorf("read %s: %w", inPath, err)
	}

	info, err := iga.ParseTexs(data)
	if err != nil {
		return fmt.Errorf("parse %s: %w", inPath, err)
	}

	fmt.Printf("  %s\n", inPath)
	fmt.Printf("    classes:       %v\n", info.Classes)
	fmt.Printf("    is igImage2:   %v\n", info.IsImage)
	if info.GuessedWidth > 0 {
		fmt.Printf("    guessed size:  %dx%d (heuristic — see docs/TEXTURE_FORMAT.md)\n", info.GuessedWidth, info.GuessedHeight)
	} else {
		fmt.Printf("    guessed size:  unknown (not a square power-of-two full-mipchain match)\n")
	}
	fmt.Printf("    pixel blob:    %d bytes (layout undecoded, see docs/TEXTURE_FORMAT.md)\n", len(info.PixelData))
	return nil
}
