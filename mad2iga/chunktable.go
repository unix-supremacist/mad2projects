package iga

import (
	"encoding/binary"
	"fmt"
)

// Section 0's own chunk table -- a mechanism the real engine's
// igIGZLoader::fixupChunk (igCore.dll) walks at load time to resolve
// reference fixups, entirely separate from anything CompilePak's other
// fixes (see pak.go) touch. See docs/IGA_FORMAT.md's "A newly-found, much
// larger candidate: Section 0's own chunk table" section for the full
// discovery writeup, including the live-debugger evidence this decode
// algorithm was validated against (a real captured SIGSEGV, and an exact
// byte-for-byte match between a decoded chunk-5 entry and this file's own
// independently-derived StringPoolEnd).
//
// Layout, confirmed against real files:
//
//	Section0+0x10  uint32  chunkCount
//	Section0+0x14  uint32  chunkTableOff (relative to Section0's own start)
//	[chunkTableOff, chunkCount x chunk]
//
// Each chunk: a 24-byte (0x18) header --
//
//	+0x00  uint32  type
//	+0x04  uint32  (unidentified)
//	+0x08  uint32  (unidentified)
//	+0x0C  uint32  count    -- number of nibble-delta-encoded entries
//	+0x10  uint32  stride   -- total chunk size including this header;
//	                           advances to the next chunk
//	+0x14  uint32  (unidentified -- not needed to read/write patch values)
//
// -- followed by `stride - 0x18` bytes of payload. For chunk types 4, 5,
// and 6 (the only ones seen in real .pak files so far; 7/0xb/0xc/0xf are
// structurally identical per fixupChunk's decompilation and handled the
// same way defensively), the payload is a nibble-packed variable-length
// delta sequence decoding to `count` cumulative segmented-pointer values
// (segID:offset, same convention as ResolveSegPointer in objgraph.go).

// chunkTypesWithSegPointerPayloads are the chunk types whose payload is
// the nibble-delta segmented-pointer sequence this file knows how to
// decode/encode. Types 4, 5, 6 are the ones confirmed present in real
// .pak files; 7/0xb/0xc/0xf share the same FUN_10042800-based resolution
// mechanism per fixupChunk's decompilation (see docs/IGA_FORMAT.md) and
// are included defensively even though no real sample has been seen using
// them, so a future file that does use them still gets fixed up rather
// than silently corrupted.
var chunkTypesWithSegPointerPayloads = map[uint32]bool{
	4: true, 5: true, 6: true, 7: true, 0xb: true, 0xc: true, 0xf: true,
}

// chunkTypesWithRawPointerPayloads are chunk types whose payload is
// `count` plain, literal `uint32` segmented pointers (segID:offset, same
// convention as everything else in this file) -- *not* nibble-delta
// encoded. Only ever consumed by `igIGZLoader::postReadChunk` (bugs 1-5
// only ever fixed `fixupChunk`'s own chunk types, which never include
// these), via `resolvePointer(this, sectionBaseTable, rawValue)` --
// confirmed by decompiling that function directly: it's exactly
// `sectionBaseTable[segID] + offset`, no delta-stream involved at all.
// Found live: chunk types 9 and 14 in a real `ENGLISH.pak` both carry a
// raw offset past the string pool's end, needing the exact same `shift`
// correction as the nibble-encoded types above, and neither was ever
// touched by any prior fix -- this is what a live debugger session
// eventually traced the persisting post-bug-5 crash to (see
// docs/IGA_FORMAT.md's bug 6 writeup). Type 8 is included defensively
// (same `resolvePointer` call shape in `postReadChunk`'s decompilation,
// just not observed needing an actual shift in the one real file checked
// so far).
var chunkTypesWithRawPointerPayloads = map[uint32]bool{
	8: true, 9: true, 0xe: true,
}

// chunkTableEntry describes one chunk in Section 0's chunk table, at
// absolute file offsets within a specific data buffer.
type chunkTableEntry struct {
	Type      uint32
	Count     uint32
	Stride    uint32
	AbsOffset uint32 // absolute offset of this chunk's own 0x18-byte header
}

// PayloadStart / PayloadEnd are this chunk's nibble-stream payload bounds
// (absolute offsets into the buffer readChunkTable was called against).
func (e chunkTableEntry) PayloadStart() uint32 { return e.AbsOffset + 0x18 }
func (e chunkTableEntry) PayloadEnd() uint32   { return e.AbsOffset + e.Stride }

// readChunkTable reads Section 0's chunk table from data, given sec0Off
// (Section 0's absolute offset within data). Returns each chunk's header
// info in file order.
func readChunkTable(data []byte, sec0Off uint32) ([]chunkTableEntry, error) {
	if int(sec0Off)+0x18 > len(data) {
		return nil, fmt.Errorf("section 0 too small for chunk-table header")
	}
	chunkCount := binary.LittleEndian.Uint32(data[sec0Off+0x10:])
	chunkTableRelOff := binary.LittleEndian.Uint32(data[sec0Off+0x14:])
	if chunkCount > 100_000 {
		return nil, fmt.Errorf("implausible chunk count %d", chunkCount)
	}
	pos := sec0Off + chunkTableRelOff
	entries := make([]chunkTableEntry, 0, chunkCount)
	for i := uint32(0); i < chunkCount; i++ {
		if int(pos)+0x18 > len(data) {
			return nil, fmt.Errorf("chunk table entry %d header out of bounds", i)
		}
		e := chunkTableEntry{
			Type:      binary.LittleEndian.Uint32(data[pos:]),
			Count:     binary.LittleEndian.Uint32(data[pos+0xc:]),
			Stride:    binary.LittleEndian.Uint32(data[pos+0x10:]),
			AbsOffset: pos,
		}
		if e.Stride == 0 || int(pos+e.Stride) > len(data) {
			return nil, fmt.Errorf("chunk table entry %d has invalid stride %d", i, e.Stride)
		}
		entries = append(entries, e)
		pos += e.Stride
	}
	return entries, nil
}

// decodeChunkPatchValues decodes a chunk's nibble-packed variable-length
// delta payload into `count` cumulative segmented-pointer values (see the
// file header comment for the encoding). One byte holds two nibbles; the
// low nibble is read first, the high nibble second (the byte pointer only
// advances after the high nibble is consumed). Each value's delta from
// the previous value is decoded 3 bits at a time, low bits first,
// continuing while the nibble's top bit (0x8) is set.
func decodeChunkPatchValues(payload []byte, count uint32) ([]uint32, error) {
	pos := 0
	high := false
	nextNibble := func() (uint8, error) {
		if pos >= len(payload) {
			return 0, fmt.Errorf("nibble stream ran past end of payload")
		}
		b := payload[pos]
		var v uint8
		if !high {
			v = b & 0xF
		} else {
			v = b >> 4
		}
		if high {
			pos++
		}
		high = !high
		return v, nil
	}
	var cum uint32
	out := make([]uint32, 0, count)
	for j := uint32(0); j < count; j++ {
		n, err := nextNibble()
		if err != nil {
			return nil, err
		}
		val := uint32(n & 7)
		shift := uint(3)
		for n&8 != 0 {
			n, err = nextNibble()
			if err != nil {
				return nil, err
			}
			val |= uint32(n&7) << shift
			shift += 3
		}
		cum = cum + 4 + val*4
		out = append(out, cum)
	}
	return out, nil
}

// encodeChunkPatchValues is the inverse of decodeChunkPatchValues: given
// the full sequence of (possibly corrected) cumulative values, re-encodes
// them into a fresh nibble-packed byte stream using the same convention.
// Every value must equal prev+4+4k for some non-negative integer k (true
// by construction for unmodified decoded values, and for values shifted
// by a multiple of 4 -- CompilePak's caller is responsible for checking
// that any pool-size shift it applies is a multiple of 4 before calling
// this, since the encoding cannot represent anything else).
func encodeChunkPatchValues(values []uint32) ([]byte, error) {
	var out []byte
	var curByte byte
	haveLow := false
	emitNibble := func(n uint8) {
		if !haveLow {
			curByte = n & 0xF
			haveLow = true
		} else {
			curByte |= (n & 0xF) << 4
			out = append(out, curByte)
			haveLow = false
		}
	}
	var prev uint32
	for _, v := range values {
		if v < prev+4 || (v-prev-4)%4 != 0 {
			return nil, fmt.Errorf("value 0x%X is not prev(0x%X)+4+4k -- cannot encode", v, prev)
		}
		delta := (v - prev - 4) / 4
		for {
			nib := uint8(delta & 7)
			delta >>= 3
			if delta != 0 {
				nib |= 8
			}
			emitNibble(nib)
			if delta == 0 {
				break
			}
		}
		prev = v
	}
	if haveLow {
		// Odd total nibble count -- pad the final byte's high nibble with
		// zero. The real format always occupies a whole number of bytes,
		// so this is the natural completion, not a guess.
		out = append(out, curByte)
	}
	return out, nil
}

// fixChunkTableSection1Refs rewrites Section 0's chunk table so every
// chunk-4/5/6/7/0xb/0xc/0xf entry whose decoded segmented pointer targets
// Section 1 (segID 0, per the empirically-confirmed convention -- see
// docs/IGA_FORMAT.md) at or past oldPoolEndRel (Section-1-relative) has
// `shift` added to its offset, matching what CompilePak's other fixes
// already do for ordinary object-graph pointer fields. Returns a new
// Section 0 byte slice (headerBytes[sec0Off:sec1Off] replaced) that may be
// a different length than the original -- callers must account for the
// resulting size delta in the rest of the file (section descriptor table,
// everything at or after the old Section 1 offset). Returns the original
// bytes unchanged (nil delta) if no entry needs adjusting.
func fixChunkTableSection1Refs(headerBytes []byte, sec0Off, sec1Off uint32, shift int32, oldPoolEndRel uint32) (newSection0 []byte, sizeDelta int, err error) {
	if shift == 0 {
		return nil, 0, nil
	}
	if shift%4 != 0 {
		return nil, 0, fmt.Errorf("pool size shift %d is not a multiple of 4 -- cannot represent in the chunk-table's nibble-delta encoding (segmented-pointer patch locations are always 4-byte-aligned)", shift)
	}

	entries, err := readChunkTable(headerBytes, sec0Off)
	if err != nil {
		return nil, 0, fmt.Errorf("read chunk table: %w", err)
	}

	needsShift := func(v uint32) bool {
		segID := v >> 24
		off := v & 0xFFFFFF
		return segID == 0 && off >= oldPoolEndRel
	}

	// Fast path: nothing in this file's chunk table needs touching.
	anyAffected := false
	for _, e := range entries {
		switch {
		case chunkTypesWithSegPointerPayloads[e.Type]:
			values, derr := decodeChunkPatchValues(headerBytes[e.PayloadStart():e.PayloadEnd()], e.Count)
			if derr != nil {
				return nil, 0, fmt.Errorf("decode chunk type %d @0x%X: %w", e.Type, e.AbsOffset, derr)
			}
			for _, v := range values {
				if needsShift(v) {
					anyAffected = true
					break
				}
			}
		case chunkTypesWithRawPointerPayloads[e.Type]:
			payload := headerBytes[e.PayloadStart():e.PayloadEnd()]
			for i := uint32(0); i+4 <= uint32(len(payload)); i += 4 {
				if needsShift(binary.LittleEndian.Uint32(payload[i:])) {
					anyAffected = true
					break
				}
			}
		}
		if anyAffected {
			break
		}
	}
	if !anyAffected {
		return nil, 0, nil
	}

	out := make([]byte, 0, int(sec1Off-sec0Off)+64)
	out = append(out, headerBytes[sec0Off:entries[0].AbsOffset]...) // section-0 header + class dir + string chunk, unchanged

	for _, e := range entries {
		if chunkTypesWithRawPointerPayloads[e.Type] {
			// Fixed-size raw uint32 pointers -- patch in place, no
			// resizing (unlike the nibble-encoded types, a shifted value
			// never changes byte length here).
			payload := append([]byte(nil), headerBytes[e.PayloadStart():e.PayloadEnd()]...)
			for i := uint32(0); i+4 <= uint32(len(payload)); i += 4 {
				v := binary.LittleEndian.Uint32(payload[i:])
				if needsShift(v) {
					segID := v >> 24
					off := v & 0xFFFFFF
					newOff := uint32(int64(off) + int64(shift))
					if newOff > 0xFFFFFF {
						return nil, 0, fmt.Errorf("chunk type %d @0x%X: shifted offset 0x%X overflows 24 bits", e.Type, e.AbsOffset, newOff)
					}
					binary.LittleEndian.PutUint32(payload[i:], (segID<<24)|newOff)
				}
			}
			out = append(out, headerBytes[e.AbsOffset:e.PayloadStart()]...)
			out = append(out, payload...)
			continue
		}
		if !chunkTypesWithSegPointerPayloads[e.Type] {
			out = append(out, headerBytes[e.AbsOffset:e.PayloadEnd()]...)
			continue
		}
		values, derr := decodeChunkPatchValues(headerBytes[e.PayloadStart():e.PayloadEnd()], e.Count)
		if derr != nil {
			return nil, 0, fmt.Errorf("decode chunk type %d @0x%X: %w", e.Type, e.AbsOffset, derr)
		}
		changed := false
		for i, v := range values {
			if needsShift(v) {
				segID := v >> 24
				off := v & 0xFFFFFF
				newOff := uint32(int64(off) + int64(shift))
				if newOff > 0xFFFFFF {
					return nil, 0, fmt.Errorf("chunk type %d entry %d: shifted offset 0x%X overflows 24 bits", e.Type, i, newOff)
				}
				values[i] = (segID << 24) | newOff
				changed = true
			}
		}
		if !changed {
			out = append(out, headerBytes[e.AbsOffset:e.PayloadEnd()]...)
			continue
		}
		newPayload, eerr := encodeChunkPatchValues(values)
		if eerr != nil {
			return nil, 0, fmt.Errorf("re-encode chunk type %d @0x%X: %w", e.Type, e.AbsOffset, eerr)
		}
		newStride := uint32(0x18 + len(newPayload))
		newHeader := make([]byte, 0x18)
		copy(newHeader, headerBytes[e.AbsOffset:e.AbsOffset+0x18])
		binary.LittleEndian.PutUint32(newHeader[0x10:], newStride)
		out = append(out, newHeader...)
		out = append(out, newPayload...)
	}

	sizeDelta = len(out) - int(sec1Off-sec0Off)
	return out, sizeDelta, nil
}
