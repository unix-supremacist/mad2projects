# The object/class schema (RTTI, field offsets)

This is the piece that actually gates "can we edit arbitrary fields": IGZ's
container format (`IGZ_FORMAT.md`) tells you where objects and pointers live,
but not what any given object's bytes *mean*. That's determined entirely by
the engine's own reflection system (`igMetaObject`/`igMetaField`), compiled
into the game's code, not stored declaratively in the IGZ file itself.

## The reflection system (`igMetaObject` / `igMetaField`)

Confirmed live in `mad2/igCore.dll` (the PC build's core engine DLL — unlike
`Mad2.exe`, it ships fully-demangled C++ export names, `Gap::Core` namespace).
Key types, all in `Gap::Core`: `igMetaObject`, `igMetaField`,
`igMetaFieldInfo`, `igMetaFieldList`, `igMetaEnum`, `igMetaFunction`, plus
list/pair wrappers. `igObject`'s constructor takes an `igMetaObject*` and an
`igMemoryPool*` — every serialized object is tagged with which meta-object
(class) it is.

Decompiling `igMetaField`'s constructors in `igCore.dll` gives its layout
(partial, but enough to cross-check the Wii build against):

```
+0x00  vtable
+0x04  flags/type tag
+0x08  bitfield (flags)
+0x0C  igStringRef   field name
+0x10  (unused in default ctor)
+0x14  (unused in default ctor)
+0x18  uint16        field byte offset within the owning object   <-- the useful one
+0x1A  uint16        field type/size code
+0x1C  uint16        default 0xFFFF
+0x1E  uint16        default 0
+0x20  uint16        default 0xFFFF
```

This was independently confirmed against the **Wii build's** PowerPC code
(different compiler, different platform, same engine): its bulk field
registration helper,
`setMetaFieldBasicPropertiesAndValidateAll__Q33Gap4Core12igMetaObjectFPPcPPvPUsi`
(`(char** names, void** fieldPtrCache, unsigned short* offsets, int count)`),
writes the name to `+0xC` and the offset to `+0x18` per field — same shape,
same offsets, two different compilers/architectures. That agreement is the
basis for trusting the rest of what's below.

`igMetaObject`'s own field-management API (`igCore.dll` exports):
`appendMetaField`, `validateAndAppendMetaField`,
`setMetaFieldBasicPropertiesAndValidateAll`, `getRegularFieldCount`,
`findType`, `createInstance(Ref)`. No generic single relocation/fixup table
was found (unlike the other IGZ variant's `RVTB` block — see
`IGZ_FORMAT.md`) — pointer-vs-scalar-ness of a field appears to be carried
entirely by the per-field type/size code at `igMetaField+0x1A`, which we
haven't fully decoded the enum for yet.

## Ground truth from the Wii demo build

`../../mad2assetextractor/wii_demo_ref/Mad2.elf` is the actual jackpot here:
a **fully unstripped, statically-linked PowerPC ELF** (63,334 symbols,
Metrowerks CodeWarrior name mangling) pulled from a Wii "Demo Action Pack"
disc (`R9AE52`, via `nodtool extract -p data` on the disc's `.rvz` — see that
directory's own files for what's there; the raw disc image itself was **not**
kept, only the ~13 MB `Mad2.elf` + a few small sample `.arc`/`.bld` files).
No DWARF line info, but the symbol table alone is enormously useful:

### 1. Field names, for free (`k_<field>__<MangledClass>` globals)

The compiler cached each class's `igMetaField*` lookup in a static global
literally named after the field: e.g.
`k_spawnRate__Q33Gap15TFBParticleInfo25GeneratorGenerationFields`,
`k_generationFields__Q33Gap15TFBParticleInfo13GeneratorInfo`. There are
**3,506 of these across 713 classes** (`grep -E " k_[A-Za-z0-9_]+__" Mad2.elf.symbols.txt`).
This alone gives the real field *name* for nearly every class, without
decompiling anything.

### 2. Field offsets, decoded directly from raw instruction bytes

Script-exposed fields additionally get a pair of trivial accessor functions,
named `get<Field>ToVariant`/`set<Field>FromVariant`. These compile to 1-3
PowerPC instructions — a `lwz`/`stw` (integer/pointer) or `lfs`/`stfs`
(float) with an immediate displacement off `r3` (the `this` pointer, first
argument per the PPC calling convention). That means the field's byte offset
can be read directly out of the raw instruction word **without needing a
disassembler or decompiler at all**:

```
opcode = (word >> 26) & 0x3F     # 32=lwz 36=stw 48=lfs 52=stfs 50=lfd 54=stfd
rA     = (word >> 16) & 0x1F     # base register
d      = sign_extend16(word & 0xFFFF)   # the offset we want, when rA == 3 (r3/this)
```

`../../mad2assetextractor/wii_demo_ref/extract_schema.py` implements this:

1. Parses `Mad2.elf.symbols.txt` (`readelf -sW Mad2.elf`).
2. Matches `get<Field>ToVariant`/`set<Field>FromVariant` symbol names and
   demangles just enough of the Metrowerks scheme to recover the owning
   class's fully-qualified name — the scheme is
   `Q<N single digit><len1><name1><len2><name2>...` (length-prefixed, so
   it's unambiguous once you know `N` is always a single digit — an earlier
   version of this script had a bug here, greedily matching `\d+` instead of
   `\d`, which silently produced garbage for every class; worth remembering
   if you extend the regex).
3. Reads that function's raw bytes directly out of `Mad2.elf`'s `.text`
   section (no objdump/Ghidra needed) and decodes any `rA==3` load/store,
   recording the offset and whether it was an int or float access.

Run: `python3 extract_schema.py wii_field_schema.json` (paths are hardcoded
to the files alongside it in `wii_demo_ref/`).

**Current result**: 50 classes, several hundred fields, with byte-exact
offsets and int-vs-float kind. Verified against independent manual
decompilation in Ghidra for `GeneratorGenerationFields` (every one of its 7
fields matched exactly: `spawnRate`=0x20, `maxAlive`=0x24,
`currentlyAlive`=0x28, `lifespan`=0x2C, `lifespanRandomness`=0x30,
`maxDistance`=0x34, `midpointPercent`=0x38).

**Found and fixed a real bug in the extractor**: offset `0` was showing up
for ~20 fields across several classes (e.g. `ActorInfo.velocity`,
`ScriptObject.its`/`.null`). Offset 0 is *always* the classIdx/vtable slot on
every object — never a legitimate field — so every one of those entries was
the script decoding the wrong instruction (most likely a thunk/stub that
loads something other than `this` into `r3` before the real body, or a
tail-call trampoline). Fixed by dropping any `d == 0` match in
`extract_schema.py`; the class count dropped slightly (54 → 50) because a
few classes had *only* spurious offset-0 fields and are now correctly
reported as having no confidently-known fields at all, rather than one wrong
one.

**Known coverage gap**: this only catches fields with the `ToVariant`/
`FromVariant` script-exposed accessor convention. Plenty of fields
(sub-object pointers, matrices, anything not exposed to the scripting layer)
have plain differently-shaped accessors or none at all. Two ways to widen
coverage, not yet done:
- Loosen the accessor regex to plain `get<Field>`/`set<Field>` (no
  `ToVariant`/`FromVariant` suffix) — riskier, since plenty of `get`/`set`-
  prefixed functions aren't field accessors at all (`getMeta`, `setName`,
  `getClassMeta`...) and won't cleanly filter by the `rA==3` signal alone.
- Decompile each class's actual bulk field-registration call site (the one
  that calls `setMetaFieldBasicPropertiesAndValidateAll` per class, passing
  its literal `{name[], &k_field[], offset[]}` tables) — this gets *every*
  field of a class in one shot including ones with no accessor at all, but
  `get_xrefs_to`/`get_function_callers` didn't find these call sites
  automatically in this pass (likely indirected through PPC long-branch glue
  code that Ghidra's static analysis didn't resolve) — finding them would
  need either better xref recovery or pattern-matching the inline
  name/offset table literals directly in `.rodata`/`.data`.

### Corrections this already produced

Two entries in `mad2iga/level.go`'s heuristic coordinate-scanner turned out
to be flat-out wrong once checked against real offsets, and were removed —
see that file's `knownOffsets` comment for the detail. Short version: neither
`GeneratorGenerationFields` nor `GeneratorMovementFields` stores a spatial
coordinate at the offsets previously assumed (they're scalar tuning
parameters); the real `GeneratorInfo` spawn position was found by manually
decompiling `Gap::TFBParticleInfo::GeneratorInfo::setVector` — a plain Vec3f
setter (not caught by the automated `ToVariant`/`FromVariant` pass) writing
12 bytes at `GeneratorInfo + 0x60`. `GeneratorInfo`'s own name (for labeling
purposes) is still unresolved — it has no name field of its own; per
`research_notes.md`'s class hierarchy it's wrapped by `GeneratorInfoList` →
`ParticleSystemInfo`, and the human-readable instance name likely lives on
that wrapper. Open TODO, marked in `level.go`.

Tested the fix against a real level (`WaterholeBLD/level.bld.orig`, the same
one `research_notes.md` originally analyzed) and got a more interesting
result than expected: even with the *correct* offset, the scanner still
finds **zero** `GeneratorInfo` positions. Reason: `level.go`'s coordinate
scanner only triggers on a homogeneous-matrix-row shape (3 floats + exact
`w=1.0` + zero bytes on both sides) — that's how `ActorInfo`/`ActorWaypoint`/
etc. store a transform, but `GeneratorInfo`'s position is a bare Vec3f with
no such neighbor, so it never matches the trigger pattern at all. Probed
(not committed) whether scanning directly for the class-index byte instead
of anchoring on a coordinate shape first would work around this — it
doesn't: a raw `classIdx == 75` (`GeneratorInfo` in Waterhole) byte match
occurs **1,524 times** in that file for only ~49 real instances, since a
small integer is simply too common to use as a location anchor by itself.
This also means `research_notes.md`'s original "49 GeneratorInfo found"
claim (from the old, now-removed `findGeneratorName` pointer-matching logic)
was almost certainly mislabeling *other* classes' genuine coordinate hits
rather than finding real `GeneratorInfo` positions — same class of
false-positive-prone technique, just one layer removed. Actually recovering
real `GeneratorInfo` instances needs genuine Section 1 graph traversal
(following an actual pointer down from `GeneratorInfoList`/
`ParticleSystemInfo`), not byte scanning of any kind. This is the strongest
argument yet for eventually replacing `level.go`'s heuristic scanner with a
real object-graph walker once `IGZ_FORMAT.md`'s Section 1 layout is nailed
down further.

## A real object-graph walker (`mad2iga/objgraph.go`)

That last point above turned into its own piece of work: `mad2iga` now has a
second, non-heuristic way to enumerate objects in a `level.bld`, alongside
`level.go`'s coordinate scanner (which is left as-is — it's still useful for
what it does, and per direct user feedback its instance-*naming* logic has a
long history of getting worse every time someone's tried to fix it, so it's
not worth re-litigating). `ParseObjectGraph`/`ObjectGraph` in
`mad2iga/objgraph.go` instead:

1. Parses the section table generically (contiguous `{offset,size,align,flags}`
   descriptors starting at `0x10`, stopping at the first non-contiguous or
   all-zero entry) instead of hardcoding two sections — this is the same
   parse `IGZ_FORMAT.md` documents, just generalized to all N sections
   instead of assuming only sections 0 and 1 matter.
2. Reads Section 1's top-level `igObjectList` header for real
   (`objectCount`@+0x0C, `objArrayOff`@+0x18, then that many Section-1-relative
   pointers) — `TopLevelObjects()`.
3. Resolves segmented pointers properly (`ResolveSegPointer`, `segID` in the
   top byte indexing directly into the section table, per `IGZ_FORMAT.md`).
4. Walks `igGroup.childList` (`+0x1C`, manually decompiled and confirmed via
   `Gap::Sg::igGroup::getChildCount`/`hasChildren`, both of which read
   `*(int*)(this+0x1C)`) through the generic `igObjectList`-shaped container
   layout (`count`@+8, `capacity`@+0xC, `data`@+0x14 — confirmed via
   decompiling `igObjectList::getCount`/`append`/`set`) — `WalkGroupChildren`.
5. `FullWalk()` combines 2-4 into one pass: every object reachable from the
   top-level list, plus everything recursively reachable by following
   `igGroup` children, deduplicated by address.

**This directly contradicts the older research notes' assumption** that
gameplay objects are never top-level: cross-checked against `level.go`'s
older coordinate-scan heuristic (by confirming both methods find the same
real `ActorInfo` addresses, including genuine level-authored instances like
`Coin_LandCroc_B_1`) and confirmed `ActorInfo` objects genuinely do appear
directly in the top-level list, not exclusively nested in the scene graph.

**Wired up**: `mad2repack objects-dump <level.bld> <out.json>` runs
`FullWalk()` and, for every object whose class has a known schema (see
`ClassSchema()` in `schema.go`, which embeds `wii_field_schema.json` directly
into the `mad2iga` binary via `go:embed`), reads out every known field's raw
value. Run against `WaterholeBLD/level.bld.orig`: 11 sections, 170 classes,
**1,802 objects reached** across 12 distinct classes (`ActorInfo` ×529,
`SoundInfo` ×438, `ScriptSet` ×365, `ScriptInfo` ×135, `StringInfo` ×108,
`SpriteInfo` ×90, `ValueInfo` ×59, `ParticleInfo` ×53, `CameraInfo` ×15,
`DirectionalLightInfo` ×7, `PlacementReference` ×2, `TFBWorldInfo` ×1).

**⚠️ Major caveat, found via direct disproof — the Wii schema's offsets are
NOT guaranteed to carry over to the PC build.** The observation above (every
`ActorInfo`'s `activeSet`/`playerSet`/`inactiveSet`/`removedSet`/`controller`/
`physicsParameters`/`vehicleParameters`/`collisionInfo` reading back identical
across all 529 Waterhole instances) first looked like it might mean something
(shared/singleton list-membership pointers). It doesn't — it means those
offsets are simply **wrong for the PC binary**. Proof: `ActorInfo`'s
`destination` field (a Vec3f, schema offset `[320, 324, 328]`) was checked
against a known-good reference — `mad2iga/level.go`'s independently, PC-file-
verified offset of `+80` for `ActorInfo`'s real transform translation (this
offset was never derived from the Wii ELF; it comes from the original,
separate PC-native byte-pattern-matching approach in `research_notes.md`).
At `+80`, the PC file holds sane world coordinates
(`69.26, 6.37, -59.89` for one real instance). At the Wii schema's
`destination` offset (`+320`), the same object holds denormalized near-zero
garbage (`0.0, -5.6e-43, 1.0e-38`) — not remotely plausible as a position.
Widening the check further: **none** of `ActorInfo`'s Wii-schema field
offsets (scanned across the whole `0-340` byte range at that object's
address) resolve to a single plausible segmented pointer (`segID` within
`0-10`, this file's actual section count) — `playerSet`'s raw value has
`segID=25`, `collisionInfo` has `segID=66`, both far out of range. The most
likely explanation is a **class-layout difference between the Wii and PC
compiles of `ActorInfo`** (different base-class/vtable size before the
class's own fields start, shifting every offset by some constant not yet
determined — no anchor field was found to solve for the shift directly).

**Practical upshot**: treat every offset in `wii_field_schema.json` as
*Wii-verified only* until independently cross-checked against real PC data.
This also retroactively weakens an earlier claim in this doc: the "matched
exactly" confirmation given for `GeneratorGenerationFields` above was hand-
decompiling the *same Wii ELF* a second time and getting the same numbers —
that's internal consistency (two extraction methods against one binary
agreeing), not independent verification against the PC build the way
`destination` was just checked and failed. **No class in this schema has
actually been confirmed correct against the PC build yet.** The
`GeneratorGenerationFields`/`GeneratorMovementFields`/`GeneratorInfo`
corrections earlier in this doc remain useful as *Wii-side* ground truth
and are very likely still directionally right (real field names, plausible
relative field ordering), but their exact byte offsets should be re-verified
against the PC binary before being relied on for editing real PC game files.
The only offset in this entire investigation actually confirmed against the
PC build is `level.go`'s original `ActorInfo: +80`, which predates all of
this Wii-ELF work and comes from a completely different, PC-native
methodology. Cross-checking each class against real PC binaries
(`igCore.dll`/the per-class `*InfoLib.dll`s, e.g. `ActorInfoLib.dll` for
`ActorInfo`) before trusting its Wii offsets for anything beyond Wii-side
curiosity is the necessary next step, not an optional nice-to-have.

**Known gaps** (same shape as the schema's own gaps above): `FullWalk` only
follows one edge type (`igGroup.childList`). Other container/reference
fields — e.g. `GeneratorInfoList.generatorInfoList`, `igSceneInfo`'s own
children — aren't followed yet, so objects *only* reachable that way (like
real `GeneratorInfo` instances, per the discussion above) are still missed.
Extending `FullWalk` to follow additional known pointer/list fields as
they're identified (the same way `igGroup.childList` was) is the natural
next increment, rather than a wholesale rewrite — the container-reading
primitives (`ReadObjectListContainer`, `ResolveSegPointer`) are already
generic enough to reuse for any other class's list field once its offset is
known.

## PC-verified fields (`ActorInfo`, first class actually cross-checked)

Direct follow-up to the "Major caveat" above: `ActorInfo` is now the first
class in this whole investigation to have real fields **independently
confirmed against the PC binary** (`ActorInfoLib.dll`), not just the Wii
ELF. This also produced a second, more impactful bug fix than the class
schema itself — see below.

**Methodology**: same idea as the Wii extraction, but via Ghidra's live
decompiler instead of raw instruction decoding, since MSVC's output isn't as
uniformly simple as the Metrowerks PPC accessors were. `ActorInfoLib.dll`'s
PE export table already carries full demangled C++ names (MSVC mangling,
e.g. `?getDestination@ActorInfo@TFBActorInfo@Gap@@QAEAAVigVec4f@Math@3@XZ`),
but — same surprise as `igCore.dll` and `Mad2.elf` earlier this session —
Ghidra's auto-analysis doesn't actually create `Function` objects at every
export address; `list_exports`/`list_functions` disagree until you
explicitly `create_function` at each address of interest (it then picks up
the already-demangled export name automatically). Once created,
`decompile_function` on each gives clean, simple `this+0xNN` bodies, same
spirit as the Wii pattern-match, just done by hand this round rather than
scripted.

**Confirmed real PC offsets for `ActorInfo`** (all in `mad2iga/schema.go`'s
`pcVerifiedFields`, each tagged `"source": "pc"` so `objects-dump`'s output
and any future consumer can tell PC-confirmed fields from Wii-only ones):

| Field | PC offset | Confirmed via |
|---|---|---|
| `collisionInfo` | `0x120` | `getCollisionInfoToVariant@0x10001c90` |
| `waypoints` | `0x14c` | `getWaypointsToVariant@0x10002700` |
| `controller` | `0x140` | `getControllerToVariant@0x10001b80` |
| `physicsParameters` / `vehicleParameters` | `0x148` (shared) | `getPhysicsParametersToVariant@0x10001c10` / `getVehicleParametersToVariant@0x10001c50` — both literally read the same field and return it only if `isOfType()` matches their own expected type, confirming the Wii schema's odd choice to give both the same offset (there, 308) was directionally right even though the number itself didn't transfer |
| `logicMatrix` | `0x60` | `logicMatrix@0x10001f80`: returns `this+0x60` when a `TFBTransform` delegate at `+0x160` is absent, i.e. `AlchemyCommonLib::TFBTransform`'s local `igMatrix44f` |
| `destination` | `0x150`/`0x154`/`0x158` (Vec3f) | `getDestination@0x100015b0` / `setDestination@0x100015c0` — **not the resting position**, see below |
| `velocity` | *(none — indirect)* | `getVelocity@0x10001de0` returns `*(this+0x160)+0x58`: stored on a separate delegate object, not inline in `ActorInfo` at all |

**Disproven fields, removed rather than re-pointed** (`pcDisprovenFields`):
`playerSet`/`activeSet` (and, by the same accessor family, presumably
`inactiveSet`/`removedSet`) turned out not to be per-instance fields at all.
`getPlayerSetToVariant`/`getActiveSetToVariant` don't read `this` — they
read a **shared global** (`__interface_ActorInfo_..._igSmartPointer_...`) at
a fixed offset off of it. This is the real explanation for the
"identical across all instances" anomaly first flagged in the object-graph
walker section above — not wrong per-instance offsets landing on garbage
(as `destination` was), but genuinely non-instance, class-shared data. Both
explanations turned out to be true for different fields in the same class,
which is itself a useful lesson: "every instance reads the same value" is
not on its own diagnostic of which failure mode you're looking at.

**A second, more consequential bug found along the way**: `ResolveSegPointer`
was resolving `segID == 0` as literal Section 0. Cross-checking `collisionInfo`/
`waypoints`/`physicsParameters`'s raw values against the real file showed
they instead resolve correctly as **Section-1-relative** (i.e. `segID == 0`
means "the pointer's own containing section," not "Section index 0") — all
three then resolve to exactly the expected class (`CollisionInfo`,
`ActorWaypointList`, `ActorParameters`). Fixed in `ResolveSegPointer`
(see its doc comment for the precise semantics and caveats), and separately
confirmed the fix didn't break the *other* consumer of that function,
`WalkGroupChildren`/`ReadObjectListContainer`: scanned the file directly for
an `igGroup` instance with a resolvable child list and found one whose 4
children all resolve to valid, correctly-classed objects
(`igSimpleUserInfo` ×4) under the corrected scheme. This fix affects every
future use of segmented pointers in this codebase, not just `ActorInfo`'s
fields — worth remembering it's now part of the baseline, not a per-class
detail.

**`objects-dump` now resolves pointer fields**: any schema field with
`Kind: ["ptr"]` (currently `ActorInfo`'s `controller`/`collisionInfo`/
`waypoints`/`physicsParameters`/`vehicleParameters`) is resolved through
`ResolveSegPointer` and reported as `{raw, resolved_addr, target_class,
resolved}` rather than a bare integer. Re-run against `WaterholeBLD`: 529/529
`ActorInfo` resolve `collisionInfo` and `waypoints`, 394/529 resolve
`physicsParameters`/`vehicleParameters` (the rest are actors with no physics
override, `raw == 0` — a legitimate "none," not a failure), `controller`
resolves for none in this level (expected — none of Waterhole's `ActorInfo`
instances are player-controlled).

**`+80` reconciled — RESOLVED.** `level.go`'s original `ActorInfo: +80`
translation offset is `worldMatrix`'s (see `pcVerifiedFields`, base `+0x20`)
translation row: `+0x20 + 3*16 = +0x50 = 80`. Confirmed directly, not just
arithmetically: read all 16 floats at a real coin's `ActorInfo+0x20`
(`WaterholeBLD`, addr `0x790c70`) and got an exact identity rotation/scale
(rows 0-2: `(1,0,0,0)/(0,1,0,0)/(0,0,1,0)`) with row 3 reading
`(74.747, 6.370, -59.887, 1.0)` — precisely the known-good coordinate. A new
schema entry, `position` (`+0x50`/`+0x54`/`+0x58`, i.e. `worldMatrix`'s row
3), exposes this directly as a plain Vec3f, so `objects-dump` now surfaces
real, correct positions for every `ActorInfo` without needing to
cross-reference `level-dump`'s separate coordinate scan at all — verified:
the same coin now shows `"position": [74.746994, 6.3700185, -59.886932]`
directly in `objects-dump`'s output.

This also nails down what `logicMatrix` (`+0x60`) actually is: checked the
same real coin's bytes there and it's all-zero, across every `ActorInfo`
sampled — genuinely dead/unused in serialized level data, confirming it's
runtime-only scratch storage (the "no delegate" fallback path
`ActorInfo::logicMatrix()` takes, which just apparently never gets written
to for ordinary placed level objects) rather than a second meaningful
transform, and not a competing candidate for "where is this object."

This upgrades the earlier "no class in this schema has actually been
confirmed correct against the PC build yet" claim: `ActorInfo` now has real,
PC-confirmed data for 6 fields, with a clear, repeatable methodology
(`create_function` at each relevant export address, decompile, read the
`this+0xNN` pattern) for extending this to more classes and more of
`ActorInfo`'s own remaining fields. That remains manual/interactive so far
— an x86-equivalent of `extract_schema.py` (pattern-matching decompiled
output or raw x86 `mov`/`movss` instructions the same way the PPC version
pattern-matches `lwz`/`stw`/`lfs`/`stfs`) across every `*InfoLib.dll` would
be the natural way to scale this up, not yet attempted.

## Files

Reference material, all in `../../mad2assetextractor/wii_demo_ref/`:

- `Mad2.elf` — the unstripped Wii demo binary (~13 MB).
- `Mad2.elf.symbols.txt` — `readelf -sW` dump (~5 MB, 63,334 symbols).
- `extract_schema.py` — the offset-extraction script described above.
- `wii_field_schema.json` — its output: `{ "Gap::Namespace::ClassName": { "fieldName": { "offset": N, "kind": [...], "accessors": [...] } } }`. A field with multiple accessors disagreeing on offset (rare) records a list of offsets instead of one — treat those as lower-confidence / needing manual review.

A Ghidra project (`mad2`, via the `ghidra-mcp` bridge) already has both
`igCore.dll` (PC) and `Mad2.elf` (Wii) imported and analyzed, for anyone
continuing this interactively rather than via the standalone script.

Consuming code, in `mad2iga/` (this repo):

- `schema.go` — embeds a copy of `wii_field_schema.json` directly into the
  `mad2iga` Go package via `go:embed` (kept in sync manually — if you
  regenerate the JSON in `wii_demo_ref/`, copy it over
  `mad2iga/wii_field_schema.json` too) and exposes `ClassSchema(bareName)`/
  `FieldInfo.FieldOffset()` for looking up a class's known fields by the
  bare name it has in an IGZ file's own Section 0 class directory (no
  namespace prefix — that information isn't present in the file itself).
- `objgraph.go` — the real Section 1 object-graph walker described above
  (`ParseObjectGraph`, `ObjectGraph.FullWalk`, `WalkGroupChildren`,
  `ResolveSegPointer`, `FieldValue`, etc.).
- `../mad2repack/objects.go` — the `objects-dump` CLI command wiring the
  above into a JSON dump (class histogram + per-object known-field values).

## Naming investigation (unsolved — read before attempting again)

Instance naming (e.g. knowing a given `ActorInfo` is `Coin_LandCroc_B_1`) is
the biggest remaining gap in "can we edit anything": object *discovery* and
*field access* both now work via real graph traversal (`objects-dump`), but
no reliable name resolution exists. This section exists so a future attempt
doesn't repeat the same dead ends — per direct user feedback, naming has a
long history of "every fix somehow made it worse," so treat anything below
as ruled-out, not as a starting point to tweak.

**What's confirmed real:**
- `igNamedObject::getName()` (Wii ELF, `Gap::Core::igNamedObject`, decompiled
  directly) reads `*(this+8)` as an **`igStringRef`** — a tagged
  pointer/index (top bit distinguishes a pool-item reference from a literal)
  — not a plain integer. This is the same encoding `igMetaField` uses for
  its own name storage (`igCore.dll`, confirmed earlier in this document).
- `level.go`'s existing heuristic already reads `ActorInfo+8` for a
  "nameVal", but treats it as a **flat sequential ordinal** into an array
  built by scanning the file's null-terminated strings in order
  (`stringsList[nameVal+1]`). That's a fundamentally different, much
  simpler interpretation than the real `igStringRef` encoding above — a
  strong candidate explanation for why naming has never worked reliably:
  the field being read might be structurally right (`+8` does look
  name-shaped, per the Wii accessor) while the *decoding* of its value has
  always been wrong.
- A second, independent, architecturally-documented naming mechanism also
  exists: the file's trailing Name Table (see `IGZ_FORMAT.md`), a flat array
  of string-pool ordinals indexed by an object's position in the top-level
  array — **not** a per-object embedded field at all. Confirmed the header
  is real: read a real `level.bld`'s trailing table and its
  `objectCount`/`nameCount` fields exactly matched the top-level object
  array's own header (1803/1803 for `WaterholeBLD`).

**What was tried and disproved (with evidence, so it isn't re-tried
blindly):**
1. *Restrict name lookups to graph-confirmed real objects* (ruling out
   false-positive objects as the cause). Tested against all 529
   structurally-real `ActorInfo` instances in `WaterholeBLD` — every lookup
   resolved to a real, well-formed string, but strings unrelated to that
   specific object (`"Current Music"`, `"Thermal_6"`, `"Hat Attach"`). Rules
   out "wrong object" as the failure mode; the field/decoding itself is
   wrong.
2. *Auto-detect the true string-pool boundary* (replacing `level.go`'s
   hardcoded `+0xA840` magic gap with a computed one, scanning forward for a
   sustained run of plausible printable strings). Landed within 4 bytes of
   the known-good hardcoded value on `WaterholeBLD` — but produced clear
   false positives (garbage boundaries) on 3 other levels tested
   (`BraveNewWildBLD`, `ConvoyChaseBLD`, `DrMelmanBLD`). Does not
   generalize; not shipped.
3. *Interpret the Name Table's per-object ordinal as an array index* into a
   string list built by null-scanning from a candidate pool start. Tested
   against a known-real coin (`WaterholeBLD`, `ActorInfo@0x790c70`, top-level
   index 1272, ordinal value 8325): resolved to `"torInfob16"` — a clear
   mid-string fragment, not a real name.
4. *Interpret the same ordinal as a byte offset* from the pool start instead
   of an array index. Same object: landed inside an embedded HLSL shader
   source blob (`"...OutUVShadow.xy = InUV2Map.xy;..."`), also not a name.
5. *Interpret `ActorInfo+8`'s raw value as a segmented pointer* (the same
   `segID`/24-bit-offset scheme confirmed for `collisionInfo`/`waypoints`/
   etc. elsewhere in this document), tried both as `segID==0` meaning literal
   Section 0 and as `segID==0` meaning "same section as the pointer" (Section
   1). Same coin, same field: the first landed inside what looks like an
   unrelated small-integer table (possibly the class directory's own
   internals); the second landed inside a different, evenly-strided table of
   implausibly large values. Neither resolved to string data at all.
6. *Find a PC-native `getName`/`setName` accessor for `ActorInfo` directly*
   (the same successful strategy used for `collisionInfo`/`waypoints`/etc.).
   `ActorInfoLib.dll`'s export table has no such symbol — if `ActorInfo`
   really does inherit a name field from `igNamedObject` the way the Wii
   accessor suggests, the accessor is either non-virtual-but-inlined,
   reached only through a vtable slot (not distinguishable from other
   generic virtuals without deeper vtable analysis — tried scanning
   `igIGZLoader`'s vtable for a distinctive "loader" method as a related
   detour and it also didn't lead anywhere quickly, see below), or `ActorInfo`
   doesn't actually get its display name this way on PC at all.

**A related detour that didn't pan out**: went looking for the actual
Section-0 string-pool *loader* code (hoping a real parse of the "gap" before
the plain string list would reveal its true size/structure, rather than a
byte-pattern guess). Found `igIGZLoader`/`igIGZSaver` classes in `igCore.dll`
and pulled their vtable (`0x10099b6c`), but the "distinctive" (non-repeated)
vtable slots turned out to just be ordinary per-class virtuals every
`igObject` overrides differently anyway (`userRelease`, `createCopy`,
`getClassMeta`) — not a specialized IGZ-parsing method. The actual
Section-0/string-pool loading logic (if it's a distinct function at all,
rather than inlined elsewhere) hasn't been located.

**Follow-up, same session — `arkRegisterInitialize@ActorInfo` decompiled
directly (`ActorInfoLib.dll`, address `0x10006300`)**: this is `ActorInfo`'s
*complete, exhaustive* field-registration function — every field the class
declares, by name and offset, in one place (the PC-side sibling of the Wii
`setMetaFieldBasicPropertiesAndValidateAll` bulk-registration pattern used
throughout this document). Full result: `controller` (`0x140`), `model`
(`0x144`), `actorParameters` (`0x148` — confirms the earlier
`physicsParameters`/`vehicleParameters` finding: both are type-checked
*views* of this one field, not separate fields), `wayPoints` (`0x14c`),
`destination` (`0x150`), `gameActor` (`0x160` — and this is the same
delegate pointer `getVelocity` dereferences, `*(this+0x160)+0x58`, tying
that earlier finding together), plus three fields inherited from a base
class with only their default value overridden here (`majorIndex`,
`shadowCastMethod` — the latter confirms `Placement`'s Wii-derived
`shadowCastMethod` field is real — and `receivesShadows`).

**This settles the question above**: there is no name/string field
anywhere in this list. `ActorInfo` almost certainly has **no embedded
per-instance display name at all** on the PC build. The `+8` field
`level.go`'s heuristic has always read as "nameVal" is not a name field —
it was never one; it's very likely landing on part of `model`/`controller`'s
storage or padding and only coincidentally producing small integers that
sometimes happen to resolve to *some* string. This is probably the actual
root cause of the whole multi-year naming struggle: chasing a field that
was never the right one to begin with. The Name Table (indexed by top-level
array position, not any per-object field) is the only mechanism left
standing.

**Follow-up test on the Name Table itself — real progress, still not a
working fix.** Tested the hypothesis that the mysterious Section-0 "gap"
(previously assumed to be a hash table, per `research_notes.md`'s
906-byte guess) is actually a flat array of `uint32` *byte offsets*
(`gap[ordinal]` → offset from the real string data's start), rather than
something requiring null-scanning to build a sequential index. Result: a
**56% hit rate** (651/1164 sampled entries) landing on real, printable text
— far above chance, real signal. But testing broadly across all 1803
top-level Name Table entries revealed the resolved strings are
systematically **truncated suffixes** of real names — `"rassFx_Shape840"`
(missing the leading `"G"` of `"GrassFx_Shape840"`), `"D_Bark"` (a fragment
of `"GiraffeAcacia1D_Bark"`), `"coalescedWorld"` appearing standalone
multiple times at different indices. That's the unmistakable signature of a
**suffix-shared/deduplicated string pool** — a common space/build-time
optimization where multiple logical strings share common trailing bytes in
one blob, each addressed by a different starting offset into the *same*
storage, rather than each string being stored as an independent
null-terminated entry. The offset-table mechanism is very likely
structurally correct; what's still missing is either (a) confirming the
target coin's top-level array index was computed correctly in the first
place (computed via matching the object's absolute address against the
top-level pointer array — not independently re-verified this round), or (b)
understanding the exact rule for where a "logical" name starts within the
shared suffix blob (not every substring boundary is a valid string start —
some other marker or table must distinguish "real named entries" from
arbitrary suffix continuations, otherwise there'd be no way for the game
itself to know where `"GrassFx_Shape840"` "really" begins rather than
resolving to `"Fx_Shape840"` or any other inner substring).

**Follow-up, same session — thread 1 checked, ruled out.** Re-verified the
top-level-index computation directly: address `0x790c70` maps uniquely and
bidirectionally to index `1272` (searched all 1803 top-level entries — only
one match, and looking index `1272` back up in the array returns the same
address). The index is not the bug.

**Then searched for the real string directly and measured against it —
concrete progress, still unresolved.** The exact string
`"Coin_LandCroc_B_1\0"` occurs exactly once in the whole file, at absolute
offset `0x4b005` (well inside Section 0, after the "gap"). Computed its
*true* sequential ordinal (counting null-terminated segments one at a time)
under four different counting conventions, none of which produced the
Name Table's stored ordinal for this object (`8325`):

| Counting scheme | Result |
|---|---|
| Sequential from `real_str_start` (the `+0xA840` boundary) | `3296` |
| Sequential from `gap_start` (start of the "gap," no boundary skip) | `16321` |
| Sequential from the class directory's own start | `16510` |
| 4-byte-aligned from `real_str_start` | never lands exactly on `0x4b005` at all (overshoots) |

None match `8325`, and no simple scale/offset relationship connects them
(`8325` isn't `3296 × N` or `3296 + N` for any obviously-meaningful `N`,
and `16321`/`16510` are roughly double `8325` but not exactly, ruling out a
clean "count pairs as one" fix too). This means the real resolution
algorithm is not simply "sequential ordinal from any of these obvious
starting points, with or without alignment" — something else determines
how a Name Table ordinal maps to a byte position, and it hasn't been found
in six independently-tested schemes across two separate investigation
passes.

**Where this leaves things**: this is a well-defined, narrowly-scoped
remaining unknown (not a vague "naming is hard") — the exact function
`ordinal(8325) → byte offset 0x4b005` for this one confirmed real
data point. Given the offset-table hypothesis's 56% raw hit-rate on
*something* plausible-looking, the mechanism family is probably close but
not exactly right (missing a header/count field before the real table
starts, a different element stride, or an extra indirection layer). Given
six schemes have now been tried and precisely ruled out with hard numbers,
further progress most likely needs either: finding the actual
ordinal-resolution code directly (a distinct function, not yet located,
possibly not even in `igCore.dll` if this was baked by an offline
asset-cooker tool rather than resolved at runtime), or brute-force solving
for the transformation using this one confirmed `(ordinal, byte offset)`
data point plus a second independently-confirmed one (not yet gathered) to
constrain the search. Time-boxed here for this session — diminishing
returns on continued blind counting-scheme guessing without new
information.

## In-place field editing (`WriteField`, `objects-compile`) — first write path

Everything above this section is read-only (`objects-dump`). This session
added the first actual **write** path onto `objgraph.go`'s real graph model
— editing where objects sit in a level, not just reading it.

**The key structural fact that makes this simple**: IGZ objects are
fixed-size. Overwriting a scalar/Vec3f field in place never changes any
object's size, so there is no reserialization, no section-table fixup, no
pointer patching — just overwrite the exact bytes of the field being
changed, at its exact absolute file offset, and leave every other byte of
the file untouched. `ObjectRef.Addr` (already existed) is already an
absolute file offset, so a field's absolute address is simply
`obj.Addr + fieldOffset` — no new offset-tracking machinery was needed.

**`ObjectGraph.WriteField(obj, fieldName, value)`** (`mad2iga/objgraph.go`)
is the writer. Two deliberate safety gates, tighter than "does the schema
know this field":

1. **Only `Source == "pc"` fields are writable.** Wii-only offsets have
   already been directly disproven for at least one `ActorInfo` field on
   the PC build (`destination`, see the "Major caveat" section above) —
   writing through an unverified offset risks silently corrupting unrelated
   bytes. Refuses with an error rather than writing anything.
2. **Only `Kind ["float"]` fields are writable** — a lone `float32` (single
   `Offset`) or the 3-float Vec3f shape (`Vec3Offset`, e.g. `position`).
   Every other kind is refused, on purpose:
   - `"ptr"` fields would need a real, validated segmented-pointer value
     pointing at another real object — not attempted; too easy to corrupt
     the graph with a bad pointer.
   - `"int"`/`"bool"`/`"enum"`/etc. fields have never had their actual
     on-disk byte *width* independently confirmed, and there's already
     direct evidence in this same schema that assuming 4 bytes would be
     wrong: `CollisionInfo.isSolid`/`.isActorSurface`/
     `.interactsWithWorld` sit at offsets 132/133/134 — one byte apart,
     i.e. genuinely single-byte fields. Blindly writing a 4-byte word at
     one of those would clobber its two neighbors. `float32` has no such
     ambiguity (IEEE 754 single precision is unambiguously 4 bytes on
     every field observed so far), which is why it's the only kind trusted
     here. Widening this to `"int"`/`"bool"` would need each field's real
     byte width confirmed first (the same `arkRegisterInitialize`
     decompilation technique that found the offsets in the first place
     would also show the width) — not done this session.

**`mad2repack objects-compile <orig_level.bld> <edits.json> <out_level.bld>`**
(`mad2repack/objects_compile.go`) wires this into the CLI. `edits.json` is
deliberately a small, edit-specific format, not `objects-dump`'s own
per-object dump shape:

```json
{
  "edits": [
    { "addr": 7815800, "class_name": "ActorInfo", "field": "position", "value": [74.260605, 7.370015, -57.886932] }
  ]
}
```

`addr` is the same absolute-file-offset `addr` `objects-dump` already prints
per object (so the two tools compose directly — dump, find the object you
want, copy its `addr` into an edit). `class_name` is optional, cross-checked
against what's actually at `addr` as a copy-paste sanity check. `value` is a
plain float or a 3-element array depending on the field's schema shape.

### Verification (this session, real level, real numbers)

Level: `mad2russia/Content/Streams/win/extracted/Waterhole/level.bld`
(129,692,552 bytes). `objects-dump` on it: 1,802 objects, matching the
count already documented above (no regression from this change).

**Test 1 — single field, single object.** Picked a real `ActorInfo` at
`addr 7815800` (`0x774278`), `position` (`worldMatrix`'s translation row,
`+0x50/+0x54/+0x58`) reading `[69.260605, 6.370015, -59.886932]`. Ran
`objects-compile` to rewrite it to `[74.260605, 7.370015, -57.886932]`
(all 3 components changed). Result:

- `objects-compile` reported the write at absolute offset `0x7742C8`
  (`= 0x774278 + 0x50`, exactly the expected `position` address), 12 bytes.
- Whole-file byte diff (both files 129,692,552 bytes, same size): **exactly
  3 bytes differ**, at offsets `0x7742CA`, `0x7742CE`, `0x7742D2` — one byte
  per float32 (small-magnitude nearby float values differ in only one
  mantissa byte each; not a bug, just IEEE 754 bit-pattern coincidence for
  these particular before/after values). All 3 differing bytes fall
  strictly inside the intended 12-byte write window
  (`0x7742C8`-`0x7742D3`); **zero bytes differ anywhere else in the
  129 MB file.**
- Re-ran `objects-dump` on the output: exactly 1 of 1,802 objects differs
  from the original dump (the edited one), its `position` now reads
  `[74.260605, 7.370015, -57.886932]` — the new value, exactly — and every
  other field of every other object (including the edited object's own
  other fields: `worldMatrix`, `collisionInfo`, `waypoints`,
  `physicsParameters`, etc.) is byte-for-byte identical to the original
  dump.

**Test 2 — two edits, two fields, two objects, one `objects-compile`
invocation** (genericity check: not just `position`, not just one object).
Edited `ActorInfo@0x774278`'s `destination` (`[0,0,0]` → `[1.5,2.5,3.5]`,
absolute offset `0x7743C8 = 0x774278+0x150`, matching `destination`'s
schema offset) and a second, different `ActorInfo@0x77D038`'s `position`
(`[70.175, 6.370023, -59.886932]` → `[170.175, 106.370026, -159.88693]`,
absolute offset `0x77D088 = 0x77D038+0x50`). Result: whole-file diff is
**18 bytes total**, every one of them inside one of the two expected
12-byte windows (`0x7743C8`-`0x7743D3` and `0x77D088`-`0x77D093`) — none
outside either. Re-`objects-dump`: exactly 2 of 1,802 objects differ from
the original, exactly at the two edited fields, everything else (both
objects' remaining fields, and all 1,800 other objects) unchanged. As
expected, `ActorInfo@0x774278`'s `worldMatrix` dump also shows the updated
translation row from Test 1 still in effect in this second output — same
underlying bytes as `position`, not a second independent field, consistent
with `position` being documented above as literally `worldMatrix`'s row 3.

**What's writable today, concretely**: any `ActorInfo` field tagged
`Source: "pc"` with `Kind: ["float"]` in `mad2iga/schema.go`'s
`pcVerifiedFields["ActorInfo"]` — in practice `position` (tested) and
`destination` (tested); `worldMatrix`/`logicMatrix` are the same kind and
mechanically writable too (untested this session — `logicMatrix` is
documented dead/unused, and writing a whole 16-float matrix via a bare
`float32`/`[3]float32` value isn't wired into `WriteField` since neither
input shape fits a 16-float payload). No `TFBScriptInfo`-namespace field
was attempted: that schema's `"vec3f"`-kind fields (e.g.
`ContactInfo.location`) use a different offset convention (single base
offset + implied stride, not the `[]interface{}` 3-offset-list shape
`Vec3Offset` parses) that `FieldValue`'s own *read* path doesn't correctly
handle yet either (see its `isFloat`/`Vec3Offset` dispatch in
`objgraph.go` — a `"vec3f"`-kind field falls through to the plain
scalar-`uint32` branch today, silently wrong) — writing through an
unproven read path isn't attempted here; fixing `FieldValue`'s `"vec3f"`
handling would be a prerequisite, not something bundled into this change.
`"ptr"`/`"int"`/`"bool"`/`"matrix44f"` kinds are refused by `WriteField`
itself, everywhere, for the reasons above.

**Follow-up, different session, different namespace — same problem,
independently confirmed.** While PC-verifying the entire `TFBScriptInfo`
namespace (`SCRIPT_FORMAT.md`'s "PC-verified field schema" — the
`ScriptSet`/`ScriptInfo`/`Op*` classes behind level scripting), went
looking for a shortcut around this whole problem: `ScriptObject` (the base
practically every class in that namespace derives from) inherits
`igNamedObject`, and unlike `ActorInfo`, `igCore.dll` really does export a
clean, real `igNamedObject::getName()`/`arkRegisterInitialize@igNamedObject`
pair confirming `_name` lives at `*(this+8)` as a **plain `char*`** (not a
tagged `igStringRef` — that was a Wii-only assumption; PC just stores a
raw pointer, or `0` for unset, per the decompiled code directly). This
looked like it might sidestep the whole Name Table investigation above
for at least this one namespace. It doesn't: reading `+8` directly off six
real `ScriptSet` instances in a real level (`VolcanoRave/level.bld`) gives
small, non-zero, **strictly sequential** integers (`0x168, 0x169, 0x16a,
0x16b, 0x16c, 0x16d` for six consecutive objects) — not remotely
plausible as resolved pointers, and confirmed *not* matching any
segmented-pointer convention already established in this codebase (tried
resolving against `Track_Angel`'s own known, unique string-pool byte
offset — found directly via `strings`+`grep`, only one match in the whole
file — under segID-relative-to-Section-0, segID-relative-to-Section-1, and
every other section's own base; none produce a byte pattern findable
anywhere in the file). This is the exact same shape as the `ordinal(8325)
→ byte offset` problem above — a small integer that indexes into
*something*, discovered completely independently via a different field in
a different class hierarchy — which is reasonably strong evidence this
"small sequential ordinal, not a resolvable pointer" pattern is a single
underlying engine-wide mechanism (most likely the same Name Table concept,
or a sibling of it), not two unrelated problems. Doesn't move the actual
fix forward, but narrows where to look: the next person chasing this
should look for one function that resolves an arbitrary small ordinal to
a string, used by *both* `ActorInfo`'s Name Table and `igNamedObject`'s
`_name` field, rather than treating them as separate mysteries.
