package iga

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strings"
)

// PakMeta represents a completely self-contained serialization of a PAK (IGZ v4) file,
// allowing it to be compiled back into a binary without needing the original file.
type PakMeta struct {
	Magic              uint32         `json:"magic"`                          // IGZ magic (0x49475A01)
	Version            uint32         `json:"version"`                        // IGZ version (4)
	HeaderData         string         `json:"header_data"`                    // Base64-encoded bytes of the entire file EXCEPT the string pool
	StringPoolStart    uint32         `json:"string_pool_start"`              // Absolute file offset where the string pool started
	StringPoolEnd      uint32         `json:"string_pool_end"`                // Absolute file offset where the string pool ended
	Strings            []string       `json:"strings,omitempty"`              // Localized gameplay strings
	AudioStrings       []string       `json:"audio_strings,omitempty"`        // Audio reference strings
	NonLanguageStrings []string       `json:"non_language_strings,omitempty"` // Other non-language strings
	Layout             []PakStringRef `json:"layout,omitempty"`               // Mapping layout of strings to original pool order
}

type PakStringRef struct {
	Type           string `json:"type"`            // "string", "audio", "non_language"
	Index          int    `json:"index"`           // index into the corresponding array
	OriginalOffset uint32 `json:"original_offset"` // offset relative to StringPoolStart
	IsExternal     bool   `json:"is_external,omitempty"`
	Encoding       string `json:"encoding,omitempty"` // "ascii" or "utf16"
}

// classifyString categorizes strings in the pak string pool.
func classifyString(s string) string {
	trimmed := strings.TrimSpace(s)
	if s == "" || s == "e" || trimmed == "" {
		return "non_language"
	}

	lower := strings.ToLower(s)
	if strings.HasSuffix(lower, ".wav") || strings.HasSuffix(lower, ".ogg") || strings.HasSuffix(lower, ".snds") ||
		strings.Contains(lower, "/sounds/") || strings.Contains(lower, "\\sounds\\") ||
		strings.Contains(lower, "/cutscenes/") || strings.Contains(lower, "\\cutscenes\\") {
		return "audio"
	}

	if strings.HasPrefix(s, "ig") ||
		strings.Contains(s, "Proxy") ||
		strings.Contains(s, "Info") ||
		strings.Contains(s, "List") ||
		strings.Contains(s, "Field") ||
		strings.Contains(s, "Array") ||
		s == "Default" ||
		s == "ENGLISH" || s == "GERMAN" || s == "FRENCH" || s == "SPANISH" || s == "ITALIAN" || s == "DUTCH" || s == "SWEDISH" || s == "RUSSIAN" {
		return "non_language"
	}

	return "string"
}

func isPrintableUTF16(ch uint16) bool {
	high := byte(ch >> 8)
	low := byte(ch)
	if high == 0x00 {
		return (low >= 32 && low <= 126) || low == 9 || low == 10 || low == 13 || (low >= 0xA0)
	}
	if high == 0x04 {
		return true
	}
	return false
}

func isPrintableASCII(b byte) bool {
	return (b >= 32 && b <= 126) || b == 9 || b == 10 || b == 13
}

func scanExternalStrings(data []byte, poolStart, poolEnd, sec1Start uint32) ([]string, []string, []PakStringRef) {
	var audioStrings []string
	var nonLangStrings []string
	var layout []PakStringRef

	type ExtractedString struct {
		val      string
		t        string
		offset   uint32
		encoding string
		length   uint32
	}
	var candidates []ExtractedString

	seenOffsets := make(map[uint32]bool)
	limit := uint32(len(data))

	// Scan the entire file at 1-byte boundaries for pointer values
	for i := uint32(2048); i < limit-4; i++ {
		val := binary.LittleEndian.Uint32(data[i : i+4])

		// It must point inside Section 0 or Section 2
		if val < 2048 || val >= limit {
			continue
		}

		if seenOffsets[val] {
			continue
		}
		seenOffsets[val] = true

		// Try UTF-16LE first
		isUTF16 := false
		var utf16Val string

		if val%2 == 0 && val < limit-8 {
			ch := binary.LittleEndian.Uint16(data[val : val+2])
			if isPrintableUTF16(ch) {
				end := val
				valid := true
				for end < limit-1 {
					c := binary.LittleEndian.Uint16(data[end : end+2])
					if c == 0 {
						break
					}
					if !isPrintableUTF16(c) {
						valid = false
						break
					}
					end += 2
				}
				if valid && end < limit-1 && data[end] == 0 && data[end+1] == 0 && (end-val)/2 >= 4 {
					utf16Bytes := data[val:end]
					runes := make([]rune, len(utf16Bytes)/2)
					for r := 0; r < len(runes); r++ {
						runes[r] = rune(binary.LittleEndian.Uint16(utf16Bytes[r*2 : r*2+2]))
					}
					utf16Val = string(runes)
					isUTF16 = true
				}
			}
		}

		if isUTF16 {
			t := classifyString(utf16Val)
			if t == "audio" || t == "non_language" {
				candidates = append(candidates, ExtractedString{
					val:      utf16Val,
					t:        t,
					offset:   val,
					encoding: "utf16",
					length:   uint32(len(utf16Val)*2 + 2),
				})
			}
			continue
		}

		// Try ASCII
		isASCII := false
		var asciiVal string

		b := data[val]
		if isPrintableASCII(b) {
			end := val
			valid := true
			for end < limit {
				c := data[end]
				if c == 0 {
					break
				}
				if !isPrintableASCII(c) {
					valid = false
					break
				}
				end++
			}
			if valid && end < limit && data[end] == 0 && (end-val) >= 4 {
				asciiVal = string(data[val:end])
				isASCII = true
			}
		}

		if isASCII {
			t := classifyString(asciiVal)
			if t == "audio" || t == "non_language" {
				candidates = append(candidates, ExtractedString{
					val:      asciiVal,
					t:        t,
					offset:   val,
					encoding: "ascii",
					length:   uint32(len(asciiVal) + 1),
				})
			}
			continue
		}
	}

	// Filter out substring duplicates
	for _, c1 := range candidates {
		isSubset := false
		for _, c2 := range candidates {
			if c1.offset > c2.offset && c1.offset < c2.offset+c2.length {
				isSubset = true
				break
			}
		}
		if isSubset {
			continue
		}

		var refIndex int
		if c1.t == "audio" {
			audioStrings = append(audioStrings, c1.val)
			refIndex = len(audioStrings) - 1
		} else {
			nonLangStrings = append(nonLangStrings, c1.val)
			refIndex = len(nonLangStrings) - 1
		}

		layout = append(layout, PakStringRef{
			Type:           c1.t,
			Index:          refIndex,
			OriginalOffset: c1.offset,
			IsExternal:     true,
			Encoding:       c1.encoding,
		})
	}

	return audioStrings, nonLangStrings, layout
}

// DumpPak reads a PAK (IGZ v4) file and outputs a JSON containing its strings and structural metadata.
func DumpPak(pakPath string, w io.Writer) error {
	data, err := os.ReadFile(pakPath)
	if err != nil {
		return fmt.Errorf("read pak: %w", err)
	}

	if len(data) < 8 {
		return fmt.Errorf("file too small to be a valid IGZ file")
	}

	magic := binary.LittleEndian.Uint32(data[0:4])
	version := binary.LittleEndian.Uint32(data[4:8])

	if magic != 0x49475A01 {
		return fmt.Errorf("invalid magic: 0x%08X (expected IGZ\\x01)", magic)
	}

	// We dynamically scan Section 1 for the bounds of the localized string pool.
	// Section 1's offset/size are the second 16-byte section descriptor
	// (offset,size,align,flags), at absolute file offset 0x20/0x24 -- see
	// docs/IGZ_FORMAT.md's "RESOLVED" note on the old Section-0-vs-.pak-header
	// discrepancy: these two fields are NOT "Section 2" (a misreading from
	// mad2assetextractor/docs/PAK_FORMAT.md's older, incorrect 8-byte-stride
	// header model) -- .pak files use the identical 16-byte section-descriptor
	// model level.bld does, confirmed byte-for-byte this session across 7 real
	// .pak files. This code was already reading the *correct* bytes (0x20/0x24
	// really is Section 1's own offset/size under that model, and Section 1 is
	// always the second descriptor regardless of how many total sections a
	// given .pak has), just under the wrong name -- renamed sec2* -> sec1* for
	// clarity, no behavior change.
	if len(data) < 0x24 {
		return fmt.Errorf("file header too small")
	}
	sec1Start := binary.LittleEndian.Uint32(data[0x20:0x24])
	sec1Size := binary.LittleEndian.Uint32(data[0x24:0x28])
	sec1End := sec1Start + sec1Size

	if sec1End > uint32(len(data)) {
		sec1End = uint32(len(data))
	}

	// Scan for the contiguous UTF-16LE string pool inside Section 1
	var poolStart, poolEnd uint32
	pos := sec1Start
	foundStart := false

	for pos < sec1End {
		if foundStart && pos+4 <= sec1End {
			// Check if we hit a double-null terminator ending the string pool list
			if data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 0 && data[pos+3] == 0 {
				break
			}
		}

		if pos+4 <= sec1End {
			// Read two consecutive UTF-16 characters
			ch1 := binary.LittleEndian.Uint16(data[pos : pos+2])
			ch2 := binary.LittleEndian.Uint16(data[pos+2 : pos+4])

			if isPrintableUTF16(ch1) && isPrintableUTF16(ch2) {
				// Measure the length of this string
				end := pos
				for end < sec1End-1 && !(data[end] == 0 && data[end+1] == 0) {
					end += 2
				}
				strLen := (end - pos) / 2

				if !foundStart {
					hasWhitespace := false
					for p := pos; p < end; p += 2 {
						c := binary.LittleEndian.Uint16(data[p : p+2])
						if c == 9 || c == 10 || c == 13 {
							hasWhitespace = true
							break
						}
					}

					if strLen >= 4 && !hasWhitespace { // First string must be at least 4 characters and have no tabs/newlines
						poolStart = pos
						foundStart = true
					} else {
						pos = end + 2
						continue
					}
				}

				pos = end + 2 // skip null terminator
				poolEnd = pos
				continue
			}
		}
		pos += 2
	}

	// Align poolStart to a 4-byte boundary
	for poolStart%4 != 0 {
		poolStart++
	}

	var stringsList []string
	var audioStrings []string
	var nonLangStrings []string
	var layout []PakStringRef
	var headerBytes []byte

	// If no string pool was found, or the pool is empty/invalid, treat as non-localized PAK
	if !foundStart || poolEnd <= poolStart {
		headerBytes = data
		poolStart = 0
		poolEnd = 0
		stringsList = []string{}
	} else {
		// Extract strings
		stringPool := data[poolStart:poolEnd]
		idx := 0
		for idx < len(stringPool)-1 {
			origOffset := uint32(idx)

			var val string
			if stringPool[idx] == 0 && stringPool[idx+1] == 0 {
				val = ""
				idx += 2
			} else {
				end := idx
				for end < len(stringPool)-1 && !(stringPool[end] == 0 && stringPool[end+1] == 0) {
					end += 2
				}

				utf16Bytes := stringPool[idx:end]
				runes := make([]rune, len(utf16Bytes)/2)
				for r := 0; r < len(runes); r++ {
					runes[r] = rune(binary.LittleEndian.Uint16(utf16Bytes[r*2 : r*2+2]))
				}
				val = string(runes)
				idx = end + 2
			}

			// Classify
			t := classifyString(val)
			var refIndex int
			switch t {
			case "audio":
				audioStrings = append(audioStrings, val)
				refIndex = len(audioStrings) - 1
			case "non_language":
				nonLangStrings = append(nonLangStrings, val)
				refIndex = len(nonLangStrings) - 1
			default:
				stringsList = append(stringsList, val)
				refIndex = len(stringsList) - 1
			}

			layout = append(layout, PakStringRef{
				Type:           t,
				Index:          refIndex,
				OriginalOffset: origOffset,
			})
		}

		// Remove string pool from header data
		headerBytes = make([]byte, len(data)-(int(poolEnd)-int(poolStart)))
		copy(headerBytes[:poolStart], data[:poolStart])
		copy(headerBytes[poolStart:], data[poolEnd:])
	}

	// Scan for external strings (audio & non-language strings outside the pool)
	audioStartIdx := len(audioStrings)
	nonLangStartIdx := len(nonLangStrings)
	extAudio, extNonLang, extLayout := scanExternalStrings(data, poolStart, poolEnd, sec1Start)

	for i := range extLayout {
		if extLayout[i].Type == "audio" {
			extLayout[i].Index += audioStartIdx
		} else if extLayout[i].Type == "non_language" {
			extLayout[i].Index += nonLangStartIdx
		}
	}

	audioStrings = append(audioStrings, extAudio...)
	nonLangStrings = append(nonLangStrings, extNonLang...)
	layout = append(layout, extLayout...)

	meta := PakMeta{
		Magic:              magic,
		Version:            version,
		HeaderData:         base64.StdEncoding.EncodeToString(headerBytes),
		StringPoolStart:    poolStart,
		StringPoolEnd:      poolEnd,
		Strings:            stringsList,
		AudioStrings:       audioStrings,
		NonLanguageStrings: nonLangStrings,
		Layout:             layout,
	}

	encoder := json.NewEncoder(w)
	encoder.SetIndent("", "  ")
	return encoder.Encode(meta)
}

// CompilePak builds a binary PAK file from its metadata JSON representation.
func CompilePak(r io.Reader, w io.Writer) error {
	var meta PakMeta
	if err := json.NewDecoder(r).Decode(&meta); err != nil {
		return fmt.Errorf("decode json metadata: %w", err)
	}

	headerBytes, err := base64.StdEncoding.DecodeString(meta.HeaderData)
	if err != nil {
		return fmt.Errorf("decode header base64: %w", err)
	}

	// For non-localized data PAKs, meta.Strings is empty. Reconstruct raw file directly.
	if len(meta.Strings) == 0 && len(meta.AudioStrings) == 0 && len(meta.NonLanguageStrings) == 0 || meta.StringPoolStart == 0 {
		if _, err := w.Write(headerBytes); err != nil {
			return fmt.Errorf("write raw pak: %w", err)
		}
		return nil
	}

	// Rebuild finalStrings list based on layout or fallback
	var finalStrings []string
	if len(meta.Layout) > 0 {
		finalStrings = make([]string, len(meta.Layout))
		for i, ref := range meta.Layout {
			var val string
			switch ref.Type {
			case "string":
				if ref.Index >= 0 && ref.Index < len(meta.Strings) {
					val = meta.Strings[ref.Index]
				}
			case "audio":
				if ref.Index >= 0 && ref.Index < len(meta.AudioStrings) {
					val = meta.AudioStrings[ref.Index]
				}
			case "non_language":
				if ref.Index >= 0 && ref.Index < len(meta.NonLanguageStrings) {
					val = meta.NonLanguageStrings[ref.Index]
				}
			default:
				return fmt.Errorf("unknown string type in layout: %s", ref.Type)
			}
			finalStrings[i] = val
		}
	} else {
		// Legacy fallback
		finalStrings = meta.Strings
	}

	// Rebuild the UTF-16LE string pool and track original and new offsets
	origOffsets := make([]uint32, len(finalStrings))
	if len(meta.Layout) > 0 {
		for i, ref := range meta.Layout {
			if ref.IsExternal {
				origOffsets[i] = ref.OriginalOffset
			} else {
				origOffsets[i] = meta.StringPoolStart + ref.OriginalOffset
			}
		}
	} else {
		// Legacy fallback: reconstruct original offsets by simulating the writing of meta.Strings
		var currentOffset uint32 = meta.StringPoolStart
		for i, s := range meta.Strings {
			origOffsets[i] = currentOffset
			currentOffset += uint32(len([]rune(s))*2 + 2)
		}
	}

	stringPool := new(bytes.Buffer)
	newOffsets := make([]uint32, len(finalStrings))

	// First pass: Compile localized strings to the pool
	if len(meta.Layout) > 0 {
		for i, ref := range meta.Layout {
			if !ref.IsExternal {
				s := finalStrings[i]
				newOffsets[i] = meta.StringPoolStart + uint32(stringPool.Len())
				writeUTF16String(stringPool, s)
			}
		}
	} else {
		for i, s := range finalStrings {
			newOffsets[i] = meta.StringPoolStart + uint32(stringPool.Len())
			writeUTF16String(stringPool, s)
		}
	}

	oldSize := meta.StringPoolEnd - meta.StringPoolStart

	// Pad localized string pool to match original size if it is smaller
	if uint32(stringPool.Len()) < oldSize {
		paddingNeeded := oldSize - uint32(stringPool.Len())
		stringPool.Write(make([]byte, paddingNeeded))
	}

	// Second pass: Append redirected external strings after the localized pool padding
	if len(meta.Layout) > 0 {
		for i, ref := range meta.Layout {
			if ref.IsExternal {
				s := finalStrings[i]
				var origVal string
				origOffset := ref.OriginalOffset
				if origOffset >= meta.StringPoolEnd {
					origOffset -= (meta.StringPoolEnd - meta.StringPoolStart)
				}

				if ref.Encoding == "ascii" {
					end := origOffset
					for end < uint32(len(headerBytes)) && headerBytes[end] != 0 {
						end++
					}
					origVal = string(headerBytes[origOffset:end])
				} else {
					end := origOffset
					for end < uint32(len(headerBytes))-1 && !(headerBytes[end] == 0 && headerBytes[end+1] == 0) {
						end += 2
					}
					utf16Bytes := headerBytes[origOffset:end]
					runes := make([]rune, len(utf16Bytes)/2)
					for r := 0; r < len(runes); r++ {
						runes[r] = rune(binary.LittleEndian.Uint16(utf16Bytes[r*2 : r*2+2]))
					}
					origVal = string(runes)
				}

				if s == origVal {
					newOffsets[i] = origOffsets[i]
				} else {
					newOffsets[i] = origOffsets[i] // offset remains unchanged

					if ref.Encoding == "ascii" {
						origLen := len(origVal)
						if len(s) > origLen {
							return fmt.Errorf("redirected path %q is longer than original path %q (%d > %d characters)", s, origVal, len(s), origLen)
						}
						copy(headerBytes[origOffset:origOffset+uint32(len(s))], []byte(s))
						for k := origOffset + uint32(len(s)); k <= origOffset+uint32(origLen); k++ {
							headerBytes[k] = 0
						}
					} else {
						origLen := len(origVal)
						if len(s) > origLen {
							return fmt.Errorf("redirected string %q is longer than original string %q (%d > %d characters)", s, origVal, len(s), origLen)
						}
						for rIdx, r := range []rune(s) {
							binary.LittleEndian.PutUint16(headerBytes[origOffset+uint32(rIdx*2):], uint16(r))
						}
						for k := origOffset + uint32(len(s)*2); k <= origOffset+uint32(origLen*2)+1; k++ {
							headerBytes[k] = 0
						}
					}
				}
			}
		}
	}

	newSize := uint32(stringPool.Len())
	shift := int32(newSize) - int32(oldSize)

	// Bug 5: Section 0's own chunk table (igIGZLoader::fixupChunk's
	// reference-fixup mechanism -- see docs/IGA_FORMAT.md's "A newly-found,
	// much larger candidate" section for the full discovery, confirmed via
	// a live debugger session catching a real SIGSEGV inside it) embeds
	// segmented-pointer patch locations into Section 1 that go stale under
	// exactly the same pool-resize edit bugs 1-4 already patch for -- but
	// bugs 1-4 only ever touch known object-graph *fields*; this table's
	// entries are a structurally separate mechanism bugs 1-4 never
	// examined.
	//
	// Computed here (using the *original*, unmodified headerBytes/meta --
	// every value this produces is Section-1-*relative*, per the
	// segID:offset convention, so it's entirely independent of anything
	// bugs 1-4 do below and of Section 0's own absolute position) but
	// *applied* as a final splice on `outBytes` at the very end of this
	// function, after every other fix has already run against the
	// original, not-yet-resized Section 0 layout. This avoids a real
	// coordinate-system trap: bugs 1-4's whole-file pointer scan compares
	// *stored* pointer values (which remain in original-file coordinates
	// no matter what) against `meta.StringPoolEnd` -- if this fix instead
	// pre-resized `headerBytes`/`meta` before that scan ran, the scan
	// would need every stored value's comparison AND `meta.StringPoolEnd`
	// itself to be adjusted consistently, which they structurally can't be
	// with a single scalar `shift` once Section 0 also changes size.
	// Applying this fix as an isolated final splice sidesteps the problem
	// entirely: nothing else in the file holds an absolute pointer *into*
	// Section 0 that would need correcting when it resizes, only the
	// section descriptor table's own offset/size fields do.
	var newSec0 []byte
	var sizeDelta0 int
	sec0Off, sec1OffOrig := uint32(0), uint32(0)
	if graph0, g0err := ParseObjectGraph(headerBytes); g0err == nil {
		sec0Off = graph0.Sections[0].Offset
		sec1OffOrig = graph0.Sec1Off
		oldPoolEndRel := meta.StringPoolEnd - sec1OffOrig
		var cerr error
		newSec0, sizeDelta0, cerr = fixChunkTableSection1Refs(headerBytes, sec0Off, sec1OffOrig, shift, oldPoolEndRel)
		if cerr != nil {
			return fmt.Errorf("fix chunk table: %w", cerr)
		}
	}

	// Specifically update Section 2 size in the header.
	// In the header, Section 2 size is at offset 0x24.
	binary.LittleEndian.PutUint32(headerBytes[0x24:], binary.LittleEndian.Uint32(headerBytes[0x24:0x28])+uint32(shift))

	// Construct the final output buffer to perform pointer adjustments on the whole file
	outBytes := make([]byte, len(headerBytes)+stringPool.Len())
	copy(outBytes[:meta.StringPoolStart], headerBytes[:meta.StringPoolStart])
	copy(outBytes[meta.StringPoolStart:meta.StringPoolStart+uint32(stringPool.Len())], stringPool.Bytes())
	copy(outBytes[meta.StringPoolStart+uint32(stringPool.Len()):], headerBytes[meta.StringPoolStart:])

	// Build a map of original string starting offsets to their new starting offsets
	stringPointersMap := make(map[uint32]uint32)
	for i := 0; i < len(finalStrings); i++ {
		stringPointersMap[origOffsets[i]] = newOffsets[i]
	}

	// We must adjust any pointers (uint32) in the file that point to file offsets >= stringPoolEnd,
	// or pointers pointing inside the string pool.
	// Scan the file starting from Section 0 (skipping the main header fields) at 4-byte boundaries.
	sec0Offset := binary.LittleEndian.Uint32(headerBytes[0x10:0x14])
	if sec0Offset < 2048 {
		sec0Offset = 2048
	}

	// The fixup pass below covers the WHOLE file, not just the region physically stored before
	// the string pool -- extending a bug where any pointer field stored AFTER the pool (in a
	// typical .pak, the bulk of the file's actual object-graph data: 383KB / 97%, for
	// ENGLISH.pak) was silently left unscanned and unpatched. See docs/IGA_FORMAT.md's "Two
	// known content-editing bugs" #2 for the full empirical writeup (40 confirmed real pointer
	// fields found physically stored in that region, verified against independently-known
	// string offsets).
	//
	// Pointers in this format are plain absolute file offsets, not IGZ_FORMAT.md's segmented
	// (segID<<24|offset) scheme -- confirmed against real examples: e.g. a real field at file
	// offset 13528 (inside the post-pool tail) holds the value 3036, which only resolves
	// correctly as a literal absolute file offset (the real target, a string, does start at byte
	// 3036). Reading it as a segmented pointer (segID=0, meaning "relative to whichever section
	// the field itself lives in" per IGZ_FORMAT.md -- Section 1, based at file offset 10496)
	// would resolve to 10496+3036=13532, which is not a valid string start. So one uniform rule
	// (">= StringPoolEnd -> add shift", else check stringPointersMap) is correct everywhere in
	// the file, including a field and its target both living in the moved tail region: since
	// both shift by the identical amount, no relative/no-op special case is needed.
	//
	// Two properties make this pass safe over a much larger scan region than the original
	// pre-pool-only version:
	//
	//  1. We scan `headerBytes` (the pre-edit file with the pool already excised, so its bytes
	//     are never rewritten by the pool-resize itself) and write any correction into the
	//     separate `outBytes` buffer, rather than scanning-and-patching one buffer in place.
	//     Patching in place has a real, observed failure mode: overwriting a value at position i
	//     changes what position i+1..i+3 decode as, so a later iteration can spuriously "find"
	//     and clobber bytes that were never a real pointer field, just because an earlier patch
	//     happened to leave a matching 4-byte pattern behind. Separating the read source
	//     (`headerBytes`, stable throughout the pass) from the write destination (`outBytes`)
	//     avoids that read/write aliasing entirely.
	//  2. We only examine 4-byte-*aligned* candidate positions (relative to each field's own
	//     original absolute file offset) instead of every byte offset. Empirically, all 71 real
	//     pointer matches found against ENGLISH.pak's independently-verified string-offset list
	//     landed on a 4-byte-aligned absolute file offset, and zero real matches were found
	//     unaligned (only garbage/substring false positives showed up off-alignment) --
	//     consistent with this being a serialized struct field, not free-floating text.
	//     Restricting to aligned positions both matches how the format actually stores fields
	//     and, over a much larger scan region (383KB vs. the original ~9KB), meaningfully cuts
	//     down the false-positive/cascading-overwrite surface a per-byte scan would expose (this
	//     class of bug was directly observed during development of this fix, on real data, before
	//     switching to this aligned-scan design -- see the "false positives" discussion in this
	//     fix's own writeup in docs/IGA_FORMAT.md for the concrete before/after evidence).
	//
	// headerBytes indices before meta.StringPoolStart are the file's head (unchanged position --
	// origOffset == i, outPos == i). Indices at or after meta.StringPoolStart are the file's tail
	// (position shifts -- origOffset == i+oldSize, since headerBytes' tail section is the
	// original file's bytes from the old StringPoolEnd onward with the pool gap excised; outPos
	// == i+newSize, since that's where the copy() above that builds outBytes placed it). The tail
	// half aligns `i` to a 4-byte boundary in ORIGINAL absolute-file-offset terms (i+oldSize),
	// not in headerBytes' own indexing, since oldSize is not guaranteed to itself be a multiple
	// of 4 and what matters is the alignment real serialized fields actually use.
	headStart := int(sec0Offset)
	for headStart%4 != 0 {
		headStart++
	}
	for i := headStart; i+4 <= int(meta.StringPoolStart); i += 4 {
		val := binary.LittleEndian.Uint32(headerBytes[i : i+4])
		if val >= meta.StringPoolEnd && val <= uint32(len(headerBytes))+oldSize {
			newVal := uint32(int32(val) + shift)
			binary.LittleEndian.PutUint32(outBytes[i:i+4], newVal)
		} else if newVal, exists := stringPointersMap[val]; exists {
			binary.LittleEndian.PutUint32(outBytes[i:i+4], newVal)
		}
	}

	tailStart := int(meta.StringPoolStart)
	for (uint32(tailStart)+oldSize)%4 != 0 {
		tailStart++
	}
	for i := tailStart; i+4 <= len(headerBytes); i += 4 {
		val := binary.LittleEndian.Uint32(headerBytes[i : i+4])

		outPos := i + int(newSize)
		if outPos < 0 || outPos+4 > len(outBytes) {
			continue
		}

		if val >= meta.StringPoolEnd && val <= uint32(len(headerBytes))+oldSize {
			newVal := uint32(int32(val) + shift)
			binary.LittleEndian.PutUint32(outBytes[outPos:outPos+4], newVal)
		} else if newVal, exists := stringPointersMap[val]; exists {
			binary.LittleEndian.PutUint32(outBytes[outPos:outPos+4], newVal)
		}
	}

	// Separately from the absolute-offset string-pointer fixups above, Section 1's own
	// top-level igObjectList header carries an array of Section-1-*relative* pointers (one per
	// top-level object -- see IGZ_FORMAT.md and objgraph.go's TopLevelObjects, which already
	// parses this structurally, not heuristically: `addr := g.Sec1Off + relPtr`). This is a
	// genuinely different addressing scheme from the string pointers above (relative to Section
	// 1's own fixed base offset, not the file start), and a blind byte-scan cannot reliably tell
	// the two apart by value alone -- small relative offsets look just like small absolute ones.
	// Discovered while cross-checking this fix against objgraph.go's FullWalk/TopLevelObjects on
	// a real grown ENGLISH.pak: 61 of 97 top-level objects resolved to garbage, because this
	// array's entries were never touched by CompilePak at all (true before this fix too -- this
	// is a distinct, pre-existing bug from the tail-scan-gap fixed above, not a regression it
	// introduced). Since this array's location and shape are exactly known (not a heuristic
	// scan), it can be fixed precisely rather than guessed at: reparse the header via
	// ParseObjectGraph, and for each entry whose absolute target (Sec1Off + relPtr) is at or past
	// the old StringPoolEnd, add the same `shift` used everywhere else above.
	if graph, gerr := ParseObjectGraph(headerBytes); gerr == nil {
		mapToOut := func(headerBytesPos int) int {
			if headerBytesPos < int(meta.StringPoolStart) {
				return headerBytesPos
			}
			return headerBytesPos + int(newSize)
		}
		sec1Off := graph.Sec1Off
		if int(sec1Off)+0x1C <= len(headerBytes) {
			count := binary.LittleEndian.Uint32(headerBytes[sec1Off+0x0C:])
			arrayOff := binary.LittleEndian.Uint32(headerBytes[sec1Off+0x18:])
			arrBase := sec1Off + arrayOff
			if count <= 1_000_000 {
				for i := uint32(0); i < count; i++ {
					fieldPos := int(arrBase + i*4)
					if fieldPos+4 > len(headerBytes) {
						break
					}
					relPtr := binary.LittleEndian.Uint32(headerBytes[fieldPos : fieldPos+4])
					absTarget := sec1Off + relPtr
					if absTarget < meta.StringPoolEnd || absTarget > uint32(len(headerBytes))+oldSize {
						continue // target didn't move, leave untouched
					}
					newRelPtr := uint32(int32(relPtr) + shift)
					outPos := mapToOut(fieldPos)
					if outPos+4 <= len(outBytes) {
						binary.LittleEndian.PutUint32(outBytes[outPos:outPos+4], newRelPtr)
					}
				}
			}
		}

		// A third, distinct stale-metadata bug, found the same way bug 2c was (cross-checking
		// against objgraph.go's structural reads rather than more byte-scanning): every real .pak
		// file's single top-level `LanguagePackInfo` object carries a `stringDict` field
		// (tfbscript_pc_field_schema.json: offset 24, kind "memoryref", PC-verified) whose raw
		// on-disk 32-bit value is NOT a plain pointer -- empirically (not from the schema, which
		// only records the field's offset) it decodes as `0x80000000 | poolByteSize`, the same
		// "top bit set = external fixup" convention docs/TEXTURE_FORMAT.md already documents for
		// an unrelated class (`igImage2.format`). `poolByteSize` is exactly `StringPoolEnd -
		// (LanguagePackInfo.Addr + 32)` -- i.e. LanguagePackInfo is always immediately followed by
		// the raw string pool, and this field is the pool's own serialized byte length. Confirmed
		// byte-exact (predicted pool-end position landed exactly on this function's own
		// independently-computed StringPoolEnd) against 6 real .pak files spanning 3 levels, both
		// mad2/mad2demo/mad2russia install trees, and multiple languages -- see
		// docs/IGA_FORMAT.md's "Bug 3" writeup for the full evidence. This is almost certainly the
		// actual cause of the game's instant crash on loading any level with an edited .pak that
		// both prior fixes above (string-pointer tail-scan-gap, top-level object-array) failed to
		// resolve: it is not a pointer value at all (topByte 0x80 means it never matches either
		// fixup pass's "value >= StringPoolEnd" or "value in stringPointersMap" checks above), so
		// every previous edit has been serializing this field silently stale -- growing or
		// shrinking the pool moves the real next-object data by `shift` bytes while this field
		// keeps reporting the ORIGINAL pool size, which likely makes the loader look for the next
		// object's classIdx word at the wrong file offset (landing mid-string after a grow) and
		// misparse it as object data.
		if top, terr := graph.TopLevelObjects(); terr == nil {
			for _, obj := range top {
				if obj.ClassName != "LanguagePackInfo" {
					continue
				}
				fieldPos := int(obj.Addr) + 24
				if fieldPos+4 > len(headerBytes) {
					continue
				}
				raw := binary.LittleEndian.Uint32(headerBytes[fieldPos : fieldPos+4])
				topByte := raw & 0xFF000000
				low24 := raw & 0x00FFFFFF
				newLow24 := uint32(int32(low24) + shift)
				if newLow24 > 0x00FFFFFF {
					// Would overflow the 24-bit field -- refuse rather than silently wrap/corrupt.
					return fmt.Errorf("LanguagePackInfo.stringDict low-24-bit pool size overflowed (orig=%d shift=%d new=%d) at field offset %d", low24, shift, newLow24, fieldPos)
				}
				newRaw := topByte | newLow24
				outPos := mapToOut(fieldPos)
				if outPos >= 0 && outPos+4 <= len(outBytes) {
					binary.LittleEndian.PutUint32(outBytes[outPos:outPos+4], newRaw)
				}
			}

		}

		// A fourth, far larger-blast-radius bug, found via direct Ghidra decompilation of
		// the real PC engine (not more byte-scanning): `igMemoryRefMetaField` -- the meta
		// field type behind `LanguagePackInfo.stringDict` above -- is confirmed (via
		// `igMemoryRefMetaField::computeSize`, igCore.dll 0x10055670, which unconditionally
		// returns 8) to always occupy exactly 8 bytes at runtime: a size dword (what the fix
		// above patches -- `0x80000000 | byteSize`, read by `igMemoryRefMetaField::
		// getMemorySize`, igCore.dll 0x10005cb0, which masks off the top marker bits) directly
		// followed by a *second* dword that is a genuine pointer -- confirmed directly against
		// LanguagePackInfo.stringDict itself: its own second dword (object addr + 28) holds a
		// small integer that resolves *exactly* to this file's own StringPoolStart under the
		// same Section-1-relative scheme (`Sec1Off + rawValue`, segID=0) IGZ_FORMAT.md
		// documents and the top-level-array fix above already uses -- e.g. ENGLISH.pak:
		// Sec1Off=10496, stored value=448, 10496+448=10944=StringPoolStart, byte-exact.
		// LanguagePackInfo's own copy of this pointer never needs adjusting in practice (its
		// target, the pool's start, never moves under a pool-resize edit -- only the pool's
		// *end* moves) which is why this went unnoticed until specifically hunted for.
		//
		// SoundDataInfo -- confirmed via `arkRegisterInitialize@SoundDataInfo`
		// (SoundInfoLib.dll 0x10003190) to register a field `_visemeStream` at offset 0x2c,
		// also kind igMemoryRefMetaField, i.e. the exact same 8-byte {size,pointer} shape --
		// is the class making up the overwhelming majority of every real .pak's top-level
		// objects (96 of 97 in ENGLISH.pak; ~87-96% across every other real .pak checked).
		// Unlike LanguagePackInfo's copy, SoundDataInfo.visemeStream's pointer (at
		// object addr + 0x30) resolves to that instance's own per-object viseme (lip-sync)
		// binary blob, which -- empirically, against every real .pak file checked (VolcanoRave
		// ENGLISH/GERMAN, Waterhole_Demo ENGLISH, ConvoyChase_Demo ENGLISH) -- lands physically
		// *after* the object's own position, i.e. squarely in the part of the file that shifts
		// under a pool-resize edit, for 87-98% of instances (94/96 in ENGLISH.pak; the
		// remainder have `size==0`, i.e. no viseme data, and their pointer is correctly
		// harmless/unresolved). Neither of this field's two dwords is examined by any of the
		// three prior fixes: the size dword is marker-shaped exactly like stringDict's and
		// never reachable by the plain-pointer heuristic passes above; the pointer dword's raw
		// value is a small Section-1-relative offset (e.g. 2496), far below
		// meta.StringPoolEnd and never a key in stringPointersMap, so both of bug 2's tests
		// silently pass over it too. This means essentially every SoundDataInfo object in
		// essentially every edited .pak has had a stale visemeStream pointer -- a dramatically
		// larger-blast-radius instance of exactly the same class of bug bug 3 fixed for one
		// single field on one single object. See docs/IGA_FORMAT.md's "Bug 4" for the full
		// writeup and evidence.
		//
		// This walk deliberately does NOT reuse `graph.TopLevelObjects()` (unlike bugs 2c/3
		// above) -- a real, distinct pitfall was found and fixed while building this exact fix
		// (see docs/IGA_FORMAT.md's "Bug 4" for the concrete before/after numbers): that method
		// resolves each top-level entry as `addr := Sec1Off + relPtr` and then indexes directly
		// into whatever buffer it was given (`headerBytes` here) at `addr`. `relPtr` is the
		// *stale*, unfixed on-disk value (bug 2c's own fixup writes its correction only into
		// `outBytes`, never back into `headerBytes`, by design -- see that fix's own comments),
		// so `addr` is really an *original-file* absolute position. `headerBytes` is `oldSize`
		// bytes shorter than the original file from `meta.StringPoolStart` onward (the pool was
		// physically excised, not just logically resized), so indexing `headerBytes[addr]` for
		// any `addr >= meta.StringPoolEnd` silently reads the bytes belonging to a *different*
		// object roughly `oldSize` bytes further into the original file -- which, since
		// SoundDataInfo makes up the overwhelming majority of objects in this region and they
		// all share the same classIdx, usually *also* looks like a plausible, validly-classed
		// SoundDataInfo instance, just the wrong one, with the wrong field values. This was
		// caught empirically, not theoretically: an early version of this fix using
		// `graph.TopLevelObjects()` against `headerBytes` silently found only 90 of the file's
		// 97 real top-level objects and produced a visemeStream pointer fixup that -- diffed
		// against a parse of the real, complete pre- and post-edit files -- never actually
		// changed any bytes at all, because it was reading/testing the wrong object's fields.
		// The walk below avoids this by working entirely in *original-file* absolute-position
		// terms (matching how `relPtr` values are actually authored) until the one point it
		// needs to physically read `headerBytes`, converting explicitly via origToHeaderBytesPos
		// at that point -- the same head/tail split `mapToOut` above already encodes, just in
		// the opposite (read-side, not write-side) direction.
		if int(sec1Off)+0x1C <= len(headerBytes) {
			origToHeaderBytesPos := func(origPos uint32) (int, bool) {
				if origPos < meta.StringPoolStart {
					return int(origPos), true
				}
				if origPos >= meta.StringPoolEnd {
					return int(origPos) - int(oldSize), true
				}
				return 0, false // falls inside the (old) pool itself -- not a valid object position
			}
			count := binary.LittleEndian.Uint32(headerBytes[sec1Off+0x0C:])
			arrayOff := binary.LittleEndian.Uint32(headerBytes[sec1Off+0x18:])
			arrBase := sec1Off + arrayOff
			if count <= 1_000_000 {
				for i := uint32(0); i < count; i++ {
					entryPos := int(arrBase + i*4)
					if entryPos+4 > len(headerBytes) {
						break
					}
					relPtr := binary.LittleEndian.Uint32(headerBytes[entryPos : entryPos+4])
					objOrigAddr := sec1Off + relPtr
					hbPos, ok := origToHeaderBytesPos(objOrigAddr)
					if !ok || hbPos < 0 || hbPos+0x34 > len(headerBytes) {
						continue
					}
					classIdx := binary.LittleEndian.Uint32(headerBytes[hbPos : hbPos+4])
					if int(classIdx) >= len(graph.ClassNames) || graph.ClassNames[classIdx] != "SoundDataInfo" {
						continue
					}
					fieldPos := hbPos + 0x30
					relPtr2 := binary.LittleEndian.Uint32(headerBytes[fieldPos : fieldPos+4])
					absTarget2 := sec1Off + relPtr2
					if absTarget2 < meta.StringPoolEnd || absTarget2 > uint32(len(headerBytes))+oldSize {
						continue // target didn't move (or size==0 / unresolved), leave untouched
					}
					newRelPtr2 := uint32(int32(relPtr2) + shift)
					outPos := mapToOut(fieldPos)
					if outPos >= 0 && outPos+4 <= len(outBytes) {
						binary.LittleEndian.PutUint32(outBytes[outPos:outPos+4], newRelPtr2)
					}
				}
			}
		}
	}

	// Apply the chunk-table fix computed earlier (bug 5), as an isolated
	// final splice -- see that computation's own comment for why this has
	// to happen last, against the already-fully-corrected `outBytes`,
	// rather than by pre-resizing `headerBytes`. At this point `outBytes`
	// still has Section 0 at its original size/position (nothing above
	// touches Section 0's own bytes), so `outBytes[sec0Off:sec1OffOrig]`
	// is exactly the original Section 0 span to replace.
	if newSec0 != nil {
		if int(sec1OffOrig) > len(outBytes) {
			return fmt.Errorf("chunk table fix: Section 1 offset %d past end of output (%d bytes)", sec1OffOrig, len(outBytes))
		}
		rebuilt := make([]byte, 0, len(outBytes)+sizeDelta0)
		rebuilt = append(rebuilt, outBytes[:sec0Off]...)
		rebuilt = append(rebuilt, newSec0...)
		rebuilt = append(rebuilt, outBytes[sec1OffOrig:]...)
		// Section descriptor table: Section 0's size (absolute offset
		// 0x14) and Section 1's offset (absolute offset 0x20) both need
		// to reflect Section 0 changing size. Section 1's own size
		// (0x24) is unaffected -- already correctly set by the shift
		// logic above, independent of Section 0's resize.
		binary.LittleEndian.PutUint32(rebuilt[0x14:], binary.LittleEndian.Uint32(rebuilt[0x14:0x18])+uint32(int32(sizeDelta0)))
		binary.LittleEndian.PutUint32(rebuilt[0x20:], binary.LittleEndian.Uint32(rebuilt[0x20:0x24])+uint32(int32(sizeDelta0)))
		outBytes = rebuilt
	}

	if _, err := w.Write(outBytes); err != nil {
		return fmt.Errorf("write output bytes: %w", err)
	}

	return nil
}

func writeUTF16String(w io.Writer, s string) {
	runes := []rune(s)
	buf := make([]byte, len(runes)*2)
	for i, r := range runes {
		binary.LittleEndian.PutUint16(buf[i*2:], uint16(r))
	}
	w.Write(buf)
	w.Write([]byte{0, 0}) // null terminator
}
