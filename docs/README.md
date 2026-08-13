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
  arbitrary fields" ultimately lives or dies. **Now solved engine-wide**,
  not just per-class: `mad2metadumper` (a diagnostic mod, see
  `CLASS_SCHEMA.md`'s "Live reflection dump" section) reads the Alchemy
  engine's own live class registry directly out of the running game
  process, giving a complete, PC-verified, real-inheritance field schema
  for all 1392 registered classes (20,237 fields) in one capture —
  superseding the older per-class Wii-ELF/decompiled-`arkRegisterInitialize`
  approach documented further down in `CLASS_SCHEMA.md` (still worth
  reading for methodology and the caveats it surfaced along the way, e.g.
  the Wii-vs-PC offset mismatch). See [`CLASS_SCHEMA.md`](CLASS_SCHEMA.md).
- **TFBScript (level logic/behavior)** — the `ScriptSet`/`ScriptObject`/
  `Op*` classes encoding cutscene triggers, AI behavior, "what happens
  when." The largest namespace in the class directory; every class now has
  a complete own+inherited field schema (see above) — real graph traversal
  works and `mad2repack script-dump` can pull a full, class-tagged,
  1000+-opcode compiled script straight out of a real level, with real
  variable names (`OpCreateVariable.varName` resolves via a string-ordinal
  table) and typed literal values (`RHSValueStack`'s int/float type-tag
  scheme). Per-instance naming via the `igNamedObject`/Name Table mechanism
  is still unsolved, but content-matching on resolved variable names works
  as a practical substitute (found `Rave_TrackReader` and several
  `Rave_Track_<Song>` scripts this way, no instance name needed). Control
  flow is fully understood — a real, named global program counter and a
  genuine global "arg stack" (confirmed by decompiling the actual
  `execute()` methods of `OpFindVariable`/`OpSetValue`/`OpCheckValue`/
  `OpForEach` in `ScriptInfoLib.dll`, not inferred) resolve every
  `branchPC` forward-jump and every found/set variable reference. The one
  remaining gap is precise: resolving *which* variable a given opcode
  reads/writes needs either the not-yet-found compile-time local-slot-index
  mechanism, or simulating that arg-stack across the real call graph —
  live-tracing a running game process (see `mad2rhythmlogger`,
  `mad2shoptrace`) is the practical workaround used so far when this
  specifically blocks a real question, rather than building that
  simulation. See [`SCRIPT_FORMAT.md`](SCRIPT_FORMAT.md).
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
- **Cross-install level diffing** — using the tools above (read-only) to
  find *real* content differences between the same level's data across
  `mad2`/`mad2russia`/`mad2demo`, versus the localization/build noise that
  dominates a naive byte diff. Methodology plus a first validated result
  (Waterhole: no real gameplay difference found between `mad2` and
  `mad2russia`, or between `mad2`'s leftover demo cut and `mad2demo`'s own)
  in [`LEVEL_DIFFS.md`](LEVEL_DIFFS.md).

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
- `https://github.com/NefariousTechSupport/AlchemyMetadataDumper` — a
  runtime (in-process, injected) reflection-metadata dumper for four
  Skylanders titles (a later build of this same engine lineage). Not reused
  directly (different, later engine build — every offset `mad2metadumper`
  actually uses was independently found and PC-verified against Mad2's own
  `igCore.dll`), but its documented approach — walk the engine's live class
  registry instead of decompiling each class by hand — is exactly what
  `mad2metadumper` does for Mad2 itself. See `CLASS_SCHEMA.md`'s "Live
  reflection dump" section.
- `https://github.com/MaxStache/madagascar-tfbtool` — a parser/decompiler
  for the *original 2005 film Madagascar*'s `.ai` TFBScript bytecode.
  Confirmed direct lineage with Mad2's own `TFBScriptInfo` namespace
  (identical opcode class names). Useful as a semantic reference (its
  confirmed RHS operand tag/expression scheme independently corroborates
  `ValueRHSVariant.arithOperator`'s semantics already in `SCRIPT_FORMAT.md`)
  — but its packed per-instruction flow-control/descendant-span header is
  **not** how Mad2's own `OpCode.internalFlags` works (checked and ruled
  out this session, see `SCRIPT_FORMAT.md`'s Round 9) — this is a later,
  differently-encoded engine build, not the same bytecode format.
- `https://github.com/MaxStache/madagascar-2-tools` — a from-scratch,
  fully-AI-generated Python reader for **Mad2's own** `.bld`/IGZ format
  (`mad2_level.py`) plus a **static** reflection dumper (`dump_ark.py` →
  `ark_meta.json`, 1473 classes) that parses `arkRegisterInternal`/
  `arkRegisterInitialize` out of the DLLs with capstone. Two concrete things it
  contributed, both cross-checked here (see `CLASS_SCHEMA.md`'s "Independent
  static cross-check" and "Naming investigation" sections): (1) it **resolves
  real per-instance names** (`ActorInfo._name` → `Tourist01`, `TrenchWarRico`,
  …) via the authoritative `TSTR`(0x01)/`RSTR`(0x04) Section-0 fixup tables —
  the exact mechanism our own heuristic `StringPool` fudged around, and the key
  to the naming problem; (2) its static reflection dump supplies field **types**
  (which our live dump lacks), agreeing with our live offsets on 3092/3098
  fields (the 6 conflicts adjudicated in Ghidra, our live dump correct on all
  6). Its `mad2_level.py` `IGZ` class is a clean blueprint for parsing the fixup
  tables properly.
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
- `mad2/rtplugins/*.dll` (`ActorRTLib.dll`, `ViewportRTLib.dll`,
  `PlacementRTLib.dll`, `LightRTLib.dll`, `SpriteRTLib.dll`,
  `animalChessRTLib.dll`) — a previously-undocumented sibling set to the
  `*InfoLib.dll` files above: same per-namespace split, but these hold the
  actual runtime *behavior* implementation (message handlers, actor
  spawn/lifecycle code) rather than reflection/data schema. Found and first
  used in `CLASS_SCHEMA.md`'s "Character-switching feasibility
  investigation" section.

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
    (`wii_field_schema.json`), hand-curated PC-verified overrides/removals
    (`pcVerifiedFields`/`pcDisprovenFields`), the `TFBScriptInfo`-specific
    PC-verified schema (`tfbscript_pc_field_schema.json`), and — highest
    coverage, though not highest priority in the merge — the complete
    live-reflection-derived schema for all 1392 engine classes
    (`live_reflection_schema.json`, from `mad2metadumper`; see
    `CLASS_SCHEMA.md`'s "Live reflection dump" section). All embedded via
    `go:embed`.
  - `fsb.go` — FSB3/Ogg audio parsing and WAV extraction.
  - `texture.go` — `.texs`/`igImage2` container and metadata parsing (no
    pixel decoder yet).
- **`mad2arc`** — read-only CLI: list/extract IGA archives.
- **`mad2repack`** — the main CLI: `unpack`/`pack`/`verify`/`replace`/
  `pak-dump`/`pak-compile`/`level-dump`/`level-compile`/`objects-dump`/
  `verify-binary`/`snd-extract`/`snd-extract-all`/`tex-info`/`script-dump`/
  `script-pretty`. `script-dump <bld> <out.json>` recursively renders a
  level's TFBScriptInfo object graph as raw nested JSON; `script-pretty
  <bld> <out.txt>` (newer) renders the same graph as real pseudocode —
  reconstructed `IF`/`FOREACH`/`LOOP { ... }` blocks (from `OpBranch`'s
  confirmed `branchPC`), resolved variable names, and real `RelOp`/
  `ArithOp`/`FlowVals` symbols decoded straight from `ScriptInfoLib.dll` —
  see `docs/SCRIPT_FORMAT.md`'s Round 10 for exactly what's confirmed vs.
  still-honestly-unresolved (notably `RHSValueStack.type`, a per-file
  external reference, not a portable tag).
- **`mad2tool`** — orchestrates `mad2repack` over the whole game's
  `Content/Streams/win/` directory (`extract`/`rebuild`), including dumping
  every nested `.pak` and `level.bld` to editable JSON and back.
- **`mad2metadumper`** — not a Go CLI tool but an in-process diagnostic
  mod (`mad2/mods/mad2metadumper.dll`, source in `mad2metadumper/`): on F4,
  dumps the Alchemy engine's own live class registry (every registered
  class, parent chain, complete field list) to `logs/mad2metadump.txt`.
  This is the actual source of `mad2iga/live_reflection_schema.json` — see
  `CLASS_SCHEMA.md`'s "Live reflection dump" section. Same "diagnostic, not
  a gameplay mod" status as `mad2rhythmlogger`/`mad2climbprobe` (see
  `CLAUDE.md`).
- **`mad2shoptrace`** — another in-process diagnostic mod
  (`mad2shoptrace/`), built to chase one specific question (why several
  Duty Free shop items never appear in-game) once static analysis hit a
  runtime-only wall — see `SCRIPT_FORMAT.md`'s Round 9 case study. Same
  vtable-patch-`execute()` technique as `mad2rhythmlogger`, applied to a
  different set of opcodes (`OpForEach`/`OpSpawn`/`OpChangeMembership`/
  `OpFindSubSet`/`OpCheckMembership`/`OpCreateVariable`) and gated to a
  different level.
- **`mad2characterswitchprobe`** — in-process diagnostic mod
  (`mad2characterswitchprobe/`) chasing a different question: can a mod let
  the player switch to another already-spawned character mid-level? Found
  and live-tested the real mechanism (`ActorInfo::_controller` +
  `ActorInterfaceResolver::playerSet`, both reassignable safely without a
  crash once candidate-selection was hardened), and traced the one
  remaining gap (the camera doesn't follow until some other engine event
  fires) to an unfound `SetCameraTargetMessage` sender in `ViewportRTLib.dll`.
  See `CLASS_SCHEMA.md`'s "Character-switching feasibility investigation"
  section for the full writeup, including two real crash post-mortems and a
  reusable live export-table-scanning technique.

- **`mad2xdeltamod`** — not a Go tool but an in-process mod
  (`mad2/mods/mad2xdeltamod.dll`): hooks `igIGZLoader::parseSections`
  in-process (see `IGA_FORMAT.md`'s own section on this) and applies
  `*.mad2xdelta` binary-diff patches to a `.pak`'s already-decompressed
  bytes in memory, so mods can ship a few-KB diff instead of a whole
  patched `.bld`. Same-length string edits confirmed working live;
  growing edits still crash, see `IGA_FORMAT.md` for the open
  investigation.
- **`mad2xdeltagen`** — native Linux CLI (own CMake project, `just
  build-xdeltagen`), the offline half of the pair above: turns original +
  modified region bytes (from the same `pak-dump`/`pak-compile` round trip
  as `mad2repack`) into one `.mad2xdelta` file.

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
- **Class field schema**: solved engine-wide via a live reflection dump
  (`mad2metadumper` reads the Alchemy engine's own class registry directly
  out of the running game — 1392 classes, 20,237 fields, full
  own+inherited chains, self-verifying and cross-checked exactly against
  every previously hand-verified offset) — see `CLASS_SCHEMA.md`'s "Live
  reflection dump" section. The per-class hand-decompilation approach
  documented earlier in that same doc (`ActorInfo` PC-verified fields, the
  `TFBScriptInfo` namespace's 75/104-class PC-verified pass) is still
  useful reading for methodology/caveats (e.g. the Wii-vs-PC offset
  mismatch that motivated PC verification in the first place), but is no
  longer the limiting factor on coverage. Real graph traversal
  (`mad2repack script-dump`) reaches actual compiled scripts (a real
  1283-opcode script pulled out of `VolcanoRave/level.bld`), now with
  every opcode's inherited fields resolved too (previously missing).
- **Instance naming**: **solved and implemented** (was our biggest open
  gap). The `_name` word (e.g. `ActorInfo+8`, an `igStringMetaField`) is a
  plain index into the **`TSTR` string table (fixup 0x01)**, and the
  **`RSTR` table (fixup 0x04)** authoritatively marks which field locations
  are such indices — so `TSTR[word]` is the real name. The "small
  sequential integers" every prior attempt got stuck on *were* those `TSTR`
  indices; the lookups were one step short, and the old heuristic
  `StringPool` (magic-gap scan + fudge seeds + `pool[ordinal+1]`) only lined
  up for script/variable/UI objects. `objgraph.go` now parses `TSTR`/`RSTR`
  from Section 0's chunk table (`buildStringTables`/`ResolveName`/
  `ResolveFieldName`, `RSTR`-gated so there are no false positives) and the
  heuristic is gone. Verified end-to-end: `objects-dump` on
  `VolcanoRave/level.bld` resolves **175 `ActorInfo` names**
  (`Rave_CS_Controller`, `Mid_CS_Gloria`, `DEBUG_LevelReset`, …) and
  `script-dump` **2052 `_varName`s**. See `CLASS_SCHEMA.md`'s "Naming
  investigation" section (rewritten) for the full story.
- **Audio**: solved, verified against the whole corpus.
- **Textures**: container/schema solved, pixel data unresolved.
- **Havok animation**: identified, not yet parseable.
- `mad2iga/level.go`'s coordinate scanner is still a heuristic byte-pattern
  scanner for finding coordinates, kept alongside (not replaced by)
  `objgraph.go`'s real walker — each has different strengths today (see
  `CLASS_SCHEMA.md`). Naming remains the biggest structural gap left in
  "can we edit anything."
