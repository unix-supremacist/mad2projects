package main

import (
	"bufio"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"mad2iga"
)

// cmdScriptPretty renders a level.bld's TFBScriptInfo object graph as
// human-readable pseudocode instead of script-dump's raw nested JSON. This
// is a best-effort recursive reconstruction, not a byte-exact decompiler --
// see the header comment it writes into every output file for exactly what
// is real/derived-from-source versus best-effort/unresolved. The real parts
// (block nesting via OpBranch.branchPC, literal RHSValueStack values,
// OpFindVariable/OpCreateVariable.varName) are all documented, confirmed
// mechanisms from docs/SCRIPT_FORMAT.md's Round 3/4 investigation -- this
// is the first time they've been wired into an actual mad2repack
// subcommand rather than a one-off proof of concept.
func cmdScriptPretty(bldPath, outPath string) error {
	data, err := os.ReadFile(bldPath)
	if err != nil {
		return fmt.Errorf("read level.bld: %w", err)
	}
	g, err := iga.ParseObjectGraph(data)
	if err != nil {
		return fmt.Errorf("parse object graph: %w", err)
	}
	top, err := g.TopLevelObjects()
	if err != nil {
		return fmt.Errorf("read top-level objects: %w", err)
	}

	var scriptInfos, scriptSets []iga.ObjectRef
	for _, o := range top {
		switch o.ClassName {
		case "ScriptInfo":
			scriptInfos = append(scriptInfos, o)
		case "ScriptSet":
			scriptSets = append(scriptSets, o)
		}
	}
	sort.Slice(scriptInfos, func(i, j int) bool { return scriptInfos[i].Addr < scriptInfos[j].Addr })
	sort.Slice(scriptSets, func(i, j int) bool { return scriptSets[i].Addr < scriptSets[j].Addr })

	f, err := os.Create(outPath)
	if err != nil {
		return fmt.Errorf("create output: %w", err)
	}
	defer f.Close()
	w := bufio.NewWriter(f)

	fmt.Fprintf(w, "# %s -- pretty-printed compiled TFBScript graph\n", filepath.Base(bldPath))
	fmt.Fprintf(w, "# %d ScriptInfo, %d ScriptSet top-level roots.\n#\n", len(scriptInfos), len(scriptSets))
	fmt.Fprint(w, `# This is a best-effort recursive reconstruction, not a byte-exact decompiler.
# What IS real/confirmed (see docs/SCRIPT_FORMAT.md's Round 3/4 and this
# session's Ghidra work):
#   - Block nesting (IF/FOREACH/LOOP/... { ... }) comes from OpBranch's real
#     branchPC forward-jump target, read straight from the interpreter's own
#     execute() methods -- not guessed structure.
#   - Variable names (OpFindVariable/OpCreateVariable.varName) are resolved
#     directly from PC-verified schema fields, not guessed.
#   - relOperator/arithOperator/flowVal ARE real symbols (==, <=, +, -,
#     CONTINUE, END, ...), decompiled straight out of ScriptInfoLib.dll's
#     own igMetaEnum tables (RelOp/ArithOp/FlowVals) this session -- not
#     inferred from usage. See relOpSymbols/arithOpSymbols/flowValSymbols'
#     own doc comments for the exact addresses and string tables read.
#   - RHSValueStack literals show BOTH an int32 and float32 reinterpretation
#     (e.g. "<int:5|float:7e-45 (type=0x4F unresolved)>") rather than
#     picking one -- ".type" turned out to be a genuine external reference
#     into a per-file table (not a portable 2-value tag the way an earlier
#     investigation believed from one file alone), so which interpretation
#     is "correct" isn't resolved yet. Judge from context: -999/0/1/5/40
#     read obviously as ints, 0.25 obviously as a float. See
#     renderRHSLiteral's own doc comment for the full finding.
# What is NOT resolved, and is shown honestly rather than faked:
#   - SET/IF's LHS side is whatever object the field structurally resolves
#     to. There is NO "most recently found variable" name-guessing here
#     (a prior proof-of-concept renderer did this and was confirmed WRONG
#     for most accesses in the one script it was tested against -- see
#     Round 4's "known, unfixed limitations"). If LHS doesn't resolve to
#     something with a real name, it prints as <ClassName@0xADDR>, not a
#     guessed name.
#   - Anything shown as <ClassName@0xADDR> is a real, structurally-resolved
#     object this renderer has no class-specific rendering rule for yet --
#     not missing data, just unformatted. Cross-reference the same address
#     in the sibling <Level>.json (script-dump) for its full raw fields.
#
`)

	for _, si := range scriptInfos {
		renderScriptInfoPretty(g, si, w)
	}
	for _, ss := range scriptSets {
		renderScriptSetPretty(g, ss, w)
	}

	if err := w.Flush(); err != nil {
		return fmt.Errorf("write output: %w", err)
	}
	fmt.Printf("  %d ScriptInfo, %d ScriptSet top-level roots rendered\n", len(scriptInfos), len(scriptSets))
	return nil
}

func renderScriptInfoPretty(g *iga.ObjectGraph, si iga.ObjectRef, w io.Writer) {
	fmt.Fprintf(w, "================================================================================\n")
	fmt.Fprintf(w, "ScriptInfo @0x%X\n", si.Addr)
	fmt.Fprintf(w, "================================================================================\n")

	if elems, resolved, ok := resolvedContainer(g, si, "masterVarList"); ok {
		fmt.Fprintf(w, "\nDeclared variables (%d):\n", len(elems))
		for i, addr := range elems {
			if !resolved[i] {
				fmt.Fprintf(w, "  <unresolved>\n")
				continue
			}
			obj, ok := g.ObjectAt(addr)
			if !ok {
				continue
			}
			name := fmt.Sprintf("<%s@0x%X>", obj.ClassName, obj.Addr)
			if nv, ok := g.FieldValue(obj, "varName"); ok {
				name = renderRawValue(g, nv, 0)
			}
			fmt.Fprintf(w, "  %s\n", name)
		}
	}

	if elemAddrs, resolved, ok := resolvedContainer(g, si, "opList"); ok {
		ops := make([]iga.ObjectRef, len(elemAddrs))
		for i, addr := range elemAddrs {
			if resolved[i] {
				if obj, ok := g.ObjectAt(addr); ok {
					ops[i] = obj
				}
			}
		}
		fmt.Fprintf(w, "\nCode (%d top-level opcodes):\n", len(ops))
		renderOps(g, ops, 0, len(ops), "  ", w)
	}
	fmt.Fprintln(w)
}

func renderScriptSetPretty(g *iga.ObjectGraph, ss iga.ObjectRef, w io.Writer) {
	fmt.Fprintf(w, "--------------------------------------------------------------------------------\n")
	fmt.Fprintf(w, "ScriptSet @0x%X\n", ss.Addr)
	if elems, resolved, ok := resolvedContainer(g, ss, "list"); ok {
		fmt.Fprintf(w, "  %d members:\n", len(elems))
		for i, addr := range elems {
			if !resolved[i] {
				fmt.Fprintf(w, "    <unresolved>\n")
				continue
			}
			obj, ok := g.ObjectAt(addr)
			if !ok {
				continue
			}
			fmt.Fprintf(w, "    %s@0x%X\n", obj.ClassName, obj.Addr)
		}
	}
	fmt.Fprintln(w)
}

// resolvedContainer reads obj.<fieldName> (expected to be a ptr field
// pointing at an igObjectList-shaped container -- OpCodeList,
// OpCreateVariableList, ScriptGroupStack, etc.) and returns its elements.
func resolvedContainer(g *iga.ObjectGraph, obj iga.ObjectRef, fieldName string) ([]uint32, []bool, bool) {
	v, ok := g.FieldValue(obj, fieldName)
	if !ok {
		return nil, nil, false
	}
	pf, ok := v.(iga.PointerFieldValue)
	if !ok || !pf.Resolved {
		return nil, nil, false
	}
	elems, resolved, err := g.ReadObjectListContainer(pf.ResolvedAddr)
	if err != nil {
		return nil, nil, false
	}
	return elems, resolved, true
}

// maxPrettyDepth bounds recursion through nested branch bodies -- real
// scripts nest maybe 5-10 deep at most (see the 649-opcode Rave_TrackReader
// case study in docs/SCRIPT_FORMAT.md), this is a generous ceiling against
// any malformed/cyclic branchPC data, not a real limitation for normal
// content.
const maxPrettyDepth = 64

// branchClassCache memoizes hasBranchPC's schema lookup per class name --
// script-pretty runs this check for every single opcode instance in a
// script, and ClassSchema's own merge/lookup isn't free.
var branchClassCache = map[string]bool{}

// hasBranchPC reports whether className's merged (own + inherited) schema
// includes a real branchPC field -- i.e. whether this opcode class
// genuinely inherits OpBranch, per docs/SCRIPT_FORMAT.md Round 3: classes
// that instead claim offset 0x24(36)/dec for their own field (e.g.
// OpSetValue.LHS@36) do NOT inherit OpBranch, and the merged schema simply
// won't contain a "branchPC"/"_branchPC" key for them.
func hasBranchPC(className string) bool {
	if v, ok := branchClassCache[className]; ok {
		return v
	}
	has := false
	if fields, ok := iga.ClassSchema(className); ok {
		_, hasPC := fields["branchPC"]
		_, hasUnderscorePC := fields["_branchPC"]
		has = hasPC || hasUnderscorePC
	}
	branchClassCache[className] = has
	return has
}

// renderOps recursively renders ops[start:end] at the given indent,
// reconstructing real nested blocks from any branch-owning opcode's
// branchPC (see hasBranchPC/docs/SCRIPT_FORMAT.md Round 3-4): an opcode at
// index i with a valid forward branchPC=j owns body [i+1, j-1], rendered
// nested one level deeper, after which rendering resumes at j.
func renderOps(g *iga.ObjectGraph, ops []iga.ObjectRef, start, end int, indent string, w io.Writer) {
	renderOpsDepth(g, ops, start, end, indent, w, 0)
}

// prettyShowAddr, when MAD2_PRETTY_ADDR is set in the environment, prefixes
// every rendered op with its file offset + PC index -- used for locating the
// exact bytes to patch (see docs/DEBUG_MENU.md). Off by default so ordinary
// pseudocode output stays clean.
var prettyShowAddr = os.Getenv("MAD2_PRETTY_ADDR") != ""

func addrPrefix(obj iga.ObjectRef, pc int) string {
	if !prettyShowAddr {
		return ""
	}
	return fmt.Sprintf("[pc=%d @0x%X] ", pc, obj.Addr)
}

func renderOpsDepth(g *iga.ObjectGraph, ops []iga.ObjectRef, start, end int, indent string, w io.Writer, depth int) {
	i := start
	for i < end {
		obj := ops[i]
		if obj.ClassName == "" {
			fmt.Fprintf(w, "%s%s<unresolved opcode>\n", indent, addrPrefix(obj, i))
			i++
			continue
		}

		header := addrPrefix(obj, i) + renderOpHeader(g, obj)

		if depth < maxPrettyDepth && hasBranchPC(obj.ClassName) {
			branchVal, hasVal := readBranchPC(g, obj)
			if hasVal && branchVal != 0xFFFFFFFF && int(branchVal) > i+1 && int(branchVal) <= end {
				bodyEnd := int(branchVal)
				fmt.Fprintf(w, "%s%s {\n", indent, header)
				renderOpsDepth(g, ops, i+1, bodyEnd, indent+"    ", w, depth+1)
				fmt.Fprintf(w, "%s}\n", indent)
				i = bodyEnd
				continue
			}
			branchNote := "no body"
			if hasVal && branchVal == 0xFFFFFFFF {
				branchNote = "end of script"
			} else if hasVal {
				branchNote = fmt.Sprintf("branchPC=%d out of range", branchVal)
			}
			fmt.Fprintf(w, "%s%s  // %s\n", indent, header, branchNote)
			i++
			continue
		}

		fmt.Fprintf(w, "%s%s\n", indent, header)
		i++
	}
}

func readBranchPC(g *iga.ObjectGraph, obj iga.ObjectRef) (uint32, bool) {
	v, ok := g.FieldValue(obj, "branchPC")
	if !ok {
		return 0, false
	}
	u, ok := v.(uint32)
	return u, ok
}

// renderOpHeader renders one opcode as a single-line pseudocode statement
// (its own text; any nested body is added separately by renderOpsDepth).
// Classes without a hand-written case fall through to genericOpSummary.
func renderOpHeader(g *iga.ObjectGraph, obj iga.ObjectRef) string {
	switch obj.ClassName {
	case "OpSetValue":
		return fmt.Sprintf("SET %s = %s", renderFieldValue(g, obj, "LHS"), renderFieldValue(g, obj, "RHS"))
	case "OpCheckValue":
		// OpCheckValue has no PC-verified friendly-named LHS/RHS/relOperator
		// (unlike OpSetValue/OpForEach) -- only the live-reflection
		// "_LHS"/"_RHS"/"_relOperator" (Kind ["unknown"]) exist in its merged
		// schema, so renderFieldValue("LHS") would silently fail ("?").
		// renderPointerFieldSmart below falls back to the underscore name
		// and resolves it manually, since we know from the class hierarchy
		// (docs/SCRIPT_FORMAT.md) these ARE pointer fields despite the
		// schema not saying so yet.
		return fmt.Sprintf("IF %s %s %s", renderPointerFieldSmart(g, obj, "LHS"), relOpSymbol(smartEnumRaw(g, obj, "relOperator")), renderPointerFieldSmart(g, obj, "RHS"))
	case "OpForEach":
		return fmt.Sprintf("FOREACH item IN %s (%s)", renderFieldValue(g, obj, "RHS"), dirLabel(g, obj))
	case "OpLoopValue":
		return fmt.Sprintf("LOOP %s TIMES", renderFieldValue(g, obj, "RHS"))
	case "OpFindVariable":
		return fmt.Sprintf("FIND-VAR %s", renderFieldValue(g, obj, "varName"))
	case "OpCreateVariable":
		return fmt.Sprintf("VAR %s", renderFieldValue(g, obj, "varName"))
	case "OpTopLevelBehavior":
		return "BEHAVIOR"
	case "OpFlow":
		return fmt.Sprintf("FLOW %s", flowValSymbol(smartEnumRaw(g, obj, "flowVal")))
	default:
		return genericOpSummary(g, obj)
	}
}

// genericOpSummary renders any opcode class script-pretty has no
// hand-written case for, using only its PC-verified ("source":"pc") schema
// fields -- deliberately excludes the live-reflection-sourced "_underscore"
// duplicate fields (all Kind ["unknown"], which FieldValue reads back as a
// raw uint32 regardless of the field's real type -- misleading for a ptr or
// float field, see FieldValue's doc comment) and Wii-only fields (offsets
// not independently PC-verified, see docs/CLASS_SCHEMA.md's Major caveat).
// A class with zero PC-verified fields renders as a bare <Class@addr> --
// honestly "opaque, no known fields" rather than guessing.
func genericOpSummary(g *iga.ObjectGraph, obj iga.ObjectRef) string {
	schema, ok := iga.ClassSchema(obj.ClassName)
	if !ok {
		return fmt.Sprintf("%s@0x%X", obj.ClassName, obj.Addr)
	}
	var names []string
	for name, info := range schema {
		if info.Source == "pc" {
			names = append(names, name)
		}
	}
	if len(names) == 0 {
		return fmt.Sprintf("%s@0x%X", obj.ClassName, obj.Addr)
	}
	sort.Strings(names)
	parts := make([]string, 0, len(names))
	for _, name := range names {
		parts = append(parts, fmt.Sprintf("%s=%s", name, renderFieldValue(g, obj, name)))
	}
	return fmt.Sprintf("%s(%s)", obj.ClassName, strings.Join(parts, ", "))
}

// dirLabel decodes OpForEach.dir using the confirmed (source-level, see
// docs/SCRIPT_FORMAT.md Round 4's execute@OpForEach decompilation)
// semantics: 0=advance forward, 1=advance backward, 2=random permutation.
// Any other raw value is shown as-is rather than guessed.
func dirLabel(g *iga.ObjectGraph, obj iga.ObjectRef) string {
	v, ok := g.FieldValue(obj, "dir")
	if !ok {
		return "dir=?"
	}
	u, ok := v.(uint32)
	if !ok {
		return "dir=?"
	}
	switch u {
	case 0:
		return "forward"
	case 1:
		return "backward"
	case 2:
		return "random"
	default:
		return fmt.Sprintf("dir=%d", u)
	}
}

// relOpSymbols, arithOpSymbols and flowValSymbols are read directly out of
// ScriptInfoLib.dll's own igMetaEnum tables (Gap::Core::igMetaEnum,
// constructed via createMetaEnum("RelOp"/"ArithOp"/"FlowVals", <name ptr
// array>, <value array>, count, 0) at 0x10022b40/0x10022bf0/0x100259f0
// respectively) -- decompiled and the underlying string/int arrays read
// directly from process memory this session (not guessed, not inferred
// from usage): RelOp={0:LE_OP,1:EQ_OP,2:GE_OP,3:LT_OP,4:GT_OP,5:NE_OP},
// ArithOp={-1:NO_OP,0:ADD_OP,1:SUB_OP,2:MULT_OP,3:DIV_OP},
// FlowVals={-2:FLOW_ELSE,-1:FLOW_ELSE_IF,0:FLOW_END,1:FLOW_CONTINUE,
// 2..10:FLOW_OUT2..FLOW_OUT10}. Cross-checked against real decoded output:
// RitesOfPassage's own `IF x op1 -999` / `IF x op5 0` decode as EQ_OP/NE_OP
// respectively, and read as exactly the "if x == unset-sentinel, init it"/
// "if x != 0" idioms real script logic would actually contain -- a live
// sanity check, not just a static claim. Negative enum values are stored
// as ordinary 4-byte ints, so they appear here as their uint32 bit pattern
// (0xFFFFFFFF for -1, 0xFFFFFFFE for -2) to match what FieldValue/
// smartEnumRaw actually reads off a raw int32 field.
var relOpSymbols = map[uint32]string{
	0: "<=", 1: "==", 2: ">=", 3: "<", 4: ">", 5: "!=",
}

var arithOpSymbols = map[uint32]string{
	0xFFFFFFFF: "NO_OP", 0: "+", 1: "-", 2: "*", 3: "/",
}

var flowValSymbols = map[uint32]string{
	0xFFFFFFFE: "ELSE", 0xFFFFFFFF: "ELSE_IF", 0: "END", 1: "CONTINUE",
	2: "OUT(2)", 3: "OUT(3)", 4: "OUT(4)", 5: "OUT(5)", 6: "OUT(6)",
	7: "OUT(7)", 8: "OUT(8)", 9: "OUT(9)", 10: "OUT(10)",
}

func relOpSymbol(raw uint32) string {
	if s, ok := relOpSymbols[raw]; ok {
		return s
	}
	return fmt.Sprintf("relop(%d)", int32(raw))
}

func arithOpSymbol(raw uint32) string {
	if s, ok := arithOpSymbols[raw]; ok {
		return s
	}
	return fmt.Sprintf("arithop(%d)", int32(raw))
}

func flowValSymbol(raw uint32) string {
	if s, ok := flowValSymbols[raw]; ok {
		return s
	}
	return fmt.Sprintf("flowval(%d)", int32(raw))
}

// smartEnumRaw reads an enum-kind field's raw uint32, trying the
// PC-verified friendly name first and falling back to the live-reflection
// "_name" variant (see renderPointerFieldSmart's doc comment for why that
// fallback is necessary for classes like OpCheckValue).
func smartEnumRaw(g *iga.ObjectGraph, obj iga.ObjectRef, fieldName string) uint32 {
	if v, ok := g.FieldValue(obj, fieldName); ok {
		if u, ok := v.(uint32); ok {
			return u
		}
	}
	if v, ok := g.FieldValue(obj, "_"+fieldName); ok {
		if u, ok := v.(uint32); ok {
			return u
		}
	}
	return 0xFFFFFFFF
}

// renderFieldValue reads obj.fieldName and renders it via renderRawValue.
func renderFieldValue(g *iga.ObjectGraph, obj iga.ObjectRef, fieldName string) string {
	v, ok := g.FieldValue(obj, fieldName)
	if !ok {
		return "?"
	}
	return renderRawValue(g, v, 0)
}

// renderPointerFieldSmart reads a field script-pretty knows semantically
// must be a pointer (from the class hierarchy/hand-written opcode cases
// above), even for classes whose merged schema has no PC-verified friendly
// name for it -- only the live-reflection "_name" variant (Kind
// ["unknown"], read back by FieldValue as a raw uint32, not a resolved
// PointerFieldValue -- see FieldValue's own doc comment on why "unknown"
// kind fields default to a raw read). Tries the friendly name first (so
// this is safe to use even for fields that DO have a PC-verified entry);
// falls back to "_"+fieldName and manually resolves the raw value as a
// segmented pointer.
func renderPointerFieldSmart(g *iga.ObjectGraph, obj iga.ObjectRef, fieldName string) string {
	if v, ok := g.FieldValue(obj, fieldName); ok {
		return renderRawValue(g, v, 0)
	}
	v, ok := g.FieldValue(obj, "_"+fieldName)
	if !ok {
		return "?"
	}
	raw, ok := v.(uint32)
	if !ok {
		return renderRawValue(g, v, 0)
	}
	if raw == 0 {
		return "null"
	}
	resolvedAddr, ok := g.ResolveSegPointer(raw)
	if !ok {
		return fmt.Sprintf("<unresolved:0x%X>", raw)
	}
	target, ok := g.ObjectAt(resolvedAddr)
	if !ok || target.ClassName == "" {
		return fmt.Sprintf("<unresolved:0x%X>", raw)
	}
	return renderValueObject(g, target, 1)
}

// maxValueRenderDepth bounds how far renderRawValue chases pointer fields
// into value-producing wrapper objects (ValueRHSVariant -> RHSValueStack,
// ScriptReference -> its target, etc.) before giving up and printing a bare
// <Class@addr> -- real value expressions in this VM are shallow (a literal,
// or at most one binary-operator wrapper around two literals), so this is a
// safety cap against unexpected cycles/depth, not a real limitation.
const maxValueRenderDepth = 4

func renderRawValue(g *iga.ObjectGraph, v interface{}, depth int) string {
	switch t := v.(type) {
	case string:
		return strconv.Quote(t)
	case uint32:
		return strconv.FormatUint(uint64(t), 10)
	case float32:
		return strconv.FormatFloat(float64(t), 'g', -1, 32)
	case [3]float32:
		return fmt.Sprintf("(%.3f, %.3f, %.3f)", t[0], t[1], t[2])
	case [16]float32:
		return "<matrix44f>"
	case iga.PointerFieldValue:
		if !t.Resolved {
			if t.Raw == 0 {
				return "null"
			}
			return fmt.Sprintf("<unresolved:0x%X>", t.Raw)
		}
		if depth >= maxValueRenderDepth {
			return fmt.Sprintf("<%s@0x%X>", t.TargetClass, t.ResolvedAddr)
		}
		obj, ok := g.ObjectAt(t.ResolvedAddr)
		if !ok {
			return fmt.Sprintf("<%s@0x%X>", t.TargetClass, t.ResolvedAddr)
		}
		return renderValueObject(g, obj, depth+1)
	default:
		return fmt.Sprintf("%v", t)
	}
}

// renderValueObject renders a resolved pointer target with class-specific
// knowledge of Mad2's own value-expression classes (see
// docs/SCRIPT_FORMAT.md's class hierarchy section) -- everything else
// falls back to a bare <Class@addr> marker.
func renderValueObject(g *iga.ObjectGraph, obj iga.ObjectRef, depth int) string {
	switch obj.ClassName {
	case "RHSValueStack":
		if s, ok := renderRHSLiteral(g, obj); ok {
			return s
		}
		return fmt.Sprintf("<RHSValueStack@0x%X>", obj.Addr)
	case "ValueRHSVariant":
		op1v, ok1 := g.FieldValue(obj, "varOp1")
		if !ok1 {
			return fmt.Sprintf("<ValueRHSVariant@0x%X>", obj.Addr)
		}
		op1 := renderRawValue(g, op1v, depth)
		if op2v, ok2 := g.FieldValue(obj, "varOp2"); ok2 {
			if pf, isPf := op2v.(iga.PointerFieldValue); isPf && pf.Resolved {
				op2 := renderRawValue(g, op2v, depth)
				return fmt.Sprintf("(%s %s %s)", op1, arithOpSymbol(smartEnumRaw(g, obj, "arithOperator")), op2)
			}
		}
		return op1
	case "ScriptReference":
		if tv, ok := g.FieldValue(obj, "obj"); ok {
			if pf, isPf := tv.(iga.PointerFieldValue); isPf && pf.Resolved {
				if target, ok := g.ObjectAt(pf.ResolvedAddr); ok {
					if target.ClassName == "OpFindVariable" || target.ClassName == "OpCreateVariable" {
						if nv, ok := g.FieldValue(target, "varName"); ok {
							return renderRawValue(g, nv, depth)
						}
					}
					return fmt.Sprintf("&%s@0x%X", target.ClassName, target.Addr)
				}
			}
		}
		return fmt.Sprintf("<ScriptReference@0x%X>", obj.Addr)
	case "OpFindVariable", "OpCreateVariable":
		if nv, ok := g.FieldValue(obj, "varName"); ok {
			return renderRawValue(g, nv, depth)
		}
		return fmt.Sprintf("<%s@0x%X>", obj.ClassName, obj.Addr)
	case "ScriptGroupStack", "SetStack", "ScriptObjectList", "igObjectList", "ScriptObjectSortedList":
		// Container operand (e.g. OpChangeMembership/OpSetReference RHS): show
		// the named members it holds rather than a bare <Class@addr>, so
		// list-building ("add Options_Opt_DEBUG to the menu") is legible. Only
		// resolves the members' own igNamedObject `_name` (via the string pool)
		// / OpFindVariable.varName -- one level deep, capped, no recursion into
		// the members' own fields.
		if depth >= maxValueRenderDepth {
			return fmt.Sprintf("<%s@0x%X>", obj.ClassName, obj.Addr)
		}
		if names := containerMemberNames(g, obj); len(names) > 0 {
			return fmt.Sprintf("{%s}", strings.Join(names, ", "))
		}
		return fmt.Sprintf("<%s@0x%X>", obj.ClassName, obj.Addr)
	default:
		return fmt.Sprintf("<%s@0x%X>", obj.ClassName, obj.Addr)
	}
}

// containerMemberNames resolves a container's member objects to human-readable
// names one level deep: an igNamedObject `_name` string-pool name for
// StringInfo/ValueInfo/etc., or the varName for an OpFindVariable/OpCreateVariable
// member. Members with no resolvable name are shown as their bare class. Capped
// at maxContainerMemberNames to keep a big list from dominating a line.
const maxContainerMemberNames = 12

func containerMemberNames(g *iga.ObjectGraph, obj iga.ObjectRef) []string {
	elems, resolved, err := g.ReadObjectListContainer(obj.Addr)
	if err != nil {
		return nil
	}
	names := make([]string, 0, len(elems))
	for i, addr := range elems {
		if i >= maxContainerMemberNames {
			names = append(names, fmt.Sprintf("...+%d", len(elems)-maxContainerMemberNames))
			break
		}
		if !resolved[i] {
			names = append(names, "?")
			continue
		}
		m, ok := g.ObjectAt(addr)
		if !ok {
			names = append(names, "?")
			continue
		}
		if m.ClassName == "OpFindVariable" || m.ClassName == "OpCreateVariable" {
			if nv, ok := g.FieldValue(m, "varName"); ok {
				names = append(names, renderRawValue(g, nv, maxValueRenderDepth))
				continue
			}
		}
		// igNamedObject `_name` -> real name via authoritative RSTR/TSTR.
		if s, ok := g.ResolveFieldName(m, "_name"); ok && s != "" {
			names = append(names, strconv.Quote(s))
			continue
		}
		names = append(names, m.ClassName)
	}
	return names
}

// renderRHSLiteral decodes an RHSValueStack's (type, value) pair into a
// literal. The 0x6A=int32/0x6B=float32 fast path (same tags
// script_dump.go's decodeRHSValueStack checks, see docs/SCRIPT_FORMAT.md
// "VolcanoRave findings, round 2") is confirmed correct when it fires, but
// is NOT a universal tag scheme -- verified this session (see
// docs/SCRIPT_FORMAT.md's "RHSValueStack.type is a real external reference,
// not a portable 2-value tag" note) that `type` is a genuine segmented
// pointer that resolves, per RHSValueStack::resolve's own decompiled logic
// in ScriptInfoLib.dll (0x10021b80), to one of several shared igMetaObject
// "measurement kind" singletons (IntMeasurement/FloatMeasurement/
// ColorMeasurement/ScreenMeasurement/...) living in the DLL's own static
// data -- external to the level file, referenced through a small per-file
// table whose exact structure isn't decoded yet. Its raw on-disk numeric
// encoding is therefore file-specific (confirmed: RitesOfPassage's own
// distinct raw tags are 0x4F/0x65/0xF4/0xC7, not 0x6A/0x6B at all), so
// 0x6A/0x6B only fire for files that happen to share VolcanoRave's segment
// layout. For any other tag, this shows both a raw int32 and float32
// reinterpretation of the value bits rather than guessing which applies --
// the same "don't guess, show both, let the reader judge" approach
// rhythm_log_decode.go already uses for OpCheckValue.cachedType.
func renderRHSLiteral(g *iga.ObjectGraph, obj iga.ObjectRef) (string, bool) {
	typeV, ok := g.FieldValue(obj, "type")
	if !ok {
		return "", false
	}
	typePf, ok := typeV.(iga.PointerFieldValue)
	if !ok {
		return "", false
	}
	valV, ok := g.FieldValue(obj, "value")
	if !ok {
		return "", false
	}
	valRaw, ok := valV.(uint32)
	if !ok {
		return "", false
	}
	switch typePf.Raw {
	case 0x6A:
		return strconv.FormatInt(int64(int32(valRaw)), 10), true
	case 0x6B:
		return strconv.FormatFloat(float64(math.Float32frombits(valRaw)), 'g', -1, 32), true
	default:
		asInt := strconv.FormatInt(int64(int32(valRaw)), 10)
		asFloat := strconv.FormatFloat(float64(math.Float32frombits(valRaw)), 'g', -1, 32)
		return fmt.Sprintf("<int:%s|float:%s (type=0x%X unresolved)>", asInt, asFloat, typePf.Raw), true
	}
}
