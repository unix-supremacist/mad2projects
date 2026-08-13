# Task: fix `.texs` DXT1/DXT3 pixel decode — still visibly broken

## Context

`mad2modloader` is a reverse-engineering + mod-loader project for Madagascar 2
(PC). `mad2iga/texture.go` decodes the game's `.texs` texture files (an IGZ
container wrapping an `igImage2` object + a raw DXT1/DXT3-compressed pixel
blob in Section 2). `docs/TEXTURE_FORMAT.md` documents the format and claims
the decode is "fully solved" with "direct visual confirmation" — **that claim
is wrong for at least one real sample, verified today by actually looking at
the rendered PNGs.** Your job is to find and fix the real remaining bug.

Read `docs/TEXTURE_FORMAT.md` in full first — it has a lot of real, correct
groundwork (container layout, field offsets, the byte-ratio format detector)
that you should trust; the bug is specifically in the **pixel block decode**
or **mip/dimension detection**, not the container parsing.

## What's confirmed broken

Built `mad2repack` (`cd mad2repack && go build -o /tmp/mad2repack .`) and ran
`tex-extract` on two real samples pulled live from the shipped game archives
(not stale test fixtures):

1. `360-wireless-controller_128.texs` (from
   `mad2/Content/Streams/win/DivingLocation_RitesOfPassage.arc`, entry
   `C:/TFB/Content/builddata/win/360-wireless-controller_128.texs`) — decodes
   to a 256x256 DXT3 image. The overall controller silhouette shape *is*
   recognizable (D-pad, analog stick circles, body outline all in roughly the
   right place), but the whole interior is covered in a harsh, regular
   checkerboard-like noise pattern that should not be there for what's
   presumably a clean UI icon asset.

2. `loadscreen_generic.texs` (same archive) — decodes to a 1024x512 DXT1
   image. This one is much more clearly broken: the outer border/frame
   decodes correctly (crisp rectangle, and some corner UI elements/text are
   legible), but essentially the **entire interior is unstructured rainbow
   static** — every pixel looks like independent random noise, no visible
   color coherence at any scale. Real DXT-compressed painted/photographic
   content, even "busy" art, retains local color coherence (nearby 4x4 blocks
   share similar hues/gradients) — pure per-pixel-random static like this is
   a strong signature of a genuine decode bug (wrong block stride, wrong
   base-mip offset/size, or a palette-index bit-ordering bug), not "the art
   is just dithered," which is what `TEXTURE_FORMAT.md`'s current text claims
   after apparently only spot-checking non-representative regions.

To reproduce: extract those two `.texs` files from the `.arc` (any archive
extraction path works — `mad2iga.Open()` + `Archive.ExtractFile(index, w)`,
or build `mad2tool` and use `unpack-all`), then
`mad2repack tex-extract <in.texs> <out.png>` and look at the PNG.

## Leads to check, roughly in order of promise

1. **Mip-chain ordering / base-mip location.** A prior investigation session
   (interrupted before finishing, never landed) found evidence that "the
   base (largest) mip is stored at the **end** of Section 2, not the start —
   the mip chain order is smallest-to-largest, not largest-to-smallest."
   The currently-committed code and docs assume the opposite (base mip
   first, per `TEXTURE_FORMAT.md`'s "Step 3" section and
   `decodeBaseLevel`/`baseLevelSize` in `texture.go`). That prior finding was
   either wrong, or right but never actually applied — worth re-deriving
   from scratch against real files rather than trusting either the old
   finding or the current docs blindly. A wrong base-mip offset would read
   the wrong bytes as "base level pixels" — reading past the intended small
   mip into unrelated trailing data would produce exactly this kind of
   "some structure, then garbage" pattern.
2. **Width/height detection.** `DetectPixelFormat`/dimension logic in
   `texture.go` uses a byte-ratio heuristic against `Section2.size` — double
   check the *real* width/height (not just the format) for these two
   specific samples against ground truth (e.g. does 1024x512 actually look
   right once decoded correctly, or is the real texture a different size and
   we're misreading `levelCount`/mip math, causing an off-by-something in
   how many bytes constitute "the base level"?).
3. **Palette index bit-ordering.** `decodeDXTColorBlock` in `texture.go`
   reads the 2-bit-per-pixel index nibble as `(idx >> (2*i)) & 3` for pixel
   `i` in row-major order — this is the standard S3TC convention and was
   reportedly already checked (see `TEXTURE_FORMAT.md`'s "tried all 4
   combinations of {row-major, column-major} x {LSB-first, MSB-first}"
   paragraph), but that check was only done on a small "interior region" of
   one sample and might have been checking the wrong thing entirely if the
   base-mip offset (lead #1) is what's actually wrong. Worth re-checking
   fresh once #1 and #2 are nailed down, not before.
4. Actually look at the raw bytes yourself for a *small*, simple, known
   asset if you can find or make one (e.g. a solid-color or simple 2-color
   test icon) to sanity check block decode in isolation from mip/offset
   guessing.

## What "done" looks like

- `mad2repack tex-extract` on `360-wireless-controller_128.texs` produces a
  clean controller icon with no checkerboard artifacting.
- Same for `loadscreen_generic.texs` — a coherent image, not rainbow static
  in the interior. (It's fine if the interior art itself looks "busy"/
  detailed once decoded correctly — just not literally random per-pixel
  noise.)
- Round-trip (`tex-extract` then `tex-insert` with the unmodified PNG, then
  `tex-extract` again) still works and produces a visually-close match, same
  as `TEXTURE_FORMAT.md`'s existing round-trip section describes.
- Update `docs/TEXTURE_FORMAT.md` to reflect what was actually wrong and
  fixed — don't leave the old, now-disproven "visually confirmed correct"
  claims in place uncorrected.
- Re-run whatever bulk/corpus checks `TEXTURE_FORMAT.md` already describes
  (the 186-file batch structural check, the autocorrelation tests) to make
  sure the fix doesn't regress files that might have been decoding
  correctly already, and note honestly if some files were fine and others
  weren't (i.e. don't assume the bug is universal just because these two
  samples show it).

## Logging your progress

**Write your progress to `texture-decode-fix-progress.md`** at the repo
root (`/home/unix/src/mad2modloader/texture-decode-fix-progress.md`) as you
go — what you've tried, what you ruled out and why (with concrete evidence,
not just "seems fine"), and your current best hypothesis if you get
interrupted or run out of budget before finishing. The user running this
task will hand that file to another agent (or come back to me) to continue
from where you left off if needed, so make it self-contained: don't assume
the reader has your conversation history, just the repo and your progress
notes.

When you're done (or if you get stuck), leave a clear final summary at the
top of that same progress file: what's fixed, what's verified, what (if
anything) is still open.
