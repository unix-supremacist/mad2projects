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

**Open, not-yet-explained observation**: every `ActorInfo`'s `activeSet`/
`playerSet`/`inactiveSet`/`removedSet`/`controller`/`physicsParameters`/
`vehicleParameters`/`collisionInfo` fields read back as **exactly the same
raw value across all 529 instances** in Waterhole. Plausible explanation,
not yet confirmed: these look like membership-list/shared-resource pointer
fields (an actor being in the "active set" vs "removed set", or sharing one
default `physicsParameters`/`collisionInfo` object) that the engine patches
in at load/fixup time rather than genuinely varying per level-authored
instance — consistent with IGZ's overall "frozen memory image + fixups"
design (`IGZ_FORMAT.md`). Not chased further this session; worth confirming
by checking whether these fields' at-rest values resolve (via
`ResolveSegPointer`) to valid, in-bounds objects, and if so what class those
objects are.

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
