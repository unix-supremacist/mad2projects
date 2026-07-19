# Madagascar 2 asset format documentation

This documents the on-disk formats used by Madagascar: Escape 2 Africa (PC),
built on the Vicarious Visions "Alchemy" engine, and the tooling in this repo
that reads/writes them (`mad2iga`, `mad2arc`, `mad2repack`, `mad2tool`).

## The formats

- **IGA** — the outer archive container (`.arc`, `.bld` files under
  `Content/Streams/win/`). Fully reverse-engineered; round-trips losslessly
  (`mad2repack verify`/`verify-all` confirm byte-identical repacks). See
  [`IGA_FORMAT.md`](IGA_FORMAT.md).
- **IGZ** — the format of the files an IGA archive contains (`.pak` files,
  `level.bld`'s payload, `.texs` textures once unwrapped from their own
  IGA-like outer shell). This is Alchemy's serialized-object-graph format: a
  frozen memory image of `igObject` instances. The container header/section
  table is solid; the object graph inside is mapped for several classes. See
  [`IGZ_FORMAT.md`](IGZ_FORMAT.md).
- **Class field schema** — the IGZ object graph's actual per-class field
  layout (which byte offsets mean what). This is where "can we edit
  arbitrary fields" ultimately lives or dies. See
  [`CLASS_SCHEMA.md`](CLASS_SCHEMA.md).
- **TFBScript (level logic/behavior)** — the `ScriptSet`/`ScriptObject`/
  `Op*` classes encoding cutscene triggers, AI behavior, "what happens
  when." The largest, highest-value, and currently least-covered part of
  the class schema — semantic structure mapped from two external reference
  projects, not yet independently verified against Mad2's own PC binaries.
  See [`SCRIPT_FORMAT.md`](SCRIPT_FORMAT.md).
- **`.snds` (audio)** — FSB3 (FMOD Sound Bank v3, a well-documented public
  format) plus a minority of plain Ogg Vorbis files sharing the same
  extension. Fully solved and verified against the entire 23,378-file
  corpus. See [`AUDIO_FORMAT.md`](AUDIO_FORMAT.md).
- **`.texs` (textures)** — IGZ-wrapped `igImage2` objects. Container and
  class field layout solved; the actual compressed pixel byte layout in
  Section 2 is still unresolved. See [`TEXTURE_FORMAT.md`](TEXTURE_FORMAT.md).
- **`.hkt` (Havok animation)** — confirmed to be genuine, unmodified Havok
  SDK skeletal/camera animation data (no physics), but not yet parseable —
  no standard packfile header found. See [`HAVOK_FORMAT.md`](HAVOK_FORMAT.md).

## Sibling reference material

- `../../mad2assetextractor/docs/` — earlier research notes (`research_notes.md`,
  `PAK_FORMAT.md`, `demo.md`) from before this tooling was migrated in-repo.
  Treat with a grain of salt — some entries there have since been confirmed
  wrong by cross-checking against real debug symbols (see `CLASS_SCHEMA.md`).
- `../../mad2assetextractor/wii_demo_ref/` — an unstripped PowerPC ELF
  (`Mad2.elf`) pulled from a Wii "Demo Action Pack" disc, plus its full symbol
  table and a script that decodes real field offsets straight out of it. The
  best source of ground truth for the object schema, but **not sufficient on
  its own** — its offsets are only confirmed correct on the Wii build; every
  class touched so far needed independent cross-checking against the PC
  binaries before its offsets could be trusted for editing real PC game
  files (see `CLASS_SCHEMA.md`'s "Major caveat").
- `https://github.com/bonesinmysoup/igRewrite8/tree/trapteam-but-real` — a
  modding tool for a sibling "ig" engine title (Skylanders), useful for
  cross-referencing the general shape of the engine's reflection/serialization
  system, but **not** byte-compatible with MAD2's IGZ v4 variant (see
  `IGZ_FORMAT.md`'s note on format variants).
- `igCore.dll` (in `mad2/`) — the PC build's core engine DLL. Unlike the
  retail `Mad2.exe`, it ships fully-demangled C++ export names
  (`Gap::Core::igMetaObject`, `igMetaField`, etc.). Auto-analysis does not
  create `Function` objects at every export address in Ghidra — use
  `create_function` at the address of interest first, then
  `decompile_function` picks up the already-demangled name automatically.
  This same quirk applies to every DLL/ELF touched so far, not just
  `igCore.dll`.
- The per-class `*InfoLib.dll` files (`ActorInfoLib.dll`, `ParticleInfoLib.dll`,
  etc., all in `mad2/`) — the PC-side source of truth for a given class's
  real field layout. This is where you go to independently confirm (or
  refute) a Wii-derived offset before trusting it.

## The tools

- **`mad2iga`** — shared Go library. Everything else here imports it via a
  `replace` directive. Key packages:
  - `iga.go`/`repack.go` — IGA archive container (`Open`/`ExtractFile`/`Repack`).
  - `pak.go` — `.pak` (IGZ) string dumping/compiling (`DumpPak`/`CompilePak`).
  - `level.go` — `level.bld` coordinate heuristic scanner
    (`DumpLevelCoords`/`CompileLevelCoords`) — byte-pattern-based, not a real
    graph walk; still useful, still limited (see `CLASS_SCHEMA.md`).
  - `objgraph.go` — the real Section-1 object-graph walker
    (`ParseObjectGraph`, `ObjectGraph.FullWalk`, `ResolveSegPointer`,
    `FieldValue`) — structurally confirmed objects, not heuristic guesses.
  - `schema.go` — the class field schema itself: Wii-derived data
    (`wii_field_schema.json`, embedded via `go:embed`) merged with
    hand-curated PC-verified overrides/removals (`pcVerifiedFields`/
    `pcDisprovenFields`).
  - `fsb.go` — FSB3/Ogg audio parsing and WAV extraction.
  - `texture.go` — `.texs`/`igImage2` container and metadata parsing (no
    pixel decoder yet).
- **`mad2arc`** — read-only CLI: list/extract IGA archives.
- **`mad2repack`** — the main CLI: `unpack`/`pack`/`verify`/`replace`/
  `pak-dump`/`pak-compile`/`level-dump`/`level-compile`/`objects-dump`/
  `verify-binary`/`snd-extract`/`snd-extract-all`/`tex-info`.
- **`mad2tool`** — orchestrates `mad2repack` over the whole game's
  `Content/Streams/win/` directory (`extract`/`rebuild`), including dumping
  every nested `.pak` and `level.bld` to editable JSON and back.

`waterhole_objects_example.json` (repo root) is a real `objects-dump` output
against `WaterholeBLD/level.bld.orig`, worth reading directly to see current
capability: every `ActorInfo` in the level, its real world position, and its
resolved links to `CollisionInfo`/`ActorWaypointList`/`ActorParameters`
objects — all structurally verified, not guessed.

## Current state, honestly

- **IGA container**: solid, verified round-trip.
- **IGZ container** (header/sections): solid for the header/section-table
  shape and the real Section-1 object graph (top-level array,
  `igGroup.childList` traversal, segmented-pointer resolution — see
  `objgraph.go`). Section 0's string pool boundary is **not** solidly
  understood (see `CLASS_SCHEMA.md`'s naming investigation) — this blocks
  reliable instance *naming*, not object *discovery* or *field access*,
  which both now work via real graph traversal.
- **Class field schema**: growing, verified-where-marked. `ActorInfo` is the
  first class with several fields confirmed against the real PC binary
  (not just the Wii ELF) — see `CLASS_SCHEMA.md`.
- **Instance naming**: unsolved. Two independent mechanisms exist in
  principle (a per-object embedded `igStringRef` field, and a file-trailing
  Name Table keyed by top-level array position) but neither has been made
  to reliably resolve real names yet — see `CLASS_SCHEMA.md`'s "Naming
  investigation" section for what's been tried and ruled out.
- **Audio**: solved, verified against the whole corpus.
- **Textures**: container/schema solved, pixel data unresolved.
- **Havok animation**: identified, not yet parseable.
- `mad2iga/level.go`'s coordinate scanner is still a heuristic byte-pattern
  scanner for finding coordinates, kept alongside (not replaced by)
  `objgraph.go`'s real walker — each has different strengths today (see
  `CLASS_SCHEMA.md`). Naming remains the biggest structural gap left in
  "can we edit anything."
