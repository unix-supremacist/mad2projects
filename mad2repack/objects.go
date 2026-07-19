package main

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"sort"

	"mad2iga"
)

// ObjectDump is the JSON shape written by `objects-dump`.
type ObjectDump struct {
	SectionCount   int               `json:"section_count"`
	ClassCount     int               `json:"class_count"`
	TotalObjects   int               `json:"total_objects"`
	ClassHistogram map[string]int    `json:"class_histogram"`
	Objects        []ObjectDumpEntry `json:"objects"`
}

type ObjectDumpEntry struct {
	Addr      uint32                 `json:"addr"`
	ClassIdx  int                    `json:"class_idx"`
	ClassName string                 `json:"class_name"`
	Fields    map[string]interface{} `json:"fields,omitempty"`
}

// isFiniteJSONValue rejects NaN/Inf floats (single or, for a Vec3f field,
// any of its 3 components) — these aren't valid JSON and would otherwise
// abort the whole dump on encode.
func isFiniteJSONValue(v interface{}) bool {
	switch x := v.(type) {
	case float32:
		return !math.IsNaN(float64(x)) && !math.IsInf(float64(x), 0)
	case [3]float32:
		for _, f := range x {
			if math.IsNaN(float64(f)) || math.IsInf(float64(f), 0) {
				return false
			}
		}
		return true
	case [16]float32:
		for _, f := range x {
			if math.IsNaN(float64(f)) || math.IsInf(float64(f), 0) {
				return false
			}
		}
		return true
	default:
		return true
	}
}

// cmdObjectsDump walks the real Section 1 object graph (mad2iga.ObjectGraph)
// rather than scanning for coordinate-shaped byte patterns like `level-dump`
// does. See docs/IGZ_FORMAT.md and docs/CLASS_SCHEMA.md for what this is
// and isn't (yet) able to reach. Every object listed here is confirmed
// structurally reachable from the file's own top-level list / igGroup
// child-list chain, not a heuristic guess. Known-schema fields (from the
// Wii-demo-derived class schema — see mad2iga/schema.go) are included where
// available; instance *names* are deliberately not resolved here (that
// heuristic has a long history of being unreliable — see
// docs/CLASS_SCHEMA.md's GeneratorInfo section for why).
func cmdObjectsDump(bldPath, jsonPath string) error {
	data, err := os.ReadFile(bldPath)
	if err != nil {
		return fmt.Errorf("read level.bld: %w", err)
	}

	graph, err := iga.ParseObjectGraph(data)
	if err != nil {
		return fmt.Errorf("parse object graph: %w", err)
	}

	objects, err := graph.FullWalk()
	if err != nil {
		return fmt.Errorf("walk object graph: %w", err)
	}

	dump := ObjectDump{
		SectionCount:   len(graph.Sections),
		ClassCount:     len(graph.ClassNames),
		TotalObjects:   len(objects),
		ClassHistogram: make(map[string]int),
		Objects:        make([]ObjectDumpEntry, 0, len(objects)),
	}

	for _, obj := range objects {
		name := obj.ClassName
		if name == "" {
			name = fmt.Sprintf("<unknown:%d>", obj.ClassIdx)
		}
		dump.ClassHistogram[name]++

		entry := ObjectDumpEntry{
			Addr:      obj.Addr,
			ClassIdx:  obj.ClassIdx,
			ClassName: name,
		}
		if schema, ok := iga.ClassSchema(name); ok {
			entry.Fields = make(map[string]interface{}, len(schema))
			for fieldName := range schema {
				v, ok := graph.FieldValue(obj, fieldName)
				if !ok {
					continue
				}
				// A schema-known field can still land on garbage if this
				// particular object's data doesn't actually match its
				// class's usual shape (e.g. a variable-size trailing
				// array pushing later fields around) — non-finite floats
				// aren't valid JSON, so drop rather than corrupt the file.
				if !isFiniteJSONValue(v) {
					continue
				}
				entry.Fields[fieldName] = v
			}
		}
		dump.Objects = append(dump.Objects, entry)
	}

	sort.Slice(dump.Objects, func(i, j int) bool { return dump.Objects[i].Addr < dump.Objects[j].Addr })

	f, err := os.Create(jsonPath)
	if err != nil {
		return fmt.Errorf("create output: %w", err)
	}
	defer f.Close()

	enc := json.NewEncoder(f)
	enc.SetIndent("", "  ")
	if err := enc.Encode(dump); err != nil {
		return fmt.Errorf("write json: %w", err)
	}

	fmt.Printf("  %d sections, %d classes, %d objects reached (%d distinct classes)\n",
		dump.SectionCount, dump.ClassCount, dump.TotalObjects, len(dump.ClassHistogram))
	return nil
}
