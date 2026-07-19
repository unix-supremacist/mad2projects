// IGZ-wrapped texture (`.texs`) parsing, for MAD2's texture assets.
//
// This is a NEW file, independent of iga.go/repack.go/pak.go/level.go. See
// docs/TEXTURE_FORMAT.md for the full reverse-engineering writeup — this
// covers substantially less ground than fsb.go/AUDIO_FORMAT.md: the
// container and the runtime C++ field schema for the `igImage2` class are
// solidly confirmed (cross-checked against the Wii demo ELF's own bulk
// field-registration code), and the base-mip-level width/height can be
// recovered indirectly for the common case (square power-of-two, full
// mipmap chain, uncompressed 4-bytes/pixel) by inverting the total pixel
// data size. What is NOT solved: how the on-disk section 1 bytes actually
// encode the live igImage2 instance's field values (direct byte search for
// the expected width/height values found no match at all — a genuine open
// problem, see the doc), and the exact intra-pixel-data byte layout in
// section 2 (raster RGBA/BGRA, mip-order reversal, planar channels, and
// Morton/Z-order swizzling were all tried and all produced incoherent
// images — see docs/TEXTURE_FORMAT.md's "Open questions" section for the
// full trail). Consequently this file extracts container-level metadata
// and the raw pixel-data blob, but does NOT decode pixels to PNG/DDS.
package iga

import (
	"encoding/binary"
	"fmt"
	"strings"
)

// IGZ container magic (0x49475A01, "IGZ\x01" read as bytes) — same constant
// as documented in IGZ_FORMAT.md for level.bld/.pak. Duplicated here (not
// imported from iga.go/pak.go) since this file intentionally stays
// self-contained per the "new file" boundary for this work.
const igzMagic = 0x49475A01

// IGZSectionDesc is one 16-byte IGZ section-table entry.
type IGZSectionDesc struct {
	Offset uint32
	Size   uint32
	Align  uint32
	Flags  uint32
}

// IGZContainer is a minimally-parsed IGZ file: just the section table, not
// the object graph within it. Sufficient for `.texs` since everything
// texture-specific this file can currently extract lives at the section
// level (class directory strings in section 0, raw pixel blob in the last
// section).
type IGZContainer struct {
	Version  uint32
	Sections []IGZSectionDesc
	data     []byte
}

// ParseIGZContainer reads the IGZ magic/version and walks the section
// descriptor table, stopping at the first entry that doesn't start where
// the previous one ended (same technique IGZ_FORMAT.md documents: there is
// no separate up-front "section count" field).
func ParseIGZContainer(data []byte) (*IGZContainer, error) {
	if len(data) < 16 {
		return nil, fmt.Errorf("file too small (%d bytes) for IGZ header", len(data))
	}
	magic := binary.LittleEndian.Uint32(data[0:4])
	if magic != igzMagic {
		return nil, fmt.Errorf("not an IGZ file (magic 0x%08X, want 0x%08X)", magic, igzMagic)
	}
	version := binary.LittleEndian.Uint32(data[4:8])

	c := &IGZContainer{Version: version, data: data}
	pos := 0x10
	var prev *IGZSectionDesc
	for pos+16 <= len(data) {
		off := binary.LittleEndian.Uint32(data[pos:])
		size := binary.LittleEndian.Uint32(data[pos+4:])
		align := binary.LittleEndian.Uint32(data[pos+8:])
		flags := binary.LittleEndian.Uint32(data[pos+12:])
		if prev != nil && off != prev.Offset+prev.Size {
			break // not a real descriptor; end of section table
		}
		if off == 0 && size == 0 && prev != nil {
			break
		}
		d := IGZSectionDesc{Offset: off, Size: size, Align: align, Flags: flags}
		c.Sections = append(c.Sections, d)
		prev = &c.Sections[len(c.Sections)-1]
		pos += 16
		if int(off+size) >= len(data) {
			break // last section ends at EOF, nothing more to read
		}
	}
	if len(c.Sections) == 0 {
		return nil, fmt.Errorf("no valid IGZ section descriptors found")
	}
	return c, nil
}

// Section returns the raw bytes of section i, or nil if out of range.
func (c *IGZContainer) Section(i int) []byte {
	if i < 0 || i >= len(c.Sections) {
		return nil
	}
	s := c.Sections[i]
	if int(s.Offset+s.Size) > len(c.data) {
		return nil
	}
	return c.data[s.Offset : s.Offset+s.Size]
}

// ClassDirectory extracts section 0's null-terminated class-name list, per
// IGZ_FORMAT.md ("dirTableOff uint32 at sec0Base+0x0C" through the string
// pool start). Returns class names in file order — an object's classIdx is
// a 0-based index into this same list (see IGZ_FORMAT.md).
func (c *IGZContainer) ClassDirectory() []string {
	sec0 := c.Section(0)
	if len(sec0) < 0x30 {
		return nil
	}
	dirOff := binary.LittleEndian.Uint32(sec0[0x0C:])
	strPoolOff := binary.LittleEndian.Uint32(sec0[0x2C:])
	if int(dirOff) >= len(sec0) || int(strPoolOff) > len(sec0) || strPoolOff < dirOff {
		return nil
	}
	var names []string
	region := sec0[dirOff:strPoolOff]
	start := 0
	for i, b := range region {
		if b == 0 {
			if i > start {
				names = append(names, string(region[start:i]))
			}
			start = i + 1
		}
	}
	return names
}

// TexsInfo is what this file can currently determine about a `.texs`
// (IGZ-wrapped igImage2) asset without a working pixel decoder — see the
// package doc comment and docs/TEXTURE_FORMAT.md for exactly what's
// confirmed vs. inferred vs. unknown.
type TexsInfo struct {
	Classes []string // section 0's class directory, e.g. ["igObjectList","igImage2","igHandleList","igStringRefList"]
	IsImage bool     // classes contains "igImage2"

	// GuessedWidth/GuessedHeight are inferred, NOT read from the file's own
	// fields (see docs/TEXTURE_FORMAT.md — direct search for the runtime
	// igImage2.width/height field values found no match anywhere in the
	// file, a still-open problem). This is a heuristic inversion of
	// PixelDataSize assuming the common case found in every MAD2 .texs
	// sample checked so far: a square power-of-two base image with a full
	// mipmap chain down to 1x1, at 4 bytes/pixel. Confirmed exactly
	// reproducing the known-good filename-hinted sizes 128, 256, and 512
	// (see docs/TEXTURE_FORMAT.md). Zero if no such match was found (e.g.
	// a non-square, non-power-of-two, or compressed texture — none of
	// which this heuristic can currently distinguish from "wrong guess",
	// so a zero result should be read as "unknown", not "not a texture").
	GuessedWidth  int
	GuessedHeight int

	PixelData []byte // raw last-section bytes; layout undecoded, see docs/TEXTURE_FORMAT.md
}

// ParseTexs parses a `.texs` file's IGZ container and reports what's
// currently extractable. It does not decode pixel data (see file header).
func ParseTexs(data []byte) (*TexsInfo, error) {
	c, err := ParseIGZContainer(data)
	if err != nil {
		return nil, err
	}
	if len(c.Sections) < 2 {
		return nil, fmt.Errorf(".texs file has only %d IGZ section(s), expected at least 2", len(c.Sections))
	}

	classes := c.ClassDirectory()
	isImage := false
	for _, cl := range classes {
		if cl == "igImage2" {
			isImage = true
			break
		}
	}

	pixelData := c.Section(len(c.Sections) - 1)

	info := &TexsInfo{
		Classes:   classes,
		IsImage:   isImage,
		PixelData: pixelData,
	}
	info.GuessedWidth, info.GuessedHeight = guessSquarePOTDimensions(len(pixelData))
	return info, nil
}

// guessSquarePOTDimensions inverts squarePOTMipChainBytes for common
// power-of-two sizes. Returns (0, 0) if no exact match is found.
func guessSquarePOTDimensions(totalBytes int) (w, h int) {
	for n := 2; n <= 8192; n *= 2 {
		if squarePOTMipChainBytes(n) == totalBytes {
			return n, n
		}
	}
	return 0, 0
}

// squarePOTMipChainBytes computes the total byte count of a square N×N
// image (N a power of two) at 4 bytes/pixel with a full mipmap chain down
// to 1×1 — i.e. sum over levels of (N>>i)^2 for i=0..log2(N), times 4.
// See docs/TEXTURE_FORMAT.md for how this formula was derived and verified
// against three independently-sized real `.texs` samples (128, 256, 512).
func squarePOTMipChainBytes(n int) int {
	total := 0
	for w := n; ; w /= 2 {
		total += w * w
		if w == 1 {
			break
		}
	}
	return total * 4
}

// SourcePathHint does a best-effort scan of section 0's string pool for a
// null-terminated string that looks like an original build-time asset path
// (contains "/" and ends in a common image extension) — useful for
// labeling, not authoritative (see IGZ_FORMAT.md's own caveats about the
// string pool's exact addressing scheme still being partly unresolved,
// specifically the "+0xA840" gap noted there — this instead scans section 0
// directly for a plausible-looking path substring, sidestepping that
// unresolved indexing scheme entirely).
func (c *IGZContainer) SourcePathHint() string {
	sec0 := c.Section(0)
	if sec0 == nil {
		return ""
	}
	start := 0
	for i, b := range sec0 {
		if b == 0 {
			if i > start {
				s := string(sec0[start:i])
				if looksLikeAssetPath(s) {
					return s
				}
			}
			start = i + 1
		}
	}
	return ""
}

func looksLikeAssetPath(s string) bool {
	if !strings.Contains(s, "/") {
		return false
	}
	lower := strings.ToLower(s)
	for _, ext := range []string{".png", ".tga", ".dds", ".bmp", ".psd"} {
		if strings.HasSuffix(lower, ext) {
			return true
		}
	}
	return false
}
