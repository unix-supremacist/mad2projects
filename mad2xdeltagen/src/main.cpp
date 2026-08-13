// mad2xdeltagen -- offline tool that turns "original region bytes" +
// "modified region bytes" (one or more regions) into one *.mad2xdelta
// patch file, the input format mad2xdeltamod (a Mad2.exe in-process mod)
// applies at runtime to avoid ever redistributing whole .bld/.pak files.
//
// Why "region bytes" and not "whole .pak file": a .bld's IGZ container
// splits each .pak into a few independently-loaded in-memory buffers at
// the exact granularity the engine itself uses (see docs/IGZ_FORMAT.md and
// mad2xdeltamod/include/mad2xdelta_format.h's file header for the full
// reverse-engineering trail) -- there is no single contiguous "whole
// decompressed .pak file" buffer in the running game to patch against.
// Diff at the same granularity the engine actually loads things at
// instead: the "section" (the matched IGZ section holding the string
// pool, found by size+CRC32 at runtime) and, for edits that grow the
// section, the "header" (a separate fixed buffer holding an internal
// offset table that must also be updated, or the grown section's shifted
// string offsets are read through a stale table).
//
// Usage: mad2xdeltagen <output.mad2xdelta> <kind> <orig.bin> <mod.bin> [<kind> <orig.bin> <mod.bin> ...]
//   <kind> is "section" or "header".
//
// Both original and modified region bytes come from the SAME mad2iga/
// mad2repack pak-dump -> edit pak.json -> pak-compile round trip already
// used for whole-file repacking -- mad2repack's pak-compile already
// computes the header's own internal chunk-table fixup correctly for the
// whole file (mad2iga/chunktable.go); this tool just slices out the two
// regions (bytes before the string pool's section offset, and from that
// offset onward) from the SAME compiled output so both patches stay
// mutually consistent.
//
// Native Linux build (own CMakeLists.txt, not part of the root MinGW
// cross-build -- see mad2music/CMakeLists.txt for the same pattern) since
// it never runs inside the game process; only mad2xdeltamod's *runtime
// apply* side needs to cross-compile for 32-bit Windows.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "xdelta3.h"

#include "../../mad2xdeltamod/include/mad2xdelta_format.h"

namespace {

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    size_t n = out.empty() ? 0 : fread(out.data(), 1, out.size(), f);
    fclose(f);
    return n == out.size();
}

bool EncodeOne(const std::vector<uint8_t>& original, const std::vector<uint8_t>& modified,
               std::vector<uint8_t>& outPatch) {
    size_t availOutput = modified.size() + original.size() + 65536;
    outPatch.resize(availOutput);
    usize_t patchSize = 0;
    int rc = xd3_encode_memory(modified.data(), static_cast<usize_t>(modified.size()), original.data(),
                                static_cast<usize_t>(original.size()), outPatch.data(), &patchSize,
                                static_cast<usize_t>(outPatch.size()), XD3_COMPLEVEL_9);
    if (rc == ENOSPC) {
        availOutput *= 4;
        outPatch.resize(availOutput);
        rc = xd3_encode_memory(modified.data(), static_cast<usize_t>(modified.size()), original.data(),
                                static_cast<usize_t>(original.size()), outPatch.data(), &patchSize,
                                static_cast<usize_t>(outPatch.size()), XD3_COMPLEVEL_9);
    }
    if (rc != 0) {
        fprintf(stderr, "error: xd3_encode_memory failed, rc=%d\n", rc);
        return false;
    }
    outPatch.resize(patchSize);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5 || (argc - 2) % 3 != 0) {
        fprintf(stderr,
                "usage: %s <output.mad2xdelta> <kind> <orig.bin> <mod.bin> [<kind> <orig.bin> <mod.bin> ...]\n"
                "  <kind> is \"section\" or \"header\"\n",
                argv[0]);
        return 1;
    }

    const char* outputPath = argv[1];
    int numRegions = (argc - 2) / 3;

    FILE* out = fopen(outputPath, "wb");
    if (!out) {
        fprintf(stderr, "error: could not open output file %s\n", outputPath);
        return 1;
    }

    Mad2XDeltaFileHeader fileHeader{};
    memcpy(fileHeader.magic, MAD2XDELTA_MAGIC, sizeof(fileHeader.magic));
    fileHeader.numRegions = static_cast<uint32_t>(numRegions);
    fwrite(&fileHeader, sizeof(fileHeader), 1, out);

    for (int r = 0; r < numRegions; ++r) {
        const char* kindStr = argv[2 + r * 3];
        const char* origPath = argv[3 + r * 3];
        const char* modPath = argv[4 + r * 3];

        uint32_t kind;
        if (strcmp(kindStr, "section") == 0) {
            kind = MAD2XDELTA_REGION_SECTION;
        } else if (strcmp(kindStr, "header") == 0) {
            kind = MAD2XDELTA_REGION_HEADER;
        } else {
            fprintf(stderr, "error: unknown region kind \"%s\" (want \"section\" or \"header\")\n", kindStr);
            fclose(out);
            return 1;
        }

        std::vector<uint8_t> original, modified;
        if (!ReadFile(origPath, original)) {
            fprintf(stderr, "error: could not read original file %s\n", origPath);
            fclose(out);
            return 1;
        }
        if (!ReadFile(modPath, modified)) {
            fprintf(stderr, "error: could not read modified file %s\n", modPath);
            fclose(out);
            return 1;
        }

        std::vector<uint8_t> patchData;
        if (!EncodeOne(original, modified, patchData)) {
            fclose(out);
            return 1;
        }

        Mad2XDeltaRegionHeader regionHeader{};
        regionHeader.kind = kind;
        regionHeader.origSize = static_cast<uint32_t>(original.size());
        regionHeader.origCrc32 = Mad2XDelta_Crc32(original.data(), original.size());
        regionHeader.patchSize = static_cast<uint32_t>(patchData.size());
        fwrite(&regionHeader, sizeof(regionHeader), 1, out);
        fwrite(patchData.data(), 1, patchData.size(), out);

        printf("region %d (%s): origSize=%u origCrc32=0x%08x patchSize=%u (from %zu -> %zu bytes)\n", r, kindStr,
               regionHeader.origSize, regionHeader.origCrc32, regionHeader.patchSize, original.size(),
               modified.size());
    }

    fclose(out);
    printf("wrote %s: %d region(s)\n", outputPath, numRegions);
    return 0;
}
