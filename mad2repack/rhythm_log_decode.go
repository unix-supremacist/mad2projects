package main

import (
	"bufio"
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"

	"mad2iga"
)

// collectScriptObjects walks every top-level ScriptInfo/ScriptSet's
// reachable script-namespace objects (opList/masterVarList and anything
// pointer-reachable from them) and returns every object found, grouped by
// class name. This is the flat equivalent of script_dump.go's renderObject
// tree-building walk, used here to build the known-address set needed to
// empirically determine the runtime<->file address delta (see
// rhythmlogger.cpp's file header and docs/SCRIPT_FORMAT.md).
func collectScriptObjects(g *iga.ObjectGraph) (map[string][]uint32, map[uint32]uint32) {
	result := make(map[string][]uint32)
	owner := make(map[uint32]uint32) // object addr -> top-level ScriptInfo/ScriptSet root addr
	visited := make(map[uint32]bool)

	var walk func(obj iga.ObjectRef, root uint32)
	walk = func(obj iga.ObjectRef, root uint32) {
		if visited[obj.Addr] {
			return
		}
		visited[obj.Addr] = true
		result[obj.ClassName] = append(result[obj.ClassName], obj.Addr)
		owner[obj.Addr] = root

		if tfbScriptContainerClasses[obj.ClassName] {
			elems, resolved, err := g.ReadObjectListContainer(obj.Addr)
			if err != nil {
				return
			}
			for i, e := range elems {
				if !resolved[i] {
					continue
				}
				if childObj, ok := g.ObjectAt(e); ok {
					walk(childObj, root)
				}
			}
			return
		}

		schema, ok := iga.ClassSchema(obj.ClassName)
		if !ok {
			return
		}
		for fieldName := range schema {
			fv, ok := g.FieldValue(obj, fieldName)
			if !ok {
				continue
			}
			if pf, isPtr := fv.(iga.PointerFieldValue); isPtr && pf.Resolved {
				if childObj, ok := g.ObjectAt(pf.ResolvedAddr); ok {
					walk(childObj, root)
				}
			}
		}
	}

	top, err := g.TopLevelObjects()
	if err != nil {
		return result, owner
	}
	for _, obj := range top {
		if obj.ClassName == "ScriptInfo" || obj.ClassName == "ScriptSet" {
			walk(obj, obj.Addr)
		}
	}
	return result, owner
}

// knownScriptLabels gives human-readable labels to the specific ScriptInfo
// addresses identified this session by content-matching their variable
// names (see docs/SCRIPT_FORMAT.md's "VolcanoRave findings" — instance
// naming itself is unsolved, this is the practical workaround). Addresses
// are specific to VolcanoRave/level.bld as currently shipped; if the file
// changes these will simply stop matching (falls back to a bare address).
var knownScriptLabels = map[uint32]string{
	3474376: "Rave_TrackReader",
	3430744: "Track_Angel-referencing script",
	3529528: "Track_Tutorial/Track_IkoIko-referencing script (782 ops)",
	4022312: "Track_IkoIko/TrackReader-referencing script (756 ops)",
	3355960: "\"Track Reader pattern\"/Dancers hand-container script",
	2261612: "biggest script (1938 ops, likely Rave_LevelController)",
}

func scriptLabel(addr uint32) string {
	if label, ok := knownScriptLabels[addr]; ok {
		return fmt.Sprintf("%s@0x%X", label, addr)
	}
	return fmt.Sprintf("script@0x%X", addr)
}

// fieldKV is a single key/value pair for logLine.kvs -- an inline fixed-size
// array instead of a map[string]uint32 per line. At ~3M lines, a per-line map
// (even empty) costs real allocation + hashing overhead that adds up to
// multi-GB RSS and multi-minute runtimes; a small linear-scanned array costs
// nothing extra since logLine is stored by value in a flat []logLine slice.
type fieldKV struct {
	key string
	val uint32
}

type logLine struct {
	kind string
	kvs  [8]fieldKV
	nkvs int
	strs map[string]string  // lazily allocated -- only STARTSEQUENCE's mediaName uses this
	nums map[string]float64 // lazily allocated -- only STARTSEQUENCE's playbackPercent uses this
	raw  string
}

func (ll *logLine) field(key string) uint32 {
	v, _ := ll.fieldOK(key)
	return v
}

func (ll *logLine) fieldOK(key string) (uint32, bool) {
	for i := 0; i < ll.nkvs; i++ {
		if ll.kvs[i].key == key {
			return ll.kvs[i].val, true
		}
	}
	return 0, false
}

func (ll *logLine) setField(key string, val uint32) {
	if ll.nkvs < len(ll.kvs) {
		ll.kvs[ll.nkvs] = fieldKV{key, val}
		ll.nkvs++
	}
}

// parseLogLine is a plain token scanner, not a regex -- a real, measured
// bottleneck at this log's scale (2.9M+ lines: a first regex-based version
// with more alternatives to try per field, plus 3 always-allocated maps
// per line, took over 3 minutes and 6.6GB before being killed). Every
// field is `key=value` separated by spaces; value's own leading character
// disambiguates its type (`0x` prefix -> hex, `"` -> quoted string,
// otherwise a decimal integer or float, checked by scanning for `.`) --
// no backtracking, no submatch-slice allocations, one pass per line.
func parseLogLine(line string) (logLine, bool) {
	line = strings.TrimSpace(line)
	if line == "" || strings.HasPrefix(line, "#") {
		return logLine{}, false
	}
	sp := strings.IndexByte(line, ' ')
	if sp < 0 {
		return logLine{}, false
	}
	ll := logLine{kind: line[:sp], raw: line}

	rest := line[sp+1:]
	for len(rest) > 0 {
		// Find this token's end: the next space, unless a quoted string
		// starts before it (mediaName="..." can itself contain spaces), in
		// which case extend to that string's closing quote instead.
		tokEnd := len(rest)
		if sp2 := strings.IndexByte(rest, ' '); sp2 >= 0 {
			tokEnd = sp2
		}
		if q := strings.IndexByte(rest[:tokEnd], '"'); q >= 0 {
			if closeQ := strings.IndexByte(rest[q+1:], '"'); closeQ >= 0 {
				tokEnd = q + 1 + closeQ + 1
			}
		}
		tok := rest[:tokEnd]
		if tokEnd < len(rest) {
			rest = rest[tokEnd+1:]
		} else {
			rest = ""
		}

		eq := strings.IndexByte(tok, '=')
		if eq < 0 {
			continue
		}
		key, val := tok[:eq], tok[eq+1:]
		switch {
		case len(val) >= 2 && val[0] == '"' && val[len(val)-1] == '"':
			if ll.strs == nil {
				ll.strs = make(map[string]string, 2)
			}
			ll.strs[key] = val[1 : len(val)-1]
		case len(val) > 2 && val[0] == '0' && (val[1] == 'x' || val[1] == 'X'):
			if v, err := strconv.ParseUint(val[2:], 16, 64); err == nil {
				ll.setField(key, uint32(v))
			}
		case strings.ContainsRune(val, '.'):
			if v, err := strconv.ParseFloat(val, 64); err == nil {
				if ll.nums == nil {
					ll.nums = make(map[string]float64, 2)
				}
				ll.nums[key] = v
			}
		default:
			if v, err := strconv.ParseInt(val, 10, 64); err == nil {
				ll.setField(key, uint32(v))
			}
		}
	}
	return ll, true
}

// logKindToClass maps a log line's leading keyword to the TFBScriptInfo
// class it corresponds to, matching rhythmlogger.cpp's WriteLine calls.
var logKindToClass = map[string]string{
	"SETVALUE":      "OpSetValue",
	"CHECKVALUE":    "OpCheckValue",
	"FOREACH":       "OpForEach",
	"FINDVAR":       "OpFindVariable",
	"SLIDEVALUE":    "OpSlideValue",
	"STARTSEQUENCE": "OpStartSequence",
}

// findDelta empirically determines the constant offset between the
// runtime process's address space (what rhythmlogger.cpp actually logged)
// and level.bld's own file-offset address space (what mad2iga.ObjectGraph
// uses), by finding the single delta value that makes the most logged
// `this=` addresses land on a real known object of the expected class.
// See rhythmlogger.cpp's file header for why this must be done empirically
// rather than computed in advance.
//
// targetSamplesPerKind caps how many lines of EACH kind get used for
// delta-voting (voting is O(sampled_lines * known_addresses_of_that_class),
// and a real play session can log well over a million lines) -- but
// critically, samples are taken via a STRIDE spread across that kind's
// *entire* occurrence in the log, not just the first N encountered. A
// whole play session covers menus/character-select/other levels before
// and after any actual VolcanoRave gameplay; taking only the first N
// lines chronologically risks sampling exclusively from content before
// the level the known-address set is even built from ever loaded,
// finding a spuriously-confident wrong delta instead of failing loudly.
const targetSamplesPerKind = 5000

// deltaCandidate is one empirically-plausible runtime<->file delta, with how
// many delta-votes it received (see findDeltaCandidates).
type deltaCandidate struct {
	delta int64
	count int
}

// findDeltaCandidates is findDelta's multi-answer sibling: a single play
// session commonly loads VolcanoRave more than once (dying/retrying a song,
// returning to the menu and back in) -- each load lands Section 1 at a
// different runtime address, so there is no single correct delta for the
// whole log, only one correct delta PER LOAD. A real capture (see
// docs/SCRIPT_FORMAT.md, this tool's "per-line delta resolution" note)
// produced 10 delta candidates in an exact vote tie, which a single-winner
// scheme would treat as 9/10 of the real data being "noise". This returns
// every candidate whose vote count is at least minVoteFraction of the top
// candidate's, sorted by vote count descending, for the caller to try one
// at a time per log line (see resolveLineDelta below) instead of committing
// to one delta for the entire file.
const minVoteFraction = 0.05

func findDeltaCandidates(addrsByKind map[string][]uint32, known map[string][]uint32) ([]deltaCandidate, error) {
	deltaVotes := make(map[int64]int)
	for kind, addrs := range addrsByKind {
		cls, ok := logKindToClass[kind]
		if !ok {
			continue
		}
		stride := len(addrs) / targetSamplesPerKind
		if stride < 1 {
			stride = 1
		}
		for i := 0; i < len(addrs); i += stride {
			thisAddr := addrs[i]
			for _, knownAddr := range known[cls] {
				delta := int64(thisAddr) - int64(knownAddr)
				deltaVotes[delta]++
			}
		}
	}
	var candidates []deltaCandidate
	for d, c := range deltaVotes {
		candidates = append(candidates, deltaCandidate{d, c})
	}
	sort.Slice(candidates, func(i, j int) bool {
		if candidates[i].count != candidates[j].count {
			return candidates[i].count > candidates[j].count
		}
		return candidates[i].delta < candidates[j].delta // stable/deterministic among ties
	})

	if len(candidates) == 0 {
		return nil, fmt.Errorf("no candidate delta found — no logged 'this' address matched any known object of its class")
	}
	shown := 10
	if shown > len(candidates) {
		shown = len(candidates)
	}
	fmt.Println("top delta candidates:")
	for _, c := range candidates[:shown] {
		fmt.Printf("  delta=%d (0x%08X) votes=%d\n", c.delta, uint32(c.delta), c.count)
	}

	minVotes := int(float64(candidates[0].count) * minVoteFraction)
	kept := candidates[:0:0]
	for _, c := range candidates {
		if c.count < minVotes {
			break // sorted descending, so nothing after this clears the bar either
		}
		kept = append(kept, c)
	}
	fmt.Printf("keeping %d of %d candidates for per-line resolution (>= %d votes)\n", len(kept), len(candidates), minVotes)
	return kept, nil
}

// resolveLineDelta picks, from candidates (highest-voted first), the first
// delta that makes runtimeAddr translate to a real object of expectedClass
// -- i.e. the delta that actually applies to the specific level-load THIS
// log line came from, not necessarily the single most globally-popular one.
func resolveLineDelta(g *iga.ObjectGraph, candidates []deltaCandidate, runtimeAddr uint32, expectedClass string) (int64, bool) {
	for _, c := range candidates {
		if isRealHit(g, c.delta, runtimeAddr, expectedClass) {
			return c.delta, true
		}
	}
	return 0, false
}

// describeAddr translates a runtime address back to file space (via
// delta) and, if it lands exactly on a known object, describes it —
// including resolving OpCreateVariable.varName/RHSValueStack literal
// values the same way mad2repack script-dump already does, since those
// are fully static once we know which file address we're looking at.
func describeAddr(g *iga.ObjectGraph, owner map[uint32]uint32, delta int64, runtimeAddr uint32) string {
	if runtimeAddr == 0 {
		return "null"
	}
	fileAddr := uint32(int64(runtimeAddr) - delta)
	obj, ok := g.ObjectAt(fileAddr)
	if !ok || obj.ClassName == "" {
		return fmt.Sprintf("file@0x%X(no object)", fileAddr)
	}
	desc := fmt.Sprintf("%s@0x%X", obj.ClassName, fileAddr)
	if root, ok := owner[fileAddr]; ok {
		desc += " in " + scriptLabel(root)
	}
	switch obj.ClassName {
	case "OpCreateVariable":
		if nv, ok := g.FieldValue(obj, "varName"); ok {
			desc += fmt.Sprintf(" name=%q", fmt.Sprintf("%v", nv))
		}
	case "OpFindVariable":
		if nv, ok := g.FieldValue(obj, "varName"); ok {
			desc += fmt.Sprintf(" varName=%q", fmt.Sprintf("%v", nv))
		}
	case "RHSValueStack":
		typeV, _ := g.FieldValue(obj, "type")
		valV, _ := g.FieldValue(obj, "value")
		if typePf, ok := typeV.(iga.PointerFieldValue); ok {
			if valU, ok := valV.(uint32); ok {
				switch typePf.Raw {
				case 0x6A:
					desc += fmt.Sprintf(" int=%d", int32(valU))
				case 0x6B:
					desc += fmt.Sprintf(" float=%v", math.Float32frombits(valU))
				default:
					desc += fmt.Sprintf(" tag=0x%X raw=%d", typePf.Raw, int32(valU))
				}
			}
		}
	case "ValueRHSVariant":
		if op1v, ok := g.FieldValue(obj, "varOp1"); ok {
			if op1pf, ok := op1v.(iga.PointerFieldValue); ok && op1pf.Resolved && op1pf.TargetClass == "RHSValueStack" {
				// Already file-space (a normal statically-resolved pointer, not a
				// runtime-logged address), so delta=0 here.
				desc += " -> " + describeAddr(g, owner, 0, op1pf.ResolvedAddr)
			}
		}
	case "ValueInfo":
		// ValueInfo.type is the same "ptr" kind as RHSValueStack.type — try the
		// same confirmed int/float type-tag scheme (0x6A/0x6B); ValueInfo is a
		// live variable-storage slot (unlike RHSValueStack, a literal
		// constant), so a real hit here is genuinely valuable: it's this
		// session's own snapshot of an actual in-game variable's value.
		typeV, _ := g.FieldValue(obj, "type")
		valV, _ := g.FieldValue(obj, "value")
		if typePf, ok := typeV.(iga.PointerFieldValue); ok {
			if valU, ok := valV.(uint32); ok {
				switch typePf.Raw {
				case 0x6A:
					desc += fmt.Sprintf(" static_value_int=%d (NOTE: static file value, not live runtime)", int32(valU))
				case 0x6B:
					desc += fmt.Sprintf(" static_value_float=%v (NOTE: static file value, not live runtime)", math.Float32frombits(valU))
				default:
					desc += fmt.Sprintf(" value_tag=0x%X raw=%d", typePf.Raw, int32(valU))
				}
			}
		}
	}
	return desc
}

// isRealHit reports whether a runtime `this` address translates (via
// delta) to a real object of EXACTLY the expected class — not just any
// resolvable object. This matters: with ~4600 objects of ~50 classes in
// the file, a wrong delta still lands on SOME real object often enough to
// look like a hit if class identity isn't checked (e.g. a logged FOREACH
// call's `this` landing on an unrelated igObjectList or TFBWorldInfo isn't
// a real match, just coincidence) — requiring the exact class the log
// line's own kind implies (a FOREACH line's `this` really was an OpForeach
// instance at the moment it was logged, guaranteed by construction in
// rhythmlogger.cpp) is what actually distinguishes signal from noise here.
func isRealHit(g *iga.ObjectGraph, delta int64, runtimeAddr uint32, expectedClass string) bool {
	if runtimeAddr == 0 {
		return false
	}
	fileAddr := uint32(int64(runtimeAddr) - delta)
	obj, ok := g.ObjectAt(fileAddr)
	return ok && obj.ClassName == expectedClass
}

// cmdRhythmLogDecode is the entry point for `mad2repack rhythm-log-decode`.
// See rhythmlogger.cpp's file header and docs/SCRIPT_FORMAT.md for the full
// context: rhythmlogger.log holds raw runtime pointers from a live
// VolcanoRave play session; this translates them back to level.bld's own
// file-offset address space and annotates every line with what's actually
// there (variable names, literal values), producing a genuinely readable
// trace of what the rhythm minigame's script logic actually did.
func cmdRhythmLogDecode(logPath, bldPath, outPath string) error {
	// The real log can be multi-GB / tens of millions of lines (a play
	// session's worth, appended run over run) -- far too big to load
	// wholesale (os.ReadFile'ing it, plus a []logLine for every line, plus
	// Go's own per-object overhead) without pushing the whole machine into
	// swap. So this makes two streaming passes over the file from disk,
	// each holding only a small compact accumulator or a single in-flight
	// line in memory at a time -- never the whole file or a slice of every
	// parsed line.

	// Pass 1: collect just (kind, this-address) pairs, streamed, to compute
	// findDelta's per-kind stride sampling. This is a couple hundred MB at
	// most even at tens of millions of lines (a few uint32s per line), not
	// multiple GB.
	addrsByKind := make(map[string][]uint32, len(logKindToClass))
	totalLines := 0
	if err := scanLogFile(logPath, func(ll logLine) {
		totalLines++
		if _, ok := logKindToClass[ll.kind]; ok {
			addrsByKind[ll.kind] = append(addrsByKind[ll.kind], ll.field("this"))
		}
	}); err != nil {
		return fmt.Errorf("pass 1 (scan for delta): %w", err)
	}
	if totalLines == 0 {
		return fmt.Errorf("no parseable lines found in %s", logPath)
	}

	bldData, err := os.ReadFile(bldPath)
	if err != nil {
		return fmt.Errorf("read level.bld: %w", err)
	}
	g, err := iga.ParseObjectGraph(bldData)
	if err != nil {
		return fmt.Errorf("parse object graph: %w", err)
	}

	known, owner := collectScriptObjects(g)
	counts := make([]string, 0, len(known))
	for cls, addrs := range known {
		counts = append(counts, fmt.Sprintf("%s=%d", cls, len(addrs)))
	}
	sort.Strings(counts)
	fmt.Printf("known script objects: %s\n", strings.Join(counts, ", "))

	candidates, err := findDeltaCandidates(addrsByKind, known)
	if err != nil {
		return err
	}
	addrsByKind = nil // no longer needed, let it be collected before pass 2

	outFile, err := os.Create(outPath)
	if err != nil {
		return fmt.Errorf("create output: %w", err)
	}
	defer outFile.Close()
	out := bufio.NewWriterSize(outFile, 1<<20)
	defer out.Flush()

	fmt.Fprintf(out, "# decoded from %s against %s, %d candidate deltas tried per-line (%d total log lines)\n", logPath, bldPath, len(candidates), totalLines)
	fmt.Fprintf(out, "# a session can load VolcanoRave more than once (retry/return to menu), each at a different\n")
	fmt.Fprintf(out, "# runtime address -- so each line is resolved against whichever candidate delta actually makes\n")
	fmt.Fprintf(out, "# its own 'this' opcode land on a real VolcanoRave object OF THE EXPECTED CLASS, not one global\n")
	fmt.Fprintf(out, "# delta for the whole file. Lines matching no candidate are other levels/menus in the same\n")
	fmt.Fprintf(out, "# session (expected noise) -- see resolveLineDelta/isRealHit.\n")
	kept, dropped := 0, 0
	// Pass 2: re-stream the same file and decode+write each line immediately
	// -- nothing here is retained past the current line.
	scanErr := scanLogFile(logPath, func(ll logLine) {
		expectedClass := logKindToClass[ll.kind]
		delta, ok := resolveLineDelta(g, candidates, ll.field("this"), expectedClass)
		if !ok {
			dropped++
			return
		}
		kept++
		switch ll.kind {
		case "SETVALUE":
			fmt.Fprintf(out, "SETVALUE this=%s RHS=%s resolvedTarget=%s\n",
				describeAddr(g, owner, delta, ll.field("this")),
				describeAddr(g, owner, delta, ll.field("RHS")),
				describeAddr(g, owner, delta, ll.field("resolvedTarget")))
		case "CHECKVALUE":
			// cachedType is a raw runtime pointer to a shared type-descriptor
			// singleton (NOT a small file-stable tag like RHSValueStack's
			// 0x6A/0x6B -- confirmed empirically this session: only ~5 distinct
			// values across 2.3M calls, none in Section 1's own address range,
			// so they're not delta-translatable and aren't confirmed stable
			// across different game launches/DLL base addresses either). Rather
			// than guess which raw value means "float" vs "int" and bake that
			// into the tool, show cachedValue decoded both ways plus the raw
			// type pointer, so a human (or a later, more targeted analysis) can
			// cross-reference which interpretation is sane per cachedType value
			// for THIS session -- see docs/SCRIPT_FORMAT.md "Round 8".
			cachedValueRaw := ll.field("cachedValue")
			fmt.Fprintf(out, "CHECKVALUE this=%s LHS=%s relOp=%d RHS=%s cachedType=0x%08X cachedValue_int=%d cachedValue_float=%v\n",
				describeAddr(g, owner, delta, ll.field("this")),
				describeAddr(g, owner, delta, ll.field("LHS")),
				int32(ll.field("relOp")),
				describeAddr(g, owner, delta, ll.field("RHS")),
				ll.field("cachedType"),
				int32(cachedValueRaw),
				math.Float32frombits(cachedValueRaw))
		case "FOREACH":
			fmt.Fprintf(out, "FOREACH this=%s dir=%d LHS=%s cachedObject=%s\n",
				describeAddr(g, owner, delta, ll.field("this")),
				ll.field("dir"),
				describeAddr(g, owner, delta, ll.field("LHS")),
				describeAddr(g, owner, delta, ll.field("cachedObject")))
		case "FINDVAR":
			varName := "?"
			if s, ok := g.ResolveStringOrdinal(ll.field("varNameOrdinal")); ok {
				varName = s
			}
			fmt.Fprintf(out, "FINDVAR this=%s varName=%q cachedObject=%s\n",
				describeAddr(g, owner, delta, ll.field("this")),
				varName,
				describeAddr(g, owner, delta, ll.field("cachedObject")))
		case "SLIDEVALUE":
			fmt.Fprintf(out, "SLIDEVALUE this=%s LHS=%s RHS=%s secondsRHS=%s cachedSlider=%s\n",
				describeAddr(g, owner, delta, ll.field("this")),
				describeAddr(g, owner, delta, ll.field("LHS")),
				describeAddr(g, owner, delta, ll.field("RHS")),
				describeAddr(g, owner, delta, ll.field("secondsRHS")),
				describeAddr(g, owner, delta, ll.field("cachedSlider")))
		case "STARTSEQUENCE":
			// parent is ClonedSequence's own static template (a real Sequence
			// object rhythmlogger.cpp chased live in-process -- see its file
			// header); mediaName/playbackMode/playbackPercent were already
			// resolved there too (mediaName is a live in-memory string, not
			// the file-at-rest ordinal encoding, so it's just passed through
			// as plain text here, no further translation needed).
			extra := ""
			if mn, ok := ll.strs["mediaName"]; ok {
				pm := ll.field("playbackMode")
				pp := ll.nums["playbackPercent"]
				extra = fmt.Sprintf(" mediaName=%q playbackMode=%d playbackPercent=%v", mn, pm, pp)
			}
			fmt.Fprintf(out, "STARTSEQUENCE this=%s LHS=%s indexRHS=%s cachedObject=%s parent=%s%s\n",
				describeAddr(g, owner, delta, ll.field("this")),
				describeAddr(g, owner, delta, ll.field("LHS")),
				describeAddr(g, owner, delta, ll.field("indexRHS")),
				describeAddr(g, owner, delta, ll.field("cachedObject")),
				describeAddr(g, owner, delta, ll.field("parent")),
				extra)
		default:
			fmt.Fprintf(out, "%s\n", ll.raw)
		}
	})
	if scanErr != nil {
		return fmt.Errorf("pass 2 (decode+write): %w", scanErr)
	}

	fmt.Printf("wrote %s (%d lines kept, %d dropped as noise from other levels/menus)\n", outPath, kept, dropped)
	return nil
}

// scanLogFile streams logPath line by line (never loading the whole file
// into memory -- this log can be multiple GB), parses each line, and invokes
// fn once per successfully-parsed line. fn must not retain the logLine
// beyond its call (its raw/kind strings alias the scanner's own buffer
// only for the duration of the call in spirit, though in practice Go's
// scanner.Text() already copies -- the point is this function itself keeps
// no per-line state around after fn returns).
func scanLogFile(logPath string, fn func(logLine)) error {
	f, err := os.Open(logPath)
	if err != nil {
		return fmt.Errorf("open: %w", err)
	}
	defer f.Close()

	scanner := bufio.NewScanner(f)
	scanner.Buffer(make([]byte, 1<<20), 1<<20)
	for scanner.Scan() {
		if ll, ok := parseLogLine(scanner.Text()); ok {
			fn(ll)
		}
	}
	return scanner.Err()
}
