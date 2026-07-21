package main

import (
	"encoding/json"
	"fmt"
	"os"

	"mad2iga"
)

// editsFile is the minimal input format for `objects-compile`: which object
// (identified the same way objects-dump's own output addresses one — its
// absolute file "addr"), which field, and what new value. Deliberately not
// objects-dump's own full per-object dump shape — that's a read format with
// dozens of fields per object; an edit only ever needs to name a handful.
//
// class_name is optional but, if present, is cross-checked against the
// object actually found at addr — a cheap sanity check that the addr wasn't
// copied from the wrong dump/level.
type editsFile struct {
	Edits []fieldEdit `json:"edits"`
}

type fieldEdit struct {
	Addr      uint32          `json:"addr"`
	ClassName string          `json:"class_name,omitempty"`
	Field     string          `json:"field"`
	Value     json.RawMessage `json:"value"`
}

// cmdObjectsCompile applies a small set of field edits to a real level.bld,
// in place, byte-exact — see mad2iga.ObjectGraph.WriteField's doc comment
// for exactly which fields are writable and why (PC-verified "float" fields
// only). Every other byte of the file is left untouched: IGZ objects are
// fixed-size, so a scalar/Vec3f field overwrite never needs to touch the
// section table, other objects, or any pointer.
func cmdObjectsCompile(bldPath, editsPath, outPath string) error {
	data, err := os.ReadFile(bldPath)
	if err != nil {
		return fmt.Errorf("read level.bld: %w", err)
	}

	ef, err := os.ReadFile(editsPath)
	if err != nil {
		return fmt.Errorf("read edits json: %w", err)
	}
	var edits editsFile
	if err := json.Unmarshal(ef, &edits); err != nil {
		return fmt.Errorf("parse edits json: %w", err)
	}
	if len(edits.Edits) == 0 {
		return fmt.Errorf("edits json has no \"edits\" entries")
	}

	// data is our own private buffer (fresh from os.ReadFile, not shared
	// with the original file on disk) — ParseObjectGraph doesn't copy it,
	// so writes to graph.Data below mutate exactly this slice, and nothing
	// else, before it's written out to outPath.
	graph, err := iga.ParseObjectGraph(data)
	if err != nil {
		return fmt.Errorf("parse object graph: %w", err)
	}

	for i, e := range edits.Edits {
		obj, ok := graph.ObjectAt(e.Addr)
		if !ok {
			return fmt.Errorf("edit %d: addr 0x%X (%d) is out of bounds", i, e.Addr, e.Addr)
		}
		if e.ClassName != "" && obj.ClassName != e.ClassName {
			return fmt.Errorf("edit %d: addr 0x%X is a %q, not %q — check the addr came from the level you think it did",
				i, e.Addr, obj.ClassName, e.ClassName)
		}
		if e.Field == "" {
			return fmt.Errorf("edit %d: missing \"field\"", i)
		}

		// Decide the Go type to decode Value as from the schema, mirroring
		// FieldValue's own read-side dispatch (see objgraph.go) so a
		// scalar-float field gets a scalar and a Vec3f field gets a triple.
		schema, ok := iga.ClassSchema(obj.ClassName)
		if !ok {
			return fmt.Errorf("edit %d: no known schema for class %q", i, obj.ClassName)
		}
		info, ok := schema[e.Field]
		if !ok {
			return fmt.Errorf("edit %d: class %q has no known field %q", i, obj.ClassName, e.Field)
		}

		var value interface{}
		if _, _, isVec3 := info.Vec3Offset(); isVec3 {
			var v3 [3]float32
			if err := json.Unmarshal(e.Value, &v3); err != nil {
				return fmt.Errorf("edit %d: field %s.%s expects a 3-element float array, got %q: %w", i, obj.ClassName, e.Field, e.Value, err)
			}
			value = v3
		} else {
			var f32 float32
			if err := json.Unmarshal(e.Value, &f32); err != nil {
				return fmt.Errorf("edit %d: field %s.%s expects a float, got %q: %w", i, obj.ClassName, e.Field, e.Value, err)
			}
			value = f32
		}

		// Read the old value first (best-effort) so the log line below can
		// show a before/after, then apply the write.
		oldValue, _ := graph.FieldValue(obj, e.Field)

		addr, n, err := graph.WriteField(obj, e.Field, value)
		if err != nil {
			return fmt.Errorf("edit %d: %w", i, err)
		}
		fmt.Printf("  edit %d: %s@0x%X.%s: %v -> %v (%d bytes at absolute offset 0x%X)\n",
			i, obj.ClassName, obj.Addr, e.Field, oldValue, value, n, addr)
	}

	if err := os.WriteFile(outPath, graph.Data, 0644); err != nil {
		return fmt.Errorf("write output: %w", err)
	}
	fmt.Printf("  wrote %s (%d edits applied)\n", outPath, len(edits.Edits))
	return nil
}
