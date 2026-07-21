# Unpack/repack with change detection (`mad2tool unpack-all` / `repack-changed`)

This documents a hashing/change-detection layer added on top of `mad2tool`'s
existing `extract`/`rebuild` pipeline, plus its real-file verification
evidence. See [`README.md`](README.md) for the format documentation index and
[`IGA_FORMAT.md`](IGA_FORMAT.md)/[`IGZ_FORMAT.md`](IGZ_FORMAT.md) for the
underlying container formats this builds on.

## What `mad2tool` already did

`mad2tool extract`/`rebuild` orchestrate `./mad2repack` as a subprocess (a
compiled binary, invoked by relative path) over `mad2/Content/Streams/win/`
specifically (hardcoded, not `mad2russia`/`mad2demo`-aware):

- `extract` unpacks every `.arc`/`.bld` into `mad2raw/<name>ARC|BLD/`, then
  recursively dumps every nested `.pak` to `<name>.pak.json` (**deleting the
  raw `.pak`** — `mad2iga.PakMeta.HeaderData` is self-contained, so nothing
  needs the raw binary afterward), every `level.bld` to `level.bld.json`
  (keeping the raw payload as `level.bld.orig`, since `CompileLevelCoords`
  patches specific float offsets into a copy of that original rather than
  re-serializing from scratch), every `.texs` to `<name>.texs.png` (see
  "extract/rebuild's own `.texs`/`.snds` handling" below — **this is new**,
  added alongside `unpack-all`'s own `.texs` support this session, but with
  a deliberately different on-disk shape), and every `.snds` per its own
  two-format split (also below).
- `rebuild` recompiles **every** `.pak.json`/`level.bld.json` found under
  `mad2raw/` unconditionally (no per-file change check — there's no cheap
  way to tell if one "really" changed without just doing the compile work),
  repacks the whole directory, then hashes the *output* archive against the
  original and deletes it if identical (a "prune after the fact" debloat
  step at the whole-archive level). `.texs`/`.snds` get their own, cheaper,
  genuinely per-file change check instead — see below, since blind
  unconditional re-encoding would be lossy for `.texs` (DXT1/DXT3
  recompression) for no reason on files nobody touched.

This was already a complete, working extract→edit→rebuild loop for
`.pak`/`level.bld`. What it did **not** have: any record of what an
editable file's content was at unpack
time, so `rebuild` has no way to know in advance which archives need
recompiling — it always does the full work and only prunes the result
afterward.

## What this adds

Two new `mad2tool` subcommands, `unpack-all` and `repack-changed`, built by
calling `mad2iga` directly (not shelling out to `mad2repack`) — same
functions `mad2repack`'s own `unpack`/`pak-dump`/`level-dump`/`pak-compile`/
`level-compile`/`repack` commands wrap
(`iga.Open`/`ExtractFile`/`RepackFromArchive`, `iga.DumpPak`/`CompilePak`,
`iga.DumpLevelCoords`/`CompileLevelCoords`), just orchestrated with an
up-front SHA256 baseline so `repack-changed` can skip both the compile step
*and* the repack step for anything that hasn't actually changed, rather than
doing the work and pruning after.

### `mad2tool unpack-all <gamedir> [outDir]`

Unpacks every `.arc`/`.bld` under `<gamedir>/Content/Streams/win/` into its
own subdirectory of `outDir` (default
`<gamedir>/Content/Streams/win/unpacked/`), one directory per archive named
after the archive file itself (e.g. `unpacked/Credits.bld/`). Each archive
entry is written out according to its format:

| Entry type | On disk | Editable form | Kind in manifest |
|---|---|---|---|
| `*.pak` | *(not kept)* | `<name>.pak.json` (`iga.DumpPak`) | `"pak"` |
| `level.bld` | `level.bld.orig` (raw, kept) | `level.bld.json` (`iga.DumpLevelCoords`) | `"level"` |
| `*.texs` (decodes OK) | `<name>.texs.orig` (raw, kept) | `<name>.texs.png` (`iga.DecodeTexs`) | `"texs"` |
| everything else (`.snds`, undecodable `.texs`, ...) | `<sanitized path>` (raw, kept) | *is* the raw file | `"raw"` |

`.pak`/`level.bld` handling exactly mirrors `mad2tool extract`'s existing
`processDumps` logic (same raw-keep/raw-drop decisions, same reasons). The
`"raw"` fallback covers every format `mad2iga` doesn't have a dump/compile
pair for (audio `.snds` — `fsb.go` only decodes to WAV/Ogg, there's no
encoder back to FSB3/IMA-ADPCM) by treating the raw extracted file itself as
the editable source: if you overwrite it with different bytes, those bytes
go into the archive verbatim on next `repack-changed`, no format-specific
compile step needed or attempted.

`.texs` (`mad2iga/texture.go`'s `DecodeTexs`/`DecodeImage`/`EncodeInsert`,
verified against DXT1/DXT3 S3TC — see `docs/TEXTURE_FORMAT.md`) gets the
same "dump to editable form" treatment as `.pak`/`level.bld`: the base
(largest) mip level is decoded to a real PNG (`<name>.texs.png`) editable in
any image editor, with the original bytes kept alongside as
`<name>.texs.orig` for the same reason `level.bld.orig` is kept — like
`iga.CompileLevelCoords`, `iga.EncodeInsert` only overwrites a byte range
(the base mip level) in a copy of that original file rather than
re-serializing the whole IGZ from scratch, so it needs the original to start
from. Mip levels beyond 0 and Sections 0/1 are therefore always carried
through byte-for-byte unchanged — see `EncodeInsert`'s own doc comment for
why full mip-chain regeneration is a known, out-of-scope limitation, not a
bug.

Not every `.texs` is guaranteed to decode (an exotic format variant outside
this repo's checked corpus is possible, even though 0 of 186 real samples
failed as of the codec's own verification). A decode failure at unpack time
is **not** fatal to the rest of the run: `unpack-all` prints a `Warning:
<archive entry>: could not decode texture, keeping raw passthrough only: ...`
line and falls back to `"raw"` handling for that one entry, same as any
other undecodable format — one unusual texture doesn't block unpacking
everything else in the archive. `repack-changed`, by contrast, treats an
*encode* failure (`iga.EncodeInsert` erroring — e.g. because the edited PNG's
dimensions no longer match the original texture's) as a hard error that
aborts the run, exactly like an unparsable `.pak.json`/`level.bld.json` edit
already does for the `"pak"`/`"level"` kinds — this repo's established
convention is "fail loudly on a bad edit", not silently drop it.

A `manifest.json` is written into each archive's subdirectory: the archive's
version/memory-pool-index/reserved fields (needed to reconstruct
`RepackOptions`), and per-entry name/CRC/offset/kind/on-disk-paths, plus a
SHA256 baseline hash of whichever file is the *editable* source for that
entry (`raw_hash` for `"raw"`/`"level"`'s `.orig`, `derived_hash` for
`"pak"`/`"level"`'s `.json`). Plain JSON, one file per archive, diffable.

### `mad2tool repack-changed <gamedir> [unpackDir] [outDir]`

Walks `unpackDir` (default `<gamedir>/Content/Streams/win/unpacked/`, i.e.
whatever `unpack-all` produced), and for each archive subdirectory:

1. Re-hashes every entry's current on-disk editable file and compares
   against `manifest.json`'s baseline.
2. For anything unchanged: nothing happens — it isn't recompiled, and (since
   it isn't added to the replacement map, see below) its original archive
   bytes are copied through byte-for-byte by `iga.RepackFromArchive` itself.
3. For anything changed: recompiles via the one function that already knows
   how (`iga.CompilePak` for `"pak"`, `iga.CompileLevelCoords` for
   `"level"`, `iga.EncodeInsert` for `"texs"`, or just the new raw bytes
   directly for `"raw"`) and adds it to a `map[string][]byte` of `{original
   entry name: new bytes}`.
4. If the map ends up empty (nothing in this archive changed), the archive
   is skipped entirely — **no `iga.RepackFromArchive` call happens at all**,
   and no output file is written. This is the actual "only repack what
   changed" behavior, not a rebuild-then-delete-if-identical debloat step
   like `rebuild`'s.
5. Otherwise, `iga.RepackFromArchive(manifest.SourcePath, outPath,
   replacements)` is called once for the whole archive — everything not in
   `replacements` is `ExtractRawData`'d straight from the original archive
   and passed through unmodified, which is what makes per-entry passthrough
   free and correct without this tool needing to reimplement it.

Output archives are written to `outDir` (default
`<gamedir>/Content/Streams/win/repacked_changed/`), one file per *changed*
archive only — archives with nothing changed produce no file there at all.

## `extract`/`rebuild`'s own `.texs`/`.snds` handling (distinct from `unpack-all`/`repack-changed`)

Added this session, alongside `unpack-all`'s own `.texs` support above —
**deliberately not identical to it**. `unpack-all`'s `"texs"` Kind keeps
both the decoded PNG *and* a `<name>.texs.orig` raw copy (see the table near
the top of this doc). `extract`/`rebuild` keeps **only the PNG** — no raw
`.texs` survives a successful decode. The two systems differ here on
purpose: `unpack-all` operates on an arbitrary `<gamedir>`, so it can't
assume anything about what else is lying around; `extract`/`rebuild`'s
`srcDir` is *always* `mad2/Content/Streams/win/` — the pristine, checked-out
game install, never mutated by either command — so `rebuild` can always
re-read a given entry's original bytes straight from there instead of
carrying a redundant copy under `mad2raw/`.

- **`.texs`** (`mad2tool/main.go`'s `dumpTexs`/`compileTexs`): `extract`
  decodes the base mip level to `<name>.texs.png` via `iga.DecodeTexs` and
  **deletes the raw `.texs`** on success (falls back to leaving the raw file
  in place, untouched, on a decode failure — same "warn, don't abort"
  behavior `unpack-all` uses). `rebuild` re-opens the *original* archive
  (`iga.Open` on the matching file under `mad2/Content/Streams/win/`,
  matched by `iga.SanitizePath`-normalized path) and re-reads that entry's
  pristine bytes fresh, every run.
  - **Per-file change detection, without a manifest file**: rather than
    keeping a hash sidecar (`unpack-all`'s approach) or unconditionally
    re-encoding every texture every rebuild (`.pak`/`level.bld`'s
    approach — unacceptable here since DXT1/DXT3 re-encoding is lossy, see
    `texture.go`), `compileTexs` re-derives the baseline in memory: decode
    the pristine original bytes the same way `dumpTexs` did at extract time
    and re-encode to PNG (`png.Encode`, deterministic — same codec, same
    input, same default options both times). If that byte-for-byte matches
    the PNG currently on disk, nothing was edited, and the **pristine
    original `.texs` bytes are used verbatim** — true byte-identical
    passthrough, no `EncodeInsert` round-trip at all. Only a PNG that
    differs from its own re-derived baseline gets `iga.EncodeInsert`'d.
  - Cleanup (`cleanupCompiledTexs`) only removes a temp `.texs` that has a
    `.texs.png` sibling — an undecodable fallback raw `.texs` (no PNG
    sibling) is left alone, since it's the permanent on-disk form for every
    future rebuild too, not a temp file.
- **`.snds`** (`mad2tool/main.go`'s `dumpSnds`/`compileSnds`): see
  [`AUDIO_FORMAT.md`](AUDIO_FORMAT.md) for the full two-format breakdown.
  There is **no FSB3/IMA-ADPCM encoder** in `mad2iga` (`fsb.go` only
  decodes) — a real, permanent asymmetry, not a gap to be filled here:
  - **Ogg Vorbis minority** (`iga.IsOggVorbis`, `iga.ExtractSnds` returns
    the bytes unchanged with `ext=".ogg"`): already a standard, directly
    editable/reusable container under a nonstandard extension. `extract`
    renames it in place to `<name>.snds.ogg` and **deletes the raw
    `.snds`**; `rebuild` copies it back to `<name>.snds` unconditionally
    (a lossless identity transform either direction, so no separate change
    check is needed — an unedited `.ogg` reproduces the exact original
    bytes on its own).
  - **FSB3 majority**: `extract` decodes to `<name>.snds.wav` *purely for
    listening/inspection* and **keeps the raw `.snds` alongside it,
    permanently** — there is no way back from an edited `.wav`. `rebuild`
    **never reads the `.wav` at all**; the original raw `.snds` bytes are
    always what gets packed. Editing the `.wav` is honestly a no-op, not a
    silently-dropped edit or a silently-served-stale-original — this is
    documented here and in `AUDIO_FORMAT.md`, not swept under the rug.
  - Cleanup (`cleanupCompiledSnds`) mirrors `cleanupCompiledTexs`: only
    removes a temp `.snds` that has a `.snds.ogg` sibling; an FSB3 raw
    `.snds` (which has a `.snds.wav` sibling instead) is never touched.

Verified against real game archives (`Credits.arc`'s Ogg-Vorbis
`hippo_grotto_v1.snds`, `Card_Match_GameARC`'s FSB3 dialogue clips,
`ConvoyChase_DemoARC`'s 16 `.texs` entries) — edit propagates correctly for
`.texs` and Ogg `.snds`, is honestly ignored for FSB3 `.snds` `.wav` edits,
and every untouched entry (including 15/16 untouched textures in the same
archive as one deliberately edited one) round-trips byte-for-byte identical
through `rebuild`. See this session's task notes for the exact commands; not
duplicated here to avoid drift from `unpack-all`'s own worked example below.

## Verification (real files, not assumed)

Performed against a throwaway copy of a real game install
(`/tmp/mad2tool_test/gamedir/`, seeded from this repo's own `mad2/`) so
nothing here touched the actual `mad2/` tree. Three archives from
`mad2/Content/Streams/win/`, chosen for size/format diversity:

- `Credits.bld` (456,826 bytes, `MemoryPoolIndex=1` — zlib-chunk compressed,
  7 `.pak` entries + 1 `level.bld`) — the one **edited**.
- `Minigame_Diving_Location_Menu.bld` (647,290 bytes, also
  `MemoryPoolIndex=1`, same shape) — left **untouched**, to prove the skip
  path.
- `Minigame_Diving_Location_Menu.arc` (4,673,590 bytes, `MemoryPoolIndex=0`
  — uncompressed, single `.snds` entry) — exercises the `"raw"` passthrough
  kind (the two `.bld`s above happen to contain only `.pak`/`level.bld`
  entries, no raw-kind ones) and an uncompressed archive.

Built with `cd mad2tool && go build -o /tmp/mad2tool .` (and
`cd mad2repack && go build -o /tmp/mad2repack .` for independent
cross-checking via `mad2repack unpack`/`pak-dump`, which share `mad2iga`'s
functions but are a different code path than `mad2tool`'s own manifest
logic).

### (a) An edit propagates, and only the edited entry changes

```
$ /tmp/mad2tool unpack-all gamedir
  Credits.bld: 8 entries -> gamedir/Content/Streams/win/unpacked/Credits.bld
  ...
```

Edited `unpacked/Credits.bld/ENGLISH.pak.json`'s `strings[0]` from
`"Paul Reiche III"` to `"PAUL REICHE III"` (same length, chosen deliberately
— see the "Known limitation" below for why).

```
$ /tmp/mad2tool repack-changed gamedir
  Credits.bld                              repacked (1/8 entries changed) -> gamedir/Content/Streams/win/repacked_changed/Credits.bld
  Minigame_Diving_Location_Menu.bld        unchanged, skipped (0/8 entries)
```

Extracted the *repacked* `Credits.bld` independently via `mad2repack unpack`
+ `pak-dump` (not `mad2tool`'s own code path):

```
$ /tmp/mad2repack unpack gamedir/Content/Streams/win/repacked_changed/Credits.bld -o verify_extract
$ /tmp/mad2repack pak-dump verify_extract/ENGLISH.pak /tmp/verify_english.json
$ python3 -c "import json; print(json.load(open('/tmp/verify_english.json'))['strings'][0])"
PAUL REICHE III
```

The edit is present in the rebuilt archive. Then extracted the *original*
`Credits.bld` the same way and hashed every one of the 8 entries from both
extractions against each other:

```
SAME     DUTCH.pak
DIFFERENT ENGLISH.pak  (orig=0c2932d1eab3702c24317c57321a89fc470f70280b79bfbeeba9858e80f1f262
                         new=cf1e62b5a1160015f3ce041fe646a7a2a30f01ea42f2789686770fd33c4f895e)
SAME     FRENCH.pak
SAME     GERMAN.pak
SAME     ITALIAN.pak
SAME     level.bld
SAME     SPANISH.pak
SAME     SWEDISH.pak
```

Exactly the one entry that was edited differs; the other 7 (including
`level.bld`, a different format entirely) are byte-identical to the
original archive's own copies — confirming `RepackFromArchive`'s
passthrough-for-unreplaced-entries behavior is what's actually doing the
work here, not a full re-encode of everything.

### (b) An untouched archive is genuinely skipped, not silently rebuilt

Hashed and timestamped every file in `unpacked/Minigame_Diving_Location_Menu.bld/`
before running `repack-changed` (which also processed the edited `Credits.bld`
in the same invocation):

```
$ find Minigame_Diving_Location_Menu.bld -type f | sort | xargs sha256sum > before_control_hashes.txt
$ find Minigame_Diving_Location_Menu.bld -type f | sort | xargs stat -c '%n %Y' > before_control_mtimes.txt
$ /tmp/mad2tool repack-changed gamedir
  Credits.bld                              repacked (1/8 entries changed) -> ...
  Minigame_Diving_Location_Menu.bld        unchanged, skipped (0/8 entries)
$ find Minigame_Diving_Location_Menu.bld -type f | sort | xargs sha256sum > after_control_hashes.txt
$ diff before_control_hashes.txt after_control_hashes.txt && echo IDENTICAL
IDENTICAL
$ find Minigame_Diving_Location_Menu.bld -type f | sort | xargs stat -c '%n %Y' > after_control_mtimes.txt
$ diff before_control_mtimes.txt after_control_mtimes.txt && echo IDENTICAL
IDENTICAL
$ ls gamedir/Content/Streams/win/repacked_changed/Minigame_Diving_Location_Menu.bld
ls: cannot access '...': No such file or directory
```

No file in its unpack directory was even rewritten (identical hashes *and*
identical mtimes — not just "recompiled to the same bytes"), and no output
archive was produced for it at all. As a control, to confirm the skip isn't
hiding a correctness problem (i.e. that a full repack of this same archive
really would be a no-op), ran `mad2repack repack` (full extract→repack
round-trip, unconditionally, `RepackFromArchive` with an empty replacement
map) against the pristine original directly:

```
$ /tmp/mad2repack repack gamedir/Content/Streams/win/Minigame_Diving_Location_Menu.bld -o /tmp/control_full_repack.bld
$ sha256sum gamedir/Content/Streams/win/Minigame_Diving_Location_Menu.bld /tmp/control_full_repack.bld
f4744e7a7b9ca7d7cf8da68bd66e982491970c2680072c119af5807b08881344  gamedir/.../Minigame_Diving_Location_Menu.bld
f4744e7a7b9ca7d7cf8da68bd66e982491970c2680072c119af5807b08881344  /tmp/control_full_repack.bld
```

Byte-identical — the round trip is lossless, so `repack-changed` skipping it
is a genuine "no work needed" decision, not a bug that happens to look like
one.

### `"raw"` kind (no dump/compile pair) on an uncompressed archive

`Minigame_Diving_Location_Menu.arc` unpacks to a single entry,
`builddata/win/gloria_dives_v1.snds` (`kind: "raw"`, no known `.snds`
encoder in `mad2iga` — see the table above). Appended 4 bytes (`TEST`) to
the raw file directly (4,669,694 → 4,669,698 bytes, a genuine size change,
and this archive's `MemoryPoolIndex` is `0` — uncompressed — a different
code path through `Repack`/`RepackFromArchive` than the two `.bld`s above):

```
$ /tmp/mad2tool repack-changed gamedir
  Minigame_Diving_Location_Menu.arc        repacked (1/1 entries changed) -> gamedir/Content/Streams/win/repacked_changed/Minigame_Diving_Location_Menu.arc
$ /tmp/mad2repack unpack gamedir/Content/Streams/win/repacked_changed/Minigame_Diving_Location_Menu.arc -o verify_arc
$ tail -c 20 verify_arc/builddata/win/gloria_dives_v1.snds | xxd
00000000: 6e98 b8f6 3870 fe79 3a03 455e 7f78 2451  n...8p.y:.E^.x$Q
00000010: 5445 5354                                TEST
$ stat -c%s verify_arc/builddata/win/gloria_dives_v1.snds
4669698
```

Confirms the raw-passthrough path (edit the extracted file directly, no
compile step) works, including a size change, on an uncompressed archive.

### `"texs"` kind: PNG edit round-trips through the real codec

Performed against a second throwaway gamedir seeded with three real archives
(again nothing under this repo's actual `mad2*/` trees was touched):

- `Credits.bld` (456,826 bytes, from `mad2/`) — regression control for
  `.pak`/`level.bld`, edited the same way as the test above
  (`ENGLISH.pak.json`'s `strings[0]`, `"Paul Reiche III"` →
  `"PAUL REICHE III"`).
- `DivingLocation_IslandFever.arc` (5,095,634 bytes, from `mad2russia/`) —
  contains exactly one `.texs`, `builddata/win/360-wireless-controller_128.texs`
  (90,108 bytes) — this is the same reference sample `mad2iga/texture.go`'s
  own doc comment already cites as visually confirmed correct (256×256,
  DXT3). The one **edited**.
- `DivingLocation_Waterhole.arc` (5,095,634 bytes, from `mad2russia/`) — left
  **untouched**, to prove the skip path still works alongside a `"texs"`-kind
  archive in the same batch.

```
$ /tmp/mad2tool unpack-all gamedir
  Credits.bld: 8 entries -> gamedir/Content/Streams/win/unpacked/Credits.bld
  DivingLocation_IslandFever.arc: 95 entries -> .../unpacked/DivingLocation_IslandFever.arc
  DivingLocation_Waterhole.arc: 95 entries -> .../unpacked/DivingLocation_Waterhole.arc
=== unpack-all complete: 3 archives ===
```

No decode-failure warning printed (the codec handled this sample cleanly, as
expected). The manifest records it as `"texs"` kind:

```json
{
  "name": "c:/tfb/content/builddata/win/360-wireless-controller_128.texs",
  "kind": "texs",
  "raw_path": "builddata/win/360-wireless-controller_128.texs.orig",
  "derived_path": "builddata/win/360-wireless-controller_128.texs.png",
  "raw_hash": "3bab5f2c...",
  "derived_hash": "01c99655..."
}
```

`360-wireless-controller_128.texs.png` is 73,436 bytes (down from the
90,108-byte source, expected — PNG-compressed RGBA vs. raw DXT3 blocks) and
decodes as a real, sane image, not garbage: `256×256` RGBA, per-channel pixel
stdev 36–46 (R/G/B), 2,867 distinct colors across 65,536 pixels — clearly
real image content, not a blank or corrupt buffer.

Edited the PNG directly (Python/Pillow, no `mad2` tooling involved): filled
the top-left 40×40 pixel block solid magenta `(255,0,255,255)`, leaving the
rest of the image untouched. Then, with `DivingLocation_Waterhole.arc`'s
whole unpack directory hashed+timestamped beforehand as a control:

```
$ /tmp/mad2tool repack-changed gamedir
  Credits.bld                              repacked (1/8 entries changed) -> .../repacked_changed/Credits.bld
  DivingLocation_IslandFever.arc           repacked (1/95 entries changed) -> .../repacked_changed/DivingLocation_IslandFever.arc
  DivingLocation_Waterhole.arc             unchanged, skipped (0/95 entries)
=== repack-changed complete: 2 repacked, 1 unchanged (skipped) ===
```

**The edit actually reached the new `.texs` bytes.** Extracted the repacked
archive independently via `mad2repack unpack` + `tex-extract` (not
`mad2tool`'s own code path) and re-decoded:

```
$ /tmp/mad2repack unpack .../repacked_changed/DivingLocation_IslandFever.arc -o verify_extract
$ /tmp/mad2repack tex-extract verify_extract/builddata/win/360-wireless-controller_128.texs verify_edited.png
  ... -> verify_edited.png (256x256, DXT3)
```

Sampling `verify_edited.png`: pixels inside the edited 40×40 region are
`(255, 0, 255, 255)` (magenta, as edited — checked at (5,5), (20,20),
(39,39)); pixels outside it are unchanged from the original decode, e.g.
`(60, 109, 109, 153)` at (100,100) — not magenta, confirming the edit didn't
leak outside its intended region (expected: DXT3 is block-compressed, so
only the ~10 blocks touching that 40×40 corner were re-encoded differently).

**Only the edited entry changed.** Extracted the *original*
`DivingLocation_IslandFever.arc` the same way and hashed all 94 non-metadata
entries from both extractions against each other: exactly one differs
(`builddata/win/360-wireless-controller_128.texs`,
`3bab5f2c...` → `b8eff21c...`), the other 93 are byte-identical — same
"only the changed entry moves" evidence standard as the `.pak` test above,
now confirmed for `"texs"`.

**The untouched control archive was genuinely skipped**, not silently
rebuilt: `DivingLocation_Waterhole.arc`'s entire unpack directory (97 files)
had identical SHA256 hashes *and* identical mtimes before/after
`repack-changed`, and no `repacked_changed/DivingLocation_Waterhole.arc` was
produced at all — same evidence standard as the `Minigame_Diving_Location_Menu.bld`
control test above.

**Existing `.pak`/`level.bld` handling in the same run is unaffected by this
work**: `Credits.bld` repacked `ENGLISH.pak` to the exact same bytes as the
pre-existing test above (`0c2932d1...` → `cf1e62b5...`, byte-for-byte
identical hash to the earlier, unrelated `.pak`-only test run) — this
change only added a new manifest `Kind`/switch case, it didn't touch the
`"pak"`/`"level"` code paths at all.

## `RepackFromArchive` stale-`DecompressedSize` bug — FIXED

`iga.RepackFromArchive` (`mad2iga/repack.go`) used to set each
`RepackEntry`'s `DecompressedSize` field from the **original** archive's
descriptor (`entries[i].DecompressedSize = fe.Descriptor.DecompressedSize`)
even when that entry was being replaced with data of a different
decompressed length — it was never updated to `len(replacement)` for the
replaced case. Confirmed directly: after growing `Credits.bld`'s
`ENGLISH.pak` from a 23,024-byte decompressed size to a true 23,070 bytes (by
extending `strings[0]` well past its original length), the *repacked*
archive's own file descriptor still reported `DecompressedSize=23024`
(checked directly via `iga.Open` on the output, not inferred) — identical to
the original, despite the real content now being 46 bytes longer.

This did **not** cause visible corruption in that specific test —
`extractCompressed` (`mad2iga/iga.go`) only uses `DecompressedSize` as a
*loop-continuation* heuristic across chunks, not to truncate a chunk's own
decompressed output, and that pak is small enough to fit in a single 32 KiB
chunk, so it always fully decoded regardless of what the stale field claimed.
The risk was narrow but real: an edit large enough to change how many 32 KiB
chunks an entry's data spans (crossing a chunk-count boundary) on a
compressed archive (`MemoryPoolIndex == 1`) could produce a `DecompressedSize`
that under- or over-states the true content across multiple chunks, which
could truncate or misread the entry on a future extraction by this or any
other reader that trusts the field — including, plausibly, the game's own
loader, which is the leading suspect for a real user-reported bug: editing
`ENGLISH.pak` to add even one extra character reliably crashes the game on
load. See `docs/IGA_FORMAT.md`'s "Two known content-editing bugs" section for
the fix, the investigation of a second, likely more severe bug found in the
same area (`mad2iga/pak.go`'s `CompilePak`, NOT fixed), and the honest
caveats around what this actually proves given nobody here can launch the
real game to confirm a crash is resolved.

**Fixed** in `mad2iga/repack.go`'s `RepackFromArchive`: when a replacement is
present, `entries[i].DecompressedSize` is now set to `uint32(len(replacement))`
— the replacement's true decompressed byte length — before it flows into
`Repack()`, instead of being left at the original entry's stale value.
Regression-tested via `mad2repack verify-all mad2/Content/Streams/win`
(95/95 archives, unchanged pass count before and after — this only affects
the replaced-entry code path, never exercised by `verify-all`'s
no-replacements passthrough).

The previous "practical mitigation" of only ever making same-length edits
(to sidestep this bug) is no longer necessary for *this* bug, but is still
recommended until the second, `pak.go`-level bug below is also fixed — a
same-length edit sidesteps both at once.
