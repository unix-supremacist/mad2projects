package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"mad2iga"
)

// loadedLevel wraps a parsed object graph plus enough provenance to write it
// back out — either as a raw level.bld (IGZ) or repacked into the .bld
// igArchive it came from.
type loadedLevel struct {
	graph       *iga.ObjectGraph
	srcPath     string
	fromArchive bool
	memberName  string // the level.bld member's name inside the archive
	dirty       bool
}

const (
	magicIGZ = 0x49475A01 // "IGZ\x01"
	magicIGA = 0x1A414749 // "IGA\x1a"
)

// loadLevel opens either a raw level.bld (IGZ) or a .bld igArchive (extracting
// its inner level.bld) and parses the object graph.
func loadLevel(path string) (*loadedLevel, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if len(data) < 4 {
		return nil, fmt.Errorf("%s is too small", filepath.Base(path))
	}
	switch binary.LittleEndian.Uint32(data[:4]) {
	case magicIGZ:
		g, err := iga.ParseObjectGraph(data)
		if err != nil {
			return nil, err
		}
		return &loadedLevel{graph: g, srcPath: path}, nil
	case magicIGA:
		arc, err := iga.Open(path)
		if err != nil {
			return nil, err
		}
		defer arc.Close()
		idx, name := -1, ""
		for i, e := range arc.Entries {
			if strings.EqualFold(e.Name, "level.bld") || strings.HasSuffix(strings.ToLower(e.Name), "/level.bld") {
				idx, name = i, e.Name
				break
			}
		}
		if idx < 0 {
			return nil, fmt.Errorf("%s has no level.bld member", filepath.Base(path))
		}
		var buf bytes.Buffer
		if err := arc.ExtractFile(idx, &buf); err != nil {
			return nil, fmt.Errorf("extract %s: %w", name, err)
		}
		g, err := iga.ParseObjectGraph(buf.Bytes())
		if err != nil {
			return nil, err
		}
		return &loadedLevel{graph: g, srcPath: path, fromArchive: true, memberName: name}, nil
	default:
		return nil, fmt.Errorf("%s is neither an IGZ nor an igArchive", filepath.Base(path))
	}
}

// save writes the (possibly edited) level to outPath. For an archive source it
// repacks the edited level.bld back into a copy of the original archive; for a
// raw IGZ it just writes the bytes. Writing over the source archive is done via
// a temp file + rename, since RepackFromArchive reads the source while writing.
func (l *loadedLevel) save(outPath string) error {
	if !l.fromArchive {
		return os.WriteFile(outPath, l.graph.Data, 0644)
	}
	replacements := map[string][]byte{l.memberName: l.graph.Data}
	if outPath != l.srcPath {
		return iga.RepackFromArchive(l.srcPath, outPath, replacements)
	}
	tmp := outPath + ".tmp"
	if err := iga.RepackFromArchive(l.srcPath, tmp, replacements); err != nil {
		return err
	}
	return os.Rename(tmp, outPath)
}
