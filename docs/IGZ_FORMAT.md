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

⚠️ **Open discrepancy**: `mad2assetextractor/docs/PAK_FORMAT.md` (written
earlier, for standalone `.pak` files rather than `level.bld`) describes a
different-looking header — `Sec0Offset`/`Sec0Size` at 0x10/0x14,
`Sec1Offset`/`Sec1Size` at 0x18/0x1c, `Sec2Offset`/`Sec2Size` at 0x20/0x24,
a `SectionCount` field at 0x28, then `Sec3Offset`/`Sec3Size`. That's an
8-byte-stride model (offset+size only, no align/flags), vs. the 16-byte
descriptor model confirmed above for `level.bld`. These have not been
reconciled yet — it's possible small `.pak` files genuinely use a simpler
sub-layout, or `PAK_FORMAT.md`'s author mis-attributed section 0's
align/flags fields as a separate "section 1". `mad2iga/pak.go`'s `DumpPak`
currently just reads `sec2Start`/`sec2Size` from fixed offsets 0x20/0x24
(i.e. trusts the `PAK_FORMAT.md` model) and has worked in practice on every
`.pak` in the game, which is some evidence for it — but it's never been
checked against the 16-byte-descriptor model the way `level.bld` was this
session. Worth resolving before building a real per-file section-count
reader.

## Section 0: class directory + string pool

Within section 0 (level.bld):

- **Class/type directory**: null-terminated ASCII class names (e.g.
  `ActorInfo`, `GeneratorInfo`, `GeneratorGenerationFields`, `igGroup`,
  `igSceneInfo`...), starting at `sec0Base + dirTableOff` where
  `dirTableOff` is a `uint32` at `sec0Base + 0x0C`, running up to
  `sec0Base + strPoolOff` (see below). An object's serialized "class index"
  (see Section 1) is a 0-based index into this list in file order. A real
  `level.bld` has on the order of 150-200 classes.
- **String pool**: the *intended* start is `sec0Base + strPoolOff`, where
  `strPoolOff` is a `uint32` at `sec0Base + 0x2C`. In practice
  `mad2iga/level.go`'s `DumpLevelCoords` adds a further hardcoded
  `+ 0xA840` before treating the bytes as null-terminated ASCII strings.
  This is an **unexplained magic constant** — `research_notes.md` separately
  hypothesizes "a hash-bucket table of `uint16` values, ~906 bytes" sitting
  between the nominal string-pool start and the first real string, but
  906 bytes ≠ 0xA840 (43,072 bytes), so these two observations haven't been
  reconciled either. Whatever occupies that gap is not yet understood; the
  `+0xA840` offset is empirically-derived (it produces correct-looking
  strings for the levels it's been tried against), not derived from a parsed
  table size. This is a good target for further work — walking the
  candidate hash-bucket structure directly, rather than skipping it with a
  hardcoded constant, would make this portable across files where the gap
  size differs.
- **String indexing**: class-instance names (e.g. `Coin_AirMelman_B_1`,
  `GrassFx_Shape223`) live in this same pool, addressed by sequential
  ordinal (0-based count of null-terminated strings from the pool start),
  not by byte offset. Two important synthetic entries — `"ttr"`,
  `"OpMoveFrom"`, `"igHandleList"` — are prepended by
  `mad2iga/level.go`'s `DumpLevelCoords` before the real pool, apparently to
  make the ordinal numbering line up (unexplained why these three
  specifically; another loose end).

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
offset.)

## What's still open

- The Section 0 vs `.pak`-header discrepancy above.
- The unexplained `+0xA840` gap before the real string pool, and whatever
  structure actually occupies it.
- Sections 2 and onward in `level.bld` (mesh/geometry/texture data) are
  identified only by size/position, not parsed.
- There is no known generic mechanism yet for "which byte offsets in a
  serialized object are pointers that need patching" — `CLASS_SCHEMA.md`'s
  approach (reading real field-offset/type info out of debug symbols) is
  the current best path to that, one class at a time, rather than a single
  discovered relocation table the way the other IGZ variant's `RVTB` block
  works.
