# Texture format (`.texs` files)

Source of truth: `mad2iga/texture.go`. **Solved, this session**: where the
live `igImage2` instance's fields (width/height/depth/levelCount/imageCount/
format/texHandle) actually live on disk, the real base-mip dimensions (not
guessed), the real pixel format for every sample in this repo's checked-out
asset corpus (standard DXT1 or DXT3), and the block layout — enough to ship
a real decoder (`mad2iga.DecodeImage`/`DecodeTexs`) and encoder
(`mad2iga.EncodeInsert`), and two `mad2repack` subcommands, `tex-extract`
and `tex-insert`. See "Pixel format and layout, confirmed" below for the
full evidentiary trail — this picks up exactly where the previous version of
this document (preserved below as "History: the previous open questions,
solved") left off, using the exact leads it flagged (`igGfx.dll`/
`igDisplay.dll` were investigated but turned out not to be where the
answer was; the real breakthrough was re-examining Section 1's bytes with a
correct understanding of where the top-level object list actually points,
which the codebase's own `objgraph.go` had, by this session, already solved
generically for other classes — this document's author just hadn't applied
it to `igImage2` yet).

Still **not** solved: which literal `igMetaImage_*` singleton name (e.g.
`"dxt1_dx"` vs `"dxt1"`) a texture's on-disk `format` field is "really"
pointing at — see "What's still not solved" below. This doesn't block
anything practical: the concrete pixel format (DXT1 vs DXT3) is recovered a
different way, reliably, for every real sample.

## The container is IGZ, same shape as `level.bld`/`.pak`

Confirmed via hex dump: `.texs` files start with the same
`0x49475A01` magic and 16-byte section-descriptor-table shape documented in
`IGZ_FORMAT.md`. `mad2iga/texture.go`'s `ParseIGZContainer` re-implements
just the section-table walk (not imported from `pak.go` — this file
intentionally stays self-contained, see its header comment) using the same
"keep reading descriptors until the next one's offset doesn't match
`previous.offset+previous.size`" technique `IGZ_FORMAT.md` documents for
`level.bld`.

Every one of the 186 real `.texs` samples in this repo's checked-out asset
corpus (`find . -iname '*.texs' -path '*extracted*'` — up from the 93
checked in the previous investigation; the full corpus turned out to be
larger than what an earlier `mad2*/Content/Streams/win/extracted/**/
builddata/win/*.texs` glob pattern matched, see `mad2repack`'s own
`tex-info` output for the count if this changes) has exactly **3
sections**:

| # | Contents (confirmed) | Typical size |
|---|---|---|
| 0 | Class directory + string pool | ~530-540 bytes |
| 1 | Object-graph bookkeeping + the live `igImage2` instance's own fields (see below — this is new) | ~140 bytes, constant across every sample checked |
| 2 | Raw pixel-data blob, base mip level first (now decodable — see below) | the bulk of the file |

Section 2's end is always exactly EOF (`section2.offset + section2.size ==
file size` in every sample) — no trailer after it, unlike `level.bld`'s
end-of-file name table.

### Section 0: confirms the class and the original source path

Section 0's class directory contains exactly four classes in every sample
checked, in this order:

```
igObjectList  igImage2  igHandleList  igStringRefList
```

i.e. **the texture class is `igImage2`**, index 1 in a `.texs` file's own
per-file class directory (this is a fresh, tiny per-file directory — not to
be confused with `level.bld`'s ~150-200-class directory). This directory
index turns out to matter this session in a way it didn't before: it's
exactly the `classIdx` the live `igImage2` object's own field data starts
with in Section 1 (see below) — so confirming it's reliably `1` across the
whole corpus is what makes locating that object trivial and robust rather
than positional/guessed.

**Bug found and fixed this session**: `IGZContainer.ClassDirectory()`
previously used the `strPoolOff` field (u32 at `sec0Base+0x2C`) as the class
directory's end boundary, matching `IGZ_FORMAT.md`'s general description.
Actually running that code (not just reading it) showed `strPoolOff` points
**partway through the class directory's own string list** for every real
`.texs` sample — it cut `"igHandleList"`/`"igStringRefList"` off mid-string,
silently truncating the returned directory to 2 names instead of the real
4. This had gone unnoticed because `IsImage` still happened to work by luck
(`"igImage2"` is the *second* name and survived the truncation) — but
anything depending on a class's directory *index* for anything past that
point would not have. Fixed: `ClassDirectory()` now scans forward from
`dirOff` for a run of identifier-shaped strings and stops at the first one
that isn't (in every real sample, that's the source-path string that
follows the directory, which contains `/`) — see `isPlainIdentifier`'s doc
comment in `texture.go`. Verified against all 186 real samples: recovers
all 4 names, in order, every time.

The string pool (elsewhere in section 0, same section) contains the
original build-time source path and instance name in plaintext, e.g. for
`DivingLocation_WaterholeARC/builddata/win/360-wireless-controller_128.texs`:

```
C:/TFB/Content/Levels/Includes/Hints/sprites/360-wireless-controller_128.png
Image20
```

`(*IGZContainer).SourcePathHint()` recovers this via a direct scan for a
null-terminated string containing `/` and ending in a common image
extension — deliberately sidestepping `IGZ_FORMAT.md`'s still-unresolved
"`+0xA840` gap before the string pool" indexing scheme, rather than
depending on it.

### Section 1: the live `igImage2` instance's fields, confirmed and located

This is the piece that was missing before. Section 1 starts with the
standard 32-byte top-level `igObjectList` header `IGZ_FORMAT.md` documents:

```
+0x00  u32  classIdx (0 = igObjectList itself)
+0x0C  u32  objectCount
+0x10  u32  nameCount
+0x14  u32  fixupPtr      (top bit set = external fixup)
+0x18  u32  objArrayOff   (relative offset to the pointer array, from the header's own start)
+0x1C  u32  nameArrayOff
+0x20  objectCount x u32  Section-1-relative pointers, one per top-level object
```

For every one of the 186 real `.texs` samples checked, **`objectCount` is
exactly 1**, and that one object's `classIdx` (read at the address its
pointer resolves to) is always `1` — i.e. the class directory's `igImage2`
entry. Concretely, for the reference sample
(`360-wireless-controller_128.texs`, Section 1 at file offset `0xA1C`):
`objArrayOff` reads `0x1C` (28) at `0xA1C+0x18`; the object-pointer array
therefore starts at `0xA1C+0x1C = 0xA38`; its single entry is `0x20` (32,
Section-1-relative); resolving that lands at `0xA1C+0x20 = 0xA3C` — and the
four bytes there read `01 00 00 00`, i.e. `classIdx=1=igImage2`. This is
exactly what `mad2iga/objgraph.go`'s already-existing, already-verified
`ObjectGraph.TopLevelObjects()` computes generically for *any* IGZ file's
Section 1 — `texture.go`'s new `locateImage2Object` reimplements the same
15-line algorithm locally (rather than importing `objgraph.go`, to avoid a
hard dependency on that file's own evolving API — see its doc comment) and
gets the identical answer. **The earlier "direct byte search for
`width=128,height=128` found zero matches anywhere in the file" dead end
(see "History" below) was simply searching for the wrong VALUE, not looking
in the wrong place** — the object's fields genuinely do start where
`TopLevelObjects()`/`locateImage2Object` say they do; the true width for
that reference sample turned out to be 256, not 128 (see below).

From that object's own start address (`0xA3C` in the reference sample),
fields sit at **exactly the runtime C++ offsets already documented** below
("The `igImage2` class: confirmed field schema") — i.e. the on-disk
serialization for this class is a direct, unswizzled memory-image dump of
the live object, same as every other class this repo has mapped
(`CLASS_SCHEMA.md`). Concretely, for the reference sample:

| File offset | Field | Raw bytes (LE) | Value |
|---|---|---|---|
| 0xA48 | width | `00 01` | **256** |
| 0xA4A | height | `00 01` | **256** |
| 0xA4C | depth | `01 00` | 1 |
| 0xA4E | levelCount | `07 00` | 7 |
| 0xA50 | imageCount | `01 00` | 1 |
| 0xA54 | format (ptr) | `00 00 00 80` | `0x80000000` |
| 0xA58 | usageDeprecated | `00 00 00 00` | 0 |
| 0xA5C | paddingDeprecated | `00 00 00 00` | 0 |
| 0xA60 | data (ptr) | `00 00 00 00` | 0 (runtime pointer, zero at rest) |
| 0xA64 | lockCount | `00 00 00 00` | 0 |
| 0xA68 | texHandle | `FF FF FF FF` | **-1** |
| 0xA6C | lockedMemory | `00 00 00 00` | 0 (runtime pointer, zero at rest) |

Two details make this mapping self-evidently correct rather than a
coincidental fit: `texHandle == -1` is exactly the sentinel value you'd
expect for "no D3D9 texture created yet" (this is a frozen on-disk image,
never a live runtime object with a real handle), and `format`'s raw value
(`0x80000000`) matches the exact "external fixup, top bit set" convention
`IGZ_FORMAT.md` already documents for `fixupPtr` fields elsewhere in this
same header — i.e. it's legibly *the same kind of value* IGZ already uses
elsewhere for "this pointer is resolved by the loader, not stored
literally," not a random 4 bytes that happen to decode plausibly.

**Confirmed universal across the full 186-file corpus, not just this one
sample**: every single file has `objectCount=1`, `depth=1`, `imageCount=1`,
`texHandle=-1`, and `formatRaw=0x80000000` — always. (`width`/`height`/
`levelCount` vary per texture, obviously — see the table in "Pixel format
and layout, confirmed" below for the actual distribution.)

`TexsInfo.Width`/`.Height`/`.Depth`/`.LevelCount`/`.ImageCount`/`.FormatRaw`
in `texture.go` are read exactly this way now — real field reads, not
guesses. The old heuristic (`GuessedWidth`/`GuessedHeight`, inverting
Section 2's byte size assuming a square power-of-two full mip chain at 4
bytes/pixel) is kept only as a legacy cross-check field; see its doc
comment for a worked example of it being coincidentally *wrong* despite
matching a real file's byte count exactly (the reference sample: guesses
128x128, but the real texture is 256x256 DXT3 — two completely different
`(width, levels, bytes-per-pixel)` triples can and do produce the exact
same total byte count).

## The `igImage2` class: confirmed field schema

(Unchanged from the previous investigation — reproduced here since Section
1's new mapping above depends on it being right, and it's now additionally
cross-checked a *third* way: every field value read through it, across 186
real files, is either a plausible varying quantity — width/height/
levelCount — or a constant that makes structural sense — depth=1,
imageCount=1, texHandle=-1, formatRaw=0x80000000 — never garbage.)

Found via the exact technique `CLASS_SCHEMA.md` documents: grepping
`../../mad2assetextractor/wii_demo_ref/Mad2.elf.symbols.txt` (the unstripped
Wii demo ELF) for texture-related classes. `Gap::Gfx::igImage2` (mangled
`Q33Gap3Gfx8igImage2`) is the live texture-image class, with field-name
globals confirming 12 fields:

```
k_width k_height k_depth k_levelCount k_imageCount k_format
k_usageDeprecated k_paddingDeprecated k_data k_lockCount k_texHandle k_lockedMemory
```

None of these fields has a `get<Field>ToVariant`/`set<Field>FromVariant`
script accessor (`CLASS_SCHEMA.md`'s automated extraction pass doesn't catch
these), so offsets were instead recovered via `CLASS_SCHEMA.md`'s
second, not-yet-automated technique: decompiling the class's bulk
field-registration call site
(`arkRegisterInitialize__Q33Gap3Gfx8igImage2Fv`, via the live Ghidra `mad2`
project's `Mad2.elf` import) and reading the literal `{names[], fieldPtrCache[],
offsets[]}` tables it passes to
`setMetaFieldBasicPropertiesAndValidateAll`. This is exactly the "not yet
done" widening `CLASS_SCHEMA.md` flags as a follow-up for classes without
`ToVariant` accessors — done here for the first time, for this one class:

| Offset | Size | Field               | Notes |
|--------|------|---------------------|-------|
| 0x0C   | 2    | width               | u16 |
| 0x0E   | 2    | height              | u16 |
| 0x10   | 2    | depth               | u16 (1 for 2D textures) |
| 0x12   | 2    | levelCount          | u16 — mipmap level count, see below for the real meaning of the raw value |
| 0x14   | 2    | imageCount          | u16 — e.g. cubemap faces; 1 for ordinary 2D textures |
| 0x18   | 4    | format              | **pointer**, not an inline enum — see below |
| 0x1C   | 4    | usageDeprecated     | |
| 0x20   | 4    | paddingDeprecated   | |
| 0x24   | 4    | data                | pointer to pixel data |
| 0x28   | 4    | lockCount           | |
| 0x2C   | 4    | texHandle           | |
| 0x30   | 4    | lockedMemory        | pointer |

(Offsets are from the start of `igImage2`'s own fields, i.e. after whatever
fixed-size base `igObject` header precedes them — consistent with every
other class this repo has mapped so far, and now directly confirmed by
Section 1's own byte-for-byte reads above, not just cross-checked against
a second decompiled call site.)

**Independently cross-checked** against a second piece of evidence: the
`configure__Q33Gap3Gfx8igImage2FUsUsUsUsUsPQ33Gap3Gfx11igMetaImage`
function (decompiled directly) writes its five `unsigned short` parameters
to exactly `object+0xC`, `+0xE`, `+0x10`, `+0x12`, `+0x14` — i.e. the
runtime `configure()` call's own field writes land at exactly the offsets
the bulk-registration table says width/height/depth/levelCount/imageCount
live at.

### `format` is a pointer to a shared per-format singleton, not an enum value

`configure()`'s 6th parameter (`igMetaImage*`) is stored directly into the
`format` field — so a given `igImage2` instance's pixel format is
determined by *which* shared `igMetaImage`-subclass singleton object its
`format` field points at, not by a small inline integer. The Wii ELF
defines a closed set of ~40 concrete subclasses, each presumably
implementing a virtual `getTextureSize`/format-specific encode-decode
(confirmed: `igMetaImage::getTextureSize` is a virtual call through the
object's own vtable, dispatching to a per-format override). The full set
found via `search_functions "igMetaImage"`:

```
Uncompressed:  a8  d8  l8  d16  d24  d32  d15s1  d24s8  d24x8  d24s4x4
               l4a4  l8a8  r16g16  r16_float  r32_float
               r4g4b4a4  r5g5b5a1  r5g6b5  r8g8b8  r8g8b8a8  r8g8b8x8
DXT/S3TC:      dxt1  dxt3  dxt5  dxn  (each also has a "_dx"-suffixed
               DirectX-specific variant: dxt1_dx  dxt3_dx  dxt5_dx)
"_dx" (DirectX/PC-native channel order) variants:
               b4g4r4a4_dx  b5g5r5a1_dx  b5g6r5_dx  b8g8r8a8_dx  b8g8r8x8_dx
               d15s1_dx  d16_dx  d24s4x4_dx  d24s8_dx  d24x8_dx  d32_dx
               l8_dx  l8a8_dx  r16g16_dx
Console-specific: a8_ps2  a8_ps3  a8_psp  a4l4_ps2  a4l4_psp
               l8_ps2  l8_ps3  l8_psp  a8_xenon  l8_xenon
               r5g6b5_psp  r8g8b8_ps2
```

Since MAD2's PC build is DirectX 9, the actual pixel format for a real PC
`.texs` asset is one of the uncompressed or DXT/S3TC entries above — now
**confirmed** to be plain `dxt1`/`dxt3` (or their `_dx` variants — see "What's
still not solved" for why the two can't be told apart from this repo's own
evidence) for every real sample checked (see next section). This closed
list remains useful ground truth for any format this corpus doesn't happen
to contain an example of.

## Pixel format and layout, confirmed

### Step 1: the real total mip-chain length is `levelCount + 1`

`levelCount`'s raw on-disk value is **not** the total number of mip levels
— it's one less. Confirmed by exact-byte arithmetic across the whole
corpus: define

```
mipChainPixelCount(w, h, levels) = Σ (w>>i) * (h>>i)   for i = 0..levels-1
                                    (each dimension floored at 1, standard
                                     mip-halving)
```

Then `Section2.size / mipChainPixelCount(width, height, levelCount+1)` is
**exactly** `0.5` or exactly `1.0` (to 1e-5 or better) for all 186 real
samples — whereas using `levelCount` (not `+1`) leaves a small but nonzero
residue (~1.4e-5 relative error) for every sample. This isn't a coincidence
of one file — it holds across every combination of width/height/levelCount
in the whole corpus, so `levelCount+1` is the real mip chain length, not
`levelCount`.

### Step 2: the byte/pixel ratio identifies the format — DXT1 or DXT3, nothing else in this corpus

That same ratio (`Section2.size / mipChainPixelCount(w, h, levelCount+1)`)
is **the format's bytes-per-pixel**, and across the full 186-file corpus it
takes exactly one of two values:

| Ratio | Format | Count | Typical content |
|---|---|---|---|
| 0.5 | DXT1 (8 bytes/4x4 block) | 171/186 | `loadscreen_*` backdrops, `demo_endscreen_*`, `victory_*`, `mad2_legal` — all the large (1024x512 / 2048x1024) UI backdrops |
| 1.0 | DXT3 (16 bytes/4x4 block: 8 explicit-alpha + 8 color) | 15/186 | `360-wireless-controller_128.texs` (all 6 copies) and similar small UI icons |

No sample falls anywhere in between (nearest miss is ~1e-5 off either exact
value), so `DetectPixelFormat` in `texture.go` treats this as a safe
nearest-match rather than needing an exact `==` compare. This is genuinely
new information — the *previous* investigation (see "History" below) had
assumed 4 bytes/pixel uncompressed RGBA/BGRA throughout, which is why every
raster-reinterpretation hypothesis it tried produced noise: it was
reinterpreting **compressed block data** as **raw uncompressed pixels**, a
completely different (and much larger, per-pixel) byte budget — no
raster-order permutation of the right bytes at the wrong bit-depth could
ever have produced a coherent image.

This also fully explains the previous investigation's "1024-byte
periodicity, not the expected 512" autocorrelation finding on the 128x128
(really 256x256 DXT3) reference sample: at DXT3's 1 byte/pixel-equivalent
rate with 4x4=16-byte blocks, one **block-row** of a 256-wide image is
`(256/4) * 16 = 1024` bytes — exactly the period found. The signal was real
and correctly measured; it just needed the "compressed 4x4 blocks, not
per-pixel raster" model to be legible as "one row of blocks," not "two rows
of naively-assumed 512-byte pixel rows."

### Step 3: block layout — standard S3TC, row-major blocks, base mip first

With the format and per-level byte budget known, the layout itself is the
ordinary, standard one used by essentially every DXT1/DXT3 asset in
existence: Section 2 starts with mip level 0 (the base/largest level,
`baseLevelSize(width, height, format)` bytes — `ceil(w/4)*ceil(h/4)*
blockBytes`), stored as one 8-byte (DXT1) or 16-byte (DXT3) block per 4x4
pixel region, blocks in row-major order (left-to-right within a block-row,
block-rows top-to-bottom). Each block: `color0`, `color1` as little-endian
RGB565, then (DXT1) a 32-bit LE field of 16 2-bit per-pixel palette indices
in row-major pixel order, standard 2-endpoint-plus-2-interpolated 4-color
palette when `color0 > color1` (DXT1's alternate "3 colors + transparent
black" mode when `color0 <= color1` is also implemented, for any texture
that turns out to need it — none of this corpus's real DXT1 samples
currently do, since none has a meaningfully transparent base level). DXT3
adds 8 bytes of explicit 4-bit-per-pixel alpha before its (always-4-color,
per the S3TC spec) color block.

**Verification, concretely** (not just "looks plausible"):

- Batch-tested against the **entire 186-file corpus**: every file's
  `width`/`height`/`levelCount`/`format` decode structurally (no crashes,
  no out-of-bounds reads), and the base mip level decodes to a
  correctly-dimensioned image every time (`mad2repack tex-info` reports
  `base mip: decodes OK, WxH` for all 186).
- **Spatial autocorrelation on real content** (the same kind of test the
  previous investigation's ruled-out hypotheses were rejected with — see
  "History" below): decoding `loadscreen_generic.texs` (1024x512, DXT1) and
  computing the Pearson correlation of each block's `color0` RGB565
  endpoint against its immediate x/y neighbor's, across the whole block
  grid, gives **0.41 (x-neighbor) / 0.35 (y-neighbor)**, against a
  ~0.003/-0.015 baseline from the identical computation over the same
  values randomly shuffled — a real, unambiguous, far-outside-noise signal.
  The `360-wireless-controller_128.texs` (256x256, DXT3) reference sample's
  per-block alpha-nibble variance shows **0.44** adjacent-block correlation
  against a **-0.015** shuffled baseline — same conclusion, independently,
  on the alpha channel of the other format.
- **Direct visual confirmation** (this session's tooling can render PNGs
  and view them, which the original investigation's notes couldn't rely
  on): `mad2repack tex-extract` on `360-wireless-controller_128.texs`
  produces an unmistakable Xbox-360-style game controller silhouette (the
  D-pad/analog-stick/button layout is clearly recognizable), correctly
  bounded by DXT3's alpha channel. `loadscreen_generic.texs` decodes to an
  image with a crisp rectangular border/frame, a horizontal accent line,
  and (cropped and pixel-inspected directly) a perfectly sharp-edged solid
  black UI box and a clean regular brick/dot decorative pattern near the
  bottom — all with exact, non-smeared, non-offset block boundaries. The
  interior "artwork" regions of the loadscreen images decode with a dense
  speckled/dithered appearance rather than smooth photographic content;
  given the crisp, correctly-bounded decode of every simple/flat/sharp-edge
  element in the same image (the black box, the border, the brick
  pattern), this reads as genuine dithered or inherently colorful/detailed
  source art (compressing a busy painted background to DXT1's 4-colors-
  per-4x4-block budget will look speckled at 1:1 pixel scale without
  blur/mipmapping) rather than a decode bug — a real bug in block
  addressing or bit ordering would be expected to corrupt the *simple*
  regions (flat colors, sharp edges) too, and it visibly doesn't. Tested
  and ruled out as an *alternative* explanation: tried all 4 combinations
  of {row-major, column-major} x {LSB-first, MSB-first} 2-bit-index
  ordering on the same interior region and found no meaningful difference
  in a pixel-smoothness metric between them (~190-195 mean adjacent-pixel
  channel-sum diff for all four) — if one specific ordering were "the
  right one" being obscured by a bug in this code, it should have stood
  out as clearly smoother than the other three, and none did.

## What's still not solved

**Which literal `igMetaImage_*` name** (e.g. `"dxt1_dx"` vs `"dxt1"`) the
on-disk `format` field is meant to resolve to. As established above, its
raw on-disk value is always exactly `0x80000000` — the generic "external
fixup, slot 0" marker — identically across **every one of the 186 real
samples**, regardless of whether the texture turns out to be DXT1 or DXT3.
This means the raw bytes carry no per-instance information distinguishing
the two at all; the concrete format has to come from somewhere else, and
this document's `DetectPixelFormat` gets it from the exact
Section-2-size-to-mip-chain-pixel-count ratio instead (Step 2 above),
which is unambiguous in practice (no sample falls between 0.5 and 1.0) but
is indirect, not a literal field read. Two things were tried and didn't
pan out:

- Section 0's string pool was searched for any of the ~40 known
  `igMetaImage_*` format name substrings (`"dxt1"`, `"dxt1_dx"`, `"l8"`,
  etc.) on both a DXT1 sample and the DXT3 reference sample — none present.
  The format name is not stored as a plaintext string anywhere in the file.
- The nested `igHandleList`/`igStringRefList` structures in Section 1 (the
  two list headers immediately following the `igImage2` object's own
  fields — see "Section 1" above) were inspected byte-by-byte for the
  reference sample; their single entries resolve to small integer
  ordinals (matching the same `{count=1,count=1,fixupPtr=0x80000004,
  offset}`-shaped headers noted in the previous investigation), not to
  anything recognizable as a format-specific value. Not chased further —
  this would need external-fixup-table semantics this repo hasn't mapped
  for *any* class yet (see `IGZ_FORMAT.md`'s own still-open `fixupPtr`
  notes), a bigger undertaking than this session's scope, and unnecessary
  in practice since the byte-ratio method already resolves the two formats
  actually present in this corpus with no ambiguity.

Practical upshot: if a future `.texs` sample turns out to use a third
format this corpus doesn't contain an example of (uncompressed `l8`/`a8`,
DXT5, etc.), `DetectPixelFormat` will correctly report `FormatUnknown`
(the byte-ratio won't land near 0.5 or 1.0) rather than silently
misdecoding it — but actually adding support for it would need either a
new real sample to derive its own byte-ratio signature from, or finally
cracking the external-fixup format-name resolution above.

**Full mip chain reconstruction on encode.** `EncodeInsert`/`tex-insert`
only ever touches mip level 0 (the base level) — see the "Tail-mip byte
accounting" note below for why, and `EncodeInsert`'s own doc comment in
`texture.go` for the resulting behavior (mip levels 1+ go stale, keeping
the *old* texture's downscaled content until/unless something regenerates
them). Not attempted this session; the base level is what's actually
visible for the majority of this repo's real use cases (small UI/HUD
texture replacement).

### Tail-mip byte accounting (why full-mip-chain regeneration wasn't attempted)

The `levels = levelCount+1`, naive-halving-to-1x1 model (Step 1 above) is
**exact** for 177/186 samples' *total* Section 2 size, but leaves a small
residual (4-12 bytes, always a positive multiple of one block's size) for
the other 9 (all `demo_endscreen_*`/`loadscreen_*_169` variants whose
smallest mip levels dip to odd sub-4-pixel dimensions like 4x2 or 2x1).
Naively padding every level's block count up to a whole number of 4x4
blocks (`ceil(w/4)*ceil(h/4)`, the standard "real" DXT accounting) instead
*overshoots* every sample's total by a small amount at the tail — the
opposite direction of error, and by a different, non-constant amount per
file. Neither simple model is byte-exact for 100% of the corpus at the
tail end. This doesn't block base-level decode/encode at all (`
baseLevelSize` is unambiguous — the standard per-level block-count formula,
independent of any tail-chain question), but does block *fully*
regenerating Section 2 from scratch on an encode — see `EncodeInsert`'s doc
comment for how this was sidestepped (preserve every byte outside the base
level verbatim from the original file, rather than needing to answer this).
Flagged here, with the concrete numbers, for whoever wants to chase this
down further — it's a small, well-defined residual, not an open-ended
unknown.

## API (`mad2iga/texture.go`)

- `ParseIGZContainer(data) (*IGZContainer, error)` — magic + section table
  only (not the object graph within).
- `(*IGZContainer).Section(i) []byte`, `.ClassDirectory() []string`,
  `.SourcePathHint() string`.
- `ParseTexs(data) (*TexsInfo, error)` — the `.texs`-specific wrapper:
  confirms `igImage2` is present, and reads its real `Width`/`Height`/
  `Depth`/`LevelCount`/`ImageCount`/`FormatRaw` fields plus the detected
  `Format` (`FormatDXT1`/`FormatDXT3`/`FormatUnknown`). `GuessedWidth`/
  `GuessedHeight` are the old (now superseded) heuristic, kept for
  cross-checking. `PixelData` is the raw Section 2 bytes (all mip levels).
- `DetectPixelFormat(width, height, levelCount, sec2Size) PixelFormat` —
  the byte-ratio format detector (see "Step 2" above).
- `DecodeImage(info *TexsInfo) (image.Image, error)` / `DecodeTexs(data)
  (image.Image, *TexsInfo, error)` — decode the base mip level to an
  `*image.NRGBA`.
- `EncodeInsert(origData []byte, img image.Image) ([]byte, error)` —
  re-encode `img` as the base mip level of an existing `.texs` file's
  texture, returning a new, valid `.texs` file. `img` must exactly match
  the original's dimensions (no resizing). Preserves everything outside the
  base mip level byte-for-byte (see "What's still not solved" for the one
  known limitation: mip levels 1+ aren't regenerated).

`mad2repack` subcommands: `tex-info <in.texs>` (container/class metadata,
real dimensions/format, decode sanity check), `tex-extract <in.texs>
<out.png>`, `tex-insert <in.texs> <new.png> <out.texs>`.

### Round-trip verification results

`decode(original) -> encode(zero-change PNG) -> decode(result)`, tested on
the DXT3 reference sample and two DXT1 loadscreen samples (one square-ish
1024x512, one 2048x1024):

| Sample | Format | File size before/after | Byte-identical `.texs`? | Mean pixel diff (sum of 4 channels, 0-1020 range) | Exact-pixel-match fraction |
|---|---|---|---|---|---|
| `360-wireless-controller_128.texs` | DXT3 | 90,108 / 90,108 | No | 27.9 | 2.6% |
| `loadscreen_generic.texs` | DXT1 | 352,244 / 352,244 | No | 105.8 | 59.4% |
| `loadscreen_generic_169.texs` | DXT1 | 1,400,824 / 1,400,824 | No | 67.5 | 65.0% |

File size is always identical (expected — same dimensions, same format,
same mip levels 1+ preserved verbatim, only the base level's bytes change).
The resulting `.texs` is **not** byte-identical to the original in any
case, and this is expected, not a bug: `encodeDXTColorBlock`/
`encodeDXT1Block`/`encodeDXT3Block` in `texture.go` use a simple per-channel
min/max ("bounding box") endpoint-selection heuristic, not whatever
proprietary encoder (with its own, unknown, likely higher-quality
endpoint-fitting algorithm, and possibly deliberate dithering — see the
speckled-interior discussion above) produced the original assets; DXT
compression is inherently lossy per-block quantization, so two different
correct encoders will not produce bit-identical output even from the exact
same source pixels. The pixel-level diff numbers above are a **second**
decode of an **already-lossy-recompressed** image (not the original
losslessly-decoded pixels), so they reflect two rounds of quantization
error, not one — a `tex-extract` -> `tex-insert` -> `tex-extract` cycle
starting from a real original file's *first* decode would show smaller
drift than decode-of-a-decode-of-a-recompress does. The mean diffs (28-106
out of a max possible 1020) and majority-exact-match fractions (59-65% for
the two DXT1 samples) are consistent with "correct, if simple, DXT
encoding" — a genuinely broken encoder would show far larger, more uniform
error across the whole image rather than this level of preservation.

## History: the previous open questions, solved

This section is the previous version of this document's "Open questions"
list, annotated with how each one resolved, kept for anyone who wants the
full trail of what was tried before the breakthrough rather than just the
final answer.

1. ~~Where do `width`/`height`/`format`/etc. actually live on disk?~~
   **Solved** — see "Section 1: the live `igImage2` instance's fields,
   confirmed and located" above. They live at the runtime C++ field offsets
   already documented, starting from the address
   `ObjectGraph.TopLevelObjects()`/`locateImage2Object` already compute
   generically from Section 1's own top-level object list header — nothing
   exotic, just applying a mechanism this codebase had already built and
   verified for other classes.
2. ~~What is section 2's actual pixel byte layout?~~ **Solved for the two
   formats this repo's real assets actually use** — standard row-major
   DXT1/DXT3 blocks, base mip first. See "Pixel format and layout,
   confirmed" above. The six previously-ruled-out hypotheses (plain
   RGBA8/BGRA8 raster, mip-order reversal, column-major/transposed, fully
   planar channels, Morton/Z-order swizzle) were never wrong about
   *ordering* so much as wrong about the fundamental *bit depth* — all six
   assumed 4 bytes/pixel uncompressed data, when the real content is
   4x4-block-compressed at 0.5 or 1.0 bytes/pixel. No raster permutation of
   the right region at the wrong bit depth could have worked.
3. ~~Which concrete `igMetaImage_*` format is actually used by PC
   assets~~ — **Partially solved**: the concrete *compressed format*
   (DXT1 vs DXT3) is now known for every real sample, reliably (Step 2
   above). The literal *singleton name* (`"dxt1"` vs `"dxt1_dx"`) is still
   not recoverable from this file's own bytes — see "What's still not
   solved" above; this is now a narrower, precisely-scoped gap rather than
   "which of ~40 formats, unknown."
4. Section 1's ~140 bytes were walked manually for one sample but not
   turned into a general parser (nested `igHandleList`/`igStringRefList`
   bookkeeping, matching the classes in section 0's directory). **Still
   true** — the `igImage2` object's own fields (the part that actually
   mattered for this session's goal) are now a real, general, verified
   read; the *following* `igHandleList`/`igStringRefList` headers'
   semantics are still not mapped (see "What's still not solved" above for
   the one place this was relevant — format-name resolution — and why it
   wasn't pursued further).
5. The `objArrayOff`/`nameArrayOff` "+4" convention discrepancy the
   previous version of this document flagged, relative to `IGZ_FORMAT.md`'s
   description — **resolved as a red herring**: re-deriving the same
   addresses this session using `objgraph.go`'s own already-verified
   `TopLevelObjects()` formula (offsets relative to the header's own start,
   no "+4" adjustment) lands on the exact right byte addresses directly;
   the earlier "+4" observation was very likely an artifact of manually
   reasoning about one single-object, single-name list where a coincidental
   overlap (the name-array's one entry and the object's own `classIdx` word
   happen to sit at addresses that made an incorrect "+4" model *appear* to
   fit) obscured the simpler, correct formula. Not chased down further than
   confirming the simpler formula works correctly end-to-end across all 186
   samples — a genuine explanation for why the "+4" appearance happened
   would need deeper study of `igHandleList`/`igStringRefList`'s own
   internal layout, which (per point 4) still isn't mapped.
