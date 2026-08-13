// IGZ-wrapped texture (`.texs`) parsing and pixel decode/encode, for MAD2's
// texture assets.
//
// This is a NEW file, independent of iga.go/repack.go/pak.go/level.go
// (though it now reimplements, locally and self-contained, the same
// Section-1 top-level-object-list walk objgraph.go's ObjectGraph type
// provides more generally — kept separate on purpose so this file doesn't
// take a hard dependency on objgraph.go's evolving API). See
// docs/TEXTURE_FORMAT.md for the full reverse-engineering writeup and the
// concrete evidence behind every claim below.
//
// Solved this session, previously open:
//   - Where width/height/depth/levelCount/imageCount/format/texHandle
//     actually live on disk: at the SAME runtime field offsets
//     (width@+0x0C etc.) documented for the live `igImage2` C++ layout,
//     starting from the address of the single object Section 1's top-level
//     igObjectList-style header points at (classIdx==igImage2's directory
//     index, which is always 1 in every real `.texs` sample checked this
//     session — 186/186). The previous "direct byte search for width=128
//     found nothing" dead end was simply because the true width isn't 128
//     at all for that sample (see below) — the search was for the wrong
//     value, not in the wrong place.
//   - The base-mip dimensions, exactly (not the old heuristic — see
//     GuessedWidth/GuessedHeight's doc comment on why the old guess and the
//     real width can legitimately disagree).
//   - The pixel format, for every sample in this repo's checked-out asset
//     corpus: standard DXT1 (0.5 bytes/pixel — 171/186 samples) or standard
//     DXT5 (1.0 bytes/pixel — 15/186 samples, all small UI icons), with
//     standard row-major block layout, smallest mip first (base level at the end of Section 2).
//     See "Pixel format" below for the full evidentiary trail.
//
// Still NOT solved: which literal `igMetaImage_*` singleton (e.g.
// "dxt1_dx" vs "dxt1") a given texture's `format` field is meant to point
// at. The on-disk `format` field is always exactly 0x80000000 — the
// generic "external fixup, slot 0" marker (top bit set, see
// docs/IGZ_FORMAT.md's `fixupPtr` note) — identically across every single
// sample checked (186/186), so it carries no per-instance information at
// all on disk; the concrete format (DXT1 vs DXT3) is instead recovered
// indirectly, per-file, from the exact ratio of Section 2's byte size to
// the mip-chain pixel count (see below) — which is unambiguous in
// practice (every sample lands within 1e-5 of exactly 0.5 or exactly 1.0,
// no file falls in between). Full mip chains beyond level 0 are also not
// regenerated on encode — see EncodeInsert's doc comment.
package iga

import (
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
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

// ClassDirectory extracts section 0's null-terminated class-name list,
// starting at `dirOff` (u32 at sec0Base+0x0C, per IGZ_FORMAT.md).
//
// This does NOT use the `strPoolOff` field (u32 at sec0Base+0x2C) as an end
// boundary, despite an earlier version of this function doing exactly that
// (and despite IGZ_FORMAT.md's general description suggesting it should
// work) — confirmed, by actually running the old code, that `strPoolOff`
// points partway *through* the class directory's own string list for every
// `.texs` sample checked (it cut "igHandleList"/"igStringRefList" off
// mid-string, silently truncating the directory to 2 names instead of the
// real 4 — `IsImage` still happened to work by luck, since "igImage2" is
// the second name and survived, but nothing depending on a class's index
// for anything past that would have). Root cause not fully chased down
// (dirOff itself also doesn't point exactly at the first name — there are
// a few stray non-zero single bytes between it and the real start, which
// this function's length>=2 + identifier-shaped filter below silently
// skips over rather than needing to explain). Instead this scans forward
// from dirOff for a run of plain-identifier-shaped strings (letters only,
// optionally trailing digits — real C++ class names all look like this)
// and stops at the first string that doesn't (in every real sample, that's
// the source-path string that follows the directory, which contains '/').
func (c *IGZContainer) ClassDirectory() []string {
	sec0 := c.Section(0)
	if len(sec0) < 0x30 {
		return nil
	}
	dirOff := binary.LittleEndian.Uint32(sec0[0x0C:])
	if int(dirOff) >= len(sec0) {
		return nil
	}
	var names []string
	start := int(dirOff)
	for pos := int(dirOff); pos < len(sec0); pos++ {
		if sec0[pos] != 0 {
			continue
		}
		if pos > start {
			s := string(sec0[start:pos])
			if isPlainIdentifier(s) {
				names = append(names, s)
			} else if len(names) > 0 {
				// A non-identifier string after we've already collected at
				// least one real class name means we've walked off the end
				// of the directory into the source-path/instance-name
				// strings that follow it — stop here. Non-identifier
				// strings BEFORE the first real name (stray single bytes
				// between dirOff and the directory's actual start) are
				// silently skipped instead, not treated as a stop signal.
				return names
			}
		}
		start = pos + 1
	}
	return names
}

// isPlainIdentifier reports whether s looks like a bare C++ identifier
// (e.g. "igImage2", "igStringRefList") — letters, with digits allowed after
// the first character. Used by ClassDirectory to tell real class names
// apart from the stray single non-zero bytes that precede them and the
// source-path/instance-name strings that follow them in section 0.
func isPlainIdentifier(s string) bool {
	if len(s) < 2 {
		return false
	}
	for i := 0; i < len(s); i++ {
		ch := s[i]
		isAlpha := (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
		isDigit := ch >= '0' && ch <= '9'
		if isAlpha || (i > 0 && isDigit) {
			continue
		}
		return false
	}
	return true
}

// PixelFormat is a `.texs` texture's actual on-disk pixel data format, as
// determined by DetectPixelFormat (see its doc comment for how this is
// inferred — the on-disk `format` field itself carries no per-instance
// information, see the package doc comment).
type PixelFormat int

const (
	FormatUnknown PixelFormat = iota
	FormatDXT1                // 8 bytes/4x4 block, 0.5 bytes/pixel
	FormatDXT3                // 16 bytes/4x4 block (8 explicit-alpha + 8 color), 1.0 bytes/pixel
	FormatDXT5                // 16 bytes/4x4 block (8 interpolated-alpha + 8 color), 1.0 bytes/pixel
)

func (f PixelFormat) String() string {
	switch f {
	case FormatDXT1:
		return "DXT1"
	case FormatDXT3:
		return "DXT3"
	case FormatDXT5:
		return "DXT5"
	default:
		return "unknown"
	}
}

// blockBytes returns the number of bytes one 4x4 pixel block occupies for
// this format, or 0 for FormatUnknown.
func (f PixelFormat) blockBytes() int {
	switch f {
	case FormatDXT1:
		return 8
	case FormatDXT3, FormatDXT5:
		return 16
	default:
		return 0
	}
}

// TexsInfo is what this file can determine about a `.texs` (IGZ-wrapped
// igImage2) asset — see the package doc comment and docs/TEXTURE_FORMAT.md
// for exactly what's confirmed vs. inferred vs. unknown.
type TexsInfo struct {
	Classes []string // section 0's class directory, e.g. ["igObjectList","igImage2","igHandleList","igStringRefList"]
	IsImage bool     // classes contains "igImage2"

	// Width/Height/Depth/LevelCount/ImageCount are read directly from the
	// live igImage2 instance's on-disk fields (see locateImage2Object) —
	// not guessed. LevelCount is the on-disk field's own raw value; the
	// real total mip chain length is LevelCount+1 (confirmed by exact-byte
	// match against Section 2's size across all 186 real samples checked —
	// see docs/TEXTURE_FORMAT.md). Zero if the object couldn't be located
	// (see the error ParseTexs returns in that case).
	Width, Height, Depth, LevelCount, ImageCount int

	// FormatRaw is the on-disk `format` field's raw value — always
	// 0x80000000 ("external fixup, slot 0") in every sample checked; kept
	// here for diagnostics, not for format detection (see Format).
	FormatRaw uint32

	// Format is the actual pixel format, detected indirectly (see
	// DetectPixelFormat). FormatUnknown if Section 2's size doesn't
	// cleanly match either known format's mip-chain byte count.
	Format PixelFormat

	// ObjectAddr is the absolute file offset of the igImage2 object's own
	// classIdx word (i.e. the start of its serialized fields) — needed by
	// EncodeInsert to know nothing else needs patching there today (width/
	// height don't change), and useful for anyone extending this to write
	// those fields too.
	ObjectAddr uint32

	// Sec2Off/Sec2Size are Section 2's absolute file offset/size — needed
	// by EncodeInsert to know where to splice new pixel bytes in.
	Sec2Off, Sec2Size uint32

	// GuessedWidth/GuessedHeight are the OLD heuristic (inverting
	// PixelDataSize assuming a square power-of-two base image with a full
	// mipmap chain to 1x1 at 4 bytes/pixel — see squarePOTMipChainBytes),
	// kept only for backward compatibility / as a cross-check. Superseded
	// by Width/Height above, which are read from the file's real fields,
	// not inferred. The two CAN legitimately disagree, and did for the
	// original reference sample this heuristic was built and verified
	// against: "360-wireless-controller_128.texs" guesses 128x128 (an
	// arithmetic coincidence — 4*Σ(128>>i)² for i=0..7 happens to equal
	// the same 87,380-byte total that the texture's REAL dimensions
	// (256x256, DXT3, levels=8, stopping at a 2x2 base for the last mip
	// rather than continuing to 1x1) also produce, via a completely
	// different formula — see docs/TEXTURE_FORMAT.md's "Two exact-byte-
	// count coincidences" section for the full arithmetic). Trust
	// Width/Height, not this.
	GuessedWidth  int
	GuessedHeight int

	PixelData []byte // raw Section 2 bytes (all mip levels concatenated, base mip first)
}

// ParseTexs parses a `.texs` file's IGZ container, locates the live
// igImage2 instance in Section 1, and reports its real fields plus the
// detected pixel format. Returns an error only for structural problems
// (not an IGZ file, not enough sections); a missing/unlocatable igImage2
// object is reported via IsImage=false with Width/Height left at 0, not an
// error, to match this function's previous "always returns something"
// contract for non-texture IGZ files.
func ParseTexs(data []byte) (*TexsInfo, error) {
	c, err := ParseIGZContainer(data)
	if err != nil {
		return nil, err
	}
	if len(c.Sections) < 2 {
		return nil, fmt.Errorf(".texs file has only %d IGZ section(s), expected at least 2", len(c.Sections))
	}

	classes := c.ClassDirectory()
	imgClassIdx := -1
	for i, cl := range classes {
		if cl == "igImage2" {
			imgClassIdx = i
			break
		}
	}

	pixelData := c.Section(len(c.Sections) - 1)
	sec2 := c.Sections[len(c.Sections)-1]

	info := &TexsInfo{
		Classes:   classes,
		IsImage:   imgClassIdx >= 0,
		PixelData: pixelData,
		Sec2Off:   sec2.Offset,
		Sec2Size:  sec2.Size,
	}
	info.GuessedWidth, info.GuessedHeight = guessSquarePOTDimensions(len(pixelData))

	if imgClassIdx < 0 {
		return info, nil
	}
	sec1Off := c.Sections[1].Offset
	addr, err := locateImage2Object(data, sec1Off, imgClassIdx)
	if err != nil {
		// Structurally an image (class present) but the object couldn't be
		// pinned down — leave Width/Height/Format zero rather than erroring,
		// same "best effort" contract as the rest of this function.
		return info, nil
	}
	info.ObjectAddr = addr
	if int(addr)+0x30+4 > len(data) {
		return info, nil
	}
	info.Width = int(binary.LittleEndian.Uint16(data[addr+0x0C:]))
	info.Height = int(binary.LittleEndian.Uint16(data[addr+0x0E:]))
	info.Depth = int(binary.LittleEndian.Uint16(data[addr+0x10:]))
	info.LevelCount = int(binary.LittleEndian.Uint16(data[addr+0x12:]))
	info.ImageCount = int(binary.LittleEndian.Uint16(data[addr+0x14:]))
	info.FormatRaw = binary.LittleEndian.Uint32(data[addr+0x18:])
	info.Format = DetectPixelFormat(info.Width, info.Height, info.LevelCount, int(sec2.Size))
	return info, nil
}

// locateImage2Object finds the single igImage2 instance in Section 1's
// top-level object list and returns the absolute file offset of its own
// classIdx word (i.e. its own field data starts here, at the same runtime
// offsets documented for the live C++ class — width@+0x0C etc.).
//
// This reimplements, locally, exactly what objgraph.go's
// ObjectGraph.TopLevelObjects does generically (Section 1's header: object
// count at +0x0C, object-pointer-array offset at +0x18, each array entry a
// Section-1-relative pointer to one object's classIdx word) — kept
// self-contained here rather than imported so this file doesn't take on a
// dependency on objgraph.go's own API shape. Verified against every one of
// this repo's 186 real `.texs` samples: every single one has exactly one
// top-level object, and its classIdx always matches the class directory's
// "igImage2" entry (always directory index 1 in practice, though this
// doesn't hardcode that — it looks the index up via imgClassIdx).
func locateImage2Object(data []byte, sec1Off uint32, imgClassIdx int) (uint32, error) {
	if int(sec1Off)+0x1C > len(data) {
		return 0, fmt.Errorf("section 1 too small to hold its own header fields")
	}
	count := binary.LittleEndian.Uint32(data[sec1Off+0x0C:])
	arrOff := binary.LittleEndian.Uint32(data[sec1Off+0x18:])
	if count == 0 || count > 1000 {
		return 0, fmt.Errorf("implausible top-level object count %d", count)
	}
	arrBase := sec1Off + arrOff
	for i := uint32(0); i < count; i++ {
		off := arrBase + i*4
		if int(off)+4 > len(data) {
			break
		}
		relPtr := binary.LittleEndian.Uint32(data[off:])
		addr := sec1Off + relPtr
		if int(addr)+4 > len(data) {
			continue
		}
		classIdx := binary.LittleEndian.Uint32(data[addr:])
		if int(classIdx) == imgClassIdx {
			return addr, nil
		}
	}
	return 0, fmt.Errorf("no top-level object with classIdx %d (igImage2) found in section 1", imgClassIdx)
}

// mipChainPixelCount sums width*height across `levels` mip levels, each
// dimension halved (floored, minimum 1) from the previous level — the same
// formula independently confirmed exact (to the byte, against Section 2's
// real size) for both known formats across every real sample checked, when
// levels = LevelCount+1 (see docs/TEXTURE_FORMAT.md).
func mipChainPixelCount(w, h, levels int) int {
	total := 0
	for i := 0; i < levels; i++ {
		total += w * h
		if w > 1 {
			w /= 2
		}
		if h > 1 {
			h /= 2
		}
	}
	return total
}

// DetectPixelFormat infers the real pixel format from the exact ratio of
// Section 2's byte size to the full mip chain's pixel count (levels =
// levelCount+1 — see mipChainPixelCount). Across all 186 real `.texs`
// samples in this repo's checked-out asset corpus, this ratio lands within
// 1e-5 of exactly 0.5 (DXT1: 171/186 samples) or exactly 1.0 (DXT3: 15/186,
// all small UI icons) — no sample fell anywhere in between, so this simple
// nearest-match is safe in practice despite being a ratio test rather than
// a literal field read (see the package doc comment for why a literal read
// isn't available). Returns FormatUnknown if the ratio isn't close to
// either (e.g. width/height/levelCount weren't found, or a genuinely
// different format is in play that this repo's asset corpus doesn't
// contain an example of).
func DetectPixelFormat(width, height, levelCount, sec2Size int) PixelFormat {
	if width <= 0 || height <= 0 || levelCount < 0 || sec2Size <= 0 {
		return FormatUnknown
	}
	pixels := mipChainPixelCount(width, height, levelCount+1)
	if pixels == 0 {
		return FormatUnknown
	}
	bpp := float64(sec2Size) / float64(pixels)
	const eps = 0.01
	switch {
	case abs64(bpp-0.5) < eps:
		return FormatDXT1
	case abs64(bpp-1.0) < eps:
		return FormatDXT5
	default:
		return FormatUnknown
	}
}

func abs64(v float64) float64 {
	if v < 0 {
		return -v
	}
	return v
}

// baseLevelSize returns the exact byte size of mip level 0 (the base,
// largest, first-in-Section-2 level) for a WxH texture in the given
// format: standard DXT block accounting, blocks of 4x4 pixels (partial
// edge blocks still cost a full block), block size per PixelFormat.blockBytes.
// This does NOT depend on levelCount or any mip-chain-tail ambiguity (see
// EncodeInsert's doc comment for why that matters) — it's the same formula
// real DXT-family encoders universally use for one level in isolation.
func baseLevelSize(width, height int, format PixelFormat) int {
	bb := format.blockBytes()
	if bb == 0 {
		return 0
	}
	blocksW := (width + 3) / 4
	blocksH := (height + 3) / 4
	return blocksW * blocksH * bb
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

// --- Pixel decode/encode ---------------------------------------------------
//
// Standard DXT1/DXT5 block compression (S3TC), row-major block layout
// (blocks left-to-right within a block-row, block-rows top-to-bottom),
// applied only to mip level 0 (the base/largest level, which sits at
// the end of Section 2 — mips are ordered smallest-to-largest; see baseLevelSize). Confirmed via spatial
// autocorrelation on real samples (not just "doesn't crash" — see
// docs/TEXTURE_FORMAT.md's "Pixel format and layout, confirmed" section for
// the concrete numbers): decoding a real DXT1 loadscreen sample's block
// color endpoints in this layout shows adjacent-block correlation ~0.35-0.41
// (both x and y neighbor directions) against a ~0.003 shuffled-block
// baseline; a real DXT3 icon sample's per-block alpha-nibble variance shows
// ~0.44 adjacent-block correlation against a ~-0.015 shuffled baseline. Both
// are far outside what a wrong/incoherent layout would produce (shuffled
// baselines sit at ~0, i.e. no spatial structure at all), and is the same
// kind of evidence-based validation docs/TEXTURE_FORMAT.md's earlier ruled-
// out hypotheses were rejected with.

// rgb565to888 expands a packed RGB565 value to 8-bit-per-channel RGB, using
// the standard bit-replication expansion (matches every real S3TC decoder,
// not just a naive left-shift).
func rgb565to888(c uint16) (r, g, b uint8) {
	r5 := uint32(c>>11) & 0x1F
	g6 := uint32(c>>5) & 0x3F
	b5 := uint32(c) & 0x1F
	r = uint8((r5*255 + 15) / 31)
	g = uint8((g6*255 + 31) / 63)
	b = uint8((b5*255 + 15) / 31)
	return
}

// rgb888to565 packs 8-bit RGB down to RGB565 (simple truncation — good
// enough for an encoder that isn't trying to be bit-exact with whatever
// proprietary tool produced the original assets; see EncodeInsert's doc
// comment on why exact round-trip isn't expected).
func rgb888to565(r, g, b uint8) uint16 {
	return uint16(r>>3)<<11 | uint16(g>>2)<<5 | uint16(b>>3)
}

// decodeDXTColorBlock decodes one 8-byte DXT1-shaped color block (color0,
// color1 as little-endian RGB565, then a 32-bit LE field of 16 2-bit
// per-pixel indices, row-major within the 4x4 block) to 16 RGBA pixels.
// forceFourColor makes this always use the 4-opaque-color interpolation
// regardless of color0 vs color1 ordering — DXT3/DXT5's color block reuses
// this exact shape but (per the S3TC spec) never uses DXT1's "3 colors +
// transparent black" mode, since alpha is carried by the separate alpha
// block instead.
func decodeDXTColorBlock(block []byte, forceFourColor bool) [16][3]uint8 {
	idx := binary.LittleEndian.Uint32(block[0:4])
	c0 := binary.LittleEndian.Uint16(block[4:6])
	c1 := binary.LittleEndian.Uint16(block[6:8])

	r0, g0, b0 := rgb565to888(c0)
	r1, g1, b1 := rgb565to888(c1)

	var palette [4][3]uint8
	palette[0] = [3]uint8{r0, g0, b0}
	palette[1] = [3]uint8{r1, g1, b1}
	if forceFourColor || c0 > c1 {
		palette[2] = [3]uint8{uint8((2*int(r0) + int(r1)) / 3), uint8((2*int(g0) + int(g1)) / 3), uint8((2*int(b0) + int(b1)) / 3)}
		palette[3] = [3]uint8{uint8((int(r0) + 2*int(r1)) / 3), uint8((int(g0) + 2*int(g1)) / 3), uint8((int(b0) + 2*int(b1)) / 3)}
	} else {
		palette[2] = [3]uint8{uint8((int(r0) + int(r1)) / 2), uint8((int(g0) + int(g1)) / 2), uint8((int(b0) + int(b1)) / 2)}
		palette[3] = [3]uint8{0, 0, 0} // transparent black; caller (DXT1 path) applies alpha=0
	}

	var out [16][3]uint8
	for i := 0; i < 16; i++ {
		code := (idx >> uint(2*i)) & 3
		out[i] = palette[code]
	}
	return out
}

// decodeDXT1Block decodes one 8-byte DXT1 block to 16 RGBA pixels
// (row-major within the block), including DXT1's "1-bit alpha" mode
// (color0<=color1 as unsigned u16 selects 3 opaque colors + transparent
// black for palette index 3).
func decodeDXT1Block(block []byte) [16]color.NRGBA {
	idx := binary.LittleEndian.Uint32(block[0:4])
	c0 := binary.LittleEndian.Uint16(block[4:6])
	c1 := binary.LittleEndian.Uint16(block[6:8])

	r0, g0, b0 := rgb565to888(c0)
	r1, g1, b1 := rgb565to888(c1)
	var palette [4][3]uint8
	palette[0] = [3]uint8{r0, g0, b0}
	palette[1] = [3]uint8{r1, g1, b1}
	palette[2] = [3]uint8{uint8((2*int(r0) + int(r1)) / 3), uint8((2*int(g0) + int(g1)) / 3), uint8((2*int(b0) + int(b1)) / 3)}
	palette[3] = [3]uint8{uint8((int(r0) + 2*int(r1)) / 3), uint8((int(g0) + 2*int(g1)) / 3), uint8((int(b0) + 2*int(b1)) / 3)}

	var out [16]color.NRGBA
	for i := 0; i < 16; i++ {
		code := (idx >> uint(2*i)) & 3
		out[i] = color.NRGBA{R: palette[code][0], G: palette[code][1], B: palette[code][2], A: 255}
	}
	return out
}

// decodeDXT3Block decodes one 16-byte DXT3 block (8 bytes explicit 4-bit
// per-pixel alpha, then 8 bytes of a DXT1-shaped always-4-color block) to
// 16 RGBA pixels.
func decodeDXT3Block(block []byte) [16]color.NRGBA {
	alphaBits := binary.LittleEndian.Uint64(block[0:8])
	rgb := decodeDXTColorBlock(block[8:16], true)
	var out [16]color.NRGBA
	for i := 0; i < 16; i++ {
		a4 := uint8((alphaBits >> uint(4*i)) & 0xF)
		a := a4 * 17 // 4-bit -> 8-bit replication (0..15 -> 0,17,...,255)
		out[i] = color.NRGBA{R: rgb[i][0], G: rgb[i][1], B: rgb[i][2], A: a}
	}
	return out
}

// decodeDXT5Block decodes one 16-byte DXT5 block (8 bytes interpolated
// alpha + 8 bytes DXT1 color block) to 16 RGBA pixels.
func decodeDXT5Block(block []byte) [16]color.NRGBA {
	a0 := block[0]
	a1 := block[1]
	aIdx := uint64(block[2]) | uint64(block[3])<<8 | uint64(block[4])<<16 | uint64(block[5])<<24 | uint64(block[6])<<32 | uint64(block[7])<<40

	var alphaPalette [8]uint8
	alphaPalette[0] = a0
	alphaPalette[1] = a1
	if a0 > a1 {
		for i := 1; i <= 6; i++ {
			alphaPalette[i+1] = uint8(((7-i)*int(a0) + i*int(a1)) / 7)
		}
	} else {
		for i := 1; i <= 4; i++ {
			alphaPalette[i+1] = uint8(((5-i)*int(a0) + i*int(a1)) / 5)
		}
		alphaPalette[6] = 0
		alphaPalette[7] = 255
	}

	c0 := binary.LittleEndian.Uint16(block[8:10])
	c1 := binary.LittleEndian.Uint16(block[10:12])
	idx := binary.LittleEndian.Uint32(block[12:16])

	r0, g0, b0 := rgb565to888(c0)
	r1, g1, b1 := rgb565to888(c1)
	var palette [4][3]uint8
	palette[0] = [3]uint8{r0, g0, b0}
	palette[1] = [3]uint8{r1, g1, b1}
	palette[2] = [3]uint8{uint8((2*int(r0) + int(r1)) / 3), uint8((2*int(g0) + int(g1)) / 3), uint8((2*int(b0) + int(b1)) / 3)}
	palette[3] = [3]uint8{uint8((int(r0) + 2*int(r1)) / 3), uint8((int(g0) + 2*int(g1)) / 3), uint8((int(b0) + 2*int(b1)) / 3)}

	var out [16]color.NRGBA
	for i := 0; i < 16; i++ {
		code := (idx >> uint(2*i)) & 3
		acode := (aIdx >> uint(3*i)) & 7
		a := alphaPalette[acode]
		out[i] = color.NRGBA{R: palette[code][0], G: palette[code][1], B: palette[code][2], A: a}
	}
	return out
}

// decodeBaseLevel decodes mip level 0 (width x height, standard DXT block
// grid, row-major blocks) to an *image.NRGBA.
//
// Block ROWS are stored bottom-to-top (targetBY = blocksH-1-by), but each
// individual 4x4 block's own 4 texel rows are stored top-to-bottom already
// in their final orientation (py, not 3-py) — NOT a true whole-image
// vertical mirror, despite looking like it should be one. Confirmed by
// direct comparison: applying the "obvious" full mirror (also reversing
// each block's internal row order) produces a checkerboard-like scramble on
// every sharp edge (border lines, tree-canopy silhouettes) while leaving
// smooth gradients deceptively plausible-looking — this is why an earlier
// pass called DXT1 decode "solid, done" despite the bug being real.
func decodeBaseLevel(data []byte, width, height int, format PixelFormat) (*image.NRGBA, error) {
	bb := format.blockBytes()
	if bb == 0 {
		return nil, fmt.Errorf("unsupported pixel format %v", format)
	}
	need := baseLevelSize(width, height, format)
	if len(data) < need {
		return nil, fmt.Errorf("base level needs %d bytes, only %d available", need, len(data))
	}
	blocksW := (width + 3) / 4
	blocksH := (height + 3) / 4

	img := image.NewNRGBA(image.Rect(0, 0, width, height))
	pos := 0
	for by := 0; by < blocksH; by++ {
		targetBY := blocksH - 1 - by
		for bx := 0; bx < blocksW; bx++ {
			block := data[pos : pos+bb]
			pos += bb
			var px [16]color.NRGBA
			switch format {
			case FormatDXT1:
				px = decodeDXT1Block(block)
			case FormatDXT3:
				px = decodeDXT3Block(block)
			case FormatDXT5:
				px = decodeDXT5Block(block)
			}
			for i := 0; i < 16; i++ {
				py, pxx := i/4, i%4
				y, x := targetBY*4+py, bx*4+pxx
				if x < width && y < height {
					img.SetNRGBA(x, y, px[i])
				}
			}
		}
	}
	return img, nil
}

// DecodeImage decodes a `.texs` file's base (largest) mip level to an
// image.Image (concretely *image.NRGBA), from already-parsed TexsInfo (see
// ParseTexs). Mip levels beyond 0 are never touched — this repo's tooling
// only ever needs the base level for a PNG round-trip (see EncodeInsert).
func DecodeImage(info *TexsInfo) (image.Image, error) {
	if !info.IsImage {
		return nil, fmt.Errorf("not an igImage2 .texs file")
	}
	if info.Width <= 0 || info.Height <= 0 {
		return nil, fmt.Errorf("could not determine texture dimensions (width=%d height=%d)", info.Width, info.Height)
	}
	if info.Format == FormatUnknown {
		return nil, fmt.Errorf("could not determine pixel format (section 2 size %d doesn't cleanly match DXT1 or DXT5 for a %dx%d, %d-level mip chain)", info.Sec2Size, info.Width, info.Height, info.LevelCount+1)
	}
	need := baseLevelSize(info.Width, info.Height, info.Format)
	if len(info.PixelData) < need {
		return nil, fmt.Errorf("pixel data too small (%d bytes) for %dx%d base level (%d bytes required)", len(info.PixelData), info.Width, info.Height, need)
	}
	baseOffset := 0
	if info.Format == FormatDXT5 {
		// DXT5 mips stored smallest-to-largest (base level at end of Section 2)
		baseOffset = len(info.PixelData) - need
	}
	return decodeBaseLevel(info.PixelData[baseOffset:], info.Width, info.Height, info.Format)
}

// DecodeTexs is a convenience wrapper: parses raw `.texs` bytes and decodes
// the base mip level in one call.
func DecodeTexs(data []byte) (image.Image, *TexsInfo, error) {
	info, err := ParseTexs(data)
	if err != nil {
		return nil, nil, err
	}
	img, err := DecodeImage(info)
	if err != nil {
		return nil, info, err
	}
	return img, info, nil
}

// --- Encoding (PNG -> texs) --------------------------------------------------

// colorAt reads a pixel from img as 8-bit RGBA, clamping (x,y) to the
// image's own bounds — used so that non-multiple-of-4 dimensions still
// produce a well-defined block (edge pixels repeat, same as most real DXT
// encoders' "clamp" edge handling) instead of an out-of-bounds read.
func colorAt(img image.Image, bounds image.Rectangle, x, y int) (r, g, b, a uint8) {
	if x >= bounds.Dx() {
		x = bounds.Dx() - 1
	}
	if y >= bounds.Dy() {
		y = bounds.Dy() - 1
	}
	c := color.NRGBAModel.Convert(img.At(bounds.Min.X+x, bounds.Min.Y+y)).(color.NRGBA)
	return c.R, c.G, c.B, c.A
}

// encodeDXTColorBlock encodes one 4x4 pixel block's RGB channels into an
// 8-byte DXT1-shaped color block, using simple per-channel min/max endpoint
// selection ("bounding box" fit — not the higher-quality principal-axis fit
// real encoders use, but correct and simple) and nearest-palette-entry
// per-pixel indexing. forceFourColor forces color0>color1 (as an unsigned
// u16) so the 4-opaque-color interpolation mode is always selected, per
// DXT3/DXT5's color-block convention; when false (plain DXT1, no alpha
// needed in this content) it still prefers 4-color mode whenever the two
// endpoints differ, since none of this repo's actual textures need DXT1's
// punch-through-alpha mode (that would require detecting fully-transparent
// pixels, not attempted — see docs/TEXTURE_FORMAT.md for why this wasn't
// needed for round-trip verification).
func encodeDXTColorBlock(px [16][3]uint8, forceFourColor bool) []byte {
	minR, minG, minB := uint8(255), uint8(255), uint8(255)
	maxR, maxG, maxB := uint8(0), uint8(0), uint8(0)
	for _, p := range px {
		if p[0] < minR {
			minR = p[0]
		}
		if p[0] > maxR {
			maxR = p[0]
		}
		if p[1] < minG {
			minG = p[1]
		}
		if p[1] > maxG {
			maxG = p[1]
		}
		if p[2] < minB {
			minB = p[2]
		}
		if p[2] > maxB {
			maxB = p[2]
		}
	}
	c0 := rgb888to565(maxR, maxG, maxB)
	c1 := rgb888to565(minR, minG, minB)
	if c0 == c1 {
		// Degenerate (solid-color) block: nudge c0 up by one 565 code point
		// so c0>c1 holds and 4-color mode is unambiguous, matching every
		// real encoder's handling of flat blocks.
		if c0 < 0xFFFF {
			c0++
		} else {
			c1--
		}
	}
	if c0 <= c1 {
		c0, c1 = c1, c0
	}
	r0, g0, b0 := rgb565to888(c0)
	r1, g1, b1 := rgb565to888(c1)
	palette := [4][3]int{
		{int(r0), int(g0), int(b0)},
		{int(r1), int(g1), int(b1)},
		{(2*int(r0) + int(r1)) / 3, (2*int(g0) + int(g1)) / 3, (2*int(b0) + int(b1)) / 3},
		{(int(r0) + 2*int(r1)) / 3, (int(g0) + 2*int(g1)) / 3, (int(b0) + 2*int(b1)) / 3},
	}
	var idx uint32
	for i, p := range px {
		best, bestDist := 0, 1<<30
		for ci, c := range palette {
			dr, dg, db := int(p[0])-c[0], int(p[1])-c[1], int(p[2])-c[2]
			d := dr*dr + dg*dg + db*db
			if d < bestDist {
				bestDist, best = d, ci
			}
		}
		idx |= uint32(best) << uint(2*i)
	}
	out := make([]byte, 8)
	binary.LittleEndian.PutUint32(out[0:4], idx)
	binary.LittleEndian.PutUint16(out[4:6], c0)
	binary.LittleEndian.PutUint16(out[6:8], c1)
	return out
}

// encodeDXT1Block encodes one 4x4 block (with alpha) to 8 DXT1 bytes. If
// every pixel is opaque, plain 4-color mode is used; if any pixel is
// significantly transparent, DXT1's 3-color+transparent-black mode is used
// instead (palette index 3 forced for transparent pixels).
func encodeDXT1Block(px [16]color.NRGBA) []byte {
	hasTransparent := false
	for _, p := range px {
		if p.A < 128 {
			hasTransparent = true
			break
		}
	}
	var rgb [16][3]uint8
	for i, p := range px {
		rgb[i] = [3]uint8{p.R, p.G, p.B}
	}
	if !hasTransparent {
		return encodeDXTColorBlock(rgb, true)
	}
	// 3-color mode: encode opaque pixels' endpoints, force c0<=c1, and
	// steer transparent pixels to index 3 by giving them a huge fake color
	// distance from indices 0-2 during the nearest-palette search below —
	// simplest correct approach is to just build the block by hand here
	// rather than reusing encodeDXTColorBlock's forceFourColor path.
	minR, minG, minB := uint8(255), uint8(255), uint8(255)
	maxR, maxG, maxB := uint8(0), uint8(0), uint8(0)
	any := false
	for i, p := range px {
		if px[i].A < 128 {
			continue
		}
		any = true
		if p.R < minR {
			minR = p.R
		}
		if p.R > maxR {
			maxR = p.R
		}
		if p.G < minG {
			minG = p.G
		}
		if p.G > maxG {
			maxG = p.G
		}
		if p.B < minB {
			minB = p.B
		}
		if p.B > maxB {
			maxB = p.B
		}
	}
	if !any {
		minR, minG, minB, maxR, maxG, maxB = 0, 0, 0, 0, 0, 0
	}
	c1 := rgb888to565(maxR, maxG, maxB)
	c0 := rgb888to565(minR, minG, minB)
	if c0 > c1 {
		c0, c1 = c1, c0
	}
	if c0 == c1 && c1 > 0 {
		c1-- // keep c0<=c1 strictly enough to select 3-color mode
	}
	r0, g0, b0 := rgb565to888(c0)
	r1, g1, b1 := rgb565to888(c1)
	palette := [3][3]int{
		{int(r0), int(g0), int(b0)},
		{int(r1), int(g1), int(b1)},
		{(int(r0) + int(r1)) / 2, (int(g0) + int(g1)) / 2, (int(b0) + int(b1)) / 2},
	}
	var idx uint32
	for i, p := range px {
		if p.A < 128 {
			idx |= 3 << uint(2*i)
			continue
		}
		best, bestDist := 0, 1<<30
		for ci, c := range palette {
			dr, dg, db := int(p.R)-c[0], int(p.G)-c[1], int(p.B)-c[2]
			d := dr*dr + dg*dg + db*db
			if d < bestDist {
				bestDist, best = d, ci
			}
		}
		idx |= uint32(best) << uint(2*i)
	}
	out := make([]byte, 8)
	binary.LittleEndian.PutUint32(out[0:4], idx)
	binary.LittleEndian.PutUint16(out[4:6], c0)
	binary.LittleEndian.PutUint16(out[6:8], c1)
	return out
}

// encodeDXT3Block encodes one 4x4 block to 16 DXT3 bytes: 8 bytes of
// explicit 4-bit-per-pixel alpha (rounded, not truncated), then an
// always-4-color DXT1-shaped color block.
func encodeDXT3Block(px [16]color.NRGBA) []byte {
	var rgb [16][3]uint8
	var alphaBits uint64
	for i, p := range px {
		rgb[i] = [3]uint8{p.R, p.G, p.B}
		a4 := uint64((uint32(p.A)*15 + 127) / 255)
		alphaBits |= a4 << uint(4*i)
	}
	out := make([]byte, 16)
	binary.LittleEndian.PutUint64(out[0:8], alphaBits)
	copy(out[8:16], encodeDXTColorBlock(rgb, true))
	return out
}

// encodeDXT5Block encodes one 4x4 block to 16 DXT5 bytes: 8 bytes of
// interpolated 8-bit alpha, then an always-4-color DXT1-shaped color block.
func encodeDXT5Block(px [16]color.NRGBA) []byte {
	var minA, maxA uint8 = 255, 0
	for _, p := range px {
		if p.A < minA {
			minA = p.A
		}
		if p.A > maxA {
			maxA = p.A
		}
	}
	a0, a1 := maxA, minA
	if a0 == a1 {
		if a0 < 255 {
			a0++
		} else if a1 > 0 {
			a1--
		}
	}
	if a0 <= a1 {
		a0, a1 = a1, a0
	}
	var alphaPalette [8]int
	alphaPalette[0] = int(a0)
	alphaPalette[1] = int(a1)
	for i := 1; i <= 6; i++ {
		alphaPalette[i+1] = ((7-i)*int(a0) + i*int(a1)) / 7
	}

	var aIdx uint64
	for i, p := range px {
		best, bestDist := 0, 1<<30
		for ci, c := range alphaPalette {
			d := int(p.A) - c
			if d < 0 {
				d = -d
			}
			if d < bestDist {
				bestDist, best = d, ci
			}
		}
		aIdx |= uint64(best) << uint(3*i)
	}

	var rgb [16][3]uint8
	for i, p := range px {
		rgb[i] = [3]uint8{p.R, p.G, p.B}
	}

	out := make([]byte, 16)
	out[0] = a0
	out[1] = a1
	out[2] = byte(aIdx)
	out[3] = byte(aIdx >> 8)
	out[4] = byte(aIdx >> 16)
	out[5] = byte(aIdx >> 24)
	out[6] = byte(aIdx >> 32)
	out[7] = byte(aIdx >> 40)
	copy(out[8:16], encodeDXTColorBlock(rgb, true))
	return out
}

// encodeBaseLevel encodes img's pixels to a base-mip-level byte blob in the
// given format, matching baseLevelSize's byte count exactly.
func encodeBaseLevel(img image.Image, width, height int, format PixelFormat) ([]byte, error) {
	bb := format.blockBytes()
	if bb == 0 {
		return nil, fmt.Errorf("unsupported pixel format %v", format)
	}
	bounds := image.Rect(0, 0, width, height)
	blocksW := (width + 3) / 4
	blocksH := (height + 3) / 4
	out := make([]byte, 0, blocksW*blocksH*bb)
	for by := 0; by < blocksH; by++ {
		targetBY := blocksH - 1 - by
		for bx := 0; bx < blocksW; bx++ {
			var px [16]color.NRGBA
			for i := 0; i < 16; i++ {
				py, pxx := i/4, i%4
				r, g, b, a := colorAt(img, bounds, bx*4+pxx, targetBY*4+py)
				px[i] = color.NRGBA{R: r, G: g, B: b, A: a}
			}
			var block []byte
			switch format {
			case FormatDXT1:
				block = encodeDXT1Block(px)
			case FormatDXT3:
				block = encodeDXT3Block(px)
			case FormatDXT5:
				block = encodeDXT5Block(px)
			}
			out = append(out, block...)
		}
	}
	return out, nil
}

// EncodeInsert re-encodes img as the base mip level of origData's texture
// and returns a full, valid `.texs` file: everything outside Section 2's
// base-mip-level byte range is copied byte-for-byte from origData
// (Sections 0/1 untouched — width/height/format don't need to change, since
// img must match the original's dimensions exactly, checked below), and any
// mip levels beyond level 0 in Section 2 are also copied byte-for-byte
// unchanged from origData rather than regenerated.
//
// That last point is a deliberate, documented limitation, not an oversight:
// regenerating a full mip chain would need this package to independently
// reverse-engineer the still-not-fully-pinned-down tail-mip-size accounting
// (9 of 186 real samples' Section 2 sizes come out a handful of bytes short
// of what naive per-level halving predicts once mips shrink below ~4px in a
// dimension — see docs/TEXTURE_FORMAT.md's "Tail-mip byte accounting" note)
// — whereas the BASE level's size is unambiguous (baseLevelSize is the
// standard DXT block-count formula, independent of that tail-mip question
// entirely). Consequence: after EncodeInsert, mip levels 1+ still show the
// OLD texture's downscaled content, stale relative to the new base level,
// until the game/engine regenerates them (if it ever does at load time —
// not established either way). Documented here and in tex-insert's own
// usage text; acceptable for this repo's actual use case (small UI/HUD
// texture replacement, where mip levels are rarely visible at all).
//
// img's dimensions must exactly match the original texture's Width/Height
// (returns an error otherwise) — resizing/re-mipping is out of scope.
func EncodeInsert(origData []byte, img image.Image) ([]byte, error) {
	info, err := ParseTexs(origData)
	if err != nil {
		return nil, err
	}
	if !info.IsImage {
		return nil, fmt.Errorf("not an igImage2 .texs file")
	}
	if info.Format == FormatUnknown {
		return nil, fmt.Errorf("could not determine original pixel format (section 2 size %d doesn't cleanly match DXT1 or DXT3)", info.Sec2Size)
	}
	b := img.Bounds()
	if b.Dx() != info.Width || b.Dy() != info.Height {
		return nil, fmt.Errorf("image is %dx%d, must exactly match the original texture's %dx%d (resizing not supported)", b.Dx(), b.Dy(), info.Width, info.Height)
	}
	newBase, err := encodeBaseLevel(img, info.Width, info.Height, info.Format)
	if err != nil {
		return nil, err
	}
	oldBaseSize := baseLevelSize(info.Width, info.Height, info.Format)
	if oldBaseSize != len(newBase) {
		return nil, fmt.Errorf("internal error: encoded base level is %d bytes, expected %d", len(newBase), oldBaseSize)
	}
	if int(info.Sec2Size) < oldBaseSize || int(info.Sec2Off)+int(info.Sec2Size) > len(origData) {
		return nil, fmt.Errorf("original file too small: section 2 size (%d) less than base level (%d)", info.Sec2Size, oldBaseSize)
	}

	out := make([]byte, len(origData))
	copy(out, origData)
	baseOff := info.Sec2Off
	if info.Format == FormatDXT5 {
		baseOff = info.Sec2Off + info.Sec2Size - uint32(oldBaseSize)
	}
	copy(out[baseOff:baseOff+uint32(oldBaseSize)], newBase)
	return out, nil
}
