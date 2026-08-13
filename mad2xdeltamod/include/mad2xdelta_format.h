#pragma once
// Shared on-disk container format for mad2xdeltamod's *.mad2xdelta patch
// files, used by both mad2xdeltagen (the native Linux tool that produces
// them offline) and mad2xdeltamod (the in-game DLL that applies them at
// runtime -- see mad2xdeltamod/src/xdeltamod.cpp's file header for the full
// picture of why this exists: a raw xdelta3/VCDIFF patch alone only tells
// you how to transform *some* source buffer into a target buffer, not
// which of a running igIGZLoader's in-memory buffers that source buffer
// actually corresponds to.
//
// A single logical string-pool edit needs up to TWO regions patched
// consistently, discovered the hard way by testing a growing edit live:
//   - REGION_SECTION: the matched IGZ section (one of igIGZLoader's
//     decompressed section buffers, found by exact size+CRC32 match against
//     one of the {size,ptr} pairs in its own igDataList -- see
//     xdeltamod.cpp). Its size field is itself packed (igMemory's own
//     size|flags layout, see kSectionSizeMask in xdeltamod.cpp), masked
//     before comparing/logging.
//   - REGION_HEADER: the fixed-format header buffer at a KNOWN, version-
//     independent offset on the igIGZLoader instance itself
//     (Gap::Core::igIGZLoader+0xc -- confirmed via readSections'
//     decompiled code: `*(void**)(this+0xc) = pvVar7` appears identically
//     in all 3 file-version branches). Growing REGION_SECTION shifts every
//     subsequent string's offset, and those offsets are recorded in this
//     separate header buffer's own internal chunk table (mad2iga/
//     chunktable.go's "Section 0" fixup, already computed correctly by
//     mad2repack pak-compile for the whole file) -- patching REGION_SECTION
//     alone applies the new bytes but leaves this table stale, which is
//     why an early same-length-only test rendered correctly but growing
//     edits didn't visibly change anything. Unlike a section, there's no
//     runtime-readable size field for this buffer worth trusting: the
//     patch's own origSize (computed once, offline, from the same file)
//     is used directly instead.
#include <cstddef>
#include <cstdint>

#define MAD2XDELTA_MAGIC "MAD2XD2\0"  // 8 bytes including the trailing NUL -- bumped for the multi-region format

enum Mad2XDeltaRegionKind : uint32_t {
    MAD2XDELTA_REGION_SECTION = 0,  // matched igIGZLoader section (size+CRC32 lookup, may grow)
    MAD2XDELTA_REGION_HEADER = 1,   // igIGZLoader+0xc fixed header buffer (known size, never grows)
};

#pragma pack(push, 1)
struct Mad2XDeltaFileHeader {
    char magic[8];         // must equal MAD2XDELTA_MAGIC exactly
    uint32_t numRegions;   // followed immediately by this many Mad2XDeltaRegionHeader+patch blocks
};

struct Mad2XDeltaRegionHeader {
    uint32_t kind;       // Mad2XDeltaRegionKind
    uint32_t origSize;   // size in bytes of the original (pre-patch) region
    uint32_t origCrc32;  // CRC32 of the original region's bytes
    uint32_t patchSize;  // byte length of the raw xdelta3/VCDIFF data that follows
    // followed immediately by `patchSize` bytes of xd3_encode_memory output
};
#pragma pack(pop)

// Standard CRC-32 (IEEE 802.3, poly 0xEDB88320), computed a byte at a time.
// Small/simple over a fast table-driven version deliberately -- patches are
// small (a level's string-pool section, not a whole archive) and this runs
// at most a few times per level load, never per frame.
inline uint32_t Mad2XDelta_Crc32(const void* data, size_t len) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}
