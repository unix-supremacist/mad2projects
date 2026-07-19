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

func scanExternalStrings(data []byte, poolStart, poolEnd, sec2Start uint32) ([]string, []string, []PakStringRef) {
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

	// We dynamically scan Section 2 for the bounds of the localized string pool.
	// Section 2 start offset is stored at 0x20 in the header.
	if len(data) < 0x24 {
		return fmt.Errorf("file header too small")
	}
	sec2Start := binary.LittleEndian.Uint32(data[0x20:0x24])
	sec2Size := binary.LittleEndian.Uint32(data[0x24:0x28])
	sec2End := sec2Start + sec2Size

	if sec2End > uint32(len(data)) {
		sec2End = uint32(len(data))
	}

	// Scan for the contiguous UTF-16LE string pool inside Section 2
	var poolStart, poolEnd uint32
	pos := sec2Start
	foundStart := false

	for pos < sec2End {
		if foundStart && pos+4 <= sec2End {
			// Check if we hit a double-null terminator ending the string pool list
			if data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 0 && data[pos+3] == 0 {
				break
			}
		}

		if pos+4 <= sec2End {
			// Read two consecutive UTF-16 characters
			ch1 := binary.LittleEndian.Uint16(data[pos : pos+2])
			ch2 := binary.LittleEndian.Uint16(data[pos+2 : pos+4])

			if isPrintableUTF16(ch1) && isPrintableUTF16(ch2) {
				// Measure the length of this string
				end := pos
				for end < sec2End-1 && !(data[end] == 0 && data[end+1] == 0) {
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
	extAudio, extNonLang, extLayout := scanExternalStrings(data, poolStart, poolEnd, sec2Start)

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

	for i := int(sec0Offset); i < int(meta.StringPoolStart); i++ {
		val := binary.LittleEndian.Uint32(outBytes[i : i+4])

		// If this is a valid offset that pointed to a location past the old string pool,
		// we shift it by the offset difference.
		if val >= meta.StringPoolEnd && val <= uint32(len(headerBytes))+oldSize {
			newVal := uint32(int32(val) + shift)
			binary.LittleEndian.PutUint32(outBytes[i:i+4], newVal)
		} else {
			// It points into the string pool or to an external string. Check if it matches exactly.
			if newVal, exists := stringPointersMap[val]; exists {
				binary.LittleEndian.PutUint32(outBytes[i:i+4], newVal)
			}
		}
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
