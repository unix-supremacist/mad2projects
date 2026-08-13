# IGZ format (`.pak` files, and `level.bld`'s payload)

IGZ ("Intrinsic Graphics Zone") is Alchemy's serialized-object-graph format.
The mental model: an IGZ file is a **frozen memory image of a graph of
`igObject` instances**, plus enough metadata for the loader to walk it and
patch pointers back into shape. Nearly everything at rest is stored as
section-relative offsets rather than real pointers.

**Important variant warning**: there is more than one on-disk shape of "IGZ"
across Alchemy-engine titles. The later Skylanders/Crash-NST/CTR:NF variant
uses 4CC-tagged fixup blocks (`TSTR`/`TMET`/`MTSZ`/`RVTB`/`EXID`/`EXNM` —
see e.g. `kishimisu/Crash-NST-Modding-Tool`, `LG-RZ/igArchiveLib`,
`NefariousTechSupport/igArchiveExtractor`). **MAD2's IGZ v4 files do not use
that scheme** — confirmed by grepping decompressed `level.bld`/`.pak` files
for those 4CCs and finding none. MAD2 uses an earlier, flatter layout
described below. Don't assume tooling or docs written for the other variant
(including `igRewrite8`, built for Skylanders) apply byte-for-byte here —
useful for cross-referencing the general reflection/serialization *concepts*
(meta-objects, meta-fields, segmented pointers), not the exact byte layout.

## Header and section table

```
0x00  uint32  Magic     0x49475A01 ("IGZ\x01")
0x04  uint32  Version   4
0x08  uint32  Reserved  0
0x0C  uint32  Reserved  0
0x10  section descriptor 0
0x20  section descriptor 1
0x30  section descriptor 2
...   (one 16-byte descriptor per section)
```

Each section descriptor is `{offset uint32, size uint32, align uint32, flags uint32}`,
all little-endian. Section 0 always follows immediately after the last
descriptor (there's no separate "how many sections" field to read up front in
the way `PAK_FORMAT.md` originally assumed — see the discrepancy note below;
in practice you can just keep reading descriptors until the next one's
`offset` doesn't match `previous.offset + previous.size`, which is how
`mad2iga`'s tooling and this doc's confirmation both worked it out).

Confirmed this way, byte-for-byte, against a real decompressed `level.bld`
(`BraveNewWildBLD/level.bld.orig`): section *N*'s offset always equals
section *N-1*'s offset + size (contiguous, no gaps), and a real level.bld has
**10 sections**.

✅ **RESOLVED (this session): `.pak` files use the exact same 16-byte
descriptor model as `level.bld` — `PAK_FORMAT.md`'s 8-byte model was a
misreading of the same bytes, not a real alternate layout.**

Checked byte-for-byte against 7 real `.pak` files (`ENGLISH`/`GERMAN`/
`RUSSIAN.pak` from `VolcanoRave`, `GERMAN.pak` from `BraveNewWild` and
`RitesOfPassage`, `ENGLISH.pak` from `ConvoyChase_Demo`, spanning `mad2`/
`mad2demo`/`mad2russia`), the same methodology used to confirm `level.bld`'s
model: parse the header as 16-byte `{offset,size,align,flags}` descriptors
starting at 0x10 and check the result is self-consistent and matches what
the file's own *content* independently proves about where each section
really starts. For `VolcanoRave/ENGLISH.pak` (396,480 bytes):

```
[0] off=0x800  (2048)  size=0x2100  (8448)   align=2048  flags=0x0
[1] off=0x2900 (10496) size=0x5E3C0 (385984) align=16    flags=0x0
```

This is a clean, exact match for both known independent checks: (1)
`mad2iga/pak.go`'s `DumpPak` already dynamically scans for the real
UTF-16LE localized-string pool by content (not by trusting any header
field) and finds it starting at file offset 10496 — exactly section 1's
`offset` field above; (2) `mad2iga/objgraph.go`'s `ParseObjectGraph`,
`TopLevelObjects`, and the whole chunk-table mechanism below (extensively
regression-tested — 143/143 real `.pak` files round-trip byte-identical,
see `docs/IGA_FORMAT.md`'s bugs 1-6) already treat `Sections[1].Offset`
(the same 0x20 field) as Section 1's real base and it works correctly
across every `.pak` in the game.

`PAK_FORMAT.md`'s 8-byte model turns out to be a straightforward
misattribution of the *same* bytes: its `Sec1Offset`/`Sec1Size` (0x18/0x1c)
are actually **Section 0's own `align`/`flags` fields** (2048 and 0 in the
example above — plausible alignment/flag values, nonsensical as a second
section's offset/size, since 2048 is *before* Section 0 even ends), and its
`SectionCount` field (0x28) is actually **Section 1's own `align` field**
(16 in the example above — a small integer that happens to look
plausible as a section count, purely by coincidence). What `PAK_FORMAT.md`
called "`Sec2Offset`/`Sec2Size`" (0x20/0x24) is the *real* Section 1's
offset/size under the 16-byte model. `.pak` files don't always have exactly
2 sections either — `ConvoyChase_Demo/ENGLISH.pak` has 3 (a third section,
`align=4 flags=0x0`, holding `AnimatableTextureDataInfo`/`igImage2` texture
data referenced by that file's class directory) — so a correct reader needs
the same "keep walking contiguous 16-byte descriptors" logic as `level.bld`,
not a fixed 2- or 3-section assumption.

**Practical impact**: `mad2iga/pak.go`'s `DumpPak` (which reads a field it
calls `sec2Start`/`sec2Size` from fixed offsets 0x20/0x24) was already
*functionally* correct by coincidence — 0x20/0x24 is genuinely Section 1's
`offset`/`size` under the confirmed model, and Section 1 is always the
second descriptor regardless of how many sections follow it, so the fixed
offset is safe. Only the naming/comments (calling it "Section 2") were
wrong, not the bytes read — see that file for a naming cleanup.

## Section 0: header + chunk table (supersedes the old "class directory + string pool" model)

**Major update, this session.** Section 0 is not just "a class directory
followed by a string pool" — it's a small header followed by a **chunk
table**, a generic list of typed sub-structures the real engine's
`igIGZLoader::fixupChunk`/`postReadChunk` (`igCore.dll`) walks at load time
to run several distinct reference-resolution passes. This mechanism was
originally found and reverse-engineered from the *other* side (a real,
live-debugged game crash while editing `.pak` files) — see
`docs/IGA_FORMAT.md`'s "A newly-found, much larger candidate: Section 0's
own chunk table" and "Bug 5"/"Bug 6" sections for the full discovery
writeup, the nibble-delta encoding derivation, and the live-debugger
evidence (a real captured `SIGSEGV` inside `fixupChunk`, and a decoded
chunk-5 entry matching this doc's own independently-derived
`StringPoolEnd` byte-for-byte). This section restates the structural facts
relevant to general IGZ container parsing, since that's this doc's job, and
adds new findings from this session (below) that specifically resolve the
old "class directory + string pool" model's biggest open question — what
the `+0xA840` gap actually was. Implementation: `mad2iga/chunktable.go`
(decode/encode) and `mad2iga/objgraph.go`'s `ParseObjectGraph` (class
directory only, so far — see below).

### Section 0 header

```
Section0+0x00  uint32  magic (0x49475A01)
Section0+0x04  uint32  version (4)
Section0+0x08  uint32  (unidentified, non-zero)
Section0+0x0C  uint32  dirTableOff  -- (see below: NOT what earlier text assumed)
Section0+0x10  uint32  chunkCount
Section0+0x14  uint32  chunkTableOff (relative to Section0's own start)
...
Section0+0x28  uint32  (unidentified -- coincidentally equals chunk 0's own
                         class count in every sample checked; likely just a
                         redundant copy, not independently important)
Section0+0x2C  uint32  strPoolOff   -- (see below: NOT the string pool's
                                        start; superseded by the chunk-table
                                        finding)
```

At `Section0 + chunkTableOff`, `chunkCount` consecutive **chunks**, each a
24-byte (0x18) header —

```
+0x00  uint32  type
+0x04  uint32  (unidentified)
+0x08  uint32  (unidentified)
+0x0C  uint32  count    -- meaning depends on type, see below
+0x10  uint32  stride   -- total chunk size including this header; this
                            (not count) is what advances to the next chunk
+0x14  uint32  (unidentified)
```

— followed by `stride - 0x18` bytes of that chunk's own payload. The last
chunk's own end lands exactly on Section 1's start, confirmed on every real
file checked (both `.pak` and `level.bld`): the chunk table fills Section 0
completely, with no gap before Section 1.

**Chunk types seen so far, and what's now known about each** (types 4-0xf
share a nibble-packed variable-length delta payload encoding a sequence of
segmented pointers — see `docs/IGA_FORMAT.md`'s "nibble-delta chunk
encoding" section for the full decode/encode algorithm, validated
byte-exact against a live crash):

| Type | Payload shape | What it is |
|------|---------------|------------|
| 0 | `count` null-terminated ASCII strings, concatenated | **The class directory** — confirmed this session (see below): `dirTableOff`/`strPoolOff` from the old header model are actually this chunk's own `AbsOffset` and `AbsOffset+Stride` in disguise, not independent fields. |
| 1 | `count` null-terminated ASCII strings, concatenated | **The real object/instance/asset name pool** — confirmed this session (see below). This is what the old "`+0xA840`" heuristic was blindly trying to reach. |
| 2 | `count` fixed 4-byte records, high-entropy/opaque bytes | Unidentified — looks hash-table-shaped (dense, no repeating structure, no readable text), not yet decoded. |
| 4, 5, 6, 7, 0xb, 0xc, 0xf | nibble-delta-encoded nibble stream, `count` entries | Segmented-pointer **reference-fixup lists**, consumed by `igIGZLoader::fixupChunk`. Types 4/5/6/7 confirmed present in real files; 0xb/0xc/0xf share the identical resolution code path per `fixupChunk`'s own decompilation but weren't observed in any sample checked. **Per-type identities** (from `MaxStache/madagascar-2-tools`, cross-checked here): **`0x04 = RSTR`** — locations that hold a **type-1 (`TSTR`) string index**, i.e. the authoritative "this word is a name index" table; **`0x05 = RVTB`** — object locations (each word 0 = class-directory index); **`0x06 = ROFS`** — locations holding a segmented pointer; **`0x07 = REXT`** — locations holding an external reference; `0x0b = TMHN`/`0x0c = RMHN` are the memory-handle table/index. `RSTR`+chunk 1 (`TSTR`) together are what resolve **instance names** (`ActorInfo._name` → `Tourist01`), superseding the `+0xA840` heuristic — see `CLASS_SCHEMA.md`'s "Naming investigation". |
| 8, 9, 0xe | `count` plain `uint32` segmented pointers (no nibble encoding) | A **second**, independent reference-fixup pass, consumed only by `igIGZLoader::postReadChunk` (not `fixupChunk`) — resolves to `igHandleList`/`igStringRefList` instances. `0x08 = ROOT` (the root object), `0x09 = ONAM` (named-object list) per the same source. |
| 0xd (13) | `count` fixed 4-byte records (plain `uint32`s) | **`MTSZ` — per-class instance-size table**, now independently confirmed: one record per class-directory entry, and every size **matches the DLL reflection object sizes** (`ark_meta.json`) — verified here **162/162 types, zero mismatch on `VolcanoRave.bld`**, and reported by `MaxStache/madagascar-2-tools` to hold across all 46 levels — two independent sources (level data vs. DLL code) agreeing. Small plausible byte sizes (`0x18`, `0x30`, `0x48`, `0x20`...). Usable as a free structural check on any reflection-derived class size. |

Chunk types 0 and 1's payloads are the direct replacement for this
section's old "class directory" and "string pool" concepts — see below for
what changes.

### The class directory (chunk type 0)

Unchanged in substance from the old model — null-terminated ASCII class
names (`ActorInfo`, `GeneratorInfo`, `igGroup`, `igSceneInfo`...), in file
order, an object's serialized `classIdx` (Section 1) being a 0-based index
into this list. What's now understood differently: `dirTableOff` (header
`+0x0C`) and `strPoolOff` (header `+0x2C`) are not two independent offsets
bracketing a flat string blob — they're **chunk 0's own `AbsOffset` and
`AbsOffset + Stride`** (confirmed exactly, `VolcanoRave/ENGLISH.pak`:
`dirTableOff=0x1C`, chunk 0's header sits at `Section0+0x1C`, and
`strPoolOff=0x64` (100) exactly equals chunk 0's own `stride` field). This
also explains a detail that never quite added up before: `dirTableOff` +
`strPoolOff` gives an offset that's 0x18 (the chunk-header size) short of
where the actual class-name text starts, because `dirTableOff` points at
chunk 0's *header*, not its payload — the real text starts at
`Section0 + dirTableOff + 0x18`.

### The real object/instance/name pool (chunk type 1) — resolves the old "`+0xA840` gap" mystery

**Confirmed, cross-checked against 3 independent real `level.bld` files**
(`VolcanoRave`, `BraveNewWild`, `RitesOfPassage` — different levels,
different install trees): the chunk with `type == 1` is a large,
null-terminated ASCII string pool holding a mix of authoring-time file
paths (`C:/TFB/Content/builddata/win/BraveNewWild_coalesce.wld`), shader
source text (`.fx`/HLSL, sometimes tens of KB in one string), and — most
usefully — real object/asset/gameplay-variable names
(`GiraffeAcacia_trunk`, `GiraffeAcaciaB_JL_inst_8Shape`, and, further in,
short gameplay identifiers like `total song beats`/`combo counter`/`beat
patterns`). **This is the same real pool the old `+0xA840`-offset heuristic
(`mad2iga/level.go`'s `DumpLevelCoords`, `mad2iga/objgraph.go`'s
`StringPool()`/`stringPoolMagicGap`) was always trying to reach — not a
separate structure.**

Proven directly, not inferred: for each of the 3 files, the string pool the
old heuristic produces (`sec0Off + strPoolOff + 0xA840` through
`sec0Off + sec0Size`) is **byte-for-byte a truncated tail slice of chunk
type 1's own payload** — searching both the old heuristic's pool and chunk
1's raw payload for the same known content strings (`"total song beats"`,
`"combo counter"`, `"beat patterns"` for `VolcanoRave`;
`"anim - headbash"`, `"anim - jump windup"`, `"anim - spin loop"` for
`RitesOfPassage`) finds them in both, at a **constant** entry-count offset
between the two lists for every target string checked within one file
(e.g. exactly 33 entries apart for `VolcanoRave`, exactly 74 apart for
`RitesOfPassage`). A constant per-file offset across multiple, unrelated
target strings is only possible if both lists are windows onto the *same*
underlying sequence — which settles it: `0xA840` is not a real structural
constant, it was always just a fixed **byte** offset that happens to land
partway into chunk 1's payload, at a point that varies (in entry-count
terms) from file to file depending on how many bytes of shader
source/paths precede it in that specific file's chunk 1. This is the
concrete root cause of `CLASS_SCHEMA.md`'s and `SCRIPT_FORMAT.md`'s
long-standing "known to not generalize perfectly to every level" caveat
about this exact heuristic.

**The remaining open piece**: real `OpCreateVariable.varName`/
`OpFindVariable.varName` fields store a small raw integer ("ordinal") that,
via the *old* heuristic + a 3-entry synthetic prefix
(`ObjectGraph.ResolveStringOrdinal`, `list[ordinal+1]`), already resolves
correctly to real variable names for `VolcanoRave` (confirmed independently
in `docs/SCRIPT_FORMAT.md`'s Round 10). Brute-forcing which constant `C`
makes `chunk1Payload[ordinal + C]` (indexing chunk 1's own raw
string list directly, no synthetic prefix, no byte-offset heuristic)
reproduce that same already-confirmed resolution for real
`OpCreateVariable` instances found by scanning each file for its own class
index gives a **clean, high-confidence match** — but `C` is not a universal
constant: `C=35` for both `VolcanoRave` and `BraveNewWild` (715/738 and
2364/2385 real objects matched, ~97-99%), but `C=73` for `RitesOfPassage`
(3439/3474 matched, ~99%). So the *shape* of the fix is now known (ordinal
indexes directly into chunk 1's payload, not a byte-skipped pool) but the
per-file base offset `C` isn't yet derived from anything structural — it's
suspicious that it's identical across two files and different for a third,
which hints at a per-file "count of some earlier/reserved entries" (a
natural place to look: whatever mechanism turns out to back the
`RHSValueStack.type` external-reference finding below — an "external
symbol" count that varies by which external classes a given level's
compiled scripts actually reference would produce exactly this pattern).
**Not yet fixed in code** — `mad2iga/objgraph.go`'s `StringPool()`/
`stringPoolMagicGap` and `mad2iga/level.go`'s `DumpLevelCoords` still use
the old byte-offset heuristic unchanged, since swapping in chunk 1's
payload without a validated portable `C` risks regressing the one
already-confirmed-working case (`VolcanoRave`) for an unproven fix
elsewhere. Whoever derives `C`'s real source should update both call
sites together (see `stringPoolMagicGap`'s own doc comment in
`objgraph.go`, and `mad2iga/chunktable.go`'s `readChunkTable`/type
constants for how to walk to chunk 1 directly instead of guessing a byte
offset).

### `RHSValueStack.type` — a live cross-DLL external reference, likely resolved via this same chunk-table mechanism (new lead, not yet closed)

`docs/SCRIPT_FORMAT.md`'s Round 10 found that `RHSValueStack.type` (offset
`0x1c`) is not a small 2-value literal-type tag as earlier text assumed —
it's a real pointer field (schema kind `["ptr"]`) whose resolved target is
compared by identity, at runtime, against a handful of shared `igMetaObject`
singletons living in **`ScriptInfoLib.dll`'s own static data**
(`IntMeasurement`, `FloatMeasurement`, `ColorMeasurement`,
`ScreenMeasurement`, `ValueInfo`) — i.e. a per-file pointer field that
somehow ends up pointing into a *different DLL's* memory, not this file's
own Section 1.

Decompiled `RHSValueStack::resolve` directly this session
(`ScriptInfoLib.dll 0x10021b80`, confirmed via Ghidra) to check how `_type`
is actually used: the relevant branch reads `iVar5 = *(int*)((int)this +
0x1c)` and immediately calls a virtual method through it
(`isOfType(...)`, an indirect call through `iVar5`'s own vtable pointer).
**This only works if `this+0x1c` already holds a real, valid, resolved
pointer by the time `resolve()` runs** — i.e. by the time any gameplay
code touches this field, the loader has *already* turned whatever this
field's at-rest on-disk encoding is into a genuine live pointer. This rules
out (or at least makes very unlikely) `_type` being read by application
code as a segmented pointer, resolved on demand, the way ordinary
object-graph pointers are — the fixup has to have happened earlier, at
load time, through some load-time pass over the file.

Given the chunk-table mechanism above is now confirmed to be exactly this —
a generic, load-time, multi-pass reference-fixup mechanism, walked by
`igIGZLoader::fixupChunk`/`postReadChunk` before any gameplay code runs —
the natural hypothesis is that `RHSValueStack.type`'s raw on-disk value
(observed: small single-byte-range integers, `0x4F`/`0x65`/`0xF4`/`0xC7`
for `RitesOfPassage`) is an index or tag consumed by one of the
**still-unidentified chunk types (2 or 0xd/13 above, or a type not yet seen
in any sample)**, which the loader uses to look up the right external
`ScriptInfoLib.dll` singleton — likely by name, via a string comparison
against chunk 1's own pool (**checked and ruled out this session**: chunk
1's payload for `RitesOfPassage` does **not** literally contain the
strings `"IntMeasurement"`/`"FloatMeasurement"`/`"ColorMeasurement"`/
`"ScreenMeasurement"`/`"ValueInfo"` anywhere — so if this is name-keyed at
all, the names live somewhere this session didn't find, or the tag is a
small enum/index into a *fixed, engine-wide* table rather than a per-file
name lookup). Directly re-deriving the exact addresses
`docs/SCRIPT_FORMAT.md`'s Round 10 reported (`0x6AD1F`/`0x6AD35`/`0x6AD97`/
`0x6ADC4` for its own `RitesOfPassage/level.bld`) against a freshly
re-unpacked copy of the same level this session did **not** reproduce
cleanly via the simplest hypothesis (`ResolveSegPointer` treating the raw
tag directly as `segID=0, offset=rawTag`) — the resulting addresses land at
non-4-byte-aligned positions inside Section 1's own top-level object
pointer array, not on anything that looks like a dedicated table. This
could mean a different exact `level.bld` build/install was used originally
(version drift between installs is plausible and not checked), or that the
real resolution mechanism genuinely isn't a plain `ResolveSegPointer` call
on the raw field bytes at all. **Still open** — the next concrete step is
decompiling `fixupChunk`'s/`postReadChunk`'s handling of chunk types 2 and
0xd specifically (not yet done; only their payload *shape* was inspected
this session, not the loader code that consumes them) to see which one (if
either) is what patches `RHSValueStack.type` at load time.

## Section 1: the object graph

The main data section starts with a top-level **`igObjectList`** (class
index 0), 32-byte header:

```
+0x00  uint32  classIdx     (0, for igObjectList itself)
+0x04  uint32  reserved
+0x08  uint32  reserved
+0x0C  uint32  objectCount
+0x10  uint32  nameCount
+0x14  uint32  fixupPtr      (top bit set = external fixup)
+0x18  uint32  objArrayOff   (relative offset to the pointer array, from +0x00)
+0x1C  uint32  nameArrayOff  (relative offset to name-index data, from +0x00)
+0x20  objectCount × uint32  Section-1-relative pointers, one per top-level object
```

**Important**: the top-level objects are mostly *resources* (textures,
shaders, `ScriptSet`/`ScriptObject` script trees) — not gameplay entities.
Things like `ActorInfo` (coins, NPCs), `ActorWaypoint`, `GeneratorInfo`
(spawners) are nested several levels deep inside the scene graph (typically
`igGroup` → `igNodeList` chains), not reachable directly off this list.

A serialized object in general:
- starts with its `classIdx` (`uint32`, index into Section 0's class
  directory);
- has fields at fixed byte offsets from its own start, per its class —
  see `CLASS_SCHEMA.md` for what's known about which offsets mean what.

### Segmented pointers

A pointer field's at-rest value is not a real address:

```
segID  := ptr >> 24         // top byte
offset := ptr & 0x00FFFFFF  // low 24 bits
absoluteFileOffset := sectionDescriptors[segID].offset + offset
```

⚠️ **Correction (found via `mad2iga/objgraph.go`'s `ResolveSegPointer`,
cross-checked against real `ActorInfo` pointer fields on a real
`level.bld`)**: `segID == 0` does **not** mean literal Section 0. It means
"relative to whatever section the pointer field itself lives in." For every
ordinary object-to-object reference inside the main object graph, that's
Section 1 — confirmed three separate ways: `ActorInfo.collisionInfo`/
`.waypoints`/`.physicsParameters` (see `CLASS_SCHEMA.md`'s "PC-verified
fields" section) all resolve to exactly their expected target class
(`CollisionInfo`/`ActorWaypointList`/`ActorParameters`) only under this
interpretation, and separately, `igGroup.childList`'s pointer resolves to a
real child object list the same way. Treating `segID == 0` as literal
Section 0 (the original, wrong reading — matching this doc's very first
version, and also what `mad2assetextractor/docs/research_notes.md`'s
`findGeneratorName` implicitly assumed) makes every such pointer resolve
into Section 0's small class-directory/string-pool area instead, which is
never right for an object-to-object reference.

`segID >= 1` is left as a direct section-table index (section *N*'s own
`segID` literally being *N*, not *N-1*) — this part is unchanged from
before, but has **not** been independently re-verified the way `segID == 0`
now has; it comes from `mad2assetextractor/docs/demo.md`'s separate
observations about cross-file segment references (e.g. "`0x05XXXXXX` for
Segment 5/global Segment 1" when a level references `global.bld`). Given
`segID == 0` turned out not to mean what it looked like it meant, it's
worth treating the nonzero case as unconfirmed too until someone actually
checks it the same way.

## Name table (end of file, `level.bld` only)

After the last section, a trailer maps each top-level `igObjectList` entry
to its human-readable name:

```
[ 28-byte header, same general shape as the igObjectList header above ]
[ objectCount × uint32, each a string-pool ordinal index ]
```

`nameTable[i]` → ordinal → Section 0 string pool → name for
`igObjectList` object `i`. (28 vs. the object list's own 32 bytes is a
4-byte discrepancy noted but not yet chased down — one field is presumably
dropped since the name table doesn't need e.g. its own nested pointer array
offset.) Not re-examined this session; given the chunk-table finding above,
this trailer is likely the same small, low-`align` final section noted in
"Sections 2+" below (a few KB, `align=4 flags=0x0`) in both `level.bld`
samples checked — plausible but not directly confirmed by re-reading its
bytes this session.

## Sections 2+ in `level.bld` (mesh/geometry/texture data) — first-pass structural survey

Still not parsed, but a size/flags survey across 2 real files
(`BraveNewWild`, `VolcanoRave`) shows a consistent pattern worth recording
for whoever picks this up next:

| | `BraveNewWild` (12 sections) | `VolcanoRave` (7 sections) |
|---|---|---|
| [0] | `align=2048 flags=0x0` (613,264 B) | `align=2048 flags=0x0` (357,152 B) |
| [1] | `align=32 flags=0x8` (11.6 MB) | `align=32 flags=0x8` (5.3 MB) |
| [2] | `align=4 flags=0xF` (16.5 MB) | `align=4 flags=0xF` (6.1 MB) |
| [3] | `align=4 flags=0xF` (16.4 MB) | `align=4 flags=0xF` (16.7 MB) |
| [4] | `align=4 flags=0xF` (16.6 MB) | `align=4 flags=0xF` (14.8 MB) |
| ... | (sections 5-9 not individually inspected) | (n/a — only 7 sections total) |
| [N-2] | `align=4 flags=0x15` (2.7 MB) | `align=4 flags=0x15` (5.6 MB) |
| [N-1] | `align=4 flags=0x0` (5,252 B) | `align=4 flags=0x0` (2,864 B) |

Both files share the exact same shape: Section 0 (`align=2048`, the chunk
table — see above), Section 1 (`align=32 flags=0x8`, the main object
graph), then one or more `align=4 flags=0xF`-tagged large sections (the
biggest in the file, presumably mesh/geometry — `0xF` as a flags value is
suggestive of "all four" of some per-section capability bitfield, unchecked),
then a `flags=0x15` section noticeably smaller than the `0xF` ones, then a
final, very small (`~3-5 KB`) `align=4 flags=0x0` section. This last
section is the natural candidate for the Name table trailer described
above (right size range, right position — last in the file), though this
wasn't independently confirmed by parsing its actual bytes against the
Name table's documented 28-byte-header shape this session. No cross-check
against `docs/TEXTURE_FORMAT.md`'s own Section 2 layout notes was done this
session (out of time budget) — worth doing before spending more effort
here, in case `.texs`'s already-solved container conventions generalize.

## What's still open

**Session summary (this pass):** the `.pak`-vs-`level.bld` header
discrepancy (old open item 1) is now **fully resolved** — both use the
identical 16-byte section-descriptor model; `PAK_FORMAT.md`'s 8-byte model
was a misreading of the same bytes. Section 0 is now understood to be a
generic **chunk table** (not just "class directory + string pool"),
reusing/restating the mechanism `docs/IGA_FORMAT.md`'s pak-crash
investigation independently discovered from the loader side; this
resolves *what* the old `+0xA840` gap was reaching for (chunk 1's own real
string pool) even though the exact per-file indexing constant into it is
still open. A new, real lead on `RHSValueStack.type`'s cross-DLL external
reference was chased partway via direct Ghidra decompilation but not
closed. A first-pass structural survey of `level.bld`'s Sections 2+ was
added. Remaining open items, in the same rough priority order as before:

- **The `+0xA840` gap mystery is substantially resolved** (see "The real
  object/instance/name pool (chunk type 1)" above): it was always a
  byte-offset heuristic landing inside the same real chunk-1 string pool
  used everywhere else, just at a file-varying entry count. What remains
  open is deriving the correct per-file additive constant `C` for
  `ordinal + C` indexing into chunk 1 directly (found empirically to be 35
  for two files, 73 for a third — not yet explained structurally), and
  then updating `mad2iga/objgraph.go`'s `StringPool()`/
  `mad2iga/level.go`'s `DumpLevelCoords` to use it instead of the old
  byte-offset heuristic.
- **`RHSValueStack.type`'s external-DLL-reference mechanism** (see above) —
  narrowed to "probably one of the chunk table's still-unidentified types
  (2 or 0xd), consumed by a load-time fixup pass before any gameplay code
  runs" via direct Ghidra decompilation of `RHSValueStack::resolve`, but
  not yet pinned to a specific chunk type or decoded structurally. This may
  turn out to be the same open question as the `C` constant above — both
  smell like "a per-file count of external/reserved entries."
- **Chunk types 2 and 0xd (13)** in Section 0's chunk table are
  structurally described (see the table above — 2 looks hash-table-shaped,
  0xd is confirmed to have exactly one 4-byte record per class-directory
  entry) but neither has been decoded/explained, nor has their consuming
  loader code (`fixupChunk`/`postReadChunk`'s handling of those two type
  values specifically) been decompiled.
- Sections 2+ in `level.bld` (mesh/geometry/texture data) — see the survey
  immediately above; still only identified by size/position/flags, no
  actual record layout decoded.
- There is no known generic mechanism yet for "which byte offsets in a
  serialized object are pointers that need patching" for **ordinary
  object-graph fields** (as opposed to the chunk-table mechanism above,
  which is a real, now-confirmed generic relocation table, just for a
  different category of reference) — `CLASS_SCHEMA.md`'s approach (reading
  real field-offset/type info out of debug symbols) is the current best
  path to that, one class at a time.
