package iga

import (
	"embed"
	"encoding/json"
	"strings"
)

// ⚠️ These offsets come from the Wii demo build (PowerPC compile) and are
// NOT confirmed to match the PC build's struct layout — see
// ../docs/CLASS_SCHEMA.md's "Major caveat" note. Directly disproved for at
// least one field so far (ActorInfo.destination reads as garbage on a real
// PC level.bld; the actual PC-verified translation offset, from an
// unrelated pre-existing PC-native heuristic, is ActorInfo+80, nowhere near
// any offset this schema records for ActorInfo). Treat every value here as
// "known on Wii" only until independently checked against a PC binary.
//
//go:embed wii_field_schema.json
var wiiFieldSchemaJSON embed.FS

// tfbScriptPcFieldSchemaJSON holds PC-verified field offsets for the entire
// TFBScriptInfo namespace (ScriptSet/ScriptInfo/Op*/Value*/Placement/etc —
// the level-logic scripting system), extracted directly from
// ScriptInfoLib.dll's own `arkRegisterInitialize@<Class>@TFBScriptInfo@Gap`
// bulk field-registration functions (one per class, the same mechanism
// that cracked ActorInfo's schema — see docs/CLASS_SCHEMA.md's
// arkRegisterInitialize finding, and docs/SCRIPT_FORMAT.md for the
// namespace's semantic map this now has real offsets for). Unlike
// wii_field_schema.json this is PC-native from the start, not a Wii port —
// every entry here is "source":"pc". See mad2iga/tfbscript_pc_field_schema.json.
//
//go:embed tfbscript_pc_field_schema.json
var tfbScriptPcFieldSchemaJSON embed.FS

// liveReflectionFieldSchemaJSON holds a COMPLETE field schema (own +
// inherited, every field's real declaring class) for every one of the
// 1392 classes the Alchemy engine's own reflection system (igArkCore)
// registers at runtime — not decompiled/guessed per class like every
// other source in this file, but read directly out of the running game
// process by mad2metadumper.dll (F4 dumps mad2/logs/mad2metadump.txt,
// converted to this JSON — see mad2iga/live_reflection_dump.tsv for the
// raw dump and mad2metadumper/src/metadumper.cpp for exactly how each
// offset was resolved and PC-verified). This supersedes the long-standing
// "10-20% field coverage, no inheritance" limitation docs/SCRIPT_FORMAT.md
// flagged for the TFBScriptInfo namespace specifically — and, since the
// dump covers literally every registered class, extends real field
// coverage to every OTHER namespace too (ActorInfo alone goes from ~15
// hand-verified fields to 49). Field names here are the engine's own
// literal names (e.g. "_LHS", "_internalFlags", underscore-prefixed) —
// deliberately NOT reconciled with the older hand-extracted schemas'
// non-underscored key spelling (e.g. tfbScriptPcSchema's "LHS"), so this
// layer only ever *adds* keys, never silently shadows a specifically
// hand-verified entry elsewhere in this file. kind is always ["unknown"]
// (the dump captures name/offset/owning-class only, not each field's real
// igXxxMetaField subtype) — extending mad2metadumper.cpp to also resolve
// kind via each field's own vtable is a natural, scoped follow-up.
//
//go:embed live_reflection_schema.json
var liveReflectionFieldSchemaJSON embed.FS

// FieldInfo describes one field of a class. Offset is either a single int
// (the common case) or a []interface{} of ints when multiple accessors
// disagreed (e.g. overloads) — treat those as lower-confidence, except see
// Vec3Offset below. Source records where this particular entry's data came
// from — "wii" (unconfirmed on PC, see the caveat below) or "pc" (confirmed
// by decompiling the real PC DLL, e.g. ActorInfoLib.dll — see
// ../docs/CLASS_SCHEMA.md's "PC-verified fields" section).
type FieldInfo struct {
	Offset    interface{} `json:"offset"`
	Kind      []string    `json:"kind"`
	Accessors []string    `json:"accessors"`
	Source    string      `json:"source,omitempty"`
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

// tfbScriptPcSchema maps a TFBScriptInfo-namespace bare class name (e.g.
// "ScriptSet", "OpSetValue") directly to its PC-verified fields — see
// tfbScriptPcFieldSchemaJSON above. Already keyed by bare name (the
// extraction script only ever saw the bare class name), unlike wiiSchema
// which needs the namespace-stripping step.
var tfbScriptPcSchema map[string]map[string]FieldInfo

// liveReflectionSchema maps a bare class name directly to its complete
// live-dumped field set — see liveReflectionFieldSchemaJSON above.
var liveReflectionSchema map[string]map[string]FieldInfo

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
		for name, info := range fields {
			info.Source = "wii"
			fields[name] = info
		}
		wiiSchemaByBareName[bare] = fields
	}

	tfbData, err := tfbScriptPcFieldSchemaJSON.ReadFile("tfbscript_pc_field_schema.json")
	if err != nil {
		panic("mad2iga: embedded tfbscript_pc_field_schema.json missing: " + err.Error())
	}
	if err := json.Unmarshal(tfbData, &tfbScriptPcSchema); err != nil {
		panic("mad2iga: embedded tfbscript_pc_field_schema.json invalid: " + err.Error())
	}

	liveData, err := liveReflectionFieldSchemaJSON.ReadFile("live_reflection_schema.json")
	if err != nil {
		panic("mad2iga: embedded live_reflection_schema.json missing: " + err.Error())
	}
	if err := json.Unmarshal(liveData, &liveReflectionSchema); err != nil {
		panic("mad2iga: embedded live_reflection_schema.json invalid: " + err.Error())
	}
}

// pcVerifiedFields holds fields confirmed by directly decompiling the real
// PC DLLs (ActorInfoLib.dll etc., not the Wii ELF) — see
// ../docs/CLASS_SCHEMA.md's "PC-verified fields" section for the
// decompiled evidence behind each entry. Small and hand-curated so far;
// growing this the same systematic way extract_schema.py did for the Wii
// ELF (an x86-equivalent pattern-matcher over decompiled/disassembled
// accessor bodies in each *InfoLib.dll) is the natural next step, not yet
// done.
var pcVerifiedFields = map[string]map[string]FieldInfo{
	"ActorInfo": {
		// The real, file-serialized world transform: a row-major 4x4 matrix
		// with translation-in-last-row convention. Directly confirmed
		// against a real coin (WaterholeBLD, ActorInfo @0x790c70): rows 0-2
		// read as an exact identity rotation/scale ((1,0,0,0)/(0,1,0,0)/
		// (0,0,1,0)), row 3 reads (74.747, 6.370, -59.887, 1.0) — exactly
		// level.go's independently-derived, already-working translation
		// value at that object's +80, which is `worldMatrixBase + 3*16`
		// (row 3 of a matrix based at +32). `position` below extracts that
		// row directly so consumers don't have to.
		"worldMatrix": {Offset: float64(0x20), Kind: []string{"matrix44f"}, Source: "pc",
			Accessors: []string{"cross-checked against level.go's independently-derived ActorInfo:+80 translation offset; see docs/CLASS_SCHEMA.md"}},
		"position": {Offset: []interface{}{float64(0x50), float64(0x54), float64(0x58)}, Kind: []string{"float"}, Source: "pc",
			Accessors: []string{"row 3 (translation) of worldMatrix above, i.e. +32 + 3*16"}},
		// AlchemyCommonLib::TFBTransform's local igMatrix44f, used ONLY when
		// ActorInfo has no external transform delegate (see logicMatrix's
		// null check on the +0x160 delegate pointer) — NOT the same as
		// worldMatrix/position above. Confirmed genuinely unused/all-zero in
		// every level-authored ActorInfo checked so far (runtime-only
		// scratch storage, never meaningfully serialized) — kept in the
		// schema mainly as a documented dead end so nobody re-investigates
		// it as a position candidate.
		"logicMatrix": {Offset: float64(0x60), Kind: []string{"matrix44f"}, Source: "pc",
			Accessors: []string{"get:logicMatrix@0x10001f80 (ActorInfoLib.dll)"}},
		// `destination` is a Vec3f, but per getDestinationToVariant's
		// PositionMeasurement Meta type and setDestination's extra int
		// parameter (turnTo-style movement request, not raw storage) this
		// is an AI movement TARGET, not the object's resting position —
		// do not use this for "where is this object" queries.
		"destination": {Offset: []interface{}{float64(0x150), float64(0x154), float64(0x158)}, Kind: []string{"float"}, Source: "pc",
			Accessors: []string{"get:getDestination@0x100015b0 (ActorInfoLib.dll)", "set:setDestination@0x100015c0 (ActorInfoLib.dll)"}},
		// Kind "ptr" (not "int"): confirmed to be a Section-1-relative
		// pointer to another real object, not a plain integer — verified by
		// resolving each of these three raw values (via ResolveSegPointer,
		// after fixing its segID==0 handling — see that function's comment)
		// against a real level.bld and getting exactly the expected class
		// on the other end (CollisionInfo, ActorWaypointList,
		// ActorParameters respectively).
		"controller":    {Offset: float64(0x140), Kind: []string{"ptr"}, Source: "pc", Accessors: []string{"get:getControllerToVariant@0x10001b80 (ActorInfoLib.dll)"}},
		"collisionInfo": {Offset: float64(0x120), Kind: []string{"ptr"}, Source: "pc", Accessors: []string{"get:getCollisionInfoToVariant@0x10001c90 (ActorInfoLib.dll)"}},
		"waypoints":     {Offset: float64(0x14c), Kind: []string{"ptr"}, Source: "pc", Accessors: []string{"get:getWaypointsToVariant@0x10002700 (ActorInfoLib.dll)"}},
		// physicsParameters and vehicleParameters are BOTH just type-checked
		// views of one shared pointer field (confirmed: both
		// getPhysicsParametersToVariant@0x10001c10 and
		// getVehicleParametersToVariant@0x10001c50 read *(this+0x148), then
		// return it only if isOfType() matches their respective expected
		// type, else 0) — not two separate fields, matching the Wii
		// schema's own otherwise-odd choice to give them the same offset
		// (308 there), just at the wrong absolute number for PC.
		"physicsParameters": {Offset: float64(0x148), Kind: []string{"ptr"}, Source: "pc", Accessors: []string{"get:getPhysicsParametersToVariant@0x10001c10 (ActorInfoLib.dll)"}},
		"vehicleParameters": {Offset: float64(0x148), Kind: []string{"ptr"}, Source: "pc", Accessors: []string{"get:getVehicleParametersToVariant@0x10001c50 (ActorInfoLib.dll)"}},
		// velocity is NOT a direct field at all: getVelocity returns
		// *(this+0x160) + 0x58 when the +0x160 delegate pointer is
		// non-null, i.e. it's stored on a separate delegate object, not
		// inline in ActorInfo. Recorded with a nil offset (skipped by
		// FieldOffset/Vec3Offset, so it simply won't appear in dumps) to
		// document the finding without producing a misleading value.
		"velocity": {Offset: nil, Kind: []string{"indirect"}, Source: "pc",
			Accessors: []string{"get:getVelocity@0x10001de0 (ActorInfoLib.dll): *(this+0x160)+0x58, this is not a plain field"}},
	},
}

// pcDisprovenFields lists fields the Wii schema recorded for a class that
// PC-side decompilation showed are not real per-instance fields at all
// (rather than just being at a different offset) — removed entirely rather
// than merely re-pointed, so callers don't get a plausible-looking but
// meaningless value.
var pcDisprovenFields = map[string][]string{
	// getPlayerSetToVariant/getActiveSetToVariant (ActorInfoLib.dll
	// 0x10001d00/0x10001d20) don't even read `this` — they read a shared
	// global (`__interface_ActorInfo_..._igSmartPointer_...`) at a fixed
	// offset off of it. Every ActorInfo instance in a level necessarily
	// reads the same value, which is exactly the "identical across all
	// instances" anomaly first noticed in objects-dump's output — not
	// wrong per-instance offsets, but genuinely non-instance data.
	// inactiveSet/removedSet were never independently checked but are the
	// same accessor family (getInactiveSetToVariant/getRemovedSetToVariant)
	// and almost certainly share this shape; excluded on that basis too.
	"ActorInfo": {"playerSet", "activeSet", "inactiveSet", "removedSet"},
}

// ClassSchema returns the known field schema for a class, keyed by its bare
// name as it appears in an IGZ file's own Section 0 class directory (e.g.
// "GeneratorInfo", not "Gap::TFBParticleInfo::GeneratorInfo"). Merges the
// Wii-derived schema with any PC-verified overrides/removals for that class
// (see pcVerifiedFields/pcDisprovenFields above) — PC data always wins where
// both exist. See docs/CLASS_SCHEMA.md for what's covered and how to extend
// it, and its "Major caveat" section for why Wii-only entries need this
// treatment at all.
func ClassSchema(bareName string) (map[string]FieldInfo, bool) {
	liveFields, hasLive := liveReflectionSchema[bareName]
	wiiFields, hasWii := wiiSchemaByBareName[bareName]
	pcFields, hasPC := pcVerifiedFields[bareName]
	tfbFields, hasTfb := tfbScriptPcSchema[bareName]
	if !hasWii && !hasPC && !hasTfb && !hasLive {
		return nil, false
	}

	merged := make(map[string]FieldInfo, len(liveFields)+len(wiiFields)+len(pcFields)+len(tfbFields))
	// live goes first: it uses the engine's own underscore-prefixed field
	// names (e.g. "_LHS"), which never collide with the older hand-
	// extracted schemas' non-underscored spelling (e.g. "LHS") below, so
	// this layer only ever adds coverage, never shadows a specifically
	// hand-verified entry.
	for name, info := range liveFields {
		merged[name] = info
	}
	for name, info := range wiiFields {
		merged[name] = info
	}
	for _, name := range pcDisprovenFields[bareName] {
		delete(merged, name)
		delete(merged, "_"+name) // live schema's own spelling of the same field
	}
	for name, info := range tfbFields {
		merged[name] = info
	}
	for name, info := range pcFields {
		merged[name] = info
	}
	return merged, true
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

// Vec3Offset returns a field's base byte offset if its recorded offset list
// is exactly 3 evenly-spaced values (e.g. [320, 324, 328], stride 4) — the
// shape a Vec3f setter/getter produces, since the extractor records every
// rA==3 load/store offset it finds in the accessor body without grouping
// them (see extract_schema.py / docs/CLASS_SCHEMA.md). Not every 3-entry
// offset list is a Vec3f in principle, but in the current schema every one
// observed is (e.g. ActorInfo.destination, GeneratorInfo's various
// direction fields) — genuinely-disagreeing overloads have offsets that
// don't form a clean arithmetic sequence and are correctly rejected here.
func (f FieldInfo) Vec3Offset() (int, int, bool) {
	list, ok := f.Offset.([]interface{})
	if !ok || len(list) != 3 {
		return 0, 0, false
	}
	vals := make([]int, 3)
	for i, v := range list {
		f, ok := v.(float64)
		if !ok {
			return 0, 0, false
		}
		vals[i] = int(f)
	}
	stride := vals[1] - vals[0]
	if stride <= 0 || vals[2]-vals[1] != stride {
		return 0, 0, false
	}
	return vals[0], stride, true
}
