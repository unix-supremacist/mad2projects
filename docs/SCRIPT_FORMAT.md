# TFBScript — level logic/behavior scripting system

This covers `TFBScriptInfo` (the `ScriptSet`/`ScriptObject`/`Op*` classes
that appear throughout `level.bld`'s object graph — by far the largest
single namespace in the class directory, and the actual encoding of level
*logic*: cutscenes, triggers, AI behaviors, "what happens when"). The
semantic map below was originally built from two external reference
projects with zero Mad2-PC-verification; **as of this session, most of it
now has real PC-verified field offsets** — see "PC-verified field schema"
below, which supersedes the semantic-only hierarchy for every class it
covers. The class hierarchy/semantics sections further down are still worth
reading for *meaning* (opcode names, what each field is for), just no
longer the only source of truth for *offsets*.

## PC-verified field schema (this session — supersedes the hypothesis below)

Applied the exact `arkRegisterInitialize@<Class>` bulk-field-registration
technique that cracked `ActorInfo`'s schema (see `CLASS_SCHEMA.md`) to the
*entire* TFBScriptInfo namespace in `ScriptInfoLib.dll`: every one of the
104 classes in this namespace has its own `arkRegisterInitialize@<Class>@
TFBScriptInfo@Gap@@CAXXZ` function, found by grep'ing the DLL's full export
table (9,314 symbols — `list_exports` alone can't page through that
economically; used `run_script_inline` — see "Tooling used" below).

**Method, concretely** (repeatable for any class in this DLL or a sibling
`*InfoLib.dll`):
1. `create_function` + `disassemble` at the `arkRegisterInitialize@X`
   address (auto-analysis never created a `Function` there — same
   quirk noted elsewhere in this repo's docs).
2. Decompile it. Every *new* field the class registers appears as a pair of
   parallel local-variable assignments: a name string (`local_X = "_LHS"`)
   and a pointer to a per-field static descriptor object whose own symbol
   name embeds both the field name and its `igMetaField` subtype, e.g.
   `&_k_LHS_OpSetValue_TFBScriptInfo_Gap__0PAVigObjectRefMetaField_Core_3_A`
   — regex `_k_([A-Za-z0-9]+)_[A-Za-z0-9_]*?PAV(ig\w+MetaField)` pulls both
   the field name and its type out of that one symbol, and is more reliable
   than trying to match the (sometimes-missing — see below) quoted name
   string directly.
3. **Crucial disambiguator**: a class's `arkRegisterInitialize` often *also*
   touches fields it merely *inherits* (overriding a default value, e.g.
   `OpSetValue` overrides its inherited `_name` field's default description
   text to `"set value||="`) — these show the same `_k_` symbol pattern but
   are **not** new fields at this offset, and don't have one. The reliable
   split: only fields whose `_k_` reference appears **before** the first
   call to `_instantiateAndAppendFields` in the decompiled text are the
   class's own new, batch-registered fields with real new offsets; anything
   after is an inherited-field default override. Classes with *no*
   genuinely new fields (`OpTopLevelBehavior`, `AbstractPlacement`'s many
   pure-marker siblings, etc.) correctly show 0 real fields — this matches
   this document's own earlier prediction from the `igRewrite8` cross-check
   table below.
4. The real byte **offsets** don't reliably survive into the decompiled C
   for every class (some show a literal `local_20 = 0x140;`-style array,
   ScriptInfo Info itself decompiled it away) — the robust source is the
   **raw disassembly** instead: every batch-registered field's offset is
   pushed as a `MOV word ptr [ESP + N],0xOFFSET` (a `uint16` array, one
   entry per field, in the same declaration order as the name/type
   symbols above) immediately before the `_instantiateAndAppendFields`
   call. Zipping the ordered name/type list against the ordered offset
   list by position is reliable — spot-checked against every class in this
   session's batch, only known-benign 0-real-field classes ever showed a
   name/offset count mismatch.

**Result**: 75 of the 104 classes have at least one confirmed new field;
the other ~29 are genuinely fieldless marker/inherited-only classes (real
finding, not extraction failure). Stored as
`mad2iga/tfbscript_pc_field_schema.json` (same `FieldInfo` shape as
`wii_field_schema.json`, `"source":"pc"` throughout), merged into
`mad2iga.ClassSchema()` alongside the Wii schema and `ActorInfo`'s own
hand-curated PC overrides — PC data always wins. Kind strings map from the
real `igMetaField` subtype: `igObjectRefMetaField`→`ptr`,
`igFloatMetaField`→`float`, `igIntMetaField`/`igUnsignedIntMetaField`→`int`,
`igEnumMetaField`→`enum`, `igBoolMetaField`→`bool`,
`igStringMetaField`→`string`, `igMatrix44fMetaField`→`matrix44f`,
`igStructMetaField`→`struct` (opaque, e.g. `ScriptObject`'s
`getFunc`/`setFunc`/`resetFunc` delegate slots), `igRawRefMetaField`→
`rawref`, `igMemoryRefMetaField`→`memoryref`.

A representative sample of what's now real (see the JSON for all 75):
`ScriptInfo` (`opList`@0x20 ptr→`OpCodeList`, `masterVarList`@0x24
ptr→`OpCreateVariableList`), `ScriptSet` (`list`@0x20 ptr→
`ScriptGroupStack`), `OpSetValue` (`LHS`@0x24, `RHS`@0x28, both ptr),
`OpCode` (`internalFlags`@0x20 enum), `OpBranch` (`branchPC`@0x24 uint),
`OpFlow` (`flowVal`@0x28 enum), `ValueRHSVariant` (`varOp1`@0x8,
`arithOperator`@0xc enum, `varOp2`@0x10, `resultFunc`@0x14 struct,
`conditioner`@0x18), `ScriptReference` (`type`@0x20 ptr, `obj`@0x24 ptr),
`OpForEach` (`LHS`@0x2c, `dir`@0x30, `RHS`@0x34, `cachedSet`@0x38,
`cachedObject`@0x3c), `OpCreateVariable` (`varContainerType`@0x24,
`varContentsType`@0x28, `varName`@0x2c, `varIndex`@0x30), `StringInfo`
(`unicodeText`@0x20 rawref, `flags`/`integerDigits`/`decimalDigits`/
`length` at 0x24/0x28/0x2c/0x30), `Placement` (the common base practically
everything in this namespace ultimately derives from —
`majorIndex`@0x100, `script`@0x104, `execBehavior`@0x108,
`localVarList`@0x10c, `collision`@0x120, `shadowCastMethod`@0x130,
`receivesShadows`@0x134, `proceduralAlpha`@0x135), `CollisionInfo`
(`shapeType`@0x20, `transform`@0x30, `width`/`height`/`length`/`weight` at
0x70/0x74/0x78/0x7c, `isSolid`@0x84 bool).

**This makes real graph traversal possible for the first time.**
`mad2iga.ObjectGraph.ReadObjectListContainer` (already written for
`igNodeList`/`GeneratorInfoList` — see `IGZ_FORMAT.md`) turned out to work
*unmodified* for this namespace's containers too, confirmed directly
against a real level: `ScriptSet.list` resolves to a `ScriptGroupStack`
that *is itself* the generic container (no further indirection — its only
own new field, `editType`@0x18, sits right after the inherited
count/capacity/data-pointer fields at the same +8/+0xc/+0x14 offsets used
everywhere else), and `ScriptInfo.opList` resolves to an `OpCodeList` the
same way. Verified against `VolcanoRave/level.bld` (see "VolcanoRave
findings" below): a `ScriptGroupStack` with 11 real `ParticleInfo` members,
and — the actual milestone — **a real, fully class-tagged, 1283-opcode
compiled script**, starting with the canonical `OpPreScript → OpStartUp →
OpSetValue → OpCreateVariableArray → OpCheckValue → ...` sequence exactly
matching this document's own semantic model below.

**New tool**: `mad2repack script-dump <level.bld> <out.json>`
(`mad2repack/script_dump.go`) recursively renders every top-level
`ScriptInfo`/`ScriptSet` using this schema — expanding known container
classes (`OpCodeList`, `OpCreateVariableList`, `OpBranchList`,
`ScriptGroupStack`, `ScriptObjectSortedList`, `ScriptVariantList`) into
their real elements, and following every other `ptr`-kind field into a
nested rendering of the target object — capped at depth 12 to bound
pathological/cyclic pointer chains. Against `VolcanoRave/level.bld`: 44
`ScriptInfo` roots (opcode counts from 9 to 1938 — a very plausible spread
for tiny `viewportSettings_*` config scripts up through big per-song/level-
controller scripts) and 156 `ScriptSet` roots, ~16MB of JSON. This is a
structural dump, not a decompiler — it does not interpret opcode semantics
or resolve instance names.

### VolcanoRave findings (the rhythm-minigame level this was investigated for)

`VolcanoRave.bld`'s string pool (plain `strings` over the unpacked
`level.bld` — no decoding needed, these are literal readable text) confirms
the level's actual authored script/asset names, matching this doc's
existing semantic model almost exactly:
`Rave_LevelController.ai`, `Rave_TrackReader.ai`, six per-song
`Rave_Track_<Name>.ai` (`Angel`, `DanceLikeAnAfrican`, `Hulelam`, `IkoIko`,
`Shango`, `Tutorial`), `Rave_Dancer.ai`, `Rave_CoinDummy.ai`,
`Rave_Midgame_Cutscenes.ai`, `Rave_start_game_menus.ai`,
`NunchukGestureEvaluator.ai`, `LavaColumnController.ai`, plus object/field
label strings (`beat patterns`, `next beat time`, `total song beats`,
`pattern offset`, `song beat val`) that read like TFBTool property-panel
labels for `Rave_LevelController`'s own script variables. The real songs
are `SambaDeJaniero.ogg`/`.snds` plus five more (`Hulelam`→`angel-inst`?
— tentative, not confirmed) referenced from `builddata/win/*.snds` (no
`RaveInfoLib.dll` exists — unlike `animalChessInfoLib.dll` for the chess
minigame — confirming this whole minigame is implemented in pure
TFBScript, not a bespoke native class).

**Not solved directly: which `ScriptInfo`/`ScriptSet` *is* literally
"Track_Angel"** — i.e., instance naming for this namespace, via the
`igNamedObject`/Name Table mechanism specifically. Tried and ruled out
this session (see `CLASS_SCHEMA.md`'s "Naming investigation" for the
pre-existing `ActorInfo` version of this same problem) — **but see "Round
2" further below: a working content-based workaround (matching on
resolved variable names) identifies the same scripts anyway**, without
needing this fixed.
- `igNamedObject::getName()` (confirmed real and clean on PC, unlike the
  Wii-only `igStringRef` assumption — `igCore.dll`, reads `*(this+8)` as a
  **plain `char*`**, not a tagged ref) is inherited by every
  `ScriptObject`-derived class in this namespace (confirmed via
  `ScriptObject`'s own `arkRegisterInitialize` touching `_name` as an
  inherited-default override, never a new field). Read `+8` directly off
  six real `ScriptSet` instances in `VolcanoRave/level.bld`: got small,
  *sequential* non-zero integers (`0x168, 0x169, 0x16a, ...`, incrementing
  exactly 1 per object) — not plausible pointers, and not derived from any
  Section-0 segmented-pointer scheme tried. This has the exact signature
  of the same unsolved "ordinal into some other table" problem
  `CLASS_SCHEMA.md` already spent a full investigation on for `ActorInfo`'s
  Name Table, encountered here independently via a different field.
  Tried resolving these against `Track_Angel`'s own confirmed, unique
  string-pool byte offset (found directly, `strings`+`grep`) under every
  segmented-pointer convention already established elsewhere in this
  codebase (segID relative to Section 0, Section 1, and each other
  section) — no scheme produces a byte pattern that appears anywhere in
  the file matching these small ordinals. Whatever indexes into a real
  name here, it isn't any pointer/offset scheme already confirmed
  elsewhere in this codebase.
- `StringInfo.unicodeText` (a real, newly-schema'd `rawref` field,
  `@0x20`) reads as a flat `0` on every `StringInfo` instance sampled in
  `VolcanoRave` — plausibly legitimate (dynamic HUD text objects
  populated at runtime, not statically authored), not investigated
  further.
  - **Follow-up (later session)**: this `StringInfo`/`length` combination
    (offsets `0x20`/`0x24`/`0x28`/`0x2c`/`0x30`, all PC-verified) was
    revisited as the lead hypothesis for a still-unresolved `ENGLISH.pak`
    edit-crashes-the-game bug, on the theory that a string's length might
    be double-recorded on a `StringInfo` object and left stale by
    `mad2iga/pak.go`'s `CompilePak` when a string's length changes. **That
    specific hypothesis was checked and refuted**: `StringInfo` does not
    appear in any real `.pak` file's own class directory at all (checked
    against 6 real `.pak` files across 3 levels/2 install trees/multiple
    languages) — it is exclusively a `level.bld` runtime class, consistent
    with `unicodeText` reading flat `0` here (never statically populated
    from a `.pak`'s pool at all). The investigation instead found a
    different, real, confirmed-stale field — `LanguagePackInfo.stringDict`,
    a `.pak`-file-only field encoding the string pool's own serialized
    byte length — now fixed. See `docs/IGA_FORMAT.md`'s "Bug 3" for the
    full writeup and evidence.
- Chased the audio side too, since the actual song filenames are known,
  unencoded text (`SambaDeJaniero.ogg`, `angel-inst_Fill_1.ogg`, ...):
  `SoundInfo.soundDataInfo` (new PC-verified field, `@0x60`, ptr) → 
  `SoundDataInfo.tfbSound` (`SoundInfoLib.dll`, `@0x28`, ptr) → `tfbSound`
  turns out to be a pure `setAbstractProxy`/`setWriteProxy` wrapper around
  the real `igSound` class (`igAudio.dll`, not yet investigated) — its own
  only field, `_head`@0x10, is an `igRawRefMetaField`, almost certainly a
  load-time cache slot rather than a static filename reference. Not
  followed further this session (would need `igAudio.dll` imported and its
  own `igSound` schema cracked the same way).

**Round 2 (same session, continued): (b) turned out to be the tractable
path.** Rather than solving instance naming for `ScriptSet`/`ScriptInfo`
generally, found two independent mechanisms that make the opcode data
itself readable without it:

**1. `OpCreateVariable.varName` genuinely resolves to real variable names**
— unlike `ScriptSet`'s inherited `igNamedObject._name` (see above, still
unsolved), reading `OpCreateVariable.varName` (a small integer, same
"ordinal" shape) through `mad2iga/level.go`'s pre-existing — and, per
`CLASS_SCHEMA.md`, previously-considered-unreliable — `stringsList[ordinal
+ 1]` heuristic (string pool starting at `sec0Off + strPoolOff + 0xA840`,
same magic gap) produces genuinely correct, sensible variable names:
`"total song beats"`, `"combo counter"`, `"beat patterns"`, `"beat FX
rates"`, `"song init time"`, `"waiting to respawn monkey"`, `"got
monkey"`, `"minigame done"`, etc. — all against real `OpCreateVariable`
instances in `VolcanoRave/level.bld`. This is the *same* lookup table and
*same* indexing scheme that failed for `ScriptSet._name` (which resolves
to unrelated `.lvl` level-manifest filenames through this exact
mechanism, confirmed side-by-side) — evidently the ordinal-to-string
mechanism is real and does work, just not for every field that superficially
looks like it should use it. Promoted to a real, reusable API:
`ObjectGraph.StringPool()` / `ObjectGraph.ResolveStringOrdinal()`
(`mad2iga/objgraph.go`), wired into the schema as a new `"string_ordinal"`
`Kind` (currently only used for `OpCreateVariable.varName` — do not assume
it's safe for other `igStringMetaField`/ordinal-shaped fields without
independently checking the same way, e.g. `StringInfo.unicodeText` reads a
flat `0` always and hasn't been shown to use this table at all).

Applying this across every `ScriptInfo`'s `masterVarList` and searching for
beat/pattern/song-related variable names immediately identifies the actual
`Rave_TrackReader` script by content — no instance name needed: one
specific `ScriptInfo` (649 opcodes) has variables including `"beat
patterns"`, `"beat FX rates"`, `"total song beats"`, `"song init time"`,
`"combo counter"`, `"players' combo counters"`, `"players' maintained combo
vals patterns"`, and its own variable list literally contains the strings
`"C:/TFB/Content/Levels/VolcanoRave/scripts/Rave_TrackReader.ai"` and
`"...Rave_Track_DanceLikeAnAfrican.ai"` as resolved values — this is
`Rave_TrackReader` itself, the shared per-frame beat-processing logic every
song's script presumably feeds its own pattern data into. Similarly
identified (by the same content-matching, still without solving instance
naming): a 46-variable `ScriptInfo` referencing `Track_Angel` /
`Rave_Track_Angel.ai`, a 62-variable one referencing `Track_Tutorial` /
`Rave_Track_Tutorial.ai` (and `Track_IkoIko`), and an 88-variable one
referencing `Track_IkoIko` / `TrackReader` directly.

**2. `RHSValueStack.type` is a type tag, not a resolvable pointer** —
despite its `igObjectRefMetaField` kind (which made `ObjectGraph.FieldValue`
try to resolve it as a segmented object pointer), its raw value is
actually a small constant discriminating how to reinterpret the sibling
`value` field's raw 32 bits. Confirmed empirically against real data (not
guessed): `type` raw `0x6A` pairs with `value` reading as plausible small
int32s (`0, 1, 4, 5, 10, 15, 20, 30, 40, 80, 120, 720, 20000, ...`); `type`
raw `0x6B` pairs with `value` decoding *bit-exact* as float32 literals
(`1065353216 → 1.0`, `1048576000 → 0.25`, `1120403456 → 100.0`). Two more
tags seen (`0xA1`, `0xA7`) don't decode cleanly as either — most likely an
object-reference variant and something else not yet confirmed. Wired into
`mad2repack/script_dump.go` (`decodeRHSValueStack`): every rendered
`RHSValueStack` now carries `value_decoded`/`value_decoded_kind` alongside
the raw fields when the tag is one of the two confirmed ones.

**What this enabled, concretely**: pulled every float-literal (`type ==
0x6B`) `OpSetValue` RHS out of `Rave_TrackReader`'s own 649 opcodes —
values `0.25, 0.35, 0.5, 0.65, 0.8, 1.0` — all in a normalized `0..1`
range, reads like tuning parameters (blend weights, hit-window
percentages) rather than literal per-song beat *timestamps*. Also checked
every `OpCheckValue` (the opcode literally named "check value," the
obvious place to look for `if (currentTime > X)`-shaped beat gating)
across all 44 scripts for float-typed conditions: found only a handful,
none forming a long or monotonic sequence (e.g. `120, 60, -60, -120` in
the `NunchukGestureEvaluator`-linked script — clearly facing/angle
thresholds in degrees, not beat times). Also checked every `ScriptSet`'s
member-count directly (`ScriptGroupStack` sizes) for a large flat
collection that might be a beat-pattern array: the biggest is 100
`ActorInfo` (a spawn pool), then groups of `SoundInfo`/`SpriteInfo`/
`StringInfo` in the 7-26 range (per-character sound variations, UI text
sets) — nothing resembling "one entry per beat."

**Honest bottom line for "beatmaps," updated**: the per-song beat data
is *not* a simple flat array reachable via `ScriptSet.list`, and doesn't
show up as an obvious literal-float-timestamp comparison chain in the
handful of large scripts checked. Two live hypotheses, neither confirmed:
(a) beat timing is computed from a continuously-updating time/beat-counter
variable (`"song init time"`, `"combo counter"`) compared against
per-pattern data stored in an object type not yet reached by this
session's traversal (e.g. a `Placement`/`Sequence` matched by *timing
metadata on the object itself* — `Sequence.playbackPercent`/
`mediaName` are already schema'd and unexplored) rather than a `ScriptSet`
grouping; or (b) the pattern is genuinely opcode-encoded but via a
different opcode than `OpCheckValue`/`OpSetValue` alone — full control-flow-
aware decoding (following `OpBranch.branchPC`/`OpFlow.flowVal` to actually
reconstruct if/else and loop structure, not just listing opcodes linearly)
hasn't been attempted, and might reveal the pattern only once opcodes are
read in real execution order/grouping rather than flat array order. Both
are concrete, scoped next steps, not "keep guessing" — this round's real
contribution is the two new confirmed decode mechanisms (variable names,
typed literals) plus ruling out the two most obvious "make it a data
table" hypotheses with direct evidence.

### Round 3 (same session, continued further): control flow, real numeric data, and the actual remaining blocker

**Control flow reconstructed.** `OpBranch.branchPC` (inherited by only some
opcode classes — `OpCode`(`internalFlags`@0x20) → `OpBranch`
(`branchPC`@0x24) → subclass fields starting at 0x28+) is a real, working
forward-jump target: confirmed by checking, for every opcode class, whether
*that class's own* schema claims offset 0x24 (if so, it's that class's own
field — e.g. `OpSetValue.LHS`@0x24 — and the class does **not** inherit
`OpBranch`; if not, 0x24 is free and is `OpBranch`'s inherited field).
Scanned all branch targets across `Rave_TrackReader`'s 649 opcodes: **308
forward branches, 0 backward, 14 sentinel (`0xFFFFFFFF` = "no branch" /
end-of-script)** — confirms this is a purely forward-branching structured
bytecode (typical "skip this block if condition false" shape); loop
repetition (`OpForEach`/`OpLoopValue`) is therefore an *interpreter-level*
behavior (the runtime re-invokes the loop opcode itself after its body
finishes) rather than an explicit backward jump instruction. This means a
real block/AST reconstruction is just: for any `OpBranch`-inheriting
opcode at index *i* with `branchPC = j`, indices `[i+1, j-1]` are its body;
recurse. Not yet implemented as actual pseudocode output, but the
mechanism is confirmed and the "classes claiming offset 0x24" exclusion
list is derivable directly from the existing schema JSON.

**`ValueStack.otherPoint` ruled out as the variable-identity field.**
Decompiled `ValueStack::resolveAnchorStack` (`ScriptInfoLib.dll
0x10022750`) directly: `otherPoint` (typed `PositionStack` per
`getOtherPoint`'s own signature) is a **spatial position-anchor chain**
(resolves through `ScriptVariant::resolveArgStack` into an
`_anchorPos`/`_anchorObj` pair — used for position-relative value
expressions like "this offset by that anchor object"), not a
general-purpose "which variable does this `OpSetValue` write to" field.
Dead end for the naming question, but a clean, confirmed one — don't
re-investigate `ValueStack`'s own fields for this purpose again.

**`OpFindVariable.varName` is also `string_ordinal`-resolvable** — same
`igStringMetaField`/small-ordinal shape as `OpCreateVariable.varName`,
confirmed to resolve correctly the same way (e.g. two real lookups in
`Rave_TrackReader`: `"sprite temp time"`, `"monkey name string"`). Updated
in the schema alongside `OpCreateVariable.varName`.

**The actual remaining blocker, precisely identified**: both
`OpCreateVariable.varIndex` (every one of `Rave_TrackReader`'s 77
variables) and `OpFindVariable.cachedObject` (every instance checked) are
**always the runtime-unbound sentinel** (`varIndex = 0xFFFFFFFF`,
`cachedObject = 0`/null) in the shipped file — variable *slot binding* is
resolved by the interpreter at load/run time, not baked in by the
compiler. This means there is no static field anywhere that says "this
`OpSetValue` writes to variable X" — that binding only exists as an
*implicit runtime stack-machine state*, established by whichever
`OpFindVariable`/`OpCreateVariable` executed earlier in the *actual
execution order* (which requires the control-flow reconstruction above to
determine, since it isn't simply "the previous array element" once
branches are involved). Turning the opcode dump into real pseudocode with
correct variable names on both sides of every assignment requires
building this stack simulation — a real (if small and constrained)
interpreter for this VM, not just more field-offset archaeology. This is
the precise, scoped piece of remaining work, not a vague "needs more
reversing."

**Real numeric data extracted, concretely** (via the confirmed `type=0x6A`
int32 / `type=0x6B` float32 tags on `RHSValueStack.value`, walking every
`OpSetValue` in program order): the script whose variables reference
`Track_Tutorial`/`Rave_Track_Tutorial.ai` contains a clean monotonic
integer sequence — **`5, 10, 15, 22, 30, 38, 48, 60, 75`** — at opcode
indices 136-160, increasing deltas (5,5,7,8,8,10,12,15) exactly matching
the shape of a scripted tutorial difficulty/hint-count ramp (not a literal
per-beat timestamp table, but real, meaningful authored data, not noise).
The `Track_Angel`-referencing script similarly has extensive structured
int/float data (repeated `50,40,0,3` / `100,100,1` / `10,10` quadruples,
values like `400`, `170`, `240`) that reads like per-effect parameter
blocks (position, duration, count) rather than a flat beat array — plausible
given `Rave_TrackReader` (not the per-song scripts) is where the actual
generic per-beat *processing* logic lives, per its own variable names
(`"beat patterns"`, `"total song beats"`, `"song init time"`) and the fact
it has variables literally named `"TrackReader"` and
`"Track_DanceLikeAnAfrican"` (parameters — this confirms `TrackReader` is
a generic routine invoked *with a reference to whichever song's data set is
currently active*, not per-song-duplicated logic).

### Round 4 (same session, continued further): the real execution model, confirmed to source, and where static analysis actually stops

Decompiled the actual `execute@<Class>` virtual-override methods directly
(not just the field-registration functions) for the opcodes that matter
most for control flow and variable binding — `OpFindVariable`,
`OpSetValue`, `OpCheckValue`, `OpForEach` — all in `ScriptInfoLib.dll`.
This is real, source-level confirmation of the execution model, not
inference from data shapes:

- **A global program counter is real and named in the binary**:
  `___PC_OpCode@TFBScriptInfo@Gap` (a plain static global). Every branching
  opcode's `execute()` ends with `if (<condition failed/not found>)
  ___PC_OpCode = this->branchPC;` — i.e. `branchPC` is exactly the
  "else, skip to here" target confirmed at the source level, not just by
  pattern-matching observed values (see Round 3's forward-branch-only
  survey, now explained: this *is* a real forward-only structured-bytecode
  interpreter with a single global PC, no call-stack-based control flow at
  the opcode level).
- **There is a genuine global "arg stack"**, shared across every
  currently-executing script (confirmed via
  `ScriptVariant::resolveArgStack`, `ScriptInfoLib.dll 0x1001a3b0` — reads
  `param_1[2]` at a fixed offset matching the generic `igObjectList` count
  field, i.e. a real, singular, already-allocated list object; walks
  backward through it type-matching against `SetVariant`/
  `ReferenceVariant`/`AbstractValueMeasurement`). `OpFindVariable` and
  `OpForEach`, on success, both call `append_igObjectList` — literally
  *pushing* their resolved result onto this same stack.
  `OpSetValue`/`OpCheckValue`'s `execute()` both call
  `ScriptVariant::resolveValueArgStack` (`0x1001a4e0`, itself just
  `resolveAnchorStack` for position-anchor chaining — see Round 2's
  `ValueStack.otherPoint` finding — followed by a delegate straight into
  the *same* `resolveArgStack`) to *pop/peek* their LHS target. This is a
  real, singleton, backward-walking stack machine, not a per-object
  pointer field anywhere in the serialized file.
- **`OpForEach` does not loop internally** — confirmed directly:
  `execute@OpForEach` (`0x1001b6f0`) resolves its target set once, then
  picks **one single element per invocation** based on `dir` (`0` = advance
  index forward, `1` = advance backward, `2` = build-and-quicksort a
  randomized index permutation the *first* time, then step through it),
  pushes that one element onto the arg stack, and returns — no inner loop
  over the whole set. The "loop" only exists across *multiple separate
  invocations* of the same `OpForEach` node over real time (i.e. the
  outer game-tick scheduler re-enters this exact script position
  repeatedly), matching Round 3's "0 backward branches" finding exactly:
  there's no backward branch because there's no in-opcode loop to branch
  back from. This is the single most important structural fact for
  understanding "beatmap" playback: whatever set `OpForEach` walks here
  advances **one item per tick/beat**, which is exactly the shape a
  rhythm-game pattern player needs.

**Built a working (if intentionally rough) recursive-descent pseudocode
renderer** as a proof of concept (not committed as a polished `mad2repack`
subcommand — see below for why), applied to `Rave_TrackReader`'s 649
opcodes: reconstructs real nested `if(...)`/`foreach(...)` blocks from
`branchPC`, resolves literal `RHSValueStack` values via Round 2's type-tag
scheme, and tracks a best-effort "most recently found/created variable"
heuristic for `OpSetValue`/`OpCheckValue`'s implicit LHS. Concretely
useful output came out of it immediately — e.g. real nested structure like
`if (monkey name string == 0) { ... }`, `find-variable 'sprite temp
time'`, literal blocks assigning `0, 1, 20000, 0, 0, 0` then (in a sibling
branch) `15, 120, 80, 0, 0, 0` — clearly two parameterized variants of the
same effect (position/duration/count-shaped tuples), consistent with
Round 3's numeric-block observations. **Known, unfixed limitations**: (1)
diamond-shaped joins (two branches converging on shared trailing code —
seen at every `OpFlow`-wrapped `else`) get rendered twice, once per
incoming path, since the renderer doesn't merge convergent control flow —
cosmetic, doesn't corrupt the underlying data, but not fit to trust as
literal source order without dedup; (2) the "most recently found
variable" heuristic is naive linear tracking with no real scope
awareness, and **most `OpSetValue` targets in `Rave_TrackReader`
specifically are never preceded by any `OpFindVariable` at all** — only
`"sprite temp time"`/`"monkey name string"` ever get looked up by name in
this script; everything else (including every `"beat patterns"`/`"total
song beats"`/`"combo counter"`-related access) must be going through some
other binding path this session didn't crack (most likely a direct
compile-time slot index into the *owning* script's `masterVarList`,
established by the compiler rather than a runtime name search — plausible
since `OpCreateVariable.varIndex` reads as the unbound sentinel in every
sampled instance, meaning if such a direct-index path exists it isn't
using that field either; not yet located).

**This is the honest, precise stopping point for static analysis alone.**
Everything up to "what opcode runs when, with what literal operands, in
what branch structure" is now solid and tool-backed. Resolving *which named
variable* every single opcode reads or writes — the last piece needed for
fully faithful pseudocode — requires either (a) finding the compile-time
direct-slot-index mechanism just described (a bounded, concrete next
target: find the accessor that reads an `OpCreateVariable`/local-slot
array by a literal compiled-in index, the same class of investigation as
everything else in this document), or (b) building an actual interpreter
that simulates the confirmed global arg-stack across the real call graph
(scripts invoking other scripts, e.g. whatever calls into
`Rave_TrackReader` passing it a specific song's pattern set) — a
qualitatively different, larger undertaking than anything else done in
this document (writing a small VM, not more static field-offset
archaeology), and out of scope for this session.

### Round 5: pivoting to a runtime hook instead of a static interpreter

Given the arg-stack is genuine runtime state (Round 4), the fastest way to
resolve "which variable does this opcode touch" turned out to be recording
it directly from the live game process instead of simulating it — this
project already has extensive precedent for exactly this
(IAT/vtable-hooking mods throughout `mad2/mods/`).

**New diagnostic mod: `mad2rhythmlogger`** (`mad2rhythmlogger/src/rhythmlogger.cpp`,
deployed as `mad2/mods/mad2rhythmlogger.dll`, **not** a gameplay mod — pure
logging, chains through to the real behavior unmodified). Vtable-patches
`execute()` (confirmed via Ghidra to sit at the identical vtable slot — 29,
byte offset `0x74` — across `OpSetValue`/`OpCheckValue`/`OpForEach`/
`OpFindVariable`'s independent vtables) directly in `ScriptInfoLib.dll`,
computing each vtable's actual runtime address as
`GetModuleHandleA("ScriptInfoLib.dll") + (staticGhidraAddress - 0x10000000)`.
For each hooked call: reads the opcode's own fields (already fully
schema'd — LHS/RHS/dir/etc), calls through to the real `execute()`
unmodified, then reads whatever field/global it populates as a side effect
(`ScriptVariant::_resolvedToObj` for `OpSetValue`/`OpCheckValue`,
`OpForEach.cachedObject`, `OpFindVariable.cachedObject` — all confirmed by
directly decompiling each class's `execute()` body, see Round 4). Logs
every call to `<exe dir>/logs/rhythmlogger.log`.

**New tool: `mad2repack rhythm-log-decode <rhythmlogger.log> <level.bld>
<output.txt>`** (`mad2repack/rhythm_log_decode.go`). The logged pointers
are raw runtime addresses in a completely different address space than
`level.bld`'s own file offsets — this empirically determines the constant
delta between the two (matching logged `this=` addresses against a flat
collection of every known script-namespace object's real file address,
picking whichever single delta value gets corroborated by the most log
lines), then translates every logged address back to file space and
annotates it (resolving `OpCreateVariable`/`OpFindVariable` names via the
string-ordinal mechanism, `RHSValueStack` literal values via the type-tag
scheme — both already-solved statically, see Round 2/3) — producing a
genuinely readable trace of what the rhythm minigame's script logic
actually did during a real play session.

**Status: built and deployed, not yet run.** This needs an actual
VolcanoRave play session (ideally through at least one full song) to
produce real log data — nothing in this round is a result *from* running
it, only the tooling to make sense of the log once it exists. Next
concrete step once a real `rhythmlogger.log` exists: run
`rhythm-log-decode`, then look specifically at `FOREACH` lines whose `LHS`
resolves near the known `Rave_TrackReader`/`Rave_Track_<Song>` script
addresses — `cachedObject` on each of those is, call by call, literally
"which pattern got picked this tick," which is the actual beatmap data
this whole investigation has been after.

### Round 6: real play-session data — the pipeline is validated, the last piece isn't found yet

A real play session was captured (~1.96M log lines, one `mad2.log` line
confirming `ScriptInfoLib.dll base=0x06EA0000` and clean hook install).
Decoding it surfaced a real methodological trap and then real,
mechanism-confirming data:

**The trap**: `findDelta`'s first version sampled only the first N lines
of each log kind, in file/chronological order. On a whole play session
(menus → character/song select → actual gameplay → possibly more menus)
that risks the sample landing entirely *before* VolcanoRave ever loaded,
producing a spuriously-confident wrong delta (a "top" candidate with 4173
votes that turned out to be real but from the *wrong part of the
session* — mostly `(no object)` output, one repeating `OpForEach` always
returning `cachedObject=null`). Fixed by sampling via a stride spread
across each kind's *entire* occurrence in the log rather than a prefix —
this immediately surfaced a new, far stronger delta candidate (10275 votes
vs. the old one's 5439) with **1,069,582** lines resolving to real
VolcanoRave objects (of 1.96M total — the rest is the same hooks firing
for other levels/menus in the same session, correctly excluded). Also
tightened the hit filter to require the *exact expected class* at the
translated address (a `FOREACH` line's `this` must resolve to `OpForEach`
specifically, not merely *some* valid object) — the looser check had let
coincidental wrong-class matches through.

**What the real data confirms, concretely**: opcodes from
`Track_Tutorial`/`Track_IkoIko`-referencing scripts really did fire during
play, with real resolved targets — e.g. a captured `SETVALUE` writing
literal `int=9` and, moments later, `int=19` into a genuine
`OpLoopValue@0x35FDD0` inside the `Track_Tutorial` script (the same script
whose static `5,10,15,22,30,38,48,60,75` difficulty-ramp sequence was found
in Round 3 — this is independent runtime confirmation that script and its
logic were genuinely exercised). This is real validation that the whole
chain — vtable hook → runtime log → empirical delta → static schema
cross-reference — works end to end, not just in theory.

**What it doesn't show yet**: a literal per-beat "catcher position" table.
The `ValueInfo` objects actually touched (11 distinct addresses, all in one
tight cluster `0x227B9C`-`0x227E90`, referenced from *multiple different*
Track_* scripts) are evidently **shared global tuning constants** (values
50, 1.5, 1, 150, -1, 0 — read/written tens of thousands of times each),
not per-song pattern data. `OpForEach.cachedObject` mostly resolves into a
separate, untracked address range (`0xFD6xxxxx`-`0xFD7xxxxx`, consistently
— almost certainly the `igMemoryPoolTemporary` runtime scratch pool
`OpForEach::execute` was seen constructing in Round 4, not level.bld's own
loaded Section 1) — when it does resolve into known territory, it's
UI-related (`StringInfo`, `SpriteInfo`), not pattern-tag data. The specific
`OpForEach` that walks the actual per-song ball-set/catcher-position
pattern hasn't been conclusively identified in this pass — it may not have
fired during this particular session's play (if gameplay was brief/on an
easy song with few sets), or its target set may only ever resolve through
that temp pool (meaning the real pattern data lives somewhere this
capture's field selection doesn't reach — e.g. needs `OpForEach.RHS`'s
*evaluated* index or a currently-unhooked opcode). Concrete next step if
continued: capture a longer/harder session (more ball-sets, more misses),
and extend `rhythmlogger.cpp` to also hook `OpSlideValue`/`OpStartSequence`
`execute()` (visible in the class list but not yet hooked) since those are
plausible carriers of the same "one event per tick" shape `OpForEach`
turned out to have.

### Round 7: OpStartSequence's parent-chase confirmed a dead end; a real live beat-gate family found via OpCheckValue

Extended `rhythmlogger.cpp` to hook `OpSlideValue`/`OpStartSequence` per
Round 6's suggestion, and separately added a live parent-chase to
`OpStartSequence`'s hook: `cachedObject` (a runtime `ClonedSequence`) →
`parent`@0x38 → assumed static `Sequence` template → `mediaName`@0x20 read
as a live C-string pointer. Captured a whole-session, all-6-opcodes,
no-level-gating log this way — it came out to **18GB** and made the
capturing machine difficult to use while it ran (this surfaces again below).
`mediaName` never resolved once in that entire capture (0 hits) — traced to
two independent problems, not one:

1. The captured log's `STARTSEQUENCE` lines only ever had 4 fields
   (`this`/`LHS`/`indexRHS`/`cachedObject`), never the 5-field
   `.../parent=.../mediaName=...` shape the deployed DLL's own embedded
   format strings show — i.e. the actual capture ran an **intermediate
   build**, before the parent-chase code existed, despite `mad2.log`
   correctly reporting `OpStartSequence` as hooked (that log message text
   was already current; the hook body underneath wasn't yet).
2. Once rebuilt and re-captured with the real parent-chase, `mediaName`
   *still* never resolved — `Sequence.mediaName`'s schema entry
   (`tfbscript_pc_field_schema.json`) turned out to be an **unverified
   `igStringMetaField` guess**, kind `"string"` (a raw pointer
   interpretation), never independently confirmed the way
   `OpCreateVariable.varName`/`OpFindVariable.varName` were (Round 2) —
   despite this doc's own Round 2 text implying it was already schema'd.
   Per Round 2's own explicit warning ("do not assume it's safe for other
   `igStringMetaField` fields without independently checking"), it wasn't
   safe, and wasn't checked. Worse: no `Sequence`-classed object is even
   *reachable* from the static script graph at all (0 found via
   `collectScriptObjects`'s walk from every `ScriptInfo`/`ScriptSet` root,
   0 as a top-level object) — so the hypothesis was never checkable
   statically either, only at runtime.

**With a real, level-gated, per-line-delta-resolved capture (see below),
`parent` was checked directly**: of 552 `STARTSEQUENCE` calls with a
non-null `parent`, 504 resolve to no object at all, and the other 48 split
across **two unrelated engine namespaces** — `Gap::TFBModelInfo::AnimationInfo`
(fields like `walk`/`run`/`jump`, nothing string-shaped) and
`Gap::TFBSoundInfo::SoundInfo` (fields like `effectsMasterVolume`/`pitch`) —
neither is `Gap::TFBScriptInfo::Sequence`. This is the signature of
coincidental address collision under `ClassSchema`'s bare-name lookup (see
`CLASS_SCHEMA.md`'s namespace-collision caveat), not a real signal.
**Conclusion: `OpStartSequence.cachedObject.parent` is not currently a
usable path to per-song media names — closed, not a "needs more capture"
gap.**

**Two lasting fixes came out of chasing this down, independent of the dead
end above:**

- **Level-gating.** `mad2rhythmlogger.cpp` now resolves
  `mad2levelredirectmod`'s `Mad2LevelRedirect_GetCurrentLevel` (via the
  shared `mad2leveldetect.h` header already used by `mad2gameeffectsmod`)
  and every hook is a no-op (still calls through to the real `execute()`
  unconditionally, just skips its own field-reads/`WriteLine`) unless the
  current level case-insensitively matches `"VolcanoRave"` — checked at
  most 4x/second, not per-call. First attempt used an exact `==` string
  compare and silently logged nothing all session: the game itself opens
  the archive as `VOLCANORAVE.ARC` (all caps, confirmed via
  `mad2levelredirectmod`'s own `[LevelLoad]` log lines), not mixed-case
  `"VolcanoRave"` — fixed with `_stricmp`. Combined with narrowing the
  active hook set per-session to just what's actually being investigated,
  this took a real capture from 18GB down to single-digit MB/low hundreds
  of MB, entirely eliminating the "makes the machine hard to use while
  playing" problem Round 6/this round's first attempt both hit.
- **Per-line delta resolution in `mad2repack rhythm-log-decode`.** The
  existing single-global-delta scheme (`findDelta`, one winner-take-all
  runtime↔file address offset for the whole log) turned out to badly
  undercount real data: a real play session commonly loads VolcanoRave more
  than once (retry a song, return to menu and back in), each load landing
  Section 1 at a different runtime base — so there is no single correct
  delta for a whole file, only one correct delta *per load*. A real capture
  produced **10 delta candidates in an exact vote tie**; the old scheme
  picked one arbitrarily and treated the other 9/10 of real gameplay data
  as noise (12.5% kept). Replaced with `findDeltaCandidates` (returns every
  candidate above `minVoteFraction` of the top vote count, not just the
  winner) and `resolveLineDelta` (tries each candidate per log *line*,
  keeping whichever one makes that line's own `this` opcode resolve to a
  real object of the expected class) — recovery went to **82-95%** kept
  across the two follow-up captures in this round. This is a real, lasting
  fix to the tool, independent of whether `OpStartSequence` itself pans
  out.

**The actual new lead: a live "beat-gate" `OpCheckValue` family.** With
`OpSetValue`/`OpCheckValue`/`OpFindVariable` re-enabled (level-gating made
this cheap again) and a real "Dance Like An African" / Medium / perfected
play session captured, grouping every `CHECKVALUE` line in `Rave_TrackReader`
by its own static opcode address surfaced several `OpCheckValue` nodes
whose `cachedValue` (the live-resolved LHS value at check time,
`cachedType`-tagged — see below) **strictly monotonically increases across
the whole session** — e.g. `1, 3, 5, 7, 9, 13, 14, 15, ..., 61` — a real,
live-updating counter (almost certainly the same `"combo counter"`/`"total
song beats"` variable identified by name back in Round 2). Far more
useful: **7 distinct `OpCheckValue` nodes all share the exact same
`LHS=ValueStack@0x366054`** (i.e. all test the *same* live counter) but
each against a **different literal integer RHS**: thresholds `13, 14, 16,
18, 19, 132, 160` were captured this session (`relOp=1` for all seven —
operator semantics not yet decoded). Irregular, non-uniform spacing across
a wide range (13 through 160) is exactly the shape of authored
*discrete event* data (e.g. "at beat 13, do X; at beat 132, do Y"), not
noise or a uniform frame-tick counter. Only 7 nodes fired in this
particular session — plausibly because "Easy"/one song only exercises a
subset of a larger, denser table; a longer/harder/different-song session
should surface more. **This is the closest thing to real per-song beatmap
data found in this whole investigation, though still short of a complete
table** — concrete next step if continued: capture Hard difficulty and/or
a different song, re-run the same `LHS=ValueStack@0x366054`-family
extraction, and check whether the RHS thresholds found are the same
(shared engine-wide gate) or different (genuinely per-song data).

**`cachedType` decoding note**: `OpCheckValue.cachedType` (the mod's own
runtime field, not `RHSValueStack.type`) turned out to be a raw pointer to
a shared type-descriptor singleton (confirmed empirically: only ~5 distinct
values across 2.3M real calls this session, e.g. `0x04CF7E94`/`0x04CF7E5C`),
**not** the small file-stable `0x6A`/`0x6B` tag `RHSValueStack.type` uses —
it isn't in Section 1's own address range, so it isn't delta-translatable,
and isn't confirmed stable across different game launches/DLL base
addresses either. `rhythm_log_decode.go` deliberately does not hardcode a
mapping from these raw values to "int"/"float" — it prints the raw
`cachedType` plus both an `int32` and `float32` reinterpretation of
`cachedValue`, leaving the type judgment to whoever's reading a specific
session's output (in this session: `0x04CF7E94` values were plausible small
ints, `0x04CF7E5C` values were plausible small floats — both checked
by hand, not assumed).

### Tooling used (Ghidra scripting, this session)

`run_script_inline` (Ghidra MCP bridge) was necessary, not just
convenient, for the bulk `arkRegisterInitialize` extraction — paging
through `list_exports`/manually decompiling 104 functions one at a time
would have been the alternative. It's gated behind
`GHIDRA_MCP_ALLOW_SCRIPTS=1`, checked by the actual Ghidra GUI process's
own environment (not the stdio bridge subprocess Claude Code launches, and
not any already-running HTTP-mode bridge instance) — enabling it requires
closing and relaunching Ghidra itself with that env var set (this repo's
`~/Projects/ghidra-mcp/run_gui.sh` now exports it unconditionally), then
reconnecting the MCP session and re-opening the project's programs
(`open_program`) since a fresh Ghidra process starts with none open.

## Two distinct formats, easy to conflate

1. **`.ai` files** — the *authoring-time* source format, edited directly by
   TFBTool (the internal level editor). Confirmed **not present anywhere in
   Mad2's shipped assets** (`find ... -iname "*.ai"` over the entire
   extracted corpus: zero hits) — only referenced by path as strings inside
   compiled data (e.g. `"C:/TFB/Content/Levels/Includes/Player_GenericStuff/
   scripts/ActivateHealthPowerup.ai"`, seen in `level.bld`'s string pool).
   These paths are source-file references from whoever authored the level,
   not files the shipped game reads.
2. **The compiled, in-engine representation** — a real object graph
   (`ScriptSet`/`ScriptObject`/`Op*` instances, same reflection/`igObject`
   mechanism as every other class covered in `CLASS_SCHEMA.md`), embedded
   directly in `level.bld`'s Section 1. **This is what's actually shipped**,
   and it's what any Mad2 script-editing tool needs to target.

These are genuinely different binary formats (confirmed by reading both
reference projects' source — see below), not two views of the same bytes.
The `.ai` format is a flat, hand-rollable opcode stream; the compiled form
is full IGZ objects, one per opcode, linked by real pointers — much more
expensive per-opcode but consistent with how every other class in this game
is represented.

## Reference #1: `.ai` binary format (`MaxStache/madagascar-tfbtool`)

A public from-scratch Python reader/pretty-printer, **targeting Madagascar
1**, not Mad2 — flagged by the user as possibly not directly applicable,
and that caveat holds: Mad2 doesn't ship this format at all (see above), so
this project's *binary reader* has no direct target in this repo. Its value
here is entirely as a **semantic reference** for what a script actually
*means* (opcode names, parameter shapes), corroborated independently by
reference #2 below (a *different* game, *different* format, same semantic
vocabulary — e.g. both separately arrived at "OpSetValue does `lhs = rhs`",
"OpForEach has a direction and a cached current item").

Format shape (`TFBScriptFile.py`): magic string (length-prefixed) → 4
unknown bytes → three length-prefixed string tables (opcode names, global
variable refs, local variable refs) → a flat instruction stream. Each
instruction: 1-byte opcode index (or `0xFF` for one of a fixed sequence of
"control block" opcodes — prescript/startup/shutdown/behavior-N, tracked by
a running counter, not stored per-instruction) → 4-byte flags (3-bit
flow-control code, `noHandler`/`noElse`/`runtimeScratch` bits, a 21-bit
descendant-span count for tree nesting) → 1-byte payload size → payload.
References (`TFBScriptReference.py`) are 4-byte bitfields: `index` (top 18
bits) selects null / a fixed set of dynamically-resolved builtins (`self`,
the nearest enclosing `for each`'s current item, etc.) / a global-table
slot / a local-table slot, depending on which numeric range it falls in;
`member`/`scope`/`sub` are smaller bitfields addressing into whatever the
index resolved to. RHS values (`TFBScriptRHS.py`) are tag-prefixed variants:
a 4-byte reference, a raw int32, a float32, an RGBA color, or a pair of
16-bit halfs — optionally followed by an operator byte and a second RHS for
a binary expression, with a subtle, explicitly-documented-as-load-bearing
detail: whether to read that second half depends on the *original engine's*
runtime type-compatibility check between the two operands (only knowable
with the live object graph in hand, per that project's own comment), so the
static decoder instead trusts the payload's declared byte length, which the
original authoring tool sized to match — verified byte-exact against 42,391
real RHS reads in that project's own (Mad1) corpus.

## Reference #2: `bonesinmysoup/igRewrite8` (branch `trapteam-but-real`)

A mature, actively-maintained C# `igObject` model + live IGZ editor for a
*different* Alchemy-engine game (branch name strongly implies Skylanders:
Trap Team). This is **not the same game as Mad2**, and none of its byte
offsets are assumed to transfer — but its `igLibrary/Tfb/Script/` directory
is a startlingly complete, already-reverse-engineered **field-level C#
model of the entire `TFBScriptInfo` namespace** (100+ files, one per class),
plus `igCauldron3/Frames/TfbScriptEditor.cs`, a working decompiler that
turns a live `tfbScriptInfo` object graph into readable pseudocode.

**Why this is trustworthy as a semantic reference despite being a different
game**: the class names match Mad2's own class directory almost 1:1 (cross-
checked against Mad2's Wii-derived `wii_field_schema.json` and the game's
own string-pool class list) — `OpSetValue`, `OpForEach`, `OpFindVariable`,
`OpLoopValue`, `OpSlideValue`, `OpStartSequence`, `OpTopLevelBehavior`,
`OpCheckFOV`, `OpFindSubSet`/`OpFindSubset`, `ScriptSet`, `ScriptReference`,
`ValueRHSVariant`, `RHSValueStack`, `PositionMeasurement`,
`OrientationMeasurement`, `ColorMeasurement`, and dozens more — this is
clearly the same TFB-internal scripting subsystem shared across multiple
titles built on this tool lineage, not a coincidental naming overlap.

### Class hierarchy (semantic, from igRewrite8's C# model)

```
igObject
  tfbScriptObject (igNamedObject)   -- base "thing with a name" for the whole namespace
    OpCode                          -- base opcode: _internalFlagsStorage, _branchPC (span for tree nesting)
      OpBranch                     -- adds a branch-target concept
        OpAbstractBehavior          -- base for OpTopLevelBehavior/OpUserBehavior/etc.
        OpAbstractCheckValue        -- base for conditionals: _LHS (ValueStack), _relOperator, _RHS (ValueRHSVariant)
          OpFindSubSet              -- adds _setRemainder, _cachedObject (ScriptSet)
        OpLoop                      -- base for OpForEach/OpLoopValue
          OpForEach                 -- _LHS (SetStack), _dir, _RHS, _cachedSet, _cachedObject
          OpLoopValue                -- _RHS (ValueRHSVariant), _numLoops
        OpFindVariable              -- _varContainerType, _varContentsType, _varName, _filter,
                                       _skipRHS, _owner (ScriptGroupStack), _indexRHS, _cachedObject
        OpSlideValue                -- _LHS (ValueStack), _RHS, _secondsRHS, _easeOutRHS, _easeInRHS, _cachedSlider
        OpStartSequence             -- _LHS (ScriptGroupStack), _indexRHS, _cachedObject (ClonedSequence)
        OpCheckFOV                  -- _fromLHS, _RHSfacing, _RHSfov, _LHS, _relOperator, _RHS, _mode, _cachedObject
  ValueRHSVariant (igObject)        -- _varOp1/_varOp2 (RHSValueStack), _arithOperator, _resultFunc, _conditioner
  ScriptReference (ReferenceVariant) -- _type, _obj (tfbScriptObject) -- a resolved variable reference
  ScriptSet (SetVariant)             -- _list (ScriptGroupStack) -- a named collection of script objects
  tfbScriptInfo (tfbComplexDataInfo) -- _opList (OpCodeList), _masterVarList (OpCreateVariableList)
                                        -- this is the actual top-level "compiled script" asset class
```

`OpSetValue` (simplest concrete opcode, useful as a first porting target):
`_LHS` (a `TFBScriptReference`-equivalent), `_RHS` (a `ValueRHSVariant`) —
literally `lhs = rhs`, matching the Mad1 `.ai` reader's independent
`COpSetValue` finding exactly (`lhs`, `rhs` fields, same semantics).

### What this reveals about Mad2's own (incomplete) schema

Cross-checked several of these classes against Mad2's own Wii-derived
`wii_field_schema.json` (`mad2iga/wii_field_schema.json`) — the mismatch in
field *count* is stark, and is exactly what `CLASS_SCHEMA.md` already flags
as that extraction's known limitation (it only catches fields with
`ToVariant`/`FromVariant` script-exposed accessors):

| Class | Mad2 Wii schema (ToVariant-only) | igRewrite8's real field count |
|---|---|---|
| `OpForEach` | 1 field | 5 (`_LHS`, `_dir`, `_RHS`, `_cachedSet`, `_cachedObject`) |
| `OpFindVariable` | 1 field | 8 |
| `OpLoopValue` | 1 field | 2 |
| `OpSlideValue` | 1 field | 6 |
| `OpStartSequence` | 1 field | 3 |
| `OpCheckFOV` | 1 field | 8 |
| `OpTopLevelBehavior` | 2 fields | 0 own + inherited (a near-empty marker subclass) |

So Mad2's current schema for this entire namespace is capturing roughly
10-20% of each class's real fields — the scripting system is, right now,
the least-covered major area of the whole object schema, despite being
arguably the highest-value one to edit (it's literally where level logic
lives — cutscene triggers, "what does this button do," AI behavior wiring).

## Honest status

- **Now PC-verified for 75 of 104 classes** — see "PC-verified field
  schema" above. The blocker described in earlier revisions of this
  section (no `arkRegisterInitialize@OpSetValue` export could be found) was
  simply an incomplete export-table read — `list_exports`' pagination
  tops out well before this DLL's full ~9,300-symbol table, and the
  earlier session's "~4,000 entries pulled" note undersold it by more than
  half. Paging all the way through (via `run_script_inline`, not
  `list_exports`) found `arkRegisterInitialize@OpSetValue` at `0x10022c20`
  immediately, plus the equivalent for all 103 sibling classes. The
  suspected MSVC identical-code-folding issue was real (confirmed for
  `OpSetValue`'s destructor specifically) but turned out to be irrelevant —
  the working angle was always the bulk registrar, exactly like
  `ActorInfo`, just not found yet.
- **What is trustworthy**: the semantic vocabulary and rough class
  hierarchy from `igRewrite8`/`madagascar-tfbtool` — still valuable for
  *meaning* (opcode names, what a field is *for*) even where PC now
  supplies the *offset*; the two continue to agree wherever both cover the
  same class.
- **What's still open**: instance naming (which `ScriptInfo` is literally
  "Track_Angel") and full opcode semantics/pseudocode decompilation — see
  "VolcanoRave findings" above for exactly what was tried and ruled out
  this session on the naming side. The remaining 29 classes with 0
  extracted fields are believed genuinely fieldless (marker/inherited-only
  subclasses), not an extraction gap, but this hasn't been independently
  spot-checked class-by-class.

## Round 9 (different session): the field-coverage gap is now solved engine-wide, `internalFlags` decoded, and a live case study (Duty Free's shop item lists)

**The "10-20% coverage, no inheritance" problem above is now solved** —
not by decompiling more classes by hand, but by reading the Alchemy
engine's own live class registry directly out of the running game process.
Full writeup, methodology, and verified offsets are in
`CLASS_SCHEMA.md`'s "Live reflection dump (`mad2metadumper`)" section — not
duplicated here. Concretely for this namespace: `mad2iga/schema.go`'s
`ClassSchema()` now returns every class's complete own+inherited field
list for all 1392 registered engine classes (20,237 fields total), not
just each class's own newly-registered fields. `OpForEach`, for example,
previously showed only its 5 own fields (`LHS`/`dir`/`RHS`/`cachedSet`/
`cachedObject`) in a `script-dump` — it now also shows its inherited
`OpCode.internalFlags`, `OpBranch.branchPC`, and `OpLoop.index`, all
PC-verified, all cross-validated exactly against every offset this
document had already hand-confirmed.

**`OpCode.internalFlags` decoded** (previously just "enum," no known
values): a bitflag set, found via `Gap::Core::igMetaEnum` decompilation
(`getinternalFlagsMetaEnum@OpCode`) and confirmed present in the real PC
`ScriptInfoLib.dll` via `strings` — `IS_EXPANDED=1`, `IS_ELSE_EXPANDED=2`,
`IS_DISABLED=4`, `IS_CASE=8`, `IS_FIRST_VISIBLE=16`,
`IS_ELSE_FIRST_VISIBLE=32`, `IS_BREAK_POINT=64`. These are visual-script-
**editor** state flags (tree-expand state, breakpoint, disabled/grayed-out
node), not the MAD1 `.ai` format's flow-control/descendant-span bit-packing
that motivated looking at this field in the first place (see below) — a
real, useful, PC-confirmed finding, just not the one originally
hypothesized.

**Reference checked and ruled out for this specific question, still useful
in general**: `github.com/MaxStache/madagascar-tfbtool`, a parser/
decompiler for the *original 2005 film Madagascar*'s `.ai` bytecode format
— confirmed direct lineage with Mad2's own `TFBScriptInfo` (identical
opcode class names: `OpSetValue`, `OpForEach`, `OpCheckValue`, `OpIfElse`,
etc.), and its documented format (a real opcode-table + global/local
string-ref tables + a flattened instruction stream with a packed
"3-bit flow-control + 21-bit descendant-span" header per instruction) is
what prompted checking whether Mad2's `internalFlags` was the same kind of
packed field. It isn't (see above) — Mad2's actual forward-`branchPC`-only
control-flow model (established in Round 3) still stands as the correct
mechanism for this specific, later engine build. Still valuable as a
semantic Rosetta stone (its `rhs.py`'s confirmed RHS operand tag scheme —
int/float/color/pair/reference, with `+`/`-`/`*`/`/` arithmetic expressions
— independently corroborates `ValueRHSVariant.arithOperator`'s existing
semantics in this doc).

### Case study: Duty Free's unused shop items (`Alex Rites Hat`,
`Gloria Swimsuit`, etc.) — static analysis exhausted, live tracing built

Real user-reported puzzle: `DutyFreeBLD/ENGLISH.pak`'s string table has
several purchasable/unlockable item names that never appear as selectable
options in the shipped game (`Alex Rites Hat`, `Gloria Swimsuit`, `Marty
Baseball Cap`, `Melman Clock Head`, `Moto Moto Mustache`, `Concept Art`,
`Card Match - bonus card 1/2`, `Mini-Golf Bonus Hole 3`), each sitting
right next to a sibling item that IS purchasable/used. Confirmed directly:
the Lemur Loot → Apparel submenu shows exactly 4 cosmetics in the shipped
game (one per main character; `Moto Moto`'s item and each character's
alternate never appear).

**Static analysis, exhausted (not from lack of trying — real, evidenced
dead ends):**
- `IS_DISABLED` opcodes exist in this level (11 total, found via the newly
  decoded `internalFlags`) but all 11 are `OpCreateVariable` declarations
  clustered around an unrelated disabled feature (`Debug_RandomMonkeySet`,
  a dev tool in the Monkey Gallery aquarium exhibit) — not near any of the
  unused item strings. Ruled out as the mechanism.
- Every `OpForEach` in the level resolves `RHS`/`varOp1` to the *same*
  shared/empty `ValueRHSVariant`/`RHSValueStack` object across every
  instance in the file (a default placeholder, not a per-instance set
  reference), and `cachedSet`/`cachedObject` are always the runtime-unbound
  null sentinel in the shipped file — the identical dead end
  `mad2rhythmlogger`'s own Round 6 hit for `VolcanoRave`'s `OpForEach`.
  Confirmed this is a structural property of the file, not fixable by
  reading more fields.

**Live tracing built and used, same technique as `mad2rhythmlogger`**: a
new mod, `mad2shoptrace` (`mad2shoptrace/src/shoptrace.cpp`), vtable-
patches `execute()` (slot 29, same shared slot as every other `Op*` class)
on `OpForEach`/`OpChangeMembership`/`OpFindSubSet`/`OpCheckMembership`/
`OpCreateVariable`, gated to `level=="DutyFree"`. All vtable addresses and
field offsets are PC-native this time — the vtables from Ghidra against
real `ScriptInfoLib.dll`, the field offsets from `mad2metadumper`'s live
reflection dump (not ported from Wii, not decompiled by hand).

**What live tracing found, across two captures** (a broad session
browsing the whole shop, then a narrow one visiting only Lemur Loot →
Apparel):

1. `cachedSet`/`cachedObject` DO resolve to real, varied heap addresses at
   runtime — the "always null" above is purely a file-at-rest property,
   not an engine constant. Confirms live tracing is the right tool once
   static analysis hits a runtime-only wall.
2. A tempting early signal (one `OpForEach` node consistently iterating
   groups of exactly 4 across ~20 different category contexts in the broad
   capture) turned out to be a **false lead** — it never fired at all in
   the narrow Apparel-only capture, so it wasn't Apparel-specific. Recorded
   here specifically so it isn't re-chased: cardinality-matching alone,
   without confirming the opcode actually fires for the screen in
   question, produces false positives.
3. The real Apparel-entry mechanism: a low-frequency `OpForEach` (`dir=2`,
   "random-shuffle") resolving to an object named `Generic_Container`
   (read live via `*(obj+8)`, see `CLASS_SCHEMA.md`'s naming note below)
   — a shared/pooled placeholder, not a per-item object.
4. Hooking `OpSpawn` (added specifically once `Generic_Container` showed
   up as a name) found the actual construction site: one spawner
   consistently creates **exactly 10** `Generic_Container` instances per
   menu screen, with a sibling spawner attaching one `UI_MenuUI_OptionText`
   label to each, plus further sibling spawns for ring/ellipse decoration
   sprites, all also clustered at 10-12. This is the signature of a
   **generic, fixed-capacity list-UI widget** (pre-allocates a constant 10
   row slots, reused for every shop screen — categories, item lists,
   everything), not a per-screen or per-category item array. This is
   exactly why static analysis never found a literal "item count" field
   anywhere in the file: there isn't one — the UI's slot count is a
   constant, and something else (not yet traced) decides how many of the
   10 slots actually get real content per screen.
5. The high-volume `OpChangeMembership`/`OpFindSubSet`/`OpCheckMembership`
   activity (thousands of calls in a few seconds of just looking at a
   menu) is per-frame UI state polling (highlight/selection/hover checks),
   not one-time list construction — a real negative finding, not just
   unexamined noise.
6. Zero `OpCreateVariable` executions with `internalFlags != 0` were
   logged in either capture — meaning the `Debug_RandomMonkeySet` script
   path never initialized during either play session. Inconclusive on
   whether the interpreter skips `IS_DISABLED` nodes (could mean it does,
   or could just mean that script only runs if you physically walk up to
   the Monkey Gallery aquarium exhibit, which wasn't done either time).

**Attempted, deployed, honestly uncertain**: patched the 7
`Debug_RandomMonkeySet`-related `OpCreateVariable`s' `internalFlags` from 4
(`IS_DISABLED`) to 0 directly in `level.bld` (`Content/Streams/mod/
DutyFree.bld`, via `mad2repack replace`) as a cheap, reversible experiment
— flagged upfront that it's unlikely to have any visible effect, since no
opcode in the compiled `opList` references those declared variables by
name at all (checked directly before patching). Point 6 above is
consistent with that expectation but doesn't confirm it either way.

**Still open, concrete next steps for whoever continues this**: which
opcode(s) actually decide how many of the 10 generic slots get bound to
real content (most likely `OpSetReference`/`OpSetValue` writing a name/
icon/enabled-state into each `Generic_Container`/`UI_MenuUI_OptionText`
pair — not yet hooked) — that's the actual place the "4 vs. up to 10"
decision lives, and would need extending `mad2shoptrace` the same way
`OpSpawn` was added mid-session once `Generic_Container` pointed at it.

## Round 10 (different session): a real pseudocode renderer (`mad2repack script-pretty`), and `RelOp`/`ArithOp`/`FlowVals` decoded from source

Round 3/4's confirmed-but-never-shipped block-reconstruction mechanism
(`OpBranch.branchPC` recursion) is now a real tool:
**`mad2repack script-pretty <level.bld> <out.txt>`** (`mad2repack/script_pretty.go`),
alongside the existing raw-JSON `script-dump`. It walks each `ScriptInfo`'s
`opList` in order and, for any opcode whose merged schema includes a real
`branchPC` field (checked structurally — does the class's own schema claim
offset 0x24/36 for a different field, per Round 3 — not hardcoded per
class), recurses `[i+1, branchPC-1]` as a nested block. Output looks like:

```
IF <ValueStack@0x2F487C> == <int:-999|float:NaN (type=0x4F unresolved)>  // no body
SET <ValueStack@0x2F487C> = <int:0|float:0 (type=0x4F unresolved)>
```

(a real "initialize this value if it's still at its unset sentinel" idiom,
found verbatim in `RitesOfPassage/level.bld`). `mad2tool extract-scripts`
now produces one `<Level>.pretty.txt` per level alongside the existing
`<Level>.json`, both under `mad2raw/scripts/`.

**`RelOp`/`ArithOp`/`FlowVals` are now real decoded symbols, not opaque
tags.** Found via the same `getXxxMetaEnum`-decompilation technique Round 9
used for `OpCode.internalFlags`, applied to three more enums, this time
reading the actual `Gap::Core::igMetaEnum::createMetaEnum(name, nameArray,
valueArray, count, ...)` call's two backing arrays directly out of
`ScriptInfoLib.dll`'s static data (not just the enum's own name string) via
Ghidra's `inspect_memory_content`:

- `getRelOpMetaEnum@TFBScriptInfo@Gap` (`0x10022b40`) → `RelOp`, 6 entries:
  `0=LE_OP(<=) 1=EQ_OP(==) 2=GE_OP(>=) 3=LT_OP(<) 4=GT_OP(>) 5=NE_OP(!=)`.
- `getArithOpMetaEnum@ValueRHSVariant` (`0x10022bf0`) → `ArithOp`, 5 entries:
  `-1=NO_OP 0=ADD_OP(+) 1=SUB_OP(-) 2=MULT_OP(*) 3=DIV_OP(/)`.
- `getFlowValsMetaEnum@OpFlow` (`0x100259f0`) → `FlowVals`, 13 entries:
  `-2=FLOW_ELSE -1=FLOW_ELSE_IF 0=FLOW_END 1=FLOW_CONTINUE
  2..10=FLOW_OUT2..FLOW_OUT10`.

Sanity-checked against real decoded output, not just trusted from the
decompile: `RitesOfPassage`'s own `IF x op1 -999` / `IF x op5 0` (pre-decode
raw tags) resolve to `EQ_OP`/`NE_OP` respectively and read exactly as the
"if unset-sentinel, initialize" / "if nonzero" idioms real script logic
would actually contain — a live plausibility check, not just a static
claim. `mad2iga/schema.go`'s embedded schema is unchanged; these three
tables live in `mad2repack/script_pretty.go` itself
(`relOpSymbols`/`arithOpSymbols`/`flowValSymbols`) since they're rendering
concerns, not object-graph field offsets.

**`RHSValueStack.type` is a real external reference, not a portable
2-value tag — this session's biggest correction to earlier text in this
document.** The "VolcanoRave findings, round 2" section above states
`0x6A confirmed == int32, 0x6B confirmed == float32` as if these were fixed
constants. They are not. Decompiling `RHSValueStack::resolve`
(`ScriptInfoLib.dll 0x10021b80`) shows `_type`'s raw 4 bytes are read
directly as an `igMetaObject*` and compared by identity
(`isOfType(IntMeasurement_Meta)`, `== ValueInfo_Meta`, etc.) against a
handful of shared singletons that live in the DLL's own static data —
`IntMeasurement`, `FloatMeasurement`, `ColorMeasurement`,
`ScreenMeasurement`, `ValueInfo`. `_type` is schema'd `kind:["ptr"]`
(correct — it genuinely is a pointer field), so its on-disk encoding is an
ordinary IGZ segmented pointer, and segmented-pointer bit patterns are
file-relative (depend on where in *that file's own* Section 1 the
referenced data sits). Confirmed empirically: `RitesOfPassage/level.bld`'s
own distinct `_type` raw values are `0x4F, 0x65, 0xF4, 0xC7` — nothing like
VolcanoRave's `0x6A`/`0x6B` at all. Resolving `_type` as a segmented
pointer (`ResolveSegPointer`) lands on a small, tightly-clustered region
(~20-100 bytes apart, e.g. `0x6AD1F`/`0x6AD35`/`0x6AD97`/`0x6ADC4` for
RitesOfPassage's four distinct tags) that is **not** a real object — no
valid class directory entry there — strongly suggesting each `level.bld`
carries its own small external-symbol-reference table (one slot per
distinct external `ScriptInfoLib.dll` static the file's compiled script
data references), not yet reverse-engineered. This is a genuine, scoped
IGZ-format question (what is this table's structure, how does a segmented
pointer address into it), not a TFBScript-specific one — flagged here as a
concrete lead for whoever picks up general IGZ documentation work (see
`docs/IGZ_FORMAT.md`).

Until that table is decoded, `script-pretty` doesn't guess which
interpretation (`int` vs `float`) applies for an unrecognized `_type` tag —
it shows both (`<int:5|float:7e-45 (type=0x4F unresolved)>`) and leaves the
judgment to whoever's reading, the same "don't guess, show both" approach
`rhythm_log_decode.go` already uses for `OpCheckValue.cachedType`. Empirically
this is easy to judge by eye per-file: `RitesOfPassage`'s `0x4F` tag pairs
consistently with plausible integers (`-999` sentinels, `0`, `1`, `5`,
`40`...) and its `0x65` tag paired with `0.25` (a plausible float, not a
plausible `1048576000` int) the one time it was checked — a clean, if
informal, per-file signal that a future session could formalize into an
automatic per-tag classifier once enough samples are checked by hand.

## Round 11 (different session): name-pool resolution wired into `script-dump`/`objects-dump` (the "which object is named what" problem, for script/UI objects)

> **Update (superseded — naming now solved authoritatively):** the heuristic
> string pool described in this round (`+0xA840` magic gap, `["ttr",…]` seed
> prefix, `pool[ordinal+1]`) has since been **replaced** by direct parsing of
> the `TSTR` string table (chunk type 1) and the `RSTR` location table (chunk
> type 4) from Section 0's own chunk table — the real, authoritative fixup
> tables. `objgraph.go` now exposes `ResolveName(addr)`/`ResolveFieldName(obj,
> field)` (RSTR-gated, no false positives) and this also cracked `ActorInfo`
> instance naming, which the heuristic never could. See `CLASS_SCHEMA.md`'s
> "Naming investigation" (rewritten) — the round below is kept for history.

The `_name` (igNamedObject) and `_varName` (`OpCreateVariable`/`OpFindVariable`)
fields carry a **small integer ordinal into `level.bld`'s Section-0 string
pool** — the exact table `mad2iga`'s `ObjectGraph.StringPool()` already builds
(`realStart = sec0Off + u32@(sec0Off+0x2C) + 0xA840`, prefixed with
`["ttr","OpMoveFrom","igHandleList"]`), resolved as `pool[ordinal+1]`
(`ResolveStringOrdinal`). The resolver existed but the dumps weren't applying
it; they now do. `script-dump` and `objects-dump` emit a sibling
`"<field>__name"` string next to every resolved `_name`/`_varName`
(`nameOrdinalFields` / `asUint32` in `mad2repack/script_dump.go`).

**This is broadly reliable, not just the one VolcanoRave `OpCreateVariable
.varName` case Round 8 confirmed.** Verified live against `global.bld`:
1734 `OpCreateVariable._varName` all resolve, 1514 to clean identifiers
(`"cam flyaround mode"`, `"music volume remembered"`, `"snap detection
stage"`...), 220 to real asset paths the scripts genuinely reference
(`.ai` scripts, `ButtonIcons/.../*.png`), and the handful that *look* garbled
(`"riteInfoa9"`, `"...InfoProxyb4"`) are genuine engine auto-generated proxy
object names — the same truncated-with-hex-suffix names that appear verbatim
in the shipped `.pak` header (`imatableTextureDataInfoProxy60c`), not
mis-resolution. `StringInfo._name` likewise resolves correctly and en masse
(the entire `Options_Opt_*`, `Controller_Opt_*`, `DEBUG_*`, `Menu_Debug*`
identifier set — see `docs/DEBUG_MENU.md`, which this cracked).

**Caveats (unchanged, worth repeating):**
- `ScriptSet`/`ScriptInfo` `_name` resolves through the *same* table to an
  **unrelated level-manifest filename** (e.g. `"WhooGloria_Complete"`), not a
  display name — a real field-meaning difference, not a resolver bug. The
  `__name` value is still emitted; judge by class.
- This solves naming for **script/variable/UI (`StringInfo`) objects**. It does
  **not** solve `ActorInfo` instance naming ("which coin is which") — that class
  has no name field at all (`arkRegisterInitialize`, see `CLASS_SCHEMA.md`); a
  different problem, still open.

**Also this round:** `script-pretty` now resolves **container operands** to their named
members — `OpChangeMembership`/`OpSetReference`/`OpCheckReference` whose LHS/RHS is a
`ScriptGroupStack`/`SetStack`/`ScriptObjectList`/`igObjectList` render as `{"MemberName",
...}` instead of `<ScriptGroupStack@addr>` (`containerMemberNames`). This is what made the
options-menu list-building legible (`OpSetReference(... RHS={"Options_Opt_DEBUG"})`, see
`docs/DEBUG_MENU.md`). It does **not** simulate the VM's shared-register dataflow, so
`OpCheckValue` operands that are runtime scratch `ValueStack`s still print as
`<ValueStack@addr>` — resolving those to source variables would need a real interpreter (the
registers are global and shared across thousands of ops, so the binding is temporal, not
addressable).
