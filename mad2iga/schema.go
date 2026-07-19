package iga

import (
	"embed"
	"encoding/json"
	"strings"
)

//go:embed wii_field_schema.json
var wiiFieldSchemaJSON embed.FS

// FieldInfo describes one field of a class, as reverse-engineered from the
// unstripped Wii demo build (see ../docs/CLASS_SCHEMA.md). Offset is either a
// single int (the common case) or a []interface{} of ints when multiple
// accessors disagreed (e.g. overloads) — treat those as lower-confidence.
type FieldInfo struct {
	Offset    interface{} `json:"offset"`
	Kind      []string    `json:"kind"`
	Accessors []string    `json:"accessors"`
}

// wiiSchema maps a fully-qualified class path (e.g. "Gap::TFBParticleInfo::GeneratorInfo")
// to its known fields. Loaded once from the embedded JSON.
var wiiSchema map[string]map[string]FieldInfo

// wiiSchemaByBareName maps just the trailing class name (e.g. "GeneratorInfo",
// what level.bld's own class directory uses — it has no namespace info) to its
// schema entry. Ambiguous if two classes share a bare name across namespaces;
// last one loaded wins, which hasn't been a problem in practice so far (checked:
// no collisions in the current 54-class schema).
var wiiSchemaByBareName map[string]map[string]FieldInfo

func init() {
	data, err := wiiFieldSchemaJSON.ReadFile("wii_field_schema.json")
	if err != nil {
		panic("mad2iga: embedded wii_field_schema.json missing: " + err.Error())
	}
	if err := json.Unmarshal(data, &wiiSchema); err != nil {
		panic("mad2iga: embedded wii_field_schema.json invalid: " + err.Error())
	}
	wiiSchemaByBareName = make(map[string]map[string]FieldInfo, len(wiiSchema))
	for classPath, fields := range wiiSchema {
		parts := strings.Split(classPath, "::")
		bare := parts[len(parts)-1]
		wiiSchemaByBareName[bare] = fields
	}
}

// ClassSchema returns the known field schema for a class, keyed by its bare
// name as it appears in an IGZ file's own Section 0 class directory (e.g.
// "GeneratorInfo", not "Gap::TFBParticleInfo::GeneratorInfo"). Only classes
// reverse-engineered via the Wii demo build's debug symbols are covered —
// see docs/CLASS_SCHEMA.md for what's covered and how to extend it.
func ClassSchema(bareName string) (map[string]FieldInfo, bool) {
	fields, ok := wiiSchemaByBareName[bareName]
	return fields, ok
}

// FieldOffset returns a field's byte offset if it's unambiguous (a single
// int, not a list of conflicting candidates from disagreeing accessors).
func (f FieldInfo) FieldOffset() (int, bool) {
	switch v := f.Offset.(type) {
	case float64:
		return int(v), true
	default:
		return 0, false
	}
}
