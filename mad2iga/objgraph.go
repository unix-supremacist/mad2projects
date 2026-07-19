package iga

import (
	"encoding/binary"
	"fmt"
	"math"
)

// Section describes one entry of an IGZ file's section table (16 bytes:
// offset, size, align, flags — see ../docs/IGZ_FORMAT.md).
type Section struct {
	Offset uint32
	Size   uint32
	Align  uint32
	Flags  uint32
}

// ObjectGraph is a parsed view of an IGZ file's section table and Section 0
// class directory, plus helpers to walk the real Section 1 object graph
// (rather than mad2iga/level.go's byte-pattern coordinate scanner). See
// ../docs/IGZ_FORMAT.md and ../docs/CLASS_SCHEMA.md for the reverse-engineering
// behind this.
type ObjectGraph struct {
	Data       []byte
	Sections   []Section
	ClassNames []string
	Sec0Off    uint32
	Sec1Off    uint32
}

// ObjectRef identifies one object instance found in the graph.
type ObjectRef struct {
	Addr      uint32 // absolute file offset of the object's own start (its classIdx word)
	ClassIdx  int
	ClassName string // "" if ClassIdx is out of range of ClassNames
}

// PointerFieldValue is what a schema field marked Kind "ptr" resolves to:
// the raw at-rest value plus, if it resolved to a real in-bounds object, the
// target's address and class name.
type PointerFieldValue struct {
	Raw          uint32 `json:"raw"`
	ResolvedAddr uint32 `json:"resolved_addr,omitempty"`
	TargetClass  string `json:"target_class,omitempty"`
	Resolved     bool   `json:"resolved"`
}

// ParseObjectGraph reads an IGZ file's section table and Section 0 class
// directory. It does not touch the string pool or Section 1's contents —
// use the ObjectGraph methods below for that.
func ParseObjectGraph(data []byte) (*ObjectGraph, error) {
	if len(data) < 0x30 {
		return nil, fmt.Errorf("file too small to be IGZ")
	}
	magic := binary.LittleEndian.Uint32(data[0:4])
	if magic != 0x49475A01 {
		return nil, fmt.Errorf("invalid IGZ magic: 0x%08X", magic)
	}

	// Section descriptors start at 0x10, 16 bytes each, contiguous
	// (section N's offset == section N-1's offset+size). Stop at the first
	// non-contiguous or all-zero entry. See ../docs/IGZ_FORMAT.md.
	var sections []Section
	off := uint32(0x10)
	var prevEnd uint32
	for i := 0; int(off)+16 <= len(data) && i < 32; i++ {
		sOff := binary.LittleEndian.Uint32(data[off:])
		sSize := binary.LittleEndian.Uint32(data[off+4:])
		sAlign := binary.LittleEndian.Uint32(data[off+8:])
		sFlags := binary.LittleEndian.Uint32(data[off+12:])
		if i > 0 && sOff != prevEnd {
			break
		}
		if sOff == 0 && sSize == 0 {
			break
		}
		sections = append(sections, Section{sOff, sSize, sAlign, sFlags})
		prevEnd = sOff + sSize
		off += 16
	}
	if len(sections) < 2 {
		return nil, fmt.Errorf("could not parse a section table (found %d sections)", len(sections))
	}

	sec0Off := sections[0].Offset
	sec1Off := sections[1].Offset

	if int(sec0Off)+0x30 > len(data) {
		return nil, fmt.Errorf("section 0 too small to hold its own header fields")
	}
	dirOff := binary.LittleEndian.Uint32(data[sec0Off+0x0C:])
	poolOff := binary.LittleEndian.Uint32(data[sec0Off+0x2C:])

	end := sec0Off + poolOff
	if end > uint32(len(data)) {
		end = uint32(len(data))
	}
	var classNames []string
	pos := sec0Off + dirOff
	for pos < end {
		if data[pos] != 0 {
			start := pos
			for pos < end && data[pos] != 0 {
				pos++
			}
			s := string(data[start:pos])
			if len(s) > 2 && s != "t\n" {
				classNames = append(classNames, s)
			}
		} else {
			pos++
		}
	}

	return &ObjectGraph{
		Data:       data,
		Sections:   sections,
		ClassNames: classNames,
		Sec0Off:    sec0Off,
		Sec1Off:    sec1Off,
	}, nil
}

// ClassIndexOf returns a class's 0-based index into ClassNames, or -1.
func (g *ObjectGraph) ClassIndexOf(name string) int {
	for i, n := range g.ClassNames {
		if n == name {
			return i
		}
	}
	return -1
}

// ClassNameOf returns a class index's name, or "" if out of range.
func (g *ObjectGraph) ClassNameOf(classIdx int) string {
	if classIdx < 0 || classIdx >= len(g.ClassNames) {
		return ""
	}
	return g.ClassNames[classIdx]
}

// ResolveSegPointer turns an at-rest segmented pointer (segID in the top
// byte, offset in the low 24 bits — see ../docs/IGZ_FORMAT.md) into an
// absolute file offset.
//
// segID == 0 is a special case, confirmed by directly cross-checking three
// of ActorInfo's real pointer fields (collisionInfo/waypoints/
// physicsParameters — see docs/CLASS_SCHEMA.md's "PC-verified fields")
// against a real level.bld: it does NOT mean literal Section 0. It means
// "relative to whatever section the pointer itself lives in" — for every
// ordinary object-to-object reference within the main object graph, that's
// Section 1, the same base TopLevelObjects() uses for the top-level object
// array's own pointers. (segID == 0 landing on Section 1 by coincidence
// here, rather than requiring the pointer's own containing section to be
// tracked explicitly, is a simplification that holds because every object
// this walker currently visits — via TopLevelObjects/WalkGroupChildren —
// lives in Section 1; it would need generalizing if a future edge is added
// that visits objects living in some other section.)
//
// segID >= 1 is left as a direct section-table index, per
// mad2assetextractor/docs/demo.md's separate (never independently
// re-verified this session) observations about cross-file segment
// references — e.g. "0x05XXXXXX for Segment 5/global Segment 1" when a
// level references global.bld. Not re-confirmed here; flagged in case it
// turns out to need the same kind of correction segID==0 just got.
func (g *ObjectGraph) ResolveSegPointer(ptr uint32) (uint32, bool) {
	segID := ptr >> 24
	offset := ptr & 0x00FFFFFF
	if segID == 0 {
		return g.Sec1Off + offset, true
	}
	if int(segID) >= len(g.Sections) {
		return 0, false
	}
	return g.Sections[segID].Offset + offset, true
}

// ReadUint32 / ReadFloat32 are bounds-checked raw field reads.
func (g *ObjectGraph) ReadUint32(addr uint32) (uint32, bool) {
	if int(addr)+4 > len(g.Data) {
		return 0, false
	}
	return binary.LittleEndian.Uint32(g.Data[addr:]), true
}

func (g *ObjectGraph) ReadFloat32(addr uint32) (float32, bool) {
	v, ok := g.ReadUint32(addr)
	if !ok {
		return 0, false
	}
	return math.Float32frombits(v), true
}

// ObjectAt reads the classIdx word at addr and resolves its class name.
func (g *ObjectGraph) ObjectAt(addr uint32) (ObjectRef, bool) {
	v, ok := g.ReadUint32(addr)
	if !ok {
		return ObjectRef{}, false
	}
	classIdx := int(v)
	return ObjectRef{Addr: addr, ClassIdx: classIdx, ClassName: g.ClassNameOf(classIdx)}, true
}

// TopLevelObjects reads Section 1's top-level igObjectList-style header
// (verified against real level.bld files — see ../docs/IGZ_FORMAT.md):
//
//	+0x0C  uint32  objectCount
//	+0x18  uint32  objArrayOff (relative to Section 1's own start)
//	+objArrayOff  objectCount x uint32, each a Section-1-relative pointer
//
// This is a real structural read, not a byte-pattern guess — cross-validated
// against mad2iga/level.go's older coordinate-scan heuristic by confirming
// they find the same real ActorInfo instances (including genuine names like
// "Coin_LandCroc_B_1") at the same addresses. Unlike the older research notes'
// assumption, gameplay objects like ActorInfo DO appear directly in this
// top-level list — they are not exclusively nested in the scene graph.
func (g *ObjectGraph) TopLevelObjects() ([]ObjectRef, error) {
	if int(g.Sec1Off)+0x1C > len(g.Data) {
		return nil, fmt.Errorf("section 1 too small to hold its own header fields")
	}
	count := binary.LittleEndian.Uint32(g.Data[g.Sec1Off+0x0C:])
	arrayOff := binary.LittleEndian.Uint32(g.Data[g.Sec1Off+0x18:])
	arrBase := g.Sec1Off + arrayOff

	if count > 1_000_000 {
		return nil, fmt.Errorf("implausible top-level object count %d", count)
	}

	objects := make([]ObjectRef, 0, count)
	for i := uint32(0); i < count; i++ {
		off := arrBase + i*4
		relPtr, ok := g.ReadUint32(off)
		if !ok {
			break
		}
		addr := g.Sec1Off + relPtr // Section-1-relative, not segmented
		obj, ok := g.ObjectAt(addr)
		if !ok {
			continue
		}
		objects = append(objects, obj)
	}
	return objects, nil
}

// igGroupChildListOffset is the byte offset of igGroup's `childList` field
// (a segmented pointer to an igNodeList). Manually decompiled and verified
// against the Wii demo build's Gap::Sg::igGroup::getChildCount/hasChildren
// (both read *(int*)(this+0x1C)) — not part of the automated
// wii_field_schema.json extraction, since neither is a ToVariant/FromVariant
// accessor. See ../docs/CLASS_SCHEMA.md.
const igGroupChildListOffset = 0x1C

// ReadObjectListContainer reads a runtime igObjectList-shaped container
// (used by igNodeList, GeneratorInfoList, and other *List classes, which all
// share this generic base — verified by decompiling
// Gap::Core::igObjectList::getCount/append/set/userResetFields):
//
//	+0x08  int32  count
//	+0x0C  int32  capacity
//	+0x14  seg ptr  data (array of count x seg-ptr elements, 4 bytes each)
//
// Returns each element resolved to an absolute file offset. An element that
// fails to resolve (e.g. a null or out-of-range segmented pointer) is
// returned as (0, false) in place rather than dropped, so callers can tell
// "N children, some unresolved" from "N children".
func (g *ObjectGraph) ReadObjectListContainer(addr uint32) ([]uint32, []bool, error) {
	countRaw, ok := g.ReadUint32(addr + 8)
	if !ok {
		return nil, nil, fmt.Errorf("address 0x%X out of bounds", addr)
	}
	count := int32(countRaw)
	if count < 0 || count > 1_000_000 {
		return nil, nil, fmt.Errorf("implausible container count %d at 0x%X", count, addr)
	}
	dataPtrRaw, ok := g.ReadUint32(addr + 0x14)
	if !ok {
		return nil, nil, fmt.Errorf("address 0x%X out of bounds", addr+0x14)
	}
	dataAbs, ok := g.ResolveSegPointer(dataPtrRaw)
	if !ok {
		return nil, nil, fmt.Errorf("container data pointer 0x%08X did not resolve", dataPtrRaw)
	}

	elems := make([]uint32, count)
	resolved := make([]bool, count)
	for i := int32(0); i < count; i++ {
		elemPtr, ok := g.ReadUint32(dataAbs + uint32(i)*4)
		if !ok {
			break
		}
		abs, ok := g.ResolveSegPointer(elemPtr)
		if ok {
			elems[i] = abs
			resolved[i] = true
		}
	}
	return elems, resolved, nil
}

// WalkGroupChildren returns the resolved, class-tagged children of an
// igGroup instance at groupAddr (via its childList -> igNodeList), one level
// deep — it does not recurse into grandchildren itself; callers wanting a
// full scene-graph walk should recurse for any child whose class is itself
// "igGroup".
func (g *ObjectGraph) WalkGroupChildren(groupAddr uint32) ([]ObjectRef, error) {
	childListPtr, ok := g.ReadUint32(groupAddr + igGroupChildListOffset)
	if !ok {
		return nil, fmt.Errorf("address 0x%X out of bounds", groupAddr+igGroupChildListOffset)
	}
	if childListPtr == 0 {
		return nil, nil
	}
	childListAddr, ok := g.ResolveSegPointer(childListPtr)
	if !ok {
		return nil, fmt.Errorf("childList pointer 0x%08X did not resolve", childListPtr)
	}
	elems, resolved, err := g.ReadObjectListContainer(childListAddr)
	if err != nil {
		return nil, err
	}
	out := make([]ObjectRef, 0, len(elems))
	for i, addr := range elems {
		if !resolved[i] {
			continue
		}
		if obj, ok := g.ObjectAt(addr); ok {
			out = append(out, obj)
		}
	}
	return out, nil
}

// FullWalk enumerates every object reachable from Section 1's top-level
// list (see TopLevelObjects), plus every descendant reachable by recursively
// following igGroup -> childList (see WalkGroupChildren), deduplicated by
// address. This is the real structural replacement for scanning the file
// for byte patterns: every object returned here is one actually reachable
// through the file's own graph structure. It's still incomplete — only
// igGroup's one known child-list edge is followed, so anything reachable
// only through other container/reference fields (e.g. GeneratorInfoList's
// generatorInfoList field, or igSceneInfo's own children) isn't included yet
// — but it covers the ordinary scene-graph nesting and is a strictly more
// reliable object list than level.go's coordinate scanner, which can only
// ever find objects that happen to contain a homogeneous-matrix-row-shaped
// float triple.
func (g *ObjectGraph) FullWalk() (map[uint32]ObjectRef, error) {
	top, err := g.TopLevelObjects()
	if err != nil {
		return nil, err
	}

	visited := make(map[uint32]ObjectRef, len(top))
	stack := make([]ObjectRef, 0, len(top))
	for _, o := range top {
		if _, ok := visited[o.Addr]; !ok {
			visited[o.Addr] = o
			stack = append(stack, o)
		}
	}

	for len(stack) > 0 {
		o := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		if o.ClassName != "igGroup" {
			continue
		}
		children, err := g.WalkGroupChildren(o.Addr)
		if err != nil {
			// Best-effort: a malformed/empty child list on one object
			// shouldn't abort the whole walk.
			continue
		}
		for _, c := range children {
			if _, ok := visited[c.Addr]; !ok {
				visited[c.Addr] = c
				stack = append(stack, c)
			}
		}
	}
	return visited, nil
}

// FieldValue reads a known field of obj using the verified Wii-derived
// schema (see schema.go / ../docs/CLASS_SCHEMA.md). Returns false if the
// class or field isn't in the schema, or the field's offset is ambiguous.
func (g *ObjectGraph) FieldValue(obj ObjectRef, fieldName string) (interface{}, bool) {
	fields, ok := ClassSchema(obj.ClassName)
	if !ok {
		return nil, false
	}
	info, ok := fields[fieldName]
	if !ok {
		return nil, false
	}
	isFloat := len(info.Kind) == 1 && info.Kind[0] == "float"
	isMatrix := len(info.Kind) == 1 && info.Kind[0] == "matrix44f"
	isPtr := len(info.Kind) == 1 && info.Kind[0] == "ptr"

	if fieldOff, ok := info.FieldOffset(); ok {
		addr := obj.Addr + uint32(fieldOff)
		if isPtr {
			raw, ok := g.ReadUint32(addr)
			if !ok {
				return nil, false
			}
			pf := PointerFieldValue{Raw: raw}
			if raw != 0 {
				if resolvedAddr, ok := g.ResolveSegPointer(raw); ok {
					if target, ok := g.ObjectAt(resolvedAddr); ok {
						pf.ResolvedAddr = resolvedAddr
						pf.TargetClass = target.ClassName
						pf.Resolved = true
					}
				}
			}
			return pf, true
		}
		if isMatrix {
			// A full 4x4 float matrix (16 floats, 64 bytes) — returned
			// whole rather than guessing which row/offset is the
			// translation, since that hasn't been independently confirmed
			// yet (see pcVerifiedFields's ActorInfo.logicMatrix comment).
			var m [16]float32
			for i := range m {
				v, ok := g.ReadFloat32(addr + uint32(i*4))
				if !ok {
					return nil, false
				}
				m[i] = v
			}
			return m, true
		}
		if isFloat {
			v, ok := g.ReadFloat32(addr)
			return v, ok
		}
		v, ok := g.ReadUint32(addr)
		return v, ok
	}

	// Not a single unambiguous offset — check whether it's a Vec3f instead
	// (see FieldInfo.Vec3Offset's doc comment).
	if isFloat {
		if base, stride, ok := info.Vec3Offset(); ok {
			x, okX := g.ReadFloat32(obj.Addr + uint32(base))
			y, okY := g.ReadFloat32(obj.Addr + uint32(base+stride))
			z, okZ := g.ReadFloat32(obj.Addr + uint32(base+2*stride))
			if okX && okY && okZ {
				return [3]float32{x, y, z}, true
			}
		}
	}
	return nil, false
}
