# IGA archive format (`.arc` / `.bld` outer container)

Source of truth: `mad2iga/iga.go` (parse/extract) and `mad2iga/repack.go`
(rebuild). This format is fully reverse-engineered — `mad2repack verify`
round-trips every archive to a byte-identical copy.

Based on the community `igArchiveExtractor` project's research into the
Alchemy engine's archive format, refined here for the specific `Version 0x02`
variant MAD2 (PC) uses.

## Header (48 bytes)

| Offset | Size | Field             | Notes                                             |
|--------|------|-------------------|----------------------------------------------------|
| 0x00   | 4    | Magic             | `0x1A414749` LE or `0x4947411A` BE (`"IGA\x1A"`)   |
| 0x04   | 4    | Version           | `0x02` for MAD2                                    |
| 0x08   | 4    | FileTableSize     | Total size of CRCs + descriptors + chunk-props tables |
| 0x0C   | 4    | FileCount         | Number of files (`N`)                              |
| 0x10   | 4    | HashSearchDiv     | `0xFFFFFFFF / N`, for the engine's CRC binary search |
| 0x14   | 4    | MemoryPoolIndex   | `0` = files stored raw; `1` = zlib-chunked         |
| 0x18   | 4    | NametableOffset   | Absolute file offset of the nametable              |
| 0x1C   | 4    | NametableSize     | Nametable size in bytes                            |
| 0x20   | 16   | Reserved[4]       | See "Chunk properties tables" below — repurposed as per-table entry counts when compressed |

The magic's byte order doubles as the endianness flag for the whole file (all
multi-byte fields flip together).

## File table (starts at 0x30)

1. **CRCs**: `N` × `uint32`, one per file, **sorted ascending** — this is the
   key the engine's hash search binary-searches over at runtime. FNV-1a
   hash of the file's archive-relative path (lowercased, per Alchemy
   convention).
2. **Descriptors**: `N` × 12 bytes, in the *same CRC-sorted order* as the CRC
   list, `{DataOffset uint32, DecompressedSize uint32, Mode uint32}`.
   - `Mode == 0xFFFFFFFF` → file stored raw/uncompressed at `DataOffset`, for
     `DecompressedSize` bytes.
   - Otherwise (only when header `MemoryPoolIndex == 1`) → `Mode` is an index
     into the chunk-properties table below, and the data at `DataOffset` is
     zlib-chunk-compressed (see "Compressed file data").
3. **Chunk properties table** (only present when `MemoryPoolIndex == 1`): see
   below.

## Compressed file data (`MemoryPoolIndex == 1`)

Each file's data is split into `0x8000`-byte (32 KiB) decompressed chunks.
At rest, each chunk is:

```
[2 bytes, big-endian] compressed size (0 = padding, skip to next 0x800 boundary)
[compressed size bytes] zlib stream (starts 0x78 if actually compressed)
[padding to next 0x800-byte boundary]
```

A chunk that wouldn't shrink is sometimes stored raw instead of
zlib-compressed. Detection: peek the byte after the 2-byte size prefix — a
real zlib stream starts `0x78`. Even then, attempt inflate and fall back to
treating the chunk as `0x8000` raw bytes on any error, since raw data can
coincidentally start with `0x78` too (`extractCompressed` in `iga.go`).

### Chunk properties tables (three of them)

There are up to three parallel chunk-properties tables, back to back, whose
*entry counts* are stored in the header's `Reserved[0..2]` fields (repurposed
— hence "Reserved" is a misnomer once `MemoryPoolIndex == 1`). Which table a
given file's chunks land in depends on file type:

- Files ending in `.bld` (level archives) go in a "level table".
- Everything else (`.pak` etc.) goes in a "pak table".
- Which physical table index (0/1/2) is the "level table" vs "pak table" is
  not fixed — `Repack()` iteratively decides based on whether the level
  table's cumulative sector count would overflow a 15-bit sector index
  (`0x8000` sectors), reassigning to table 0 if so. See `repack.go`'s
  `Repack()` for the exact iteration (it converges in practice in ≤5 passes).
- Table 0's entries are 2×`uint16` per chunk (sector offset as a 32-bit value
  split high/low, with the high half's top bit forced set as a marker) plus
  a trailing 2×`uint16` total-sector-count; tables 1/2 pack more densely
  (1×`uint16` per chunk with the top bit set as a "compressed" marker, or a
  single combined count+flag `uint16` for table 2). See `buildRepack`'s
  three near-identical loops in `repack.go` for the exact bit layout — it's
  empirically reverse-engineered and confirmed via `verify`/`verify-binary`,
  not derived from any external spec.

This is the messiest part of the format and the one place where "why" isn't
fully understood, only "what" (verified by round-tripping every archive in
the game and diffing byte-for-byte). If you need to touch `repack.go`, treat
`mad2repack verify-all mad2/Content/Streams/win` as the regression test.

## Nametable

Starts at `NametableOffset`, `NametableSize` bytes:

```
[N × uint32]  offsets, relative to the nametable's own start, into...
[...]         null-terminated path strings (e.g. "c:/tfb/content/builddata/win/foo.texs")
```

Ordered to match the CRC-sorted file table. `iga.SanitizePath` strips the
`c:/tfb/content/` (or `content/`) prefix and drive letter when extracting to
disk.

## Repacking

`mad2iga.Repack()` rebuilds an archive from scratch given a list of
`RepackEntry` (name, CRC, data, and — for compressed archives being
round-tripped rather than freshly authored — the original raw compressed
bytes via `KeepZlib`). `RepackFromArchive()` is the common case: open an
existing archive, optionally substitute specific files' data (`replacements`
map), and re-emit everything else byte-for-byte from the original
compressed chunks (so files you don't touch don't even get re-compressed,
keeping repacks fast and diffs minimal).

## Three known content-editing bugs (investigated for a real user-reported crash)

Real, reported symptom: editing `ENGLISH.pak` (a `.pak` inside e.g.
`VolcanoRave.bld`) to add even a single extra character crashes the game on
load, every time. Three distinct bugs were found while investigating this,
across two sessions — all three now fixed. The first two fixes (bugs 1 and
2/2b/2c) were tested live in-game by the user and **did not** stop the
crash, which is why a third session went looking for — and found — bug 3
below: a genuinely different class of stale metadata (a serialized pool
*byte length*, not a pointer), never touched by either of the first two
fixes' pointer-fixup logic since it isn't shaped like a pointer at all. See
bug 3's own writeup for exactly why it was missed and how it was confirmed.
None of the three has been confirmed as *the* cause by actually launching
the game and watching it succeed (not something this repo's tooling can do
— see `CLAUDE.md`'s own note on this) — but bug 3 is, as of this writing,
the most likely remaining explanation for the crash the first two fixes
didn't resolve: see "What's still open" below.

### Bug 1 — stale `DecompressedSize` in `RepackFromArchive` (FIXED)

See `docs/UNPACK_REPACK.md`'s own writeup for the full history. Summary: a
replaced entry's outer-archive descriptor (`FileDescriptor.DecompressedSize`,
this doc's own File table section above) was left at the *original* file's
size instead of being updated to the replacement's true size. Both
`extractCompressed`/`ExtractRawData` (`iga.go`) and `Repack()`'s own
`KeepZlib` chunk-offset-reconstruction walk (`repack.go`) use this field as a
byte-count loop bound, not a cross-check against the physical chunk stream —
so a stale, too-small value can silently drop trailing 32 KiB chunks (or
mis-build that entry's chunk-properties-table rows) whenever an edit is large
enough to cross a chunk-count boundary. Fixed by setting
`entries[i].DecompressedSize = uint32(len(replacement))` for replaced
entries in `RepackFromArchive`. Regression-tested clean:
`mad2repack verify-all mad2/Content/Streams/win` passes 95/95 both before and
after (this bug and its fix only affect the replaced-entry path, which
`verify-all`'s no-replacements passthrough never exercises).

Concrete repro against the real, shipped file: growing
`mad2/Content/Streams/win/VolcanoRave.bld`'s `ENGLISH.pak` (via
`iga.DumpPak`/edit/`iga.CompilePak`) from 396,512 to a true 396,702
decompressed bytes and repacking with `RepackFromArchive`:

| | `DecompressedSize` written to the descriptor |
|---|---|
| Original archive | 396512 |
| Repacked with the pre-fix code | 396512 (stale — wrong) |
| Repacked with the fix | 396702 (correct) |

Re-extracting the fixed repack recovers `ENGLISH.pak` byte-identical to the
grown source (`sha256` match, 396,702 bytes, no truncation). Note: for this
*specific* 190-byte growth, re-extracting the pre-fix (buggy) repack also
happened to come back correct — 396,512 and 396,702 both fit within the same
13-chunk budget, so the chunk-count-boundary condition that actually triggers
visible truncation wasn't crossed by this particular edit size. This matches
the original finding in `UNPACK_REPACK.md`: the bug is real and now fixed,
but not every growing edit will have visibly misbehaved under it — it depends
on where the resulting size falls relative to 32 KiB chunk boundaries.

### Bug 2 — `CompilePak`'s pointer-fixup pass has a scan-range gap (FIXED, plus a related bug 2b found and fixed alongside it)

Found while investigating why bug 1's narrow trigger condition (crossing a
32 KiB chunk boundary) didn't feel like a good match for "**any** added
character crashes the game" — that phrasing implies something that breaks on
*every* size-changing edit, not just some of them. `mad2iga/pak.go`'s
`CompilePak` has exactly that shape of bug, one level down in the format
stack (inside the `.pak`/IGZ payload itself, not the outer IGA archive).

Background: a `.pak` is a single IGZ object graph (see `IGZ_FORMAT.md`). The
localized UTF-16LE string pool `CompilePak` edits (`meta.StringPoolStart` to
`meta.StringPoolEnd`) sits **inside** the file's main data section, with a
large amount of other object-graph data both before and after it — for
`ENGLISH.pak` specifically: 10,944 bytes before the pool, a 1,996-byte pool,
then **383,572 bytes after it** (97% of the 396,512-byte file). When the pool
changes size (`shift := newSize - oldSize`), every byte physically located
after the pool moves by `shift`, and any pointer field elsewhere in the file
whose *value* references one of those moved bytes needs `shift` added to it
to stay correct — this is exactly the "shift pointers past the old pool end"
logic `CompilePak` already has.

The bug: that fixup loop is bounded `for i := int(sec0Offset); i <
int(meta.StringPoolStart); i++` — it only scans bytes **before** the pool.
Any pointer field physically stored **at or after** `StringPoolStart`
(i.e. within the pool itself, or anywhere in that 383 KB tail) is never
examined, regardless of what it points to. Confirmed two ways against the
real `ENGLISH.pak`:

1. **Code-level**: `outBytes[meta.StringPoolStart+len(newPool):]` is set
   directly from `headerBytes[meta.StringPoolStart:]` — the tail is a pure
   `copy()`, structurally unreachable by the fixup loop above it.
2. **Empirically, against the real file**: scanning the file for 4-byte
   values that exactly match one of the 109 independently-verified
   "external" string offsets `DumpPak` itself found (`scanExternalStrings`)
   turned up 71 such matches — **40 of them physically stored at or after
   the old `StringPoolEnd`** (e.g. bytes at file offset 13528 hold the value
   3036, an exact, verified string offset) — i.e. real pointer fields
   provably do live in the unscanned tail region. Then, empirically:
   compiling a grown-strings edit (+95 characters / +190 bytes, real
   `CompilePak` call, unmodified code) and diffing the original file's tail
   (`data[12940:]`, 383,572 bytes) against the same span in the recompiled
   output (`data[12940+190:]`) showed the two are **byte-for-byte
   identical** — conclusive proof no pointer patching happens there at all,
   on the real code path, for a real edit. Any real pointer in that region
   whose target also moved (i.e. pointed to something at or after the old
   `StringPoolEnd`) is left holding its stale, now-wrong value.

This is a categorically broader trigger condition than bug 1 — it doesn't
need a 32 KiB chunk boundary crossing, just `shift != 0`, i.e. **any edit
that changes the pool's total encoded size at all**, which is exactly "add
even one character." This is a strong, mechanism-matching candidate for the
user's actual crash, arguably a better fit than bug 1.

**Fixed.** `CompilePak`'s fixup pass now covers the whole file, not just the
region before the pool. What was determined and built:

1. **Encoding**: pointer fields that reference *strings* are plain absolute
   file offsets, not `IGZ_FORMAT.md`'s segmented (`segID<<24|offset`) scheme.
   Confirmed against real tail-region examples: a real field at file offset
   13528 holds the value 3036 — the real string is genuinely at absolute byte
   3036. Reading it as a segmented pointer (`segID=0`, meaning "relative to
   whichever section the field itself lives in" — Section 1, based at file
   offset 10496, per this doc's `IGZ_FORMAT.md`-derived correction) would
   resolve to 10496+3036=13532, which is not a real string. All 71 of the
   independently-verified external-string offsets found via 4-byte-aligned
   exact-value matching (40 of them in the tail) confirmed this reading, with
   zero counterexamples.
2. **The fix**: the fixup pass was extended to a second range covering
   everything from the pool's end to EOF, applying the exact same
   `>= StringPoolEnd → add shift` / `stringPointersMap` remap logic already
   used pre-pool — reasoned about and empirically confirmed to be correct
   uniformly, including a field and its target both living in the tail (both
   shift by the identical amount, so no relative/no-op special case is
   needed; see bug 2b below for the actual exception to this that *was*
   found).
3. **A real regression found and fixed along the way**: a first version of
   this fix (scanning byte-by-byte, matching the pre-pool loop's own stride,
   over the much larger ~383 KB tail instead of ~9 KB) produced observable
   corruption — patching a value at position *i* changes what position
   *i+1..i+3* decode as, so a later iteration of a per-byte scan can
   spuriously "find" and clobber bytes that were never a real pointer field,
   just because an earlier patch left a matching 4-byte pattern behind. Over
   a big enough region this isn't rare. Fixed by (a) reading from the stable,
   pre-edit `headerBytes` and writing only into the separate `outBytes`
   buffer (no read/write aliasing within one pass), and (b) restricting to
   4-byte-*aligned* candidate positions — empirically, all 71 real string
   matches landed exactly on an aligned absolute file offset, zero real
   matches were unaligned. The pre-pool loop was rewritten the same way for
   consistency (see bug 2c below — it turned out to have the identical
   latent issue, just never triggered before due to its much smaller
   original scan range).

**Verification** (against the real, shipped `ENGLISH.pak`,
`mad2/Content/Streams/win/VolcanoRave_unpacked/ENGLISH.pak`, plus a broader
sweep):

- **No-op round-trip** (dump → recompile with no edits) is still
  byte-identical, both before and after this fix — regression-tested across
  **all 143 `.pak` files** found under `mad2/`, `mad2russia/`, `mad2demo/`'s
  `Content/Streams/win/`: 143/143 pass.
- **Growing edit** (+28 characters / +56 bytes to one localized string):
  all 40 known-real tail→head string pointers keep their correct
  (unchanged) value at their new, shifted position; a broader sweep of 735
  raw `>= oldStringPoolEnd`-shaped 4-byte values in the tail all land at
  exactly `oldValue + shift` at their new position.
- **Shrinking edit** (string content reduced): `CompilePak` already pads the
  pool back up to its original encoded size whenever an edit would make it
  smaller (`stringPool.Write(make([]byte, paddingNeeded))`), so `shift` is
  mathematically always `>= 0` — a `.pak` never actually shrinks through
  `CompilePak` as written, regardless of how much string content is removed.
  A shrinking-content edit was still tested end-to-end (confirms the
  within-pool per-string offset remapping still works); as expected, the
  observed file-level `shift` was 0 and every check above passed trivially
  (nothing needed to move). The tail-fixup arithmetic itself is signed and
  handles a hypothetical negative `shift` correctly (same
  `uint32(int32(val)+shift)` idiom the pre-existing pre-pool loop already
  used), but there is currently no real edit that exercises a true negative
  shift given the padding behavior above.
- **Larger edits**: a synthetic "grow every string a bit" edit was run
  against `ENGLISH.pak` (67 of 68 strings grown, `shift=+1590`) and against
  three other real `.pak` files of very different sizes/levels/languages
  (`mad2demo/.../global_unpacked/ENGLISH.pak`, 17 KB; `mad2russia/.../RitesOfPassage/ENGLISH.pak`,
  2.5 MB, `shift=+3968`, 32,691 raw candidate tail pointers checked; VolcanoRave's own
  `GERMAN.pak`). All passed the same string-pointer and structural checks
  cleanly.
- **Object-graph structural check**: `mad2iga/objgraph.go`'s
  `TopLevelObjects()`/`FullWalk()` (independent of this fix's own code) were
  run against the original and each recompiled/regrown `.pak` — see bug 2c
  below for what this actually caught.

### Bug 2c — a second, distinct pointer class was found broken by the same mechanism, and fixed too (top-level `igObjectList` array)

While cross-checking bug 2's fix against `objgraph.go`'s independent
object-graph walker (not part of writing the fix itself — a genuine
after-the-fact validation), a **second, structurally distinct** bug turned
up, present identically before and after bug 2's fix (i.e. not a regression
it introduced): Section 1's own top-level `igObjectList` header carries an
array of `objectCount` pointers, one per top-level object
(`IGZ_FORMAT.md`'s Section 1 layout, `objArrayOff`/`objectCount`). Unlike the
string-reference pointers above, `objgraph.go`'s already-validated
`TopLevelObjects()` resolves these as **Section-1-relative**, not absolute
(`addr := g.Sec1Off + relPtr`) — confirmed directly: the array's raw stored
values are small (e.g. 2444, 2532...), and only make sense added to Section
1's own base offset (10496): `10496+2444=12940`, exactly `StringPoolEnd`,
i.e. the first real object right after the pool. Read as literal absolute
offsets (as this format's string pointers are), the same values point
nonsensically into the file's own 48-byte header region.

This means a blind byte-value scan cannot always tell whether a given
4-byte field is an absolute string pointer or a section-relative object
pointer — small numbers look the same either way. But this *specific* array
has a fully known, fixed shape (not a heuristic), so it can be fixed
precisely instead of guessed at: `CompilePak` now also reparses the header
via `ParseObjectGraph` (already-existing, already-tested code, reused as-is
from `objgraph.go`, same package) and walks this one array directly, adding
`shift` to any entry whose *resolved* absolute target (`Sec1Off + relPtr`)
moved past the old `StringPoolEnd`.

**Impact of the bug, measured before this part of the fix**: recompiling
`ENGLISH.pak` with even a single grown string, then running
`objgraph.go.TopLevelObjects()` against the result, resolved **61 of 97
top-level objects to garbage class indices** (`SoundDataInfo` for all 97 in
the original; nonsense values like class index 2097275, or wrapping back to
`igObjectList`/class 0, in the unfixed recompile). This is arguably a more
direct, more severe crash mechanism than the string-pointer bug above — it
breaks basic navigation to the majority of a `.pak`'s top-level objects, not
just specific localized-string lookups. After this fix: **0/97 mismatches**,
confirmed against `ENGLISH.pak` (both the small single-string edit and the
`+1590`-byte all-strings edit) and the three other real `.pak` files listed
above, `FullWalk()`'s recovered class histograms matching exactly
(`SoundDataInfo x90` / `LanguagePackInfo x1` for `ENGLISH.pak`, unchanged
pre/post edit).

### Bug 3 — stale `LanguagePackInfo.stringDict` pool-length field (FIXED, found in a later session after bugs 1/2/2c did not stop the crash live in-game)

**Why this was found at all**: the user tested a build with bugs 1 and
2/2b/2c fixed live, in the real game, on real hardware — and it still
crashed instantly on load. A byte-identical *unmodified* copy of the same
`ENGLISH.pak`, through the exact same file-redirect path, did **not**
crash — ruling out the redirect mechanism and confirming the bug was still
somewhere in how `CompilePak` reconstructs edited content. The next
session's lead was `docs/SCRIPT_FORMAT.md`'s PC-verified `StringInfo` class
(`unicodeText` rawref @0x20, `length` @0x30) — the hypothesis being that a
string's length might be double-recorded in some object separate from the
raw pool bytes, and left stale.

**That specific hypothesis was checked and refuted.** `StringInfo` does not
appear anywhere in a real `.pak` file's own class directory — confirmed
against 6 real `.pak` files (`VolcanoRave_unpacked/ENGLISH.pak` and
`GERMAN.pak`, `mad2demo`'s `global_unpacked/ENGLISH.pak` and
`Waterhole_Demo_unpacked/ENGLISH.pak`, `mad2russia`'s
`BraveNewWild/ENGLISH.pak` and `RUSSIAN.pak`): every one of them has
exactly the same tiny 4-5 class directory (`igObjectList`,
`LanguagePackInfo`, `SoundDataInfo` and/or `AnimatableTextureDataInfo`/
`igImage2`, plus a truncated `igHa` — almost certainly `igHandleList`, cut
off by a pre-existing, separate class-directory-parsing boundary issue in
`ObjectGraph.ParseObjectGraph`, not investigated further this session).
`StringInfo` is a `level.bld`-only runtime class (confirmed present in
`VolcanoRave/level.bld` per `SCRIPT_FORMAT.md`'s own earlier finding, where
its `unicodeText` field reads a flat `0` on every sampled instance) — the
`.pak`'s own localized strings are looked up into it at runtime some other
way, not via a serialized pointer/length pair `CompilePak` could ever leave
stale.

**Chasing the same class of bug a different way — "search the object graph
for fields near/pointing into the string pool region" — found a real one.**
Every `.pak`'s single top-level `LanguagePackInfo` object has a
`tfbscript_pc_field_schema.json`-registered field, `stringDict` (offset 24,
kind `"memoryref"`, PC-verified — `arkRegisterInitialize@LanguagePackInfo`,
`ScriptInfoLib.dll` `0x100236f0`). The schema only records its offset, not
its encoding; decoding its raw on-disk value empirically shows it is
**not** a plain pointer — it's `0x80000000 | poolByteSize`, the exact same
"top bit set = external fixup, low bits = payload" convention
`docs/TEXTURE_FORMAT.md` already documents for an unrelated field
(`igImage2.format`). `poolByteSize` is **exactly**
`StringPoolEnd - (LanguagePackInfo.Addr + 32)` — i.e. `LanguagePackInfo` is
always exactly 32 bytes and is always immediately followed by the raw
UTF-16LE string pool, and this field is the pool's own serialized byte
length, confirmed **byte-exact** (predicted pool-end position landed
exactly on this repo's own independently-computed `StringPoolEnd`, cross-
checked by reading the 4 bytes at that exact predicted offset and
confirming they're a real object's `classIdx` word — `2`, i.e.
`SoundDataInfo`, not garbage) against all 6 `.pak` files listed above.

Critically, **this field is never touched by either of the two existing
fixup passes**, because it doesn't look like a pointer to either of them:
its raw value (`0x80000000`-ish, ~2.1 billion) is far larger than
`meta.StringPoolEnd`'s upper bound check in the tail-scan pass
(`val <= uint32(len(headerBytes))+oldSize`), and it isn't a member of
`stringPointersMap` either (that map only has exact original string-pool
offsets as keys, not `0x80000000 | offset`-shaped values). So every content
edit `CompilePak` has ever produced has been serializing this field
**silently stale** — left holding the *original* pool byte length no
matter how much the pool actually grew or shrank.

**Reproduced concretely** against `ENGLISH.pak` (`VolcanoRave`): growing 67
of 68 strings (`shift = +1590` bytes) and recompiling with the bug still
present left `LanguagePackInfo.stringDict` reading `0x800007CC` (low 24
bits = 1996, the *original* pool size) in the output — while the real pool
now extends 1590 bytes further. Reading the 4 bytes at the stale predicted
boundary (`structuralPoolStart + 1996`) in the broken output landed **mid-
string**, not on any real object's `classIdx` word — i.e. anything trusting
this field to find where the pool ends and the next object begins would
misparse live string data as a "next object" and immediately choke on it.
This is a very plausible match for "instant, silent crash on load," and,
unlike bugs 1/2/2c, was never in scope for either of those two fixes.

**Fix**: `CompilePak` (`mad2iga/pak.go`) now reparses the same
`ParseObjectGraph`-derived header (already being reparsed for bug 2c's
top-level-array fix, reused as-is) to find the `LanguagePackInfo` top-level
object, and adds `shift` to its `stringDict` field's low 24 bits (preserving
the top byte/fixup-marker unchanged), writing the corrected value into the
output buffer the same way bug 2c's array-entry fixup already does. Refuses
(returns an error) rather than silently wrapping if a pathological edit
would overflow the 24-bit field.

**Verified**: after the fix, the same `+1590`-byte grow-edit produces
`stringDict = 0x80000E02` (low 24 bits = 3586 = 1996 + 1590, exactly the
new pool size), and the 4 bytes at the now-correctly-predicted boundary read
`02 00 00 00` — a clean `SoundDataInfo` `classIdx` word, not garbage. The
existing 143/143-file no-op round-trip (unaffected, since `shift == 0` there
makes this fix a no-op) and the existing structural `objgraph.go`
regression checks (0/97 top-level object mismatches, matching class
histograms) both still pass unchanged. A fresh end-to-end delivery was also
built and independently re-verified: `ENGLISH.pak` grown, repacked into a
scratch `VolcanoRave.bld` via `RepackFromArchive`, then **re-extracted back
out of that finished archive** and checked again — `stringDict`'s low 24
bits (6150) again landed exactly on a clean `SoundDataInfo` `classIdx` word
at the predicted boundary, confirming the fix survives the full
dump→edit→compile→repack→extract round trip, not just the isolated
`CompilePak` call.

**Honest confidence**: this is the strongest evidence-backed candidate
found so far for the still-open crash — a field structurally proven to
matter (some part of the format uses it to know where the pool ends) that
was provably being left stale by every prior edit, now fixed and verified
byte-exact through the whole real pipeline. It has *not* been confirmed by
actually launching the game (still not possible from this environment —
see `CLAUDE.md`), so it is not guaranteed to be the last bug in this file.
See "What's still open" below.

### Bug 4 — stale `SoundDataInfo.visemeStream` pointer, on ~90-98% of every real `.pak`'s objects (FIXED, found via real Ghidra decompilation of the shipped engine after bugs 1-3 still did not stop a live crash)

**Why this was found at all**: bugs 1-3 above were all found and verified by
static/structural analysis of this repo's own Go-side format model, each
confirmed only against this repo's *own* re-extraction/re-parsing tooling —
never against the real game engine's own loader code. After three rounds
of "plausible bug, fixed, verified against our own tools, still crashes,"
this session did the thing flagged as the natural next step if that kept
happening: opened `igCore.dll` and `ScriptInfoLib.dll`/`SoundInfoLib.dll`
in the project's live Ghidra instance (`ghidra-mcp`, project `mad2`) and
decompiled the *actual* PC engine loading/deserialization code, rather than
inferring another candidate from file structure alone.

**What was decompiled, concretely** (all addresses from the real PE
binaries, `create_function` + `decompile_function` via the Ghidra MCP
bridge, following this repo's established "auto-analysis doesn't create
`Function` objects at export addresses" workaround):

- `?postRead@LanguagePackInfo@TFBScriptInfo@Gap@@UAEXXZ` (`ScriptInfoLib.dll
  0x1001dd60`) — the one class-specific virtual override on `LanguagePackInfo`
  that looked like a plausible "on-load validation" hook. Decompiled: it's
  gated behind a global `__insight_...` flag (the TFBTool in-editor debug
  runtime) that's off in the shipped game, and even when active it only
  copies a raw pointer into a debug global and special-cases two CJK
  language IDs (`0x15`/`5`) for font metrics — **no size/bounds validation of
  `stringDict` against the pool at all, in the shipped build.** This answers
  the investigation's question 1 directly: the real loader does **not**
  explicitly validate `LanguagePackInfo.stringDict` against the actual pool
  size at load time — there is no such check to violate. Whatever crashes
  the game is not a rejected validation, it's a bad pointer being used.
- `?computeSize@igMemoryRefMetaField@Core@Gap@@UAEGXZ` (`igCore.dll
  0x10055670`) — unconditionally returns `8`. Confirms `igMemoryRefMetaField`
  (the meta-field type behind `stringDict`, per its own `k_stringDict`
  descriptor symbol) always occupies exactly 8 bytes at runtime: a size
  dword immediately followed by a second, distinct dword.
- `?getMemorySize@igMemoryRefMetaField@Core@Gap@@QBEHPBVigObject@23@@Z`
  (`igCore.dll 0x10005cb0`) — `return *(uint*)(obj+fieldOffset) & 0x7ffffff;`.
  Confirms the *first* dword is read with the top marker bits masked off —
  exactly matching bug 3's empirical `0x80000000 | poolByteSize` finding,
  now confirmed from the real accessor's own code, not just by
  pattern-matching observed values.
- `?getElementCount@...` (two overloads, `igCore.dll 0x10005cd0`/`0x10005cf0`)
  — computed on the fly as `getMemorySize() / elementTypeSize`, **never
  reads the second dword at all**. This ruled out an initial hypothesis
  (that the second dword might be a redundant stored "element count") and
  pointed at the alternative that it's a pointer instead.
- `igIGZLoader` (`igCore.dll`, the real binary-`.igz`/`.pak` loader class —
  distinct from `igIGXFile`'s XML/binary-hybrid authoring-tool read/write
  path, which was a dead end also checked and ruled out this session, see
  below) — `validateFixedHeader` (`0x10042920`) confirmed the file's own
  `Version` field (4 for MAD2) selects `offsetShift=0`, `offsetMask=0xFFFFFF`
  (low 24 bits), `memPoolShift=0x18`, `memPoolMask=0xFF` (top byte) — i.e.
  the real engine's own pointer-packing convention for version-4 IGZ files
  is genuinely "top byte = section index, low 24 bits = offset", matching
  `IGZ_FORMAT.md`'s documented segmented-pointer scheme byte-for-byte, now
  confirmed from the loader's own field-initialization code rather than only
  inferred from observed pointer values. `resolvePointer`/`fixupChunk`/
  `fixupChunks` (`0x428a0`/`0x4dac0`/`0x4e290`) confirmed the loader really
  does walk per-chunk-type fixup lists resolving exactly this way
  (`sectionBase[segID] + offset`), for several distinct field-kind
  categories (rawref-style self-resolving pointers, object refs, handle
  refs, memory blocks, etc.) — real evidence that MAD2's IGZ v4 *does* have
  a genuine chunk-based relocation mechanism, contrary to this doc's
  previous "no known generic mechanism yet" note under "What's still open"
  in `IGZ_FORMAT.md` (not fully mapped out this session — which numbered
  chunk-type case specifically carries plain, non-handle
  `igMemoryRefMetaField` pointers was not conclusively pinned down among
  the ~16 cases seen — but its *existence* and *encoding convention* are now
  directly confirmed from the binary, which is what this fix's correctness
  actually depends on).
- `?arkRegisterInitialize@SoundDataInfo@TFBSoundInfo@Gap@@CAXXZ`
  (`SoundInfoLib.dll 0x10003190`) — decompiled using the same bulk
  field-registration technique `CLASS_SCHEMA.md`/`SCRIPT_FORMAT.md` already
  established for `ActorInfo`/the `TFBScriptInfo` namespace, applied here
  for the first time to `SoundInfoLib.dll`'s `TFBSoundInfo` namespace (not
  previously schema-mined by this repo at all). `SoundDataInfo` registers
  exactly two fields: `_tfbSound` (offset `0x28`, `igObjectRefMetaField`)
  and **`_visemeStream` (offset `0x2c`, `igMemoryRefMetaField` — the same
  meta-field type as `LanguagePackInfo.stringDict`, i.e. the same 8-byte
  {size, pointer} runtime shape)**.

**The actual bug, confirmed empirically against real files**: reading
`LanguagePackInfo.stringDict`'s own *second* dword (object address + 28,
never touched by bug 3's fix, which only patches the *first* dword at +24)
in a real `ENGLISH.pak` gives `448` — and `Sec1Off (10496) + 448 = 10944`,
**exactly** `StringPoolStart`, byte-for-byte. This confirms the second dword
of an `igMemoryRefMetaField` really is a genuine pointer, encoded exactly
like the top-level object array's own entries (`Sec1Off`-relative, segID=0
implicit) — and, since a `.pak`'s pool always starts at a *fixed* position
(only its *end* moves under an edit), this specific instance of the pointer
never actually needs adjusting, which is exactly why it went unnoticed
through three prior rounds of investigation.

`SoundDataInfo.visemeStream` is the same field *shape*, but its pointer
(object address + `0x30`) targets that instance's own per-object viseme
(lip-sync) binary blob — data that is **not** fixed in position. Checked
directly against every real top-level `SoundDataInfo` object in
`ENGLISH.pak` (96 of the file's 97 top-level objects — `SoundDataInfo` is
the overwhelming majority class in every real `.pak` checked): **94 of 96**
resolve (`Sec1Off + storedOffset`) to a location physically *after* that
object's own position in the file — squarely in the region that moves
under a pool-resize edit (the remaining 2 have `size==0`, i.e. no viseme
data, and their pointer is inert). Cross-checked against 4 more real files
across 3 different levels, both `mad2`/`mad2demo` install trees, and
multiple languages (`GERMAN.pak`, `Waterhole_Demo/ENGLISH.pak`,
`ConvoyChase_Demo/ENGLISH.pak`) — the same pattern holds: 87-98% of
`SoundDataInfo` instances' `visemeStream` pointer targets a *later* file
position than the object's own.

**Why none of the three prior fixes ever touched this field**: the size
dword (`0x80xxxxxx`-marked, e.g. `0x80000021`) is exactly the same shape as
`stringDict`'s own size dword and is never reachable by the plain-pointer
heuristic passes (its magnitude is far outside every bound those checks
test). The pointer dword's raw on-disk value is a small `Sec1Off`-relative
offset (e.g. `2496`) — far below `meta.StringPoolEnd` and never a key in
`stringPointersMap`, so bug 2's two tests (`value >= StringPoolEnd`, `value
in stringPointersMap`) both silently pass over it, the same way they missed
`stringDict` before bug 3. Bug 2c's array-entry fix only touches the
top-level `igObjectList`'s own array, not fields nested inside individual
objects. Bug 3's fix is a hardcoded single-field, single-class,
single-object special case. This means **essentially every `SoundDataInfo`
object in essentially every edited `.pak` this tool has ever produced has
had a stale `visemeStream` pointer** — a dramatically larger blast radius
than bug 3 (one field on one object) — hitting 87-98% of a typical `.pak`'s
entire top-level object population.

**A real, distinct pitfall found and fixed while building this fix's own
code** (documented directly in `pak.go`'s comments, since it's a genuinely
subtle, easy-to-reintroduce trap): the natural-looking approach — reusing
`graph.TopLevelObjects()` (already used by bugs 2c/3) against `headerBytes`
to enumerate objects and find `SoundDataInfo` instances by class name —
turned out to be silently wrong. `TopLevelObjects()` resolves each entry as
`addr := Sec1Off + relPtr` (using the *stale*, unfixed on-disk `relPtr`,
since bug 2c's own fixup only ever writes its correction into `outBytes`,
never back into `headerBytes`) and then **indexes directly into whatever
buffer it was given** at that address. `headerBytes` is `oldSize` bytes
*shorter* than the real file from `StringPoolStart` onward (the pool is
physically excised there, not just logically resized — see `DumpPak`'s own
construction of `headerBytes`), so for any object whose true position is
`>= StringPoolEnd`, indexing `headerBytes[addr]` silently reads the bytes
belonging to a *different* object roughly `oldSize` bytes further into the
real file. Because `SoundDataInfo` (`classIdx == 2`) makes up the
overwhelming majority of objects in this region, the wrong object very
often *also* has `classIdx == 2` — so this failure mode doesn't error out
or look obviously wrong, it just quietly inspects/patches the wrong
object's fields. Caught empirically, not theoretically: an early version of
this fix using `graph.TopLevelObjects()` against `headerBytes` found only
90 of the file's real 97 top-level objects and, when tested against a real
grown edit, produced **zero actual byte changes** to any `visemeStream`
pointer — a silent no-op that looked superficially like "nothing needed
fixing" rather than "the fix is reading the wrong data." Fixed by walking
the top-level array manually and explicitly converting every
`Sec1Off + relPtr`-resolved *original-file* absolute position into the
correct `headerBytes` position (`pos - oldSize` for anything at or past the
old pool end, unchanged otherwise) before ever reading through it — the
same head/tail split `mapToOut` already encodes for the write side, applied
here on the read side.

**Fix**: `CompilePak` (`mad2iga/pak.go`) now walks Section 1's top-level
object array directly (not via `graph.TopLevelObjects()`, per the pitfall
above), identifies every `SoundDataInfo` instance by reading its `classIdx`
at the correctly-mapped `headerBytes` position, and — for each one whose
`visemeStream` pointer (object address + `0x30`) resolves to a target at or
past the old `StringPoolEnd` — adds `shift` to the stored offset, written
into `outBytes` the same way every fix above does.

**Verified**, with the same rigor as bugs 1-3:

- **No-op round trip**: still byte-identical across **all 143 real `.pak`
  files** under `mad2/`, `mad2demo/`, `mad2russia/`'s `Content/Streams/win/`
  (143/143 pass, unaffected by this fix since `shift == 0` there).
- **Growing edit, structural** (grow every localized string, real
  `CompilePak` call, checked against an independent re-parse of both the
  pre- and post-edit files via `objgraph.go`'s own `ParseObjectGraph`/
  `TopLevelObjects` — not this fix's own code): **0 mismatches across all
  96 `SoundDataInfo.visemeStream` pointers** in `ENGLISH.pak` (`shift =
  +4216` for a max-growth edit, `+1768` for the same test other bugs used),
  and **0 mismatches** repeated against `GERMAN.pak` (VolcanoRave, `shift =
  +1768`), `Waterhole_Demo/ENGLISH.pak` (160 `SoundDataInfo` checked, `shift
  = +2184`), `ConvoyChase_Demo/ENGLISH.pak` (88 checked, `shift = +1040`),
  and `mad2russia`'s `VolcanoRave/RUSSIAN.pak` (96 checked, `shift =
  +1768`) — 5 files, 3 levels, both `mad2`/`mad2demo`/`mad2russia` install
  trees, English/German/Russian. `LanguagePackInfo.stringDict`'s own two
  dwords re-checked in the same run and still correct (bug 3 unaffected;
  its pointer-half, per the analysis above, correctly stays unchanged since
  its target never moves).
- **Full pipeline, end to end**: `ENGLISH.pak` grown (68 of 68 strings),
  repacked into a scratch `VolcanoRave.bld` via `RepackFromArchive` (not the
  live `mad2/` install, not `mad2/Content/Streams/mod/VolcanoRave.bld`),
  then **re-extracted back out of that finished archive** and diffed
  byte-identical against the pre-repack edited `.pak`, then independently
  re-parsed and re-checked structurally: 96/96 `SoundDataInfo` objects'
  `visemeStream` pointers correct, 0 mismatches, confirming the fix
  survives the full dump → edit → compile → repack → extract round trip
  exactly like bug 3's own final verification did.

**What was checked and ruled out along the way** (real dead ends, not
guesses left unchecked): `igIGXFile`'s `readRawFieldMemory`/
`readBinaryMemory`/`writeBinaryMemory` family (`igCore.dll`) initially
looked like promising "how does a MemoryRef field get read" candidates, but
decompiling them showed they operate against `igDirectory`/`igDirEntry`
(TFBTool's XML/binary-hybrid authoring-tool save format), not the shipped
binary `.pak`/`.igz` files at all — a different read path entirely, not
reachable from the real game's own load sequence. `igMemoryRefMetaField`'s
`construct`/`allocateFieldMemory` (`igCore.dll 0x5aaa0`/`0x57610`) looked
like on-load hooks but decompiled to fresh-instantiate-from-pool logic (new
object construction, not deserializing existing bytes) — not the
load-time fixup path either.

**Honest confidence**: this is now the strongest evidence-backed candidate
in this whole investigation — not just "a plausible field, self-verified,"
but a bug whose *encoding convention* (segmented pointer, `Sec1Off`-relative)
is confirmed directly from the real engine's own loader code
(`igIGZLoader::validateFixedHeader`/`resolvePointer`), whose *field type and
offset* is confirmed directly from the real engine's own class registration
code (`arkRegisterInitialize@SoundDataInfo`), and whose *blast radius* (87-98%
of every real `.pak`'s objects, not one field on one object) is by far the
largest of any bug found in this investigation — a strong candidate to
explain "instant crash on load, every time, no exceptions" in a way bug 3
alone (one field, one object, a target that turned out not to even need
adjusting) never fully could. It has *still* not been confirmed by actually
launching the game (not possible from this environment — see `CLAUDE.md`);
see "What's still open" below for the honest residual-risk picture and the
concrete plausible crash mechanism this investigation's Ghidra work
suggests (a heap-overrun read during viseme-data deserialization, not a
clean validation failure).

### Ghidra investigation summary: what the real engine confirms, and the likely crash mechanism

The session that found and fixed bug 4 was specifically tasked with getting
past "plausible field, self-verified against our own tools" and grounding
the investigation in the real shipped engine's own code (`igCore.dll`,
`ScriptInfoLib.dll`, `SoundInfoLib.dll`, all reversed live via the
project's `ghidra-mcp` bridge — see bug 4's own writeup above for every
function/address decompiled). Answering the three questions that
investigation was framed around:

1. **Does the real loader validate `LanguagePackInfo.stringDict` (or any
   other field) against the actual pool/file size at load time?** No.
   `postRead@LanguagePackInfo` — the one class-specific hook that could have
   done this — is confirmed (by direct decompilation) to be a no-op in the
   shipped build, gated behind a debug/editor-only flag that's off at
   runtime. There is no size/bounds sanity check to violate; a bad pointer
   is simply used as-is.
2. **Is there any other field, anywhere in a `.pak`'s object graph, whose
   value depends on the string pool's size, that none of the prior fixes
   touched?** Yes — `SoundDataInfo.visemeStream`'s pointer half (bug 4
   above), confirmed via the exact same `igMemoryRefMetaField` mechanism as
   `stringDict` itself, affecting 87-98% of a typical `.pak`'s top-level
   objects rather than one single field on one single object. This is a
   direct, confirmed answer to the question bugs 1-3 could only speculate
   about in this section's own earlier revisions (see git history) — the
   "wider object graph... almost certainly contains many more
   object-to-object pointers" residual risk flagged in every prior revision
   of this section was real, and bug 4 is the first concrete instance of it
   found and fixed.
3. **Is the instant-crash-on-open timing more consistent with a cheap
   validation failure or a hard access violation?** The Ghidra evidence
   points at the latter. With no validation hook doing anything (per point
   1), a stale `visemeStream` pointer would be resolved by the generic
   `igIGZLoader` fixup pass (`fixupChunks`/`resolvePointer`, confirmed real
   and confirmed to use exactly the segmented-pointer convention this fix
   relies on) into an address that's off by `shift` bytes — plausibly still
   inside the file's own loaded Section-1 buffer for a small `shift`, but
   the subsequent read/copy of that viseme blob's `size` bytes (sizes up to
   ~512KB were observed for some instances) starting from the wrong offset
   can easily run past the end of that buffer for a large enough edit or
   unlucky offset — a heap-overrun read, not a graceful validation failure.
   This is consistent with "opens successfully, dies essentially
   immediately, no further log line from any mod": Section 1's fixup pass
   runs very early in the load sequence, well before any mod-visible
   gameplay state would be reachable. This has not been proven by an actual
   crash-dump/debugger session against the real game (not available in this
   environment — see `CLAUDE.md`), so it remains the best-supported
   *hypothesis* for the exact mechanism, not a certainty.

**What's still open (residual risk, be honest about this)**: `CompilePak`'s
fixup pass, even after all four fixes, is still not a *complete*,
schema-driven pointer-fixup implementation — it specifically handles string
references, the top-level object array, `LanguagePackInfo.stringDict`, and
`SoundDataInfo.visemeStream`, not a general "every `igObjectRefMetaField`/
`igMemoryRefMetaField`/`igRawRefMetaField` field on every class" pass. Two
concrete, unclosed gaps found and explicitly *not* chased further this
session, for honesty:

- `SoundDataInfo.tfbSound` (`igObjectRefMetaField`, offset `0x28`) — checked
  directly against all 96 real instances in `ENGLISH.pak`: **every one is
  `0`/null** at rest, consistent with `SCRIPT_FORMAT.md`'s own earlier
  finding that this field is a runtime-only cache slot populated by the
  audio system, never statically serialized — so it's confirmed *not* a
  live risk for `.pak` files specifically, but this was checked for exactly
  one class in exactly this file shape, not proven as a general rule for
  every `igObjectRefMetaField` in every class.
- The generic chunk-based relocation mechanism confirmed real in
  `igIGZLoader::fixupChunk` (bug 4's own writeup above) was not fully
  mapped out — which of its ~16 numbered chunk-type cases specifically
  carries plain (non-handle) `igMemoryRefMetaField` pointers like
  `visemeStream`'s was not conclusively pinned down; this fix's correctness
  rests on the confirmed *encoding convention* (byte-exact, verified
  against real files) rather than on identifying the exact case number.
  Fully closing this class of risk for *any* class/field combination this
  tool hasn't specifically checked needs either the general per-class
  field-pointer-type schema work `CLASS_SCHEMA.md` already describes as
  open, or mapping the chunk-type table completely — not more guessing from
  the writer side.

None of the four bugs above has been confirmed as *the* actual crash cause
by launching the real game and watching it succeed — not possible from this
environment (see `CLAUDE.md`). All four are real, reproduced,
mechanism-plausible bugs, all four are now fixed and regression-tested as
described above, and bug 4 in particular is now grounded directly in the
real shipped engine's own decompiled code rather than inferred purely from
file structure. Bugs 1, 2/2b/2c, and 3 were already tested live by the user
and did **not** stop the crash on their own — bug 4's dramatically larger
blast radius (87-98% of a typical `.pak`'s objects, vs. one field on one
object) and its direct grounding in real loader/class-registration code
make it the strongest candidate yet, but not a guaranteed final answer. A
fresh ready-to-test artifact incorporating all four fixes was built for the
user during this session: `ENGLISH.pak`'s localized strings grown (68 of 68
strings, the same `" >>> EDITED BY MAD2IGA..."` marker-text convention as
every previous session's artifact) and repacked into a scratch
`VolcanoRave.bld` (not the live `mad2/` install, and not
`mad2/Content/Streams/mod/VolcanoRave.bld` either — see the handoff notes
for the exact scratch path). If that still crashes the game in practice,
the next concrete step is closing the two residual gaps just above —
particularly mapping which `igIGZLoader` chunk-type case actually carries
`igMemoryRefMetaField`/`igObjectRefMetaField` pointers in general, so the
next candidate field can be found by reading the loader's own relocation
table rather than checking classes one at a time.
