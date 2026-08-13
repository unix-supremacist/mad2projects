# Cross-install level content diffing (mad2 / mad2russia / mad2demo)

Methodology and first result for a broader question: given the three game
installs in this repo (`mad2/` — US/international retail, `mad2russia/` —
Russian localized retail, `mad2demo/` — the standalone playable demo), what
*actually* differs between the same level's data across them, once you
strip out localization noise and incidental build artifacts? This documents
the approach and the first level run through it end-to-end, **Waterhole**,
as a methodology check before scaling to the rest of the level list (see
`mad2launcher/alchemy.go`'s `LevelNames` for the full roster).

## Which files exist where

Only two of the three installs ship a given level as a rule — `mad2demo`
only has the levels included in the standalone demo. For Waterhole
specifically:

| Archive | mad2 | mad2russia | mad2demo |
|---|---|---|---|
| `Waterhole.bld`/`.arc` (full level) | yes | yes | no |
| `Waterhole_Demo.bld`/`.arc` (demo cut) | yes (leftover) | no | yes |

So there are two independent three-way comparisons available here: the
full level (`mad2` vs `mad2russia` only) and the demo cut (`mad2` vs
`mad2demo` only — `mad2` apparently still ships the demo-cut archives
alongside the full level, unused by the retail game but present on disc).

## Tooling used, and which parts are trustworthy for this

All read-only, via `mad2repack` (`unpack`, `objects-dump`, `script-dump`)
and plain file hashing — nothing here writes to any install:

- **`mad2repack unpack`** — full IGA archive extraction. Good for a
  structural, per-file inventory (what's in the archive, how big, same
  filenames on both sides).
- **`mad2repack objects-dump`** — the real Section-1 object-graph walker
  (`mad2iga/objgraph.go`). Gives a per-class object count (`class_histogram`)
  and, for schema-known classes, resolved fields including real `position`/
  `worldMatrix` vectors and resolved pointer targets (`{raw, resolved_addr,
  target_class, resolved}`). This is the reliable one — every object listed
  is structurally confirmed reachable, not a byte-pattern guess.
- **`mad2repack script-dump`** — full compiled `ScriptInfo`/`ScriptSet`
  trees with real opcode class names and resolved string values (variable
  names via `OpCreateVariable.varName`, etc.). Also structurally real, not
  heuristic.
- **`mad2repack pak-dump`** — per-language `.pak` string tables. The actual
  subtitle/UI text (`strings`) is reliable. Its `audio_strings`/
  `non_language_strings` output, however, comes from a **raw byte-pattern
  scan** of the string pool (documented as such in `mad2iga/pak.go`) — it is
  sensitive to unrelated shifts in the pool's byte layout and produced
  clearly-truncated garbage entries in this run (see below). Don't trust it
  for a byte-exact diff; do trust the well-formed `strings` list.
- **`mad2iga/level.go`'s coordinate scanner** (`level-dump`) — explicitly
  documented in-repo as heuristic, byte-pattern-based, not a real graph
  walk. Confirmed here to throw off false-diffs from mere layout shifts
  (see below) — **don't use it alone as evidence of a real per-level
  difference**; only `objects-dump`/`script-dump` results should be trusted
  for that.

The general trap this documents: two builds of "the same" level almost
never hash identically, because whichever localization pak(s) got baked
into that particular disc build shifts every subsequent object's absolute
byte offset/pointer value in the file, even when zero actual level content
changed. Anything that dumps raw pointers, memory-pool indices, ref-counts,
or a naive byte-pattern scan will show a wall of "differences" that are
pure build noise. The only way to tell a real content difference from that
noise is to diff **resolved, schema-typed** values (positions, resolved
target classes, resolved string names) rather than raw fields.

## Waterhole: full level (`mad2` vs `mad2russia`)

Unpacked both `Waterhole.bld`+`.arc`:

- `Waterhole.bld`: mad2 has 8 files (`level.bld` + 7 language `.pak`s —
  DUTCH/ENGLISH/FRENCH/GERMAN/ITALIAN/SPANISH/SWEDISH); mad2russia has 3
  (`level.bld` + ENGLISH.pak + RUSSIAN.pak). Purely the expected
  localization-SKU difference.
- `Waterhole.arc`: mad2 has 2037 files (333 non-localized base files + 6
  language subdirs × 284 localized-audio files each); mad2russia has 617
  (the same 333 base files + 1 `russian/` subdir × 284 files). **All 333
  non-localized base files are byte-identical (SHA256) between the two** —
  zero diffs. The `dutch/`/`russian/` (etc.) subdirs contain the same 284
  relative filenames on both sides (only the audio content itself, i.e. the
  spoken language, differs — not diffed further, expected to differ by
  design).
- `level.bld` itself differs (SHA256 differs, decompressed size 129,771,872
  bytes for mad2 vs 129,692,552 for mad2russia — a ~79 KB difference) —
  this is the one file that needed the real object-graph tools, not just
  hashing, to know whether that's a real content edit or just layout noise.

**`objects-dump` result**: both sides reach exactly 1802 objects across the
same 12 classes, with an **identical per-class histogram**: 529 `ActorInfo`,
365 `ScriptSet`, 438 `SoundInfo`, 135 `ScriptInfo`, 108 `StringInfo`, 90
`SpriteInfo`, 59 `ValueInfo`, 53 `ParticleInfo`, 15 `CameraInfo`, 7
`DirectionalLightInfo`, 2 `PlacementReference`, 1 `TFBWorldInfo`. Matching
every `ActorInfo` instance in file order and comparing its resolved
`position` (world XYZ): **0 of 529 differ**. Every raw pointer-shaped field
(`_soundList`, `_collision`, `_wayPoints`, `_clones`, `_script`, memory-pool
index/ref-count/resolve-state bookkeeping, etc.) *does* differ on nearly
every object — confirmed to be exactly the "byte-offset shifted by the
different language-pak layout" noise described above, not a real edit
(spot-checked several: e.g. `_memPoolIndex`/`_refCount`/`_readOnly` on a
sampled `ActorInfo` were identical between builds — 3/3/3 both sides — the
apparent "differences" are concentrated on *other* objects whose pool index
genuinely does shift with layout, exactly as expected, and never on
anything gameplay-visible like position).

**`script-dump` result**: dumped the full 135 `ScriptInfo` + 365 `ScriptSet`
trees from both. Extracted every `class` name and every resolved string
value (`varName`, sound/media names, etc.) in traversal order — 121,402
tokens on both sides — and diffed: **0 positional differences**. Every
opcode type, every variable name, every resolved literal string in
Waterhole's entire script/trigger/AI logic is identical between the two
builds. (A follow-up attempt to also diff small numeric fields, not just
strings/classes, ran into exactly the bookkeeping-noise problem described
above — nearly every one of the 500 script roots showed spurious diffs
once fields like `_memPoolIndex`/`_refCount`/`_resolveState` were included
unfiltered, which is a tooling/methodology dead end, not a real signal; the
class+string comparison above is the trustworthy version of this check.)

**`pak-dump` (ENGLISH.pak) result**: the 108 actual subtitle/UI `strings`
are byte-identical between mad2's and mad2russia's `ENGLISH.pak` — 0
diffs. The heuristic `audio_strings`/`non_language_strings` scan reported
~25-27 "different" entries each direction, but every one is a clearly
truncated substring of a filename present on the other side too (e.g.
`WH_HitByMangos2_GLORIA.wav` appears complete on one side and
`/Content/Levels/Waterhole/Sounds/WH_HitByMangos2_GLORIA.wav` — same file,
different truncation point) — a byte-scan artifact from the pool shifting
by a few bytes, not a real asset difference. No genuinely new/missing audio
cue or line found.

**Conclusion for Waterhole (full level): no real gameplay/content
difference found between `mad2` and `mad2russia`.** Object placement,
script/trigger logic, and subtitle text are identical; the only actual
differences are the expected ones (which languages are included) plus
incidental serialization noise (pointer/pool-index values, ~79 KB of
layout padding) that doesn't correspond to anything a player would ever
see differently.

## Waterhole_Demo cut (`mad2`'s leftover copy vs `mad2demo`'s real copy)

Same treatment, `Waterhole_Demo.bld`: both have all 7 non-Russian language
paks (this demo predates/is independent of the Russian SKU). `level.bld`
differs by hash (unsurprising — different build dates, Jul 2026 repack vs.
the original Sep 26 2008 demo disc). `objects-dump`: both reach exactly
1152 objects across the same 12-class histogram (321 `ActorInfo`, 228
`ScriptSet`, 237 `SoundInfo`, 84 `ScriptInfo`, 84 `StringInfo`, 80
`SpriteInfo`, 56 `ValueInfo`, 46 `ParticleInfo`, 6 `CameraInfo`, 7
`DirectionalLightInfo`, 2 `PlacementReference`, 1 `TFBWorldInfo`); 0/321
`ActorInfo` position diffs. `script-dump`: 180,220 class+string tokens on
both sides, **0 positional differences**.

**Conclusion: `mad2`'s bundled `Waterhole_Demo` archive and `mad2demo`'s
own copy are the same demo content**, just repacked at different times
(different raw bytes/pointers, identical actual design).

## Waterhole full level vs. Waterhole_Demo cut — real, confirmed differences

The two comparisons above (`mad2` vs `mad2russia`, and `mad2`'s vs.
`mad2demo`'s copy of the *same* demo archive) were both "same content,
different install" checks, and both came back clean. That's a different
question from what the demo cut itself actually changes relative to the
full level — i.e. diffing `Waterhole.bld` against `Waterhole_Demo.bld`
*within* an install. Prompted by known player-visible demo behavior (a
"demo version" watermark burned into cutscenes, and certain areas of the
demo forcibly cutting to `ConvoyChase` instead of continuing), this
comparison **does** show real, level-data-driven differences — confirmed
via `script-dump`'s resolved script-include (`varName`) paths, cross-checked
both ways (grepped for zero hits on the *other* side, not just presence on
one):

- **`DemoIconManager.ai`** — `Waterhole_Demo` includes
  `C:/TFB/Content/Levels/Demo_Levels/Includes/scripts/DemoIconManager.ai`
  (source path shared across every demo level, but compiled directly into
  each level's own `level.bld` — no separate `Demo_Levels.arc/.bld` exists
  on disk in `mad2demo/Content/Streams/win/`, so this is baked per-level,
  not loaded from a shared archive at runtime). Zero hits for
  `DemoIconManager`/`IconManager` anywhere in the full `Waterhole.bld`'s
  script dump. This is very likely the watermark mechanism — level data,
  not (purely) `global.bld` as originally guessed, though see the
  `global.bld` diff below for a related piece.
- **`Airplane_Repair_Gate_Keeper.ai`** — present **only** in
  `Waterhole_Demo` (one hit; zero hits for `Gate`/`Airplane` anywhere in the
  full level's script dump). Name strongly suggests a checkpoint/gate
  script that ends the playable demo slice once reached and kicks the
  player into the next piece of demo content — a strong candidate for the
  "forces you into ConvoyChase" behavior. **Not yet traced to its actual
  opcode logic/branch target** — the `varName` hit here is the script's own
  *include path*, found inside the level's global master-variable list
  (every include in a level gets flattened into that list), not the
  specific `ScriptInfo` instance that runs its logic; pinning down exactly
  what it branches to needs the same kind of opcode/operand tracing
  `docs/SCRIPT_FORMAT.md` already flags as the one general unsolved gap
  (resolving which variable/branch target a given opcode actually reads) —
  a live-runtime trace (`mad2rhythmlogger`/`mad2shoptrace`'s technique)
  would be the practical way to actually confirm the destination rather
  than guessing from the name.
- **`Transition_WGCAVES.ai`** and **`Load_Level.ai`** — present in the full
  level, **absent** from `Waterhole_Demo`. The demo cut simply never wires
  up the exit toward Water Caves (unsurprising — that's deeper story
  content not reachable in a demo slice).

## `global.bld`: mad2demo carries extra global data mad2's retail build doesn't

Checked the user's own hypothesis directly — `mad2demo/Content/Streams/win/global.bld`
vs. `mad2/Content/Streams/win/global.bld` (both formatted the same way as a
level: a `level.bld` object graph plus per-language `.pak`s).
`objects-dump`: mad2's retail global reaches 1544 objects across 13
classes; mad2demo's reaches **1554** — 10 more. Per-class histogram is
identical except:

| Class | mad2 (retail) | mad2demo |
|---|---|---|
| `StringInfo` | 492 | **499** (+7) |
| `ValueInfo` | 364 | **367** (+3) |

Every other class (`ActorInfo`, `ScriptInfo`, `ScriptSet`, `SpriteInfo`,
`SoundInfo`, `ModelInfo`, `TFBBodyInfo`, etc.) matches exactly. This is a
real, confirmed structural difference: **mad2demo's global data genuinely
has extra string and value objects the retail global doesn't carry** —
consistent with the user's own guess of "a demo flag somewhere in
global.bld," and a plausible root cause for the reported crash when a demo
level is loaded against the retail `global.bld` instead (the demo's level
scripts, e.g. the `DemoIconManager` include above, likely resolve a
variable/name that only exists in the demo's own global data — a dangling
reference against the retail one). `global.bld`'s own script dump has zero
literal `"Demo"` hits on either side, so these extra `StringInfo`/`ValueInfo`
objects aren't identifiable by a text grep the way script includes are —
`objects-dump`'s schema doesn't yet resolve `StringInfo._unicodeText` or a
`ValueInfo`'s value to real content, only structural presence/count. Fully
naming the 7 extra strings / 3 extra values (and pinning down which script
references them) is the natural follow-up, not done in this pass.

## Recommended path for the rest of the level list

The Waterhole run validates the approach works and is cheap enough to
repeat: per level, per install-pair that actually has it,

1. `mad2repack unpack` both archives; hash-compare every non-language-subdir
   file in the `.arc` (catches non-level asset edits — textures, models —
   for free, since those aren't in `level.bld` at all).
2. `mad2repack objects-dump` both `level.bld`s; compare `class_histogram`
   and, for any class with a `position`/`worldMatrix` field, diff those
   values object-for-object (file order, since counts match 1:1 whenever
   nothing changed).
3. `mad2repack script-dump` both; diff the class+resolved-string token
   stream (not raw fields — see the bookkeeping-noise trap above).
4. `mad2repack pak-dump` the shared-language `.pak` (usually `ENGLISH.pak`)
   from both sides; diff only `strings`, not `audio_strings`/
   `non_language_strings`.

A real difference would show up as: a changed `class_histogram` count (an
object added/removed), a changed `position`/`worldMatrix` on a matched
object, a changed opcode/variable-name token in the script stream, or a
changed subtitle string — anything else (pointers, pool indices, ref
counts, hashes/offsets that only exist to point somewhere else in the same
file) is exactly the noise this session identified and should be ignored.
Given the scale (dozens of levels × up to 3 install pairs each), this is
naturally a good candidate to script into a proper `mad2tool`/`mad2repack`
subcommand rather than repeating by hand in a shell — not built yet, since
this session's job was validating the method works at all before investing
in that.
