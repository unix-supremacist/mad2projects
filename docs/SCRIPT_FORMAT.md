# TFBScript — level logic/behavior scripting system

This covers `TFBScriptInfo` (the `ScriptSet`/`ScriptObject`/`Op*` classes
that appear throughout `level.bld`'s object graph — by far the largest
single namespace in the class directory, and the actual encoding of level
*logic*: cutscenes, triggers, AI behaviors, "what happens when"). Nothing in
this document is Mad2-PC-verified yet — it's a structural/semantic map built
from two external reference projects, meant to make future PC-side
verification much faster than starting from nothing (the same problem
`CLASS_SCHEMA.md` solved for `ActorInfo`/`GeneratorInfo`/etc., not yet
attempted for this namespace).

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

- **Not verified against Mad2 at all yet** — every field name/type above
  comes from a different game (`igRewrite8`) or a different game's
  different format (`madagascar-tfbtool`'s Mad1 `.ai` reader). Treat this
  entire document as a *hypothesis generator*, not ground truth, the same
  way the Wii ELF's offsets needed independent PC confirmation before being
  trusted (see `CLASS_SCHEMA.md`'s "Major caveat").
- **What is trustworthy**: the semantic vocabulary and rough class
  hierarchy — two independent projects, targeting two different games in
  two different formats, agree on what these opcodes *do*. That's strong
  corroboration for *meaning*, even with zero confirmed Mad2 byte offsets.
- **Started, not finished**: `ScriptInfoLib.dll` (Mad2's own PC DLL for this
  namespace) is now imported into the Ghidra project (it wasn't at the
  start of this investigation). Confirmed directly: `OpSetValue` exists as
  `?...@OpSetValue@TFBScriptInfo@Gap@@...` in the real export table — the
  class name match to both reference projects is not a coincidence, it's
  the literal same class in the shipped PC binary. But finding its real
  field *offsets* wasn't as immediate as `ActorInfo`'s was: no
  `arkRegisterInitialize@OpSetValue` export exists (unlike `ActorInfo`,
  which had one that listed every field in one decompile), and the
  fallback attempt — decompiling `OpSetValue`'s destructor for field
  cleanup code — hit MSVC's identical-code-folding: the address resolved
  to a *different* class's destructor (`AttachNodeReference`) that the
  linker had merged in because the two compiled to byte-identical trivial
  bodies (both presumably just chain to a base destructor with no
  class-specific cleanup, common for classes whose fields are all plain
  values with no owned resources to release). That means the destructor
  approach won't reveal `OpSetValue`'s fields either — need a different
  angle (its constructor, or a real accessor/usage site, the same way
  `ActorInfo`'s pointer fields were each found via their own individual
  `getXToVariant` exports rather than one shortcut).
- **Recommended next step**: retry with `OpSetValue`'s *constructor*
  (`??0OpSetValue@TFBScriptInfo@Gap@@...`, not yet located/tried) or search
  for any exported non-virtual accessor touching `_LHS`/`_RHS` specifically
  (the `getModifyType@OpSetValue` virtual override that *does* exist is
  worth decompiling too — it's a real, class-specific function, just not
  obviously about `_LHS`/`_RHS` from its name alone). Once even one or two
  opcodes are PC-verified this way, the same bulk-field-registration
  technique that cracked `ActorInfo` in one shot should work identically
  here if a per-class registration export does exist somewhere in this
  DLL — the two pages of exports pulled this session (~4,000 entries) may
  not be the complete table (`get_metadata` reports 9,313 total symbols in
  this DLL); a full pass hasn't been done.
- Once even one or two opcodes are PC-verified this way, the same
  bulk-field-registration-function technique that cracked `ActorInfo` in
  one shot (`CLASS_SCHEMA.md`'s `arkRegisterInitialize` finding) should work
  identically here — these are ordinary `igMetaObject`-reflected classes,
  no different in kind from `ActorInfo`/`GeneratorInfo`, just not yet
  looked at.
