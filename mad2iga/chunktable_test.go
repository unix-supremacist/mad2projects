package iga

import (
	"bytes"
	"encoding/json"
	"os"
	"testing"
)

const testPakPath = "/home/unix/src/mad2modloader/mad2/Content/Streams/win/VolcanoRave_unpacked/ENGLISH.pak"

func loadTestPak(t *testing.T) []byte {
	t.Helper()
	data, err := os.ReadFile(testPakPath)
	if err != nil {
		t.Skipf("test fixture not available: %v", err)
	}
	return data
}

// TestChunkTableDecodeMatchesKnownPoolEnd validates the nibble-delta
// decoder against a real, independently-derived ground-truth value: this
// document's own StringPoolStart/StringPoolEnd computation should appear
// verbatim as one of chunk 5's decoded entries (see docs/IGA_FORMAT.md's
// chunk-table writeup for why this specific cross-check is meaningful,
// not coincidental -- it's the same value a live-debugger session
// independently observed the real engine compute at a captured crash).
func TestChunkTableDecodeMatchesKnownPoolEnd(t *testing.T) {
	data := loadTestPak(t)

	var buf bytes.Buffer
	if err := DumpPak(testPakPath, &buf); err != nil {
		t.Fatalf("DumpPak: %v", err)
	}
	var meta PakMeta
	if err := json.Unmarshal(buf.Bytes(), &meta); err != nil {
		t.Fatalf("unmarshal PakMeta: %v", err)
	}

	graph, err := ParseObjectGraph(data)
	if err != nil {
		t.Fatalf("ParseObjectGraph: %v", err)
	}
	sec0Off := graph.Sections[0].Offset
	sec1Off := graph.Sec1Off
	poolEndRel := meta.StringPoolEnd - sec1Off

	entries, err := readChunkTable(data, sec0Off)
	if err != nil {
		t.Fatalf("readChunkTable: %v", err)
	}

	found := false
	for _, e := range entries {
		if e.Type != 5 {
			continue
		}
		values, derr := decodeChunkPatchValues(data[e.PayloadStart():e.PayloadEnd()], e.Count)
		if derr != nil {
			t.Fatalf("decodeChunkPatchValues(type 5): %v", derr)
		}
		for _, v := range values {
			if v&0xFFFFFF == poolEndRel && v>>24 == 0 {
				found = true
			}
		}
	}
	if !found {
		t.Fatalf("expected chunk type 5 to contain an entry decoding to StringPoolEnd (Section-1-relative 0x%X), none found", poolEndRel)
	}
}

// TestChunkTableEncodeDecodeRoundTrip verifies encodeChunkPatchValues is a
// true inverse of decodeChunkPatchValues for every real chunk-4/5/6
// payload in the test file, both unmodified and with a synthetic shift
// applied to every entry past a cut point (simulating what
// fixChunkTableSection1Refs does for real).
func TestChunkTableEncodeDecodeRoundTrip(t *testing.T) {
	data := loadTestPak(t)
	graph, err := ParseObjectGraph(data)
	if err != nil {
		t.Fatalf("ParseObjectGraph: %v", err)
	}
	entries, err := readChunkTable(data, graph.Sections[0].Offset)
	if err != nil {
		t.Fatalf("readChunkTable: %v", err)
	}

	tested := 0
	for _, e := range entries {
		if !chunkTypesWithSegPointerPayloads[e.Type] {
			continue
		}
		payload := data[e.PayloadStart():e.PayloadEnd()]
		values, derr := decodeChunkPatchValues(payload, e.Count)
		if derr != nil {
			t.Fatalf("decode type %d: %v", e.Type, derr)
		}

		// Unmodified round trip.
		reenc, eerr := encodeChunkPatchValues(values)
		if eerr != nil {
			t.Fatalf("encode type %d: %v", e.Type, eerr)
		}
		redecoded, derr2 := decodeChunkPatchValues(reenc, e.Count)
		if derr2 != nil {
			t.Fatalf("re-decode type %d: %v", e.Type, derr2)
		}
		for i := range values {
			if values[i] != redecoded[i] {
				t.Fatalf("type %d entry %d: round-trip mismatch, got 0x%X want 0x%X", e.Type, i, redecoded[i], values[i])
			}
		}

		// Shifted round trip: add a multiple-of-4 shift to every entry at
		// or past the midpoint value, matching the real fix's shape (a
		// single growth point, everything after it moves uniformly).
		mid := values[len(values)/2]
		shifted := make([]uint32, len(values))
		copy(shifted, values)
		const testShift = 3536 // matches this session's own real test edit's shift
		for i, v := range shifted {
			if v >= mid {
				shifted[i] = v + testShift
			}
		}
		reenc2, eerr2 := encodeChunkPatchValues(shifted)
		if eerr2 != nil {
			t.Fatalf("encode shifted type %d: %v", e.Type, eerr2)
		}
		redecoded2, derr3 := decodeChunkPatchValues(reenc2, e.Count)
		if derr3 != nil {
			t.Fatalf("re-decode shifted type %d: %v", e.Type, derr3)
		}
		for i := range shifted {
			if shifted[i] != redecoded2[i] {
				t.Fatalf("shifted type %d entry %d: round-trip mismatch, got 0x%X want 0x%X", e.Type, i, redecoded2[i], shifted[i])
			}
		}
		tested++
	}
	if tested == 0 {
		t.Fatal("no chunk-4/5/6 entries found in test file -- test is not exercising anything")
	}
}
