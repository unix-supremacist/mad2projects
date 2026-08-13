package iga

import (
	"sort"
	"strconv"
)

// objtree.go builds a generic, human-navigable view of the IGZ object graph —
// the same "walk from the root, print every object with its fields and follow
// every reference" model MaxStache/madagascar-2-tools' myscirpt.py renders as
// level.txt, reimplemented in Go on top of the authoritative fixup tables
// (ROFS pointers / RSTR names / TSTR strings — see objgraph.go). It is
// type-agnostic: which words are pointers vs. name indices vs. scalars is
// decided by the fixup tables, not by a per-field metafield type (which our
// live reflection schema doesn't carry), so it works for every class the
// schema knows a field layout for. Shared by mad2repack's `level-tree` text
// dumper and the mad2leveleditor GUI.

// FieldEntry is one displayable field of an object.
type FieldEntry struct {
	Name   string
	Offset int
	// Kind is a coarse display/edit category derived from the fixup tables
	// first, then the schema: "ptr", "string", "float", "vec3f", "matrix44f",
	// or "raw" (an unclassified 4-byte word, shown as an integer).
	Kind string
	// Value holds the resolved value: string (for "string"), float32
	// ("float"), [3]float32 ("vec3f"), [16]float32 ("matrix44f"), *ObjectRef
	// (for a "ptr" that resolved to a real object; nil for a null/dangling
	// pointer), or uint32 ("raw").
	Value interface{}
	// Editable is true iff WriteField would accept a new value for this field
	// (a PC-verified "float"/Vec3f field — see WriteField's doc comment).
	Editable bool
}

// LabeledChild is one child object reference reached from a parent, tagged
// with the field or list index it was reached through.
type LabeledChild struct {
	Label string // e.g. "_sceneInfo" for a pointer field, "[3]" for a list element
	Child ObjectRef
}

// ObjectFields returns obj's fields in offset order, each classified and
// resolved via the authoritative fixup tables (falling back to the schema
// kind for plain scalars). Returns nil if the class has no known schema.
func (g *ObjectGraph) ObjectFields(obj ObjectRef) []FieldEntry {
	fields, ok := ClassSchema(obj.ClassName)
	if !ok {
		return nil
	}
	type nf struct {
		name string
		info FieldInfo
		off  int
		has  bool
	}
	ordered := make([]nf, 0, len(fields))
	for name, info := range fields {
		off, has := info.FieldOffset()
		ordered = append(ordered, nf{name, info, off, has})
	}
	sort.Slice(ordered, func(i, j int) bool {
		if ordered[i].has != ordered[j].has {
			return ordered[i].has // fields with a clean offset first
		}
		if ordered[i].off != ordered[j].off {
			return ordered[i].off < ordered[j].off
		}
		return ordered[i].name < ordered[j].name
	})

	out := make([]FieldEntry, 0, len(ordered))
	for _, f := range ordered {
		e := FieldEntry{Name: f.name, Offset: f.off}
		kind := ""
		if len(f.info.Kind) == 1 {
			kind = f.info.Kind[0]
		}

		if !f.has {
			// No single clean offset (e.g. a Vec3f split across accessors).
			if v, ok := g.FieldValue(obj, f.name); ok {
				e.Value = v
				if _, ok := v.([3]float32); ok {
					e.Kind = "vec3f"
					e.Editable = f.info.Source == "pc"
				} else {
					e.Kind = kindOrRaw(kind)
				}
				out = append(out, e)
			}
			continue
		}

		addr := obj.Addr + uint32(f.off)
		if child, isPtr, isNull := g.FollowPointer(addr); isPtr {
			e.Kind = "ptr"
			if !isNull {
				if o, ok := g.ObjectAt(child); ok {
					co := o
					e.Value = &co
				}
			}
			out = append(out, e)
			continue
		}
		if s, ok := g.ResolveName(addr); ok {
			e.Kind = "string"
			e.Value = s
			out = append(out, e)
			continue
		}
		// Plain scalar — read per schema kind.
		if v, ok := g.FieldValue(obj, f.name); ok {
			e.Value = v
			switch v.(type) {
			case float32:
				e.Kind = "float"
				e.Editable = f.info.Source == "pc"
			case [16]float32:
				e.Kind = "matrix44f"
			case [3]float32:
				e.Kind = "vec3f"
				e.Editable = f.info.Source == "pc"
			default:
				e.Kind = kindOrRaw(kind)
			}
			out = append(out, e)
		}
	}
	return out
}

func kindOrRaw(kind string) string {
	if kind == "" || kind == "unknown" {
		return "raw"
	}
	return kind
}

// ChildRefs returns obj's child object references — every ROFS pointer field
// that resolves to a real object, plus, for list-like objects (a class with
// both `_count` and `_data`), each resolved element — in a stable order
// (pointer fields by offset, then list elements by index). This is what the
// tree walker recurses into.
func (g *ObjectGraph) ChildRefs(obj ObjectRef) []LabeledChild {
	var out []LabeledChild
	for _, f := range g.ObjectFields(obj) {
		if f.Kind == "ptr" {
			if cp, ok := f.Value.(*ObjectRef); ok && cp != nil {
				out = append(out, LabeledChild{Label: f.Name, Child: *cp})
			}
		}
	}
	out = append(out, g.ListElements(obj)...)
	return out
}

// ListElements resolves the elements of a list-like object (one carrying both
// a `_count` and a `_data` igMemoryRef field, per the serialized layout the
// reference reader uses: _data word 0 = element count/size, word 1 = a
// segmented pointer to a `count`-long array of element pointers). Returns
// nothing for a non-list object or an unresolved/empty backing array.
func (g *ObjectGraph) ListElements(obj ObjectRef) []LabeledChild {
	fields, ok := ClassSchema(obj.ClassName)
	if !ok {
		return nil
	}
	countInfo, hasCount := fields["_count"]
	dataInfo, hasData := fields["_data"]
	if !hasCount || !hasData {
		return nil
	}
	countOff, ok := countInfo.FieldOffset()
	if !ok {
		return nil
	}
	dataOff, ok := dataInfo.FieldOffset()
	if !ok {
		return nil
	}
	count, ok := g.ReadUint32(obj.Addr + uint32(countOff))
	if !ok || count == 0 || count > 1_000_000 {
		return nil
	}
	ptr, ok := g.ReadUint32(obj.Addr + uint32(dataOff) + 4) // igMemoryRef: {size, ptr}
	if !ok || ptr == 0 {
		return nil
	}
	dataAbs, ok := g.ResolveSegPointer(ptr)
	if !ok {
		return nil
	}
	out := make([]LabeledChild, 0, count)
	for i := uint32(0); i < count; i++ {
		elemAddr := dataAbs + i*4
		child, isPtr, isNull := g.FollowPointer(elemAddr)
		if !isPtr || isNull {
			continue
		}
		if o, ok := g.ObjectAt(child); ok {
			out = append(out, LabeledChild{Label: "[" + strconv.Itoa(int(i)) + "]", Child: o})
		}
	}
	return out
}

// RootObjects returns the top-level object list's contents (Section 1's
// object array — see TopLevelObjects), the natural roots for a full tree walk.
func (g *ObjectGraph) RootObjects() ([]ObjectRef, error) {
	return g.TopLevelObjects()
}
