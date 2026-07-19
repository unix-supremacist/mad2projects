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
