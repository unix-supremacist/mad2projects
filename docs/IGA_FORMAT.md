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

**Live re-test after all four fixes: still crashes, identically.** Built a
fresh artifact with all four fixes applied (`ENGLISH.pak`, all 68 localized
strings grown, repacked into a scratch `VolcanoRave.bld`, deployed to
`mad2/Content/Streams/mod/VolcanoRave.bld`), launched the real game directly
(bypassing `mad2relauncher`'s crash-relaunch so the log wouldn't get
clobbered), and had the user load into VolcanoRave. The log confirms the
redirect fires correctly (`[mad2assetloader] Redirect (NtCreateFile):
...VOLCANORAVE.BLD -> ...mod\VOLCANORAVE.BLD`) and the process is gone
immediately after with no further log lines from any mod — the exact same
instant-death signature as every previous round. This rules out bugs 1-4
(all real, all still correctly in place) as sufficient, and motivated the
chunk-table investigation below.

### A newly-found, much larger candidate: Section 0's own chunk table

Decompiled `igIGZLoader::fixupChunk`/`fixupChunks`/`readSections`/
`parseSections` directly in `igCore.dll` via Ghidra (not previously done —
prior sessions worked from `validateFixedHeader`/`resolvePointer`-adjacent
functions only). This surfaced a structure neither `pak.go` nor any of the
four fixes above has ever read: **Section 0 has its own small header +
chunk table**, separate from the class directory / string pool already
documented above:

```
Section0+0x00  uint32  magic (0x49475A01)
Section0+0x04  uint32  version (4)
Section0+0x08  uint32  (unidentified, non-zero)
Section0+0x0C  uint32  dirTableOff (already known — class directory)
Section0+0x10  uint32  chunkCount
Section0+0x14  uint32  chunkTableOff (relative to Section0's own start)
...
Section0+0x28  uint32  (unidentified, "5" in this sample)
Section0+0x2C  uint32  strPoolOff (already known)
```

At `Section0 + chunkTableOff`, `chunkCount` consecutive chunk headers, each
`{type u32, f4 u32, f8 u32, count u32, stride u32}` (20 bytes), followed by
`stride - 20` bytes of the chunk's own payload; `stride` (not `count`) is
what advances to the next chunk, and the last chunk's end lands exactly on
Section 1's own start — confirmed byte-exact against a real `ENGLISH.pak`
(9 chunks: types 0, 13, 1, 5, 4, 6, 8, 9, 14; the type-1 "string" chunk
turned out to be 182 `C:/TFB/Content/Levels/.../*.wav` dev-path strings —
**unrelated** to the localized-string pool `CompilePak` edits, confirmed by
directly reading its bytes, not assumed).

This chunk table is what the real engine's `igIGZLoader::fixupChunk` walks
at load time (called from `parseSections`, called from `update()`'s state
machine after `readSections`) to do reference-resolution passes over the
loaded file — a totally different, and apparently much larger, mechanism
than the individual class-field pointers bugs 1-4 patch one at a time.
Chunk types 4, 5, and 6 in this same sample have counts of 188, 94, and 199
respectively — **481 reference-fixup entries in this one file alone**, none
of which any fix so far has touched, read, or even known existed.

Decompiling `fixupChunk`'s case 4 (and the shared helper it calls,
`FUN_10042800` at `igCore.dll:0x42800`) shows each entry resolves a
*segmented pointer* (same `segID:offset` scheme `ResolveSegPointer` already
uses, confirmed via `validateFixedHeader`'s version-gated mask/shift setup —
version 4 files use the 24-bit-offset/8-bit-segID split) to a real patch
address, then overwrites whatever small index is stored there with a
resolved reference. Critically, for IGZ version 4 specifically, the
*sequence* of patch-target indices is not a flat array of literal offsets —
it's decoded via a nibble-packed variable-length delta scheme
(`FUN_10042800`'s `param_4 >= 4` branch), which is why `bug 2`'s whole-file
literal-pointer byte scan structurally could never have found these: they
don't look like plain pointer values in the raw bytes at all.

**Update: confirmed live, this is the actual crash.** Attached a real
debugger (`wine winedbg --gdb`, launched as the direct parent of `Mad2.exe`
so Linux's `ptrace_scope=1` restriction doesn't block the attach — external
attach to an already-running process, e.g. via bare PID, was tried first
and failed with `error 87`/YAMA denial; launching as the debugger's own
child process avoids that entirely) against the live game, inside the same
gamescope window the user plays in. Set a conditional breakpoint at
`fixupChunk`'s entry and let the user play through to loading the edited
`VolcanoRave.bld` for real. **The game segfaults inside `fixupChunk`
itself**, confirmed by a real `SIGSEGV` caught in the debugger (not
inferred): `igCore.dll+0x4dfd1` (`fixupChunk+0x511`), instruction
`mov (%ecx,%edx,4),%edx`, disassembly immediately preceding it
(`shl $0x16` / `and $0x37ffff` / `or $0x80000`) matching **case 5**'s tail
exactly (the `(...) << 0x16 | (...) & 0x37ffff | 0x80000` line in the
decompilation above) — so the crash is in case 5, not case 4 as the
conditional breakpoint was originally set for (which is why it never
tripped; the actual crash happened on a later, unconditional continuation).

Concrete register/memory evidence at the crash, and what it means:

- `this` (kept in `ESI` in this optimized build, not `ECX` — a register
  allocation detail the decompilation abstracts away) `= 0x19d11a08`.
  `this+0x14 = 4` (confirms the `FUN_10042800` version-branch parameter
  really is the file version, resolving the earlier static-analysis
  confusion about that handoff).
- `this+0xc = 0x19d12800` — confirmed byte-for-byte to be the loaded
  **Section 0** buffer (`0x49475A01 0x00000004 0x1C130001 0x0000001C
  0x00000009 0x0000001C ...` — exactly Section 0's header/chunk-table
  fields dumped from the real file earlier in this doc), and a direct
  string check at `this_c+0x8C4` reads `"ls/Volca"` (mid-string from one of
  chunk 1's `.../Levels/VolcanoRave/...` paths) — confirms the mapping
  precisely.
- The crashing resolution chain: `EAX` (the computed "patch location",
  `piVar19` in the decompilation) `= 0x37fe2ca0`, entirely outside Section
  0's buffer — i.e. **not** the class-index table case 5 is supposed to
  patch into. Backed out via the section-base table
  (`this+0x824`'s own `+0x14`, `local_14 = 0x19d12400`, whose only non-zero
  entry is `local_14[0] = 0x37fe0e20`): `EAX - local_14[0] = 0x37fe2ca0 -
  0x37fe0e20 = 0x1E80`. So this patch resolves as `segID=0` (i.e.
  "relative to whatever section this pointer field's own section is" — the
  same convention `IGZ_FORMAT.md`'s `ResolveSegPointer` already documents),
  `offset=0x1E80`, into **Section 1** (confirmed: `local_14[0]` is a
  different buffer than the already-identified Section 0 buffer, and
  `0x1E80` is far too large to be a Section-0-relative offset into the tiny
  9-chunk table anyway).
- **`0x1E80` (7808 decimal) is past our edited pool's original end.** The
  localized string pool bug 1-4 already work with sits at Section-1-relative
  offset `0x2AC0 - 0x2900 = 0x1C0` through `0x328C - 0x2900 = 0x98C` (using
  this doc's own earlier `StringPoolStart`/`StringPoolEnd`/`Sec1Off` numbers
  for `ENGLISH.pak`). `0x1E80 > 0x98C` — this patch location's target lands
  *after* the pool, exactly the class of offset bugs 1-4's `shift` logic
  exists to correct for known fields, but **chunk 5's own patch-location
  offset itself is never touched by `CompilePak` at all** — it's embedded
  in Section 0's chunk-table payload (decoded via the nibble-delta scheme),
  which `CompilePak` correctly never edits since none of *its own bytes*
  change... except that those bytes *encode a target offset into Section 1*,
  and growing the pool moves everything in Section 1 past it by `shift`
  without that encoded offset ever being updated to match.
- The value actually read at the (wrong) patch location, `0xCF00FFDE`, is
  then used as an array index one instruction later
  (`mov (%ecx,%edx,4),%edx` — reading `local_18[0xCF00FFDE]`, where
  `local_18` comes from `this+0x810`'s class-metaobject table built by
  chunk 0), producing a wild address and the actual `SIGSEGV`. The
  underlying corruption (reading garbage instead of a small class index)
  happens one instruction earlier, at the crash-reported IP; the true root
  cause is one level further up the chain, in the stale patch-location
  offset itself.

**This confirms the theory, not just for chunk 5 but very likely chunk 4
and chunk 6 identically** (all three share the same `segID:offset`
resolution mechanism via `FUN_10042800` and `this+0x824`'s section-base
table — see the decompilation above): **any chunk-table fixup entry (types
4/5/6, 481 of them in `ENGLISH.pak` alone) whose embedded target offset
falls at or past the string pool's original end needs `shift` added to it,
exactly like bugs 2-4's known fields, but `CompilePak` has never done this
for any of them.** The fix needs to: (1) parse Section 0's chunk table
(now documented above), (2) for chunk types 4/5/6 (and check 7/0xb/0xc/0xf
too, even though this file doesn't use them), decode each entry's embedded
segmented-pointer patch-location, and (3) if the decoded `offset` component
is `>= old StringPoolEnd` (Section-1-relative) and `segID` corresponds to
Section 1, add `shift` to it and re-encode.

### The nibble-delta chunk encoding, fully reverse-engineered and validated

Static reading of `FUN_10042800` got the core shape right (4-bit nibbles,
continuation bit `0x8`, 3-bit value accumulation, `return prev + 4 +
delta*4`) but had the wrong payload start offset, which produced
plausible-but-wrong values (close in magnitude, never exact). Fixed by
brute-forcing the payload start position against the one concrete data
point the live debugger session gave us (the crashing entry's real decoded
offset, `0x1E80`, for chunk 5): **the chunk header is 24 bytes (0x18), not
the 20 bytes (0x14) assumed earlier** — `{type, f4, f8, count, stride}` (the
five fields already documented above) plus one more `uint32` at `+0x14`
whose purpose isn't pinned down (possibly a nibble/bit-length hint; not
needed to read or write the fixup values themselves, so not chased
further). The nibble payload starts at `chunk + 0x18`, not `chunk + 0x14`.

With that correction, decoding is not just plausible but **exactly right**:
chunk 5's entries are `[0x4, 0x1A0, 0x98C, 0x9E4, 0xACC, ...]` —
entry 2, `0x98C`, is byte-for-byte identical to this document's own
independently-derived `StringPoolEnd` (Section-1-relative) for
`ENGLISH.pak`. Not a coincidence: this is the loader's own string-dictionary
bookkeeping, encoded via this chunk mechanism, landing exactly where it
should. All three chunk types (4/5/6) decode cleanly with this correction,
each consuming all but 2-4 trailing bytes of their declared payload
(plausible end-of-stream padding).

**Decode algorithm** (nibble stream starting at `chunk+0x18`, one byte at a
time, low nibble first within each byte, byte pointer only advances after
the *second* (high) nibble of a byte is consumed):

```
cum := 0
for each of `count` entries:
    val := 0
    shift := 0
    loop:
        nibble := next 4 bits (low half of current byte, then high half,
                                advancing the byte pointer after the high half)
        val |= (nibble & 7) << shift
        shift += 3
        if nibble & 8 == 0: break   // no continuation bit -> done
    cum = cum + 4 + val*4
    // cum is a standard segmented pointer: segID = cum>>24, offset = cum&0xFFFFFF
```

**Encoding** (the inverse, needed to write shifted values back) is the
reverse of this: for each entry, `delta := (cum - prevCum - 4) / 4` (must
be an exact non-negative integer — `cum` values are always `prevCum + 4 +
4*k`), then emit `delta`'s bits 3 at a time, low-to-high, setting the
continuation bit (`8`) on every nibble except the last, packing two nibbles
per byte (low half first, matching the decoder).

**Fix scope, now fully understood (not yet implemented)**: unlike bugs 1-4,
which only ever change *values* at fixed byte positions, correctly fixing
this means the **re-encoded delta for a shifted entry can need a different
number of nibbles than the original** (a `shift` of a few thousand bytes
turns a small delta into a much larger one, needing more nibbles) — so a
correct fix must fully re-encode the affected chunk's payload from scratch
(recomputing every delta between consecutive corrected values, not
patching bytes in place), which changes that chunk's byte length, which
changes the chunk table's total length, which changes **Section 0's total
size**, which shifts **Section 1's absolute file offset** — on top of, and
compounding with, the shift already caused by the string pool growing
inside Section 1. This is a strictly bigger structural change than
anything `CompilePak` does today (bugs 1-4 only ever shift things *within*
Section 1; this shifts Section 1 itself, and needs the section descriptor
table's own offsets updated to match) — real, understood, tractable work,
just not a quick patch. Concretely: `CompilePak` needs to (1) parse and
fully re-encode chunks 4/5/6's payloads with corrected values, compute the
resulting `ΔSection0Size`, (2) shift Section 1 (and any later sections)'
descriptor offsets by `ΔSection0Size` in addition to the existing
string-pool `shift`, and (3) apply `shift` to any chunk-4/5/6 entry whose
Section-1-relative offset is `>= old StringPoolEnd`, using the encoder
above.

**What's still open (residual risk, be honest about this)**: `CompilePak`'s
fixup pass, even after all four fixes, is still not a *complete*,
schema-driven pointer-fixup implementation — it specifically handles string
references, the top-level object array, `LanguagePackInfo.stringDict`, and
`SoundDataInfo.visemeStream`, not a general "every `igObjectRefMetaField`/
`igMemoryRefMetaField`/`igRawRefMetaField` field on every class" pass, and
(per the chunk-table finding above) not the file's own chunk-based
reference-fixup mechanism at all. Two smaller, previously-known concrete
gaps, found and explicitly *not* chased further before the chunk-table
discovery above, for honesty:

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

## RESOLVED: the pak-edit crash is fixed, confirmed live end-to-end

**Bugs 5 and 6 together (both below) fully fix the long-standing crash.**
Grew all 68 localized strings in a real `ENGLISH.pak` (the same
`" >>> EDITED BY MAD2IGA <<<"` marker convention used throughout this
investigation), repacked into `VolcanoRave.bld`, deployed to the test
redirect slot, and had the user load into VolcanoRave for real (not under
a debugger — bug 6's fix was tested both under `wine winedbg --gdb` and
in an ordinary launch). **The level loads and plays completely normally**:
no crash, no corruption, full 3D rendering, working UI, live overlay mods
all functioning — and the edited marker text is visibly showing on the
in-game pause menu's title banner and all three buttons ("Volcano Rave
EDITED BY MAD2IGA", "EDITED BY MAD2IGA" on Help/Start/Quit), confirming
the edit is genuinely applied, not just non-crashing.

Both fixes were necessary; bug 5 alone (confirmed via live debugger) moved
the crash further into the load sequence but didn't resolve it — bug 6,
found by continuing the same live-debugger methodology into a second,
independent chunk-table consumer, closed the gap. See each bug's own
section below for the full technical writeup, including the concrete
evidence trail (a live-captured `SIGSEGV`, register/memory state at the
crash, and the exact chunk-table bytes before and after each fix).

## Bug 5 — Section 0's chunk table (fixed, and it worked: the crash moved)

Implemented in `mad2iga/chunktable.go`, wired into `CompilePak` in
`pak.go`. Full technical writeup of the discovery, the nibble-delta
encoding (fully reverse-engineered and validated — a decoded chunk-5 entry
matches this file's own independently-derived `StringPoolEnd` exactly),
and the fix's design (an isolated final splice on `outBytes`, applied
after every other fix, to sidestep a real coordinate-system conflict
between "positions within `headerBytes`" and "stored pointer values,
which stay in original-file coordinates" — see the code's own extensive
comments in `pak.go` and `chunktable.go`) is above, in the "A newly-found,
much larger candidate" section. Unit tests in
`mad2iga/chunktable_test.go` confirm the decode/encode round-trip,
including under a synthetic shift matching this session's real test edit.

**Confirmed live, with a debugger, that this fix is real and effective**:
built a fresh artifact with all five fixes (68/68 strings grown), launched
the actual game under `wine winedbg --gdb` (attached as the debugger's own
direct child process — external `ptrace` attach to an already-running PID
was tried first and blocked by `ptrace_scope=1`; launching as the
debugger's child sidesteps that restriction entirely), and had the user
play through to loading the edited `VolcanoRave.bld` for real, twice.

**The result: the crash moved.** Before bug 5, the game reliably crashed
inside `igIGZLoader::fixupChunk` itself (case 5's stale patch-location
offset — see the writeup above). After bug 5, that specific crash no
longer happens — the game progresses further into the load sequence
before hitting a **different** crash, confirmed via a second live
debugger session: a `SIGSEGV` calling through address `0x00000000`
(a null function pointer) inside `igObject::isOfType`
(`igCore.dll+0x56770`), called from `igObject::resetField`-adjacent
object-graph-traversal code. `isOfType` dereferences its own `this`'s
vtable pointer (`(**(code**)(*(int*)this + 0x48))()`) to walk a
metaobject-inheritance chain; at the crash, `this = 0x3803eec8`, and
reading `*(int*)this` yields `0x10CD0` — much too low a value to be a real
vtable pointer in any loaded module, meaning either `this` itself is a
wild/garbage object pointer, or it points at memory that was never
properly initialized as an `igObject` in the first place.

**Why this is good news, not a new dead end**: this crash is structurally
*different* in kind from the fixupChunk crash bugs 1-5 all addressed —
those were all "a stored offset points at stale data after the pool
moved." This one is "some object reachable during graph traversal has a
garbage `this` pointer," i.e. a distinct stale-*reference* bug (an object
handle, object-array entry, or similar not yet covered by any existing
fix) rather than a stale-*offset* one. It was **unreachable** before bug 5
— the game never got far enough into loading to hit it. This is
consistent with (and a live, concrete confirmation of) the "residual risk"
section's own repeated warning that the object graph "almost certainly
contains many more object-to-object pointers" than bugs 1-5 have found —
this is a real instance of exactly that, now actually observed rather
than speculated about.

**Confirmed reproducible, and the real caller identified.** Re-ran the
same test twice more, once with a conditional breakpoint on `isOfType`
(which turned out to add so much per-call overhead during `GLOBAL.BLD`'s
own object-graph deserialization — `isOfType` fires constantly during
completely ordinary loading, long before `VolcanoRave.bld` — that it made
the game appear to hang for 6+ minutes; not an actual hang, just breakpoint
evaluation overhead on a hot path, resolved by removing the condition and
just catching the natural `SIGSEGV`) and once completely bare. **The crash
is 100% deterministic**: `this=0x3803eec8`, `*(int*)this=0x10CD0` (the bad
"vtable pointer"), `EDI=0x19D148C2` — identical values across separate
process launches. This isn't memory-layout-dependent corruption; it's a
specific, reproducible bug tied to specific file content.

Walked the stack manually (the automatic `bt` stack walk breaks down after
frame 1 — `isOfType` and its caller don't follow the standard
push-ebp/mov-ebp,esp prologue `bt` assumes, so the frame-pointer chain it
tries to follow is stale/unrelated data, not a real bug in the crash
itself) by scanning the raw stack dump for plausible in-module return
addresses. Found `igIGZLoader::update` (`igCore.dll+0x51187`) and
`igIGZLoader::load` (`igCore.dll+0x51362`) on the stack — expected, this
is the same loader state machine bugs 1-5 already work with — and,
critically, `igIGZLoader::postReadChunks` (`igCore.dll+0x49278`, just past
that function's own entry at `0x49250`).

**This is a second, independent consumer of the chunk table**, not
previously examined. `parseSections` calls `fixupChunks` (bugs 1-5's own
target) *then* `postReadChunks` — a second full pass over the *same*
Section-0 chunk table. Decompiled `postReadChunks`/`postReadChunk`
(singular, `igCore.dll+0x48f10`): it walks the identical chunk list
(`this+0xc`'s chunk-count/offset — the exact structure
`chunktable.go`'s `readChunkTable` already parses) and, for chunk **type
5 specifically**, does something `fixupChunk`'s own case 5 never did:

```c
// (this + 0x824)'s own +0x14 -- the SAME section-base table used
// everywhere else for segID:offset resolution.
iVar1 = *(int *)(*(int *)(this + 0x824) + 0x14);
local_c = *(void **)(ArkCore() + 0xd8);  // igArkCore's class-registration table
do {
    uVar5 = FUN_10042800(...);  // decode next value -- SAME nibble stream bugs 1-5 already fix
    piVar9 = (int *)(iVar1[segID] + offset - (int)local_c * 4);
    (**(code **)(*piVar9 + 4))(0);
    (**(code **)(*piVar9 + 0x1c))(0);
    (**(code **)(*piVar9 + 0x14))(1);
    (**(code **)(*piVar9 + 0x30))();  // -- one of these four vtable calls chains into isOfType,
    ...                                //    where the live crash actually happens
} while (...)
```

This resolves the **exact same decoded segmented-pointer values** my
existing chunk-5 fix already shifts correctly (confirmed: the same
`FUN_10042800`/`iVar1[segID]+offset` resolution as `fixupChunk`'s own case
5, and as `chunktable.go`'s `decodeChunkPatchValues`) — but then
additionally subtracts `local_c*4` (a pointer into `igArkCore`'s
class-registration table) before treating the result as an **object
pointer** and calling four vtable slots on it. Two live possibilities,
not yet distinguished:

1. My existing chunk-5 fix (shift the offset by `shift` when it's `>=` the
   old pool end) is *correct* for what `fixupChunk`'s own case 5 needs,
   but `postReadChunk`'s case 5 needs the *same* corrected value — meaning
   the fix should already cover this, and the remaining bug is somewhere
   in *this* function's own `local_c`/`iVar1` handling instead (e.g. maybe
   `local_c`, the ArkCore class-table pointer, is itself something that
   needs to be current/correct and isn't related to the pool-shift at
   all).
2. Chunk type 5 is genuinely **dual-purpose** — its raw values mean
   different things to `fixupChunk` (a Section-1-relative segmented
   pointer, needing the pool shift) vs. `postReadChunk` (an index into
   ArkCore's class table, needing no shift at all, or a *different*
   correction) — in which case correctly fixing `fixupChunk`'s
   consumption may have been *necessary* (it was — the original crash
   genuinely moved) but isn't *sufficient*, and `postReadChunk`'s
   resolution formula needs its own, separate analysis to determine what
   (if anything) about it depends on the edit at all.

**Next step**: set a breakpoint directly inside `postReadChunk`'s case-5
loop (not at the `isOfType` crash site, which is too far downstream to see
`piVar9`/`local_c`/`iVar1` directly) — e.g. at the four vtable-call
instructions themselves — and dump `piVar9`, `local_c`, and `iVar1[segID]`
for the specific iteration that produces the bad object, to determine
which of the two possibilities above is correct. This needs its own
dedicated live session; the `isOfType`-conditional-breakpoint approach
that worked for finding *this* much is too slow for stepping through
`postReadChunk` itself, since it's on a comparatively hot path during
ordinary loading — use an unconditional breakpoint scoped tightly to just
the four vtable-call instructions' addresses instead, or single-step from
`postReadChunk`'s own entry once `VOLCANORAVE.BLD`'s specific load is
confirmed in progress (watch `mad2/logs/mad2.log` for the
`VOLCANORAVE.BLD` redirect line, *then* arm the breakpoint, to avoid
tripping on `GLOBAL.BLD`'s own unrelated chunk-5 processing first).

**Methodological check performed, worth recording**: before continuing
down this path, tested whether `gdb`'s default behavior (stop on every
`SIGSEGV`) was itself misleading — Wine/Windows SEH can silently recover
from an access violation the *app's own* exception handler catches, which
a debugger normally intercepts *before* that handler ever runs, potentially
making a harmless, always-present, silently-recovered exception look like
"the crash." Tested directly: `handle SIGSEGV nostop print pass` (forward
the signal to the app instead of stopping) on a fresh launch found the
early, unrelated-looking crash from one attempt above
(`igCore.dll+0x8fe0`, landing mid-instruction, seen once while chasing
`postReadChunk`) really is exactly that — a benign, silently-recovered
exception during ordinary early-boot loading (confirmed: the game
continued completely normally afterward, all the way through the title
screen and menu, with that signal delivered-and-passed-through). **But a
second signal, during/immediately after `VOLCANORAVE.BLD`'s load, was
*not* recovered from and genuinely terminated the process** (`winedbg`
reported exit via unhandled `STATUS_ACCESS_VIOLATION`), on the same
thread ID pattern as every previous capture of the `isOfType`/
`postReadChunk` crash. This confirms the crash this section has been
chasing is the *real*, fatal, unhandled one — not a debugger artifact —
and rules out the concern that gdb's stop-on-SIGSEGV default was
generating false leads. Good to proceed with the `postReadChunk`
breakpoint plan above with confidence.

## Bug 6 — chunk types 9 and 14's raw (non-nibble-encoded) pointers were never shifted (FIXED, and this is what actually closed the crash)

Rather than the planned `postReadChunk`-internal breakpoint (needing
precise timing to avoid `GLOBAL.BLD`'s own unrelated chunk-5 traffic), a
shortcut worked instead: `local_c` (`postReadChunk` case 5's ArkCore
class-table term) comes from a **singleton**, `ArkCore_function()`
(`igCore.dll+0x37c10`), which just `return`s a global variable —
`igCore.dll`'s own static data at Ghidra address `0x101972b0`, i.e. file
offset `0x1972b0`, runtime `igCore.dll_base + 0x1972b0`. Read directly
from a live session at the moment of the actual crash (interrupting `gdb`
with `SIGINT` right after a natural, uninstrumented `SIGSEGV` had already
stopped it — no need to precisely time a breakpoint):
`*(int*)(igCore.dll_base+0x1972B0)` → the ArkCore singleton pointer →
`*(int*)(singleton+0xd8)` → **`0` (NULL)**.

With `local_c == 0`, `postReadChunk`'s case-5 formula
(`piVar9 = iVar1[segID] + offset - local_c*4`) degenerates to exactly
`iVar1[segID] + offset` — **the same resolved address `fixupChunk`'s own
case 5 already gets right** (bug 5's fix). So chunk type 5 was not
actually dual-purpose after all (ruling out possibility 2 from the
previous section) — but the crash persisted anyway, meaning the *real*
problem was never chunk 5 at the crash site itself. This redirected the
search to the other chunk types `postReadChunk` handles: types 8, 9, and
14 (`0xe`), whose cases call `resolvePointer(this, sectionBaseTable,
rawValue)` directly — no `FUN_10042800`/nibble-delta involved at all.
Decompiling `resolvePointer` (`igCore.dll+0x428a0`) confirms it's exactly
`sectionBaseTable[segID] + offset` — a **plain, literal, unencoded
segmented-pointer dword** stored directly in the chunk's payload.

Checked directly against real `ENGLISH.pak` bytes: chunk type 8's single
raw value (`0x4`) doesn't need shifting, but **chunk type 9's
(`0x5E0A8`) and chunk type 14's (`0x5E244`) both land past the old string
pool's end** — needing exactly the same `+= shift` correction bugs 1-5
apply elsewhere, and **never received it**, since `chunktable.go`'s
`chunkTypesWithSegPointerPayloads` (the nibble-delta types bug 5 already
handles) never included these. Chunk types 9/14 resolve to `igHandleList`/
`igStringRefList` instances (`this+0x830`/`this+0x834` in `postReadChunk`,
matching `fixupChunk`'s own case-7/0xe assignments to the same fields) —
consistent with the crash's actual call chain (`igHandleList::dynamicCast`/
`igStringRefList::dynamicCast`, both plausible callers of `isOfType`
internally to validate the resolved object's type before casting).

**Fix**: added `chunkTypesWithRawPointerPayloads` (`{8, 9, 0xe}`) to
`mad2iga/chunktable.go`, handled separately from the nibble-encoded types
in `fixChunkTableSection1Refs` — read/write `count` plain `uint32`
segmented pointers directly (no delta decode/encode needed, and
critically no resizing: unlike the nibble-encoded types, a shifted raw
dword never changes byte length, so these chunks needed no `stride`
update and no knock-on Section-0-resize handling).

**Verified**:
- Structural: re-parsed the compiled pak's object graph after the fix —
  97/97 top-level objects still resolve correctly (same check bugs 1-5's
  own verification used).
- Direct byte check: chunk 9's offset went from `0x5E0A8` to `0x5EE78`
  and chunk 14's from `0x5E244` to `0x5F014` — both shifted by exactly
  `+0xDD0` (3536 decimal), matching this edit's real string-pool growth
  precisely.
- **Live, end-to-end, in the actual game**: see "RESOLVED" at the top of
  this section — the level loads and plays completely normally, with the
  edited marker text visibly rendering on the in-game pause menu.

**Both bugs 5 and 6 were necessary; neither alone was sufficient.** Bug 5
fixes the nibble-delta-encoded chunk types (4/5/6/7/0xb/0xc/0xf), which
`fixupChunk` needs. Bug 6 fixes the plain-raw-dword chunk types (8/9/0xe),
which only `postReadChunk` needs, in a completely separate pass over the
same underlying chunk table. Both were invisible to bugs 1-4's original
approach (reading known class fields one at a time) because neither is a
class field at all — both live in Section 0's chunk table, a
loader-internal mechanism with no counterpart in this codebase's
originally-documented object-graph model.

## In-process xdelta distribution (`mad2xdeltamod`/`mad2xdeltagen`) — same string-pool edit, applied live in memory instead of shipping a whole `.bld`

Separate motivation from everything above: this repo can't redistribute
whole `.bld`/`.arc` files (copyrighted game assets), and a `.pak` can't be
shipped as a loose file either — the game never opens a bare `.pak` via
`CreateFile`/`NtCreateFile`, only whole archives (see
`mad2assetloader/src/file_redirect.cpp`). A `.bld` can't be xdelta-diffed
directly either: each file entry inside it is independently zlib-chunk-
compressed (see "Compressed file data" above), so a one-byte text edit
reshuffles the entire compressed stream — there's no useful byte-level
similarity between an original and edited `.bld` to diff against. The goal:
ship a tiny binary diff against the game's own already-decompressed,
in-memory bytes, and apply it there, at runtime — no full archive or `.pak`
ever touches disk.

### The hook point

Found by reverse-engineering `igCore.dll` directly (Ghidra, live), starting
from zlib's own embedded error strings (`igCore.dll` statically links zlib
1.2.3 with no exported symbols) and working forward to the real call site:

- `Gap::Core::igArchive::computeChunkProperties` (`igCore.dll:0x37af0`) uses
  a literal `0x8000` — the same 32 KiB chunk size this doc's "Compressed
  file data" section already documents from the static Go side.
- The actual per-chunk decompressor, `igCore.dll:0x37ab0` (unnamed by
  Ghidra), is a thin wrapper: reads the 2-byte big-endian size prefix, then
  calls zlib's `uncompress()` (`igCore.dll:0x7d810`, confirmed via
  `inflateInit2_`/`inflate`/`inflateEnd`'s own embedded error strings —
  `"invalid stored block lengths"` etc. at `0x100a3d74` and neighbors).
- Dispatched via an async job (`Gap::Core::igArchive::startNewTasks`,
  `igCore.dll:0x408c0`) through `igJobManager` — decompression happens
  off the render thread, per chunk.
- `Gap::Core::igIGZLoader` (`igCore.dll`) is the `.pak`/`level.bld`-payload
  loader, a state machine (`update()`/`advanceState()`) stepping through
  `openFile` → `readFixedHeader` → `validateFixedHeader` → `readSections` →
  **`parseSections`** → `fixupChunks`. `load(this, char* filename, ...)`
  (`igCore.dll:0x512e0`) stores the filename directly at `this+8` (an
  `igStringRef` — confirmed to be a bare `char*` at its own offset 0, see
  `igStringRef::operator char const*`'s one-line body).
- `readSections` (`igCore.dll:0x483d0`) mallocs one buffer per active IGZ
  section and, for each, drives the *same* chunked-zlib pipeline above
  (`igFileContext::createWorkItem`/`igArchive::addWork`, blocking) — so by
  the time `parseSections` is called, every section's bytes are fully
  decompressed and sitting in their own buffer.

**`Gap::Core::igIGZLoader::parseSections` (`igCore.dll` RVA `0x51000`) is
the hook point**: called once per loaded `.pak`/`level.bld` payload, after
every section is fully decompressed but before anything is parsed, with the
filename already resolvable off the instance. Its own prologue — `push esi;
mov esi,[esp+8]` — is exactly 5 bytes and a clean instruction boundary, the
minimum a 5-byte JMP detour needs.

### New primitive: `Mad2HookUtil_InstallInlineHook`

This repo's existing hooking infra (`mad2hookutil`, see CLAUDE.md's "The IAT
hooking pattern") only did IAT patching and COM vtable-slot overwrites —
neither applies to an arbitrary internal function found by address, not
resolved by name or reached through a vtable. Added a third primitive: a
standard 5-byte relative-JMP inline detour. Caller supplies `stolenBytes`
(manually verified via disassembly, not auto-detected — no length-
disassembler here); `InstallInlineHook` copies that many original bytes
into a fresh `PAGE_EXECUTE_READWRITE` trampoline followed by a JMP back to
`target+stolenBytes`, then overwrites the target's own first 5 bytes with a
JMP to the caller's hook function (NOP-padding any remainder). The hook
function itself must be raw asm (GCC `__attribute__((naked))`, verified
against this exact toolchain's C symbol name-mangling — `i686-w64-mingw32`
prefixes cdecl symbols with `_`, confirmed via a standalone
compile+`objdump` smoke test before relying on it): entered via JMP (not
CALL) with whatever register/stack state the real function's entry point
expected, it must save/restore every clobbered register around its own
`call` to a C callback so the following stolen bytes execute in exactly the
state they originally would.

### The patch format and pipeline

`mad2xdeltamod/include/mad2xdelta_format.h` defines a small container
(magic `MAD2XD2`, then N regions, each `{kind, origSize, origCrc32,
patchSize}` + raw `xd3_encode_memory` output). `mad2xdeltagen` (native
Linux CLI, own CMake project like `mad2music/`, links the xdelta3 source
already vendored for `extern/jak-project`) takes original/modified region
byte pairs and produces one of these files; `mad2xdeltamod` (the in-game
mod, hooks `parseSections` as above) loads `*.mad2xdelta` files from
`<exe dir>/xdelta_patches/`, matches one to whichever file is loading, and
applies each region in memory. Two region kinds turned out to be necessary
(see below): `section` (an IGZ section, found by exact size+CRC32 match)
and `header` (a fixed buffer at a known offset, see "Section 0" below).

Both original and modified region bytes come from the same
`mad2repack pak-dump` → edit `pak.json` → `pak-compile` round trip already
used for whole-file repacking (this doc's bugs 1-6) — `pak-compile` already
computes every fixup a growing string-pool edit needs for the *whole
file*; `mad2xdeltagen`'s inputs are just byte-slices of that same compiled
output, sliced at the boundaries the running game actually uses (found
live, see below — not assumed from the file format alone).

### Live findings, most surprising first

Rebuilt the documented VolcanoRave edit (all 68 `ENGLISH.pak` strings
grown with the `" >>> EDITED BY MAD2IGA <<<"` marker, same as bugs 5/6's
own test artifact) as an xdelta patch and iterated live, each round-trip
via `mad2.log`:

1. **The runtime filename is CRC-keyed, not name-keyed.** `igIGZLoader`'s
   filename string is `"<archive path>/0x<CRC32, hex>"` — e.g.
   `.../VOLCANORAVE.BLD/0x75F7FBB1` — never the human-readable member name.
   Confirmed exactly: `0x75F7FBB1 == 1979186097`, `ENGLISH.pak`'s own CRC
   in `VolcanoRave.bld`'s file table (`mad2repack unpack`'s
   `metadata.json`). The archive format looks entries up by CRC (see
   `iga.go`'s `Entries[i].CRC`) and apparently never carries the name this
   far at runtime. Patch files are named `<ARCHIVE>.bld_0x<CRC32>.mad2xdelta`
   accordingly.
2. **A section's size field is packed, not a plain byte count.** Reading a
   section's raw size gave `268821472` for a section that's really `386016`
   bytes — caused a real, confirmed live crash (`mad2relauncher` auto-
   relaunching right after) the first time this was read unconditionally
   without masking. `268821472 & 0x07FFFFFF == 386016`, byte-exact. This is
   `Gap::Core::igMemory`'s own packed layout (`igMemory::getSize()`'s whole
   body is `return *(uint*)this & 0x7ffffff`) — the section buffer was
   allocated via `igMemory::mallocAligned`, which stores `{packed
   size|flags, pointer}`. The high 5 bits are flags (`getOwnsMemory`/
   `getHasAlignment`) that must be preserved (not zeroed) when writing a
   new size back.
3. **Section 1 (the string pool's section) is exactly `[Sec1Off:EOF]`.**
   For this `ENGLISH.pak` (396512 bytes total), the one active section has
   masked size `386016` — exactly `396512 - 10496`, and its CRC32 matches
   a diff of `bytes[10496:]` exactly. Same `Sec1Off=10496` this doc's Bug
   5 writeup already established from the static/offline side.
4. **Same-length edits work, live, end to end, right now.** A patch that
   only swaps same-length text (`Help→HELP`, `Quit→QUIT`, `Start→START`,
   `Volcano Rave→VOLCANO RAVE` — deliberately the same four strings bugs
   5/6's own pause-menu test used) applied in place and **rendered
   correctly on the real pause menu**, confirmed by the user live. This
   validates the entire mechanism end to end: hook point, filename/CRC
   matching, section identification, masked-size handling, in-place write.
5. **Growing the section alone doesn't render, and doesn't crash either —
   it's silently ignored.** The full 68-string grown patch applies cleanly
   (new pool allocation, correct CRC match, no crash) but the pause menu
   still shows the *original* text. Isolated by finding (4) above first —
   proves the gap is specifically about growth, not the mechanism itself.
6. **Growing needs "Section 0" patched too, and it's not at the offset
   file-format reasoning alone would suggest.** A growing string pool
   shifts every subsequent string's offset; those offsets live in a
   separate fixed-format buffer — `igIGZLoader+0xc` (confirmed
   version-independent: `*(void**)(this+0xc) = pvVar7` appears identically
   in all 3 `readSections` file-version branches) — which is exactly
   "Section 0" as this doc's Bug 5/6 sections already named it (and
   confirmed again here: this buffer's live first 64 bytes are byte-
   identical to a specific slice of the real file, see next point).
   First guess (`bytes[0:10496]`, "everything before Section 1") read
   *unrelated HLSL shader source text* ~450 bytes in — clearly reading
   past a too-small real allocation into neighboring heap memory. The
   actual size, read directly off the live instance
   (`sizeIfGe2 = *(uint*)(this+0x24)` for this file's version 4 — the
   same field `readSections`' non-`<2` branches use for this exact
   allocation's `mallocAligned` call) is **8448 bytes**, and it starts at
   **file offset 2048, not 0** — confirmed by an exact 64-byte hex match
   between the live buffer and `bytes[2048:2048+64]` of the real file.
   `2048 + 8448 = 10496`, i.e. Section 0's buffer is `bytes[2048:Sec1Off]`;
   the file's first 2048 bytes are read separately (`readFixedHeader`'s
   own preliminary `0x800`-byte probe) and, as far as could be determined,
   never persisted anywhere `parseSections` can reach.
7. **Even with both regions correctly identified and byte-verified
   (matching CRC32, clean write, no crash at write time), the growing
   patch still crashes** — shortly after `parseSections` returns, during
   the object-graph walk. This is the same crash *shape* as Bug 5 above
   (`this+0xc` = Section 0 buffer, patch target computed from its chunk
   table lands outside it) but reproduced through live in-memory patching
   rather than a whole-file swap, which this doc's Bug 5/6 fixes were
   originally verified against. **Leading, unconfirmed hypothesis**: the
   file's first 2048 bytes (`readFixedHeader`'s own probe, never patched
   by either region above) differ between original and grown files at a
   handful of scattered offsets (20-21, 32-33, 36-37 — small enough to be
   count/size fields, not content) — if `validateFixedHeader`/an early
   stage caches a value from there (e.g. a total chunk/object count) before
   `parseSections` ever runs, and `fixupChunk`/`postReadChunk` cross-check
   their resolved addresses against that cached value, patching Section
   0's *content* correctly while leaving this earlier-cached count stale
   would reproduce exactly this failure — content-correct, crash-anyway.
   **Not yet confirmed** — a live `wine winedbg --gdb` session (same
   technique bugs 5/6 used, see their own writeups above for the exact
   `ptrace_scope`-avoidance reasoning) was started to catch the exact
   `SIGSEGV`/`EIP`/register state, but paused before a crash was captured.
   Resume by running the game under `wine winedbg --gdb Mad2.exe` (see
   `mad2xdeltamod`'s own file header for the up-to-date hook offsets) with
   the current growing patch deployed, play into `VolcanoRave`, and capture
   `bt`/`info registers` at the resulting `SIGSEGV` — then cross-reference
   against Bug 5's own crash-site addresses (`igCore.dll+0x4dfd1`
   `fixupChunk+0x511`) to see if it's the literal same instruction or a new
   one.

### Status

- **Same-length string edits**: fully working, verified live in the real
  game (word swaps, capitalization, retranslation with matching length).
  Ship today via `mad2xdeltamod` + a `section`-only patch.
- **Growing edits** (the original motivating example — appending marker
  text): mechanically applies both regions correctly (verified: exact
  CRC32 match, clean decode, clean write, no crash at write time) but
  crashes shortly after in the object-graph walk, and does not yet render
  even on the runs that don't crash. Root cause not yet confirmed — see
  the debugger session above, not yet resumed.
- Every offset/RVA above is specific to this repo's current `igCore.dll`
  build; if it's ever replaced, all of them need re-verifying the same way
  they were found (Ghidra + live `mad2.log` cross-checks), not assumed
  stable.
