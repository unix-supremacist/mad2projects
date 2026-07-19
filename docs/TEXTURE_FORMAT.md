# Texture format (`.texs` files)

Source of truth: `mad2iga/texture.go`. Partially reverse-engineered: the
outer container and the runtime C++ field schema for the texture class
(`igImage2`) are solid, confirmed against independent evidence in two
different ways. The exact on-disk byte layout of the actual pixel data is
**not** solved despite substantial effort — see "Open questions" for the
full trail so this doesn't need to be redone from scratch. Consequently
there is currently no pixel-to-PNG/DDS decoder; `mad2iga` extracts
container-level metadata and the raw pixel-data blob only.

## The container is IGZ, same shape as `level.bld`/`.pak`

Confirmed via hex dump: `.texs` files start with the same
`0x49475A01` magic and 16-byte section-descriptor-table shape documented in
`IGZ_FORMAT.md`. `mad2iga/texture.go`'s `ParseIGZContainer` re-implements
just the section-table walk (not imported from `pak.go` — this file
intentionally stays self-contained, see its header comment) using the same
"keep reading descriptors until the next one's offset doesn't match
`previous.offset+previous.size`" technique `IGZ_FORMAT.md` documents for
`level.bld`.

Every one of the 93 `.texs` samples has exactly **3 sections**:

| # | Contents (confirmed) | Typical size |
|---|---|---|
| 0 | Class directory + string pool | ~530-540 bytes |
| 1 | Object-graph bookkeeping (igObjectList/igHandleList/igStringRefList housekeeping — see below) | ~140 bytes, constant across every sample checked |
| 2 | Raw pixel-data blob (undecoded — see "Open questions") | the bulk of the file |

Section 2's end is always exactly EOF (`section2.offset + section2.size ==
file size` in every sample) — no trailer after it, unlike `level.bld`'s
end-of-file name table.

### Section 0: confirms the class and the original source path

Section 0's class directory (same "null-terminated ASCII names, `dirTableOff`
u32 at `sec0Base+0x0C`" scheme as `IGZ_FORMAT.md` documents for `level.bld`)
contains exactly four classes in every sample checked, in this order:

```
igObjectList  igImage2  igHandleList  igStringRefList
```

i.e. **the texture class is `igImage2`**, index 1 in a `.texs` file's own
per-file class directory (this is a fresh, tiny per-file directory — not to
be confused with `level.bld`'s ~150-200-class directory).

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

### Section 1: generic object-graph bookkeeping, not yet fully mapped

Section 1 is a constant ~140 bytes in every sample and starts with the
standard 32-byte top-level `igObjectList` header `IGZ_FORMAT.md` documents
(`classIdx=0, objectCount=1, nameCount=1, fixupPtr=0x80000004, ...`).
Manually walked byte-by-byte for the 128×128 sample above: the remaining
~108 bytes decode as what looks like two more nested list-style headers
(matching the `igHandleList`/`igStringRefList` classes also present in
section 0's directory) — repeating `{count=1, count=1,
fixupPtr=0x80000004, offset}`-shaped 16-32-byte groups. This was **not**
fully mapped field-by-field; see "Open questions".

One offset-convention wrinkle noted in passing: `IGZ_FORMAT.md` describes
`objArrayOff`/`nameArrayOff` (the top-level `igObjectList` header's +0x18/
+0x1C fields) as offsets "from +0x00" of the header. Empirically, for this
file, the object/name arrays actually start 4 bytes *after* where that
description would put them — consistent with the offsets instead being
relative to +0x04 (i.e. measured from after the `classIdx` field, not from
the very start of the header). Not chased down further; flagging since it
might matter for whoever writes a general Section-1 object-graph walker
(per `IGZ_FORMAT.md`'s own "still open" list).

## The `igImage2` class: confirmed field schema

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
| 0x12   | 2    | levelCount          | u16 — mipmap level count |
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
other class this repo has mapped so far.)

**Independently cross-checked** against a second piece of evidence: the
`configure__Q33Gap3Gfx8igImage2FUsUsUsUsUsPQ33Gap3Gfx11igMetaImage`
function (decompiled directly) writes its five `unsigned short` parameters
to exactly `object+0xC`, `+0xE`, `+0x10`, `+0x12`, `+0x14` — i.e. the
runtime `configure()` call's own field writes land at exactly the offsets
the bulk-registration table says width/height/depth/levelCount/imageCount
live at. Two independent decompiled call sites agreeing is the same
cross-check standard `CLASS_SCHEMA.md` uses elsewhere (e.g.
`GeneratorGenerationFields`, PC vs. Wii).

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

Since MAD2's PC build is DirectX 9, the actual format for any given PC
`.texs` asset is almost certainly one of the "_dx"-suffixed (or plain
uncompressed) variants — but **which one was not confirmed** for any real
sample, because the live instance's `format` pointer value couldn't be
located in the on-disk bytes at all (see next section). This list is still
useful ground truth: it's the *complete closed set* of values the field can
take, for whoever picks this up next.

## What's confirmed vs. unresolved about actual pixel layout

### Confirmed: base-mip dimensions, via an exact arithmetic match

Even without being able to read the `width`/`height` fields directly (see
below), the base-level texture dimensions can be recovered indirectly for
the common case. Section 2's total byte size matches **exactly** — to the
byte, not approximately — the formula for a square power-of-two base image
with a **full mipmap chain down to 1×1**, at **4 bytes/pixel**:

```
total = 4 * Σ (N >> i)²  for i = 0 .. log2(N)     (N = base width = height)
```

Verified against three independently-sized real samples (chosen because
their filenames hint at their size, letting the match be checked rather
than assumed):

| File | Section 2 size | N solving the formula | Filename hint |
|---|---|---|---|
| `360-wireless-controller_128.texs` | 87,380 | **128** | `_128` suffix |
| `loadscreen_generic.texs` | 349,524 | **256** | (4:3 UI backdrop) |
| `loadscreen_generic_169.texs` | 1,398,100 | **512** | (16:9 variant, higher-res) |

All three are *exact* matches (not "close"); a 4×4-floor mip chain (instead
of 1×1) or any inter-level padding both break the match by a small but
non-zero amount, so this isn't a coincidental round-number fit. Both
loadscreen images being stored as *square* POT textures despite being used
as wide/narrow UI backdrops is consistent with an old-console-targeting
asset pipeline padding non-square art out to the nearest POT square and
letting the renderer crop UVs at draw time — plausible given this engine
also shipped on PS2/PSP/GameCube/Wii/Xbox (all with POT texture
requirements), though not independently confirmed.

`mad2iga.guessSquarePOTDimensions` implements this as a heuristic (search
N = 2..8192 for an exact match) and is exposed via `TexsInfo.GuessedWidth/
GuessedHeight`. Batch-tested against all 93 real `.texs` samples: **84/93
(90%) get an exact match**; the remaining 9 (all `loadscreen_*` variants —
non-square widescreen crops, evidently not padded to a POT square the same
way) correctly report "no match" (`GuessedWidth/Height == 0`) rather than a
wrong guess, since the search only accepts exact byte-count equality.

### Unresolved: where the live `igImage2` field values live on disk

This is the more important open problem. A direct byte search of the
entire 128×128 sample file for the expected on-disk pattern of
`width=128, height=128` as adjacent little-endian `u16`s (`80 00 80 00`)
found **zero matches anywhere in the file** — not in section 1, not at the
start of section 2, nowhere. This means the generic "object starts with
`classIdx`, then has fields at the fixed runtime offsets" model
`IGZ_FORMAT.md` describes for the general case does not directly explain
how *this* object's field values are actually stored at rest — or section
1 encodes them in some indirect/compact form not yet identified, or
they're not per-instance-serialized at all in the way expected. Genuinely
unresolved; flagged rather than guessed at further.

### Unresolved: pixel byte layout inside section 2

Despite section 2's *size* being pinned down exactly (see above), every
attempt to interpret its *bytes* as a coherent image failed. Tried, on the
128×128 sample, using the size-implied base-mip region (first or last
65,536 bytes depending on mip-ordering assumption):

- Plain raster RGBA8 and BGRA8 (channel-order swap only) — both produce the
  same incoherent vertical-striped noise pattern (channel order clearly
  isn't the (only) problem).
- Largest-mip-first vs. smallest-mip-first ordering — both equally
  incoherent.
- Column-major / transposed raster — incoherent (produces the same noise
  rotated 90°, not a fix).
- Fully planar channel layout (all R bytes, then all G, then B, then A,
  for the whole base-mip region) — incoherent.
- Morton/Z-order (bit-interleaved x/y) swizzle over the whole 128×128 base
  level — incoherent.

A byte-level autocorrelation scan (checking mean absolute difference between
`byte[i]` and `byte[i+period]` across the base-mip region, for periods 4..2048
in steps of 4) found a **dominant period of 1,024 bytes** — much stronger
than the "expected" 512-byte period a naive 128-pixel/4-bytes-per-pixel
raster row would predict (512 wasn't even a local minimum; nearby periods
384/512/640 all scored about the same as noise). This is a real, structured
signal — not noise — but its cause wasn't identified: candidates considered
but not conclusively tested include console-style block/tile swizzling
(common on Xbox 360/PS3 but not expected for a PC/DirectX-9 asset), a
2-row interleave/grouping scheme, or the true base width actually being 256
rather than 128 (which would fit the 1024-byte period at 4 Bpp, but
contradicts the exact mip-chain-size match at N=128 — the two pieces of
evidence disagree and this wasn't reconciled).

Given this, **no pixel decoder is shipped**. `TexsInfo.PixelData` exposes
the raw section-2 bytes as-is for whoever picks this up next, already
narrowed down to: confirmed dimensions (for 90% of samples), a confirmed
closed set of ~40 possible pixel formats, and a concrete, reproducible
"1024-byte periodicity" lead to chase.

## API (`mad2iga/texture.go`)

- `ParseIGZContainer(data) (*IGZContainer, error)` — magic + section table
  only (not the object graph within).
- `(*IGZContainer).Section(i) []byte`, `.ClassDirectory() []string`,
  `.SourcePathHint() string`.
- `ParseTexs(data) (*TexsInfo, error)` — the `.texs`-specific wrapper:
  confirms `igImage2` is present, exposes `GuessedWidth`/`GuessedHeight`
  (0 if not confidently determined) and the raw `PixelData` blob.

No CLI command is wired up yet for `.texs` beyond what would trivially dump
`TexsInfo` to JSON plus the raw blob to a `.bin` file — not added, since
shipping a "texture extractor" that can't actually produce a correct image
would be worse than nothing; see "Open questions" for what's needed first.

## Open questions (recap, for whoever continues this)

1. **Where do `width`/`height`/`format`/etc. actually live on disk?** Direct
   byte search found nothing; needs either tracing the PC `igCore.dll`'s
   actual file-deserialization code path for `igImage2` (not just its
   in-memory field layout), or a real Section-1 object-graph walker per
   `IGZ_FORMAT.md`'s own "still open" item.
2. **What is section 2's actual pixel byte layout?** Six layout hypotheses
   ruled out (see above); the 1024-byte autocorrelation periodicity is the
   most concrete lead.
3. **Which concrete `igMetaImage_*` format** is actually used by PC assets
   — never confirmed, though the full closed set of ~40 candidates is now
   known.
4. Section 1's ~140 bytes were walked manually for one sample but not
   turned into a general parser (nested igHandleList/igStringRefList
   bookkeeping, matching the classes in section 0's directory).
5. The `objArrayOff`/`nameArrayOff` "+4" convention discrepancy noted above,
   relative to `IGZ_FORMAT.md`'s description.
