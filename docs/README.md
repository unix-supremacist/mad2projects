# Madagascar 2 asset format documentation

This documents the on-disk formats used by Madagascar: Escape 2 Africa (PC),
built on the Vicarious Visions "Alchemy" engine, and the tooling in this repo
that reads/writes them (`mad2iga`, `mad2arc`, `mad2repack`, `mad2tool`).

Two nested formats are involved:

- **IGA** — the outer archive container (`.arc`, `.bld` files under
  `Content/Streams/win/`). Fully reverse-engineered; round-trips losslessly
  (`mad2repack verify`/`verify-all` confirm byte-identical repacks). See
  [`IGA_FORMAT.md`](IGA_FORMAT.md).
- **IGZ** — the format of the files an IGA archive contains (`.pak` files,
  `level.bld`'s payload once unwrapped from its own IGA-like outer shell).
  This is Alchemy's serialized-object-graph format: a frozen memory image of
  `igObject` instances. The container header/section table is solid; the
  object graph inside is only partially mapped. See
  [`IGZ_FORMAT.md`](IGZ_FORMAT.md).
- The IGZ object graph's actual per-class field layout (which byte offsets
  mean what) is a separate reverse-engineering problem from the container
  format, covered in [`CLASS_SCHEMA.md`](CLASS_SCHEMA.md). This is where
  "can we edit arbitrary fields" ultimately lives or dies.

## Sibling reference material

- `../../mad2assetextractor/docs/` — earlier research notes (`research_notes.md`,
  `PAK_FORMAT.md`, `demo.md`) from before this tooling was migrated in-repo.
  Treat with a grain of salt — some entries there have since been confirmed
  wrong by cross-checking against real debug symbols (see `CLASS_SCHEMA.md`).
- `../../mad2assetextractor/wii_demo_ref/` — an unstripped PowerPC ELF
  (`Mad2.elf`) pulled from a Wii "Demo Action Pack" disc, plus its full symbol
  table and a script that decodes real field offsets straight out of it. This
  is the single best source of ground truth for the object schema — see
  `CLASS_SCHEMA.md` for how it's used and how to extend it.
- `https://github.com/bonesinmysoup/igRewrite8/tree/trapteam-but-real` — a
  modding tool for a sibling "ig" engine title (Skylanders), useful for
  cross-referencing the general shape of the engine's reflection/serialization
  system, but **not** byte-compatible with MAD2's IGZ v4 variant (see
  `IGZ_FORMAT.md`'s note on format variants).
- `igCore.dll` (in `mad2/`) — the PC build's core engine DLL. Unlike the retail
  `Mad2.exe`, it ships fully-demangled C++ export names (`Gap::Core::igMetaObject`,
  `igMetaField`, etc.) and is the PC-side cross-check for everything found in
  the Wii ELF.

## The tools

- **`mad2iga`** — shared Go library (`Open`/`ExtractFile`/`Repack`/`DumpPak`/
  `CompilePak`/`DumpLevelCoords`/`CompileLevelCoords`). Everything else here
  imports it via a `replace` directive.
- **`mad2arc`** — read-only CLI: list/extract IGA archives.
- **`mad2repack`** — the main CLI: `unpack`/`pack`/`verify`/`replace`/
  `pak-dump`/`pak-compile`/`level-dump`/`level-compile`/`verify-binary`.
- **`mad2tool`** — orchestrates `mad2repack` over the whole game's
  `Content/Streams/win/` directory (`extract`/`rebuild`), including dumping
  every nested `.pak` and `level.bld` to editable JSON and back.

## Current state, honestly

- IGA container: solid, verified round-trip.
- IGZ container (header/sections): solid for the header/section-table shape;
  section *contents* beyond Section 0's class directory and string pool are
  understood only where documented below.
- Object field schema: growing, verified-where-marked (see `CLASS_SCHEMA.md`).
  `mad2iga`'s `level.go` coordinate dumper is still a heuristic byte-pattern
  scanner, not a real object-graph walker — it now uses verified offsets
  where we have them, but it's still fundamentally guessing at *which* bytes
  are a coordinate rather than walking the real section-1 object graph field
  by field. Replacing it with a real schema-driven parser is the natural next
  big step once `CLASS_SCHEMA.md`'s coverage is wide enough.
