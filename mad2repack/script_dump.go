package main

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"sort"

	"mad2iga"
)

// tfbScriptContainerClasses are the TFBScriptInfo-namespace classes known to
// directly embed the generic igObjectList container shape (count@+8,
// data seg-ptr@+0x14 — see mad2iga.ObjectGraph.ReadObjectListContainer)
// rather than being an ordinary schema'd object. Confirmed empirically: a
// ScriptSet's `list` field resolves to a ScriptGroupStack whose own bytes
// parse directly as this container (no further indirection), and a
// ScriptInfo's `opList` field resolves to an OpCodeList behaving the same
// way — both verified against a real VolcanoRave/level.bld (a ScriptGroupStack
// with a real 11-element ParticleInfo group, and an OpCodeList with a real
// 1283-opcode compiled script). Not exhaustive — extend as more container
// classes are confirmed the same way.
var tfbScriptContainerClasses = map[string]bool{
	"OpCodeList":             true,
	"OpCreateVariableList":   true,
	"OpBranchList":           true,
	"ScriptGroupStack":       true,
	"ScriptObjectSortedList": true,
	"ScriptVariantList":      true,
}

// cmdScriptDump recursively renders a level.bld's TFBScriptInfo object graph
// (ScriptInfo/ScriptSet/Op* opcodes and friends) starting from every
// top-level ScriptInfo and ScriptSet object, following pointer fields via
// the PC-verified schema in mad2iga/tfbscript_pc_field_schema.json (see
// docs/SCRIPT_FORMAT.md). This is a structural dump, not a decompiler —
// it does not resolve instance names (unsolved, see docs/CLASS_SCHEMA.md's
// "Naming investigation") or interpret opcode semantics beyond class name
// and field values.
func cmdScriptDump(bldPath, jsonPath string) error {
	data, err := os.ReadFile(bldPath)
	if err != nil {
		return fmt.Errorf("read level.bld: %w", err)
	}
	graph, err := iga.ParseObjectGraph(data)
	if err != nil {
		return fmt.Errorf("parse object graph: %w", err)
	}
	top, err := graph.TopLevelObjects()
	if err != nil {
		return fmt.Errorf("read top-level objects: %w", err)
	}

	type root struct {
		Addr uint32                 `json:"addr"`
		Node map[string]interface{} `json:"node"`
	}
	var scriptInfos, scriptSets []root

	for _, obj := range top {
		switch obj.ClassName {
		case "ScriptInfo":
			scriptInfos = append(scriptInfos, root{obj.Addr, renderObject(graph, obj, make(map[uint32]bool), 0)})
		case "ScriptSet":
			scriptSets = append(scriptSets, root{obj.Addr, renderObject(graph, obj, make(map[uint32]bool), 0)})
		}
	}
	sort.Slice(scriptInfos, func(i, j int) bool { return scriptInfos[i].Addr < scriptInfos[j].Addr })
	sort.Slice(scriptSets, func(i, j int) bool { return scriptSets[i].Addr < scriptSets[j].Addr })

	out := map[string]interface{}{
		"script_info_count": len(scriptInfos),
		"script_set_count":  len(scriptSets),
		"script_infos":      scriptInfos,
		"script_sets":       scriptSets,
	}

	f, err := os.Create(jsonPath)
	if err != nil {
		return fmt.Errorf("create output: %w", err)
	}
	defer f.Close()
	enc := json.NewEncoder(f)
	enc.SetIndent("", "  ")
	if err := enc.Encode(out); err != nil {
		return fmt.Errorf("write json: %w", err)
	}

	fmt.Printf("  %d ScriptInfo, %d ScriptSet top-level roots dumped\n", len(scriptInfos), len(scriptSets))
	return nil
}

// maxRenderDepth bounds recursion through nested pointer chains (e.g. a
// Slider's anchorObj, or an OpForEach's cachedObject) — TFBScript graphs can
// point back at already-rendered objects (this IS followed again up to the
// depth cap rather than deduplicated by a visited-set cutoff, since the same
// object legitimately appearing twice via two different fields is normal;
// only a strict depth cap prevents runaway/cyclic expansion).
const maxRenderDepth = 12

// maxContainerElements caps how many elements of a container (OpCodeList
// etc.) get individually recursed into — large scripts (a real 1283-opcode
// ScriptInfo was found in VolcanoRave) are rendered in full since 1283 is
// still tractable, but this guards against any pathological/corrupt count.
const maxContainerElements = 4000

func renderObject(g *iga.ObjectGraph, obj iga.ObjectRef, visiting map[uint32]bool, depth int) map[string]interface{} {
	node := map[string]interface{}{
		"class": obj.ClassName,
		"addr":  obj.Addr,
	}
	if depth >= maxRenderDepth {
		node["truncated"] = "max depth"
		return node
	}
	if visiting[obj.Addr] {
		node["cycle"] = true
		return node
	}

	if tfbScriptContainerClasses[obj.ClassName] {
		elems, resolved, err := g.ReadObjectListContainer(obj.Addr)
		if err != nil {
			node["container_error"] = err.Error()
			return node
		}
		visiting[obj.Addr] = true
		defer delete(visiting, obj.Addr)

		n := len(elems)
		truncated := false
		if n > maxContainerElements {
			n = maxContainerElements
			truncated = true
		}
		items := make([]interface{}, 0, n)
		for i := 0; i < n; i++ {
			if !resolved[i] {
				items = append(items, map[string]interface{}{"unresolved": true})
				continue
			}
			childObj, ok := g.ObjectAt(elems[i])
			if !ok {
				items = append(items, map[string]interface{}{"invalid_addr": elems[i]})
				continue
			}
			items = append(items, renderObject(g, childObj, visiting, depth+1))
		}
		node["count"] = len(elems)
		node["elements"] = items
		if truncated {
			node["truncated"] = fmt.Sprintf("showing first %d of %d elements", maxContainerElements, len(elems))
		}
		return node
	}

	schema, ok := iga.ClassSchema(obj.ClassName)
	if !ok {
		return node
	}
	visiting[obj.Addr] = true
	defer delete(visiting, obj.Addr)

	fieldNames := make([]string, 0, len(schema))
	for name := range schema {
		fieldNames = append(fieldNames, name)
	}
	sort.Strings(fieldNames)

	fields := make(map[string]interface{}, len(fieldNames))
	for _, name := range fieldNames {
		v, ok := g.FieldValue(obj, name)
		if !ok {
			continue
		}
		if pf, isPtr := v.(iga.PointerFieldValue); isPtr {
			if !pf.Resolved {
				fields[name] = map[string]interface{}{"raw": pf.Raw, "resolved": false}
				continue
			}
			childObj, ok := g.ObjectAt(pf.ResolvedAddr)
			if !ok {
				fields[name] = map[string]interface{}{"raw": pf.Raw, "resolved": false}
				continue
			}
			fields[name] = renderObject(g, childObj, visiting, depth+1)
			continue
		}
		if !isFiniteJSONValue(v) {
			continue
		}
		fields[name] = v
	}
	if obj.ClassName == "RHSValueStack" {
		decodeRHSValueStack(g, obj, fields)
	}
	node["fields"] = fields
	return node
}

// decodeRHSValueStack applies the type-tag scheme confirmed this session
// (see docs/SCRIPT_FORMAT.md "VolcanoRave findings, round 2"): RHSValueStack's
// `type` field is *not* a resolvable object pointer despite its
// igObjectRefMetaField kind — its raw value is a small tag (0x6A confirmed
// == int32, 0x6B confirmed == float32, seen bit-exact against real literals
// like 1.0/0.25/100.0 in VolcanoRave/level.bld) that says how to reinterpret
// the sibling `value` field's raw bits. Other tags observed (0xA1, 0xA7)
// don't decode cleanly as either and are left undecoded — most likely an
// object-reference or a third value kind not yet confirmed. Re-reads `type`/
// `value` directly (rather than trusting the already-rendered `fields` map)
// since the generic ptr-rendering path above discards the raw tag once it
// resolves to *something* (usually garbage, since it isn't really a pointer).
func decodeRHSValueStack(g *iga.ObjectGraph, obj iga.ObjectRef, fields map[string]interface{}) {
	typeV, ok := g.FieldValue(obj, "type")
	if !ok {
		return
	}
	typePf, ok := typeV.(iga.PointerFieldValue)
	if !ok {
		return
	}
	valV, ok := g.FieldValue(obj, "value")
	if !ok {
		return
	}
	valRaw, ok := valV.(uint32)
	if !ok {
		return
	}
	switch typePf.Raw {
	case 0x6A:
		fields["value_decoded"] = int32(valRaw)
		fields["value_decoded_kind"] = "int"
	case 0x6B:
		fields["value_decoded"] = math.Float32frombits(valRaw)
		fields["value_decoded_kind"] = "float"
	}
}
