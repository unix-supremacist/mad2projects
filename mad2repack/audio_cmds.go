package main

import (
	"fmt"
	"image/png"
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

// cmdTexInfo prints what mad2iga can determine about a `.texs` file —
// container/class metadata, the real (not guessed) dimensions/format read
// off the live igImage2 instance, and the pixel blob size. See
// docs/TEXTURE_FORMAT.md for the full reverse-engineering trail behind
// every field printed here.
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
	if info.Width > 0 {
		fmt.Printf("    size:          %dx%d, depth=%d, levelCount=%d (mip chain length %d), imageCount=%d\n",
			info.Width, info.Height, info.Depth, info.LevelCount, info.LevelCount+1, info.ImageCount)
		fmt.Printf("    format:        %s (formatRaw=0x%08X — external fixup marker, not per-instance; see docs/TEXTURE_FORMAT.md)\n", info.Format, info.FormatRaw)
	} else {
		fmt.Printf("    size:          unknown (could not locate the igImage2 object's fields)\n")
	}
	if info.GuessedWidth > 0 {
		fmt.Printf("    old heuristic guess: %dx%d (superseded — kept only as a cross-check, see TexsInfo.GuessedWidth's doc comment)\n", info.GuessedWidth, info.GuessedHeight)
	}
	fmt.Printf("    pixel blob:    %d bytes total (all mip levels)\n", len(info.PixelData))
	if info.Format != iga.FormatUnknown {
		if img, err := iga.DecodeImage(info); err == nil {
			b := img.Bounds()
			fmt.Printf("    base mip:      decodes OK, %dx%d (see 'tex-extract' to save as PNG)\n", b.Dx(), b.Dy())
		} else {
			fmt.Printf("    base mip:      decode failed: %v\n", err)
		}
	}
	return nil
}

// cmdTexExtract decodes a `.texs` file's base mip level and writes it as a
// PNG. See docs/TEXTURE_FORMAT.md for exactly what's confirmed about the
// decode (DXT1/DXT3 block layout, verified via spatial-autocorrelation
// tests on real samples — not just "doesn't crash").
func cmdTexExtract(inPath, outPath string) error {
	data, err := os.ReadFile(inPath)
	if err != nil {
		return fmt.Errorf("read %s: %w", inPath, err)
	}
	img, info, err := iga.DecodeTexs(data)
	if err != nil {
		return fmt.Errorf("decode %s: %w", inPath, err)
	}
	f, err := os.Create(outPath)
	if err != nil {
		return fmt.Errorf("create %s: %w", outPath, err)
	}
	defer f.Close()
	if err := png.Encode(f, img); err != nil {
		return fmt.Errorf("encode PNG: %w", err)
	}
	fmt.Printf("  %s -> %s (%dx%d, %s)\n", inPath, outPath, info.Width, info.Height, info.Format)
	return nil
}

// cmdTexInsert re-encodes a PNG as the base mip level of an existing
// `.texs` file's texture, writing a new `.texs` file. The PNG must exactly
// match the original texture's dimensions (see EncodeInsert's doc comment
// for why resizing isn't supported) — everything outside the base mip
// level (container structure, any mip levels beyond 0) is preserved
// byte-for-byte from the input `.texs`. See EncodeInsert's doc comment for
// the mip-levels-beyond-0-go-stale caveat.
func cmdTexInsert(inTexsPath, inPngPath, outPath string) error {
	origData, err := os.ReadFile(inTexsPath)
	if err != nil {
		return fmt.Errorf("read %s: %w", inTexsPath, err)
	}
	pf, err := os.Open(inPngPath)
	if err != nil {
		return fmt.Errorf("open %s: %w", inPngPath, err)
	}
	img, err := png.Decode(pf)
	pf.Close()
	if err != nil {
		return fmt.Errorf("decode PNG %s: %w", inPngPath, err)
	}
	newData, err := iga.EncodeInsert(origData, img)
	if err != nil {
		return fmt.Errorf("encode %s into %s: %w", inPngPath, inTexsPath, err)
	}
	if err := os.WriteFile(outPath, newData, 0644); err != nil {
		return fmt.Errorf("write %s: %w", outPath, err)
	}
	fmt.Printf("  %s + %s -> %s (%d bytes)\n", inTexsPath, inPngPath, outPath, len(newData))
	return nil
}
