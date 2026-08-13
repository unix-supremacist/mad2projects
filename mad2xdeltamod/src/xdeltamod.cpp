// mad2xdeltamod -- applies *.mad2xdelta patches (see
// ../include/mad2xdelta_format.h) to a loaded .pak's decompressed IGZ
// section bytes, in memory, at the exact moment between "fully
// decompressed" and "object graph parsed".
//
// Why this exists: .bld/.arc archives can't be redistributed whole (see
// CLAUDE.md's asset-format-reverse-engineering section), and a .pak can't
// be shipped as a loose file instead either -- the game never opens a bare
// .pak via CreateFile/NtCreateFile, only whole .bld/.arc archives (see
// mad2assetloader/src/file_redirect.cpp). And .bld can't be xdelta-diffed
// directly either: each file entry inside it is independently zlib-chunk-
// compressed (docs/IGA_FORMAT.md), so a one-byte text edit reshuffles the
// entire compressed stream -- there is no useful byte-level similarity
// between an original and edited .bld to diff against. So instead: ship a
// tiny binary diff against one specific IGZ section's DECOMPRESSED bytes
// (see mad2xdeltagen, the offline tool that produces these), and apply it
// here, at runtime, right before the loader parses that section -- no full
// archive or .pak ever touches disk.
//
// The hook point, Gap::Core::igIGZLoader::parseSections, and every offset
// this file pokes at (igIGZLoader's own fields, igDataList's layout,
// igMemoryPool::mallocAligned's signature) were found by reverse-
// engineering igCore.dll directly (Ghidra, live in this repo's session --
// not from any header/symbol source): igIGZLoader::readSections mallocs
// one heap buffer per active IGZ section and, for each one, drives the
// SAME zlib-chunk decompression pipeline documented in docs/IGA_FORMAT.md
// (Gap::Core::igArchive's 0x8000-byte chunk decompressor, itself a plain
// wrapper around zlib's uncompress()); by the time parseSections is
// called, every section's bytes are fully decompressed and sitting in
// their own contiguous buffer, tracked in two parallel igDataLists on the
// igIGZLoader instance -- exactly what this hook needs, and no earlier or
// later point in the load sequence offers the same guarantee.
#include <windows.h>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xdelta3.h"

#include "../include/mad2xdelta_format.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "../../mad2sharedlog/mad2sharedlog.h"

namespace {

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2xdeltamod]", fmt, args);
    va_end(args);
}

// ---- igCore.dll offsets, reverse-engineered live against this repo's
// mad2/igCore.dll (see file header). If a future game update ships a
// different igCore.dll build, these will need re-verifying -- there is no
// way to detect a mismatch other than the hook simply not firing where
// expected, or (in the worst case) firing on the wrong bytes, which is why
// every patch application below is gated on an exact size+CRC32 match
// against the section it expects, not applied blindly by index. ----

// RVA of Gap::Core::igIGZLoader::parseSections.
constexpr uintptr_t kParseSectionsRva = 0x51000;
// Stolen prologue length: "push esi; mov esi,[esp+8]" is exactly 5 bytes
// and a clean instruction boundary (verified via disassembly) -- the
// minimum InstallInlineHook needs for its 5-byte JMP detour.
constexpr int kParseSectionsStolenBytes = 5;
// RVA of Gap::Core::igMemoryPool::mallocAligned(this, size, alignment) --
// only called when a patch needs to GROW a section past its original
// allocated size (e.g. adding marker text to a string table).
constexpr uintptr_t kMallocAlignedRva = 0x759b0;

// igIGZLoader instance field offsets.
constexpr uintptr_t kOff_Filename = 0x08;             // igStringRef -- a bare char* at its own offset 0
constexpr uintptr_t kOff_HeaderPtr = 0x0c;             // fixed header buffer -- see MAD2XDELTA_REGION_HEADER below
constexpr uintptr_t kOff_SectionSizePtrList = 0x820;  // igDataList<{u32 size; u32 ptr}>* -- one per active section
constexpr uintptr_t kOff_SectionPoolList = 0x828;     // igDataList<u32 pool>*, parallel to the above

// igDataList field offsets (from decompiling its own getElementCount/
// makeRoom/constructor -- it has no public "get data pointer" accessor,
// only protected internals, so these were read directly off the object).
constexpr uintptr_t kOff_DataList_Count = 0x08;
constexpr uintptr_t kOff_DataList_Data = 0x14;

struct SectionSlot {
    uint32_t size;  // packed -- see kSectionSizeMask below, NOT a plain byte count
    uint32_t ptr;
};

// The size field isn't a plain byte count: it's the same packed layout as
// Gap::Core::igMemory's own first field (this section's buffer was
// allocated via igMemory::mallocAligned, which stores {packed size+flags,
// pointer} -- see igMemory::getSize()'s own decompiled body,
// "return *(uint*)this & 0x7ffffff"). Confirmed live: reading a slot's
// size raw gave 268821472 for a section that's really 386016 bytes --
// 268821472 & kSectionSizeMask == 386016, byte-exact. The high 5 bits are
// flags (getOwnsMemory/getHasAlignment etc.) this mod never needs to
// interpret, only preserve when writing a new size back.
constexpr uint32_t kSectionSizeMask = 0x07FFFFFFu;

uint32_t GetSlotSize(const SectionSlot& s) { return s.size & kSectionSizeMask; }

void SetSlotSize(SectionSlot& s, uint32_t newSize) {
    s.size = (s.size & ~kSectionSizeMask) | (newSize & kSectionSizeMask);
}

using MallocAlignedFn = void*(__thiscall*)(void* pool, unsigned int size, unsigned int alignment);

struct RegionEntry {
    uint32_t kind = MAD2XDELTA_REGION_SECTION;
    uint32_t origSize = 0;
    uint32_t origCrc32 = 0;
    std::vector<uint8_t> patchData;
};

struct PatchEntry {
    std::string matchKey;  // normalized "<archive>_<file>" (or bare filename) this patch targets
    std::vector<RegionEntry> regions;
};

std::vector<PatchEntry> g_patches;

// Case-folds and, if the input looks like a path (contains '/' or '\'),
// reduces it to its last two segments joined by '_'. Confirmed live
// (mad2.log) that igIGZLoader's own filename string is
// "<archive path>/0x<CRC32, hex>" -- e.g.
// "X:\...\Content/Streams/win/VOLCANORAVE.BLD/0x75F7FBB1" -- NOT the
// human-readable member name; the engine looks archive entries up by CRC
// (see mad2iga/iga.go's own Entries[i].CRC) and apparently never carries
// the name string this far. So patch files must be named
// "<ARCHIVE>.bld_0x<CRC32>.mad2xdelta" (case-insensitive; the CRC is the
// archive entry's own CRC, e.g. from `mad2repack unpack`'s metadata.json
// "crc" field, converted to hex) -- NOT "<ARCHIVE>.bld_<member name>" as
// you might otherwise guess. Applied to both the runtime filename and each
// patch file's own stem (see LoadPatches) so matching is case-insensitive
// either way.
std::string NormalizeMatchKey(const char* rawName) {
    std::string s(rawName ? rawName : "");
    for (char& c : s) {
        if (c == '\\') c = '/';
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    size_t lastSlash = s.find_last_of('/');
    if (lastSlash == std::string::npos) return s;
    std::string lastSeg = s.substr(lastSlash + 1);
    std::string rest = s.substr(0, lastSlash);
    size_t prevSlash = rest.find_last_of('/');
    std::string secondLastSeg = (prevSlash == std::string::npos) ? rest : rest.substr(prevSlash + 1);
    return secondLastSeg + "_" + lastSeg;
}

void LoadPatches() {
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char* slash = strrchr(exePath, '\\');
    char* slash2 = strrchr(exePath, '/');
    if (slash2 && (!slash || slash2 > slash)) slash = slash2;
    if (slash) {
        *(slash + 1) = '\0';
    } else {
        exePath[0] = '\0';
    }

    std::string dir = std::string(exePath) + "xdelta_patches\\";
    std::string pattern = dir + "*.mad2xdelta";

    WIN32_FIND_DATAA findData;
    HANDLE h = FindFirstFileA(pattern.c_str(), &findData);
    if (h == INVALID_HANDLE_VALUE) {
        Log("no patches found in %s\n", dir.c_str());
        return;
    }
    do {
        std::string fullPath = dir + findData.cFileName;
        FILE* f = fopen(fullPath.c_str(), "rb");
        if (!f) continue;

        Mad2XDeltaFileHeader fileHeader;
        bool ok = fread(&fileHeader, sizeof(fileHeader), 1, f) == 1 &&
                  memcmp(fileHeader.magic, MAD2XDELTA_MAGIC, sizeof(fileHeader.magic)) == 0;
        if (!ok) {
            Log("skipping %s: bad or unreadable header (wrong magic? old single-region format?)\n",
                findData.cFileName);
            fclose(f);
            continue;
        }

        PatchEntry entry;
        bool truncated = false;
        for (uint32_t r = 0; r < fileHeader.numRegions && !truncated; ++r) {
            Mad2XDeltaRegionHeader regionHeader;
            if (fread(&regionHeader, sizeof(regionHeader), 1, f) != 1) {
                truncated = true;
                break;
            }
            RegionEntry region;
            region.kind = regionHeader.kind;
            region.origSize = regionHeader.origSize;
            region.origCrc32 = regionHeader.origCrc32;
            region.patchData.resize(regionHeader.patchSize);
            if (regionHeader.patchSize > 0 &&
                fread(region.patchData.data(), 1, regionHeader.patchSize, f) != regionHeader.patchSize) {
                truncated = true;
                break;
            }
            entry.regions.push_back(std::move(region));
        }
        fclose(f);
        if (truncated) {
            Log("skipping %s: truncated region data\n", findData.cFileName);
            continue;
        }

        std::string stem = findData.cFileName;
        size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        entry.matchKey = NormalizeMatchKey(stem.c_str());

        Log("loaded patch %s: matchKey=%s, %zu region(s):\n", findData.cFileName, entry.matchKey.c_str(),
            entry.regions.size());
        for (auto& region : entry.regions) {
            Log("  kind=%s origSize=%u origCrc32=0x%08x patchSize=%zu\n",
                region.kind == MAD2XDELTA_REGION_HEADER ? "header" : "section", region.origSize, region.origCrc32,
                region.patchData.size());
        }
        g_patches.push_back(std::move(entry));
    } while (FindNextFileA(h, &findData));
    FindClose(h);
}

// Diagnostic-first, apply-second: a crash during an earlier diagnostic
// pass (confirmed via mad2.log: a fresh "Attaching to process..." line
// appeared right after a raw, unmasked size/ptr got logged, meaning
// mad2relauncher caught and relaunched a real crash) showed that blindly
// dereferencing a ptr/size pulled off these structures is NOT always safe.
// IsBadReadPtr + a generous sane-size cap is a defensive guess, not a
// confirmed-necessary fix, but cheap enough to keep everywhere.
constexpr uint32_t kMaxSaneRegionSize = 16u * 1024u * 1024u;

bool LooksReadable(uint32_t ptr, uint32_t size) {
    if (ptr == 0 || size == 0 || size > kMaxSaneRegionSize) return false;
    return !IsBadReadPtr(reinterpret_cast<const void*>(static_cast<uintptr_t>(ptr)), size);
}

// Decodes region.patchData against `sourceBytes`/`sourceSize` into outBuf,
// resizing outBuf as needed. Returns false (and logs) on any xd3 failure.
bool DecodeRegion(const RegionEntry& region, const void* sourceBytes, uint32_t sourceSize, const char* matchKey,
                   std::vector<uint8_t>& outBuf, usize_t& outSize) {
    outBuf.resize(static_cast<size_t>(region.origSize) + region.patchData.size() + 4096);
    int rc = xd3_decode_memory(region.patchData.data(), static_cast<usize_t>(region.patchData.size()),
                                reinterpret_cast<const uint8_t*>(sourceBytes), sourceSize, outBuf.data(), &outSize,
                                static_cast<usize_t>(outBuf.size()), 0);
    if (rc != 0) {
        Log("xd3_decode_memory failed for %s, rc=%d -- leaving region untouched\n", matchKey, rc);
        return false;
    }
    return true;
}

// MAD2XDELTA_REGION_SECTION: find the one {size,ptr} slot in igIGZLoader's
// own section igDataList whose (masked) size and CRC32 match this region's
// recorded original, decode the patch against it, and write the result
// back -- in place if it still fits, or via a fresh allocation from the
// SAME igMemoryPool the engine used for this section if the edit grew it.
void ApplySectionRegion(BYTE* base, const RegionEntry& region, const std::string& matchKey) {
    BYTE* sizePtrList = *reinterpret_cast<BYTE**>(base + kOff_SectionSizePtrList);
    BYTE* poolList = *reinterpret_cast<BYTE**>(base + kOff_SectionPoolList);
    if (!sizePtrList || !poolList) return;

    int count = *reinterpret_cast<int*>(sizePtrList + kOff_DataList_Count);
    SectionSlot* slots = *reinterpret_cast<SectionSlot**>(sizePtrList + kOff_DataList_Data);
    uint32_t* pools = *reinterpret_cast<uint32_t**>(poolList + kOff_DataList_Data);

    Log("%s: %d section(s) present, looking for size=%u crc32=0x%08x\n", matchKey.c_str(), count, region.origSize,
        region.origCrc32);
    for (int i = 0; i < count; ++i) {
        uint32_t size = GetSlotSize(slots[i]);
        Log("  section[%d]: size=%u (raw=0x%08x) ptr=0x%08x pool=0x%08x\n", i, size, slots[i].size, slots[i].ptr,
            pools[i]);
    }

    for (int i = 0; i < count; ++i) {
        uint32_t size = GetSlotSize(slots[i]);
        if (size != region.origSize) continue;
        if (!LooksReadable(slots[i].ptr, size)) continue;
        const void* sectionBytes = reinterpret_cast<const void*>(static_cast<uintptr_t>(slots[i].ptr));
        if (Mad2XDelta_Crc32(sectionBytes, size) != region.origCrc32) continue;

        std::vector<uint8_t> outBuf;
        usize_t outSize = 0;
        if (!DecodeRegion(region, sectionBytes, size, matchKey.c_str(), outBuf, outSize)) return;

        if (outSize <= size) {
            memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(slots[i].ptr)), outBuf.data(), outSize);
            SetSlotSize(slots[i], static_cast<uint32_t>(outSize));
            Log("applied section patch %s in place (%u -> %u bytes)\n", matchKey.c_str(), region.origSize,
                static_cast<unsigned>(outSize));
        } else {
            HMODULE igCore = GetModuleHandleA("igCore.dll");
            auto mallocAligned = reinterpret_cast<MallocAlignedFn>(reinterpret_cast<BYTE*>(igCore) + kMallocAlignedRva);
            void* pool = reinterpret_cast<void*>(static_cast<uintptr_t>(pools[i]));
            void* newBuf = mallocAligned(pool, static_cast<unsigned int>(outSize), 16);
            if (!newBuf) {
                Log("mallocAligned failed growing %s to %u bytes -- leaving section untouched\n", matchKey.c_str(),
                    static_cast<unsigned>(outSize));
                return;
            }
            memcpy(newBuf, outBuf.data(), outSize);
            // Deliberately leak the old (smaller) buffer: it came from the
            // engine's own pool, and freeing it via a guessed-at API risks
            // a worse bug than a one-time, few-KB, per-load leak.
            slots[i].ptr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(newBuf));
            SetSlotSize(slots[i], static_cast<uint32_t>(outSize));
            Log("applied section patch %s via new pool allocation (%u -> %u bytes, grew)\n", matchKey.c_str(),
                region.origSize, static_cast<unsigned>(outSize));
        }
        return;
    }
    Log("section patch %s matched filename but no section matched size=%u crc32=0x%08x (game may store sections "
        "differently than expected -- check the section sizes/crcs above)\n",
        matchKey.c_str(), region.origSize, region.origCrc32);
}

// MAD2XDELTA_REGION_HEADER: igIGZLoader+0xc holds a fixed-format header
// buffer at a version-independent offset (confirmed: `*(void**)(this+0xc)
// = pvVar7` appears identically in all 3 file-version branches of
// readSections). There's no runtime-readable size for it worth trusting
// the way section sizes are (packed, version-dependent fields) -- this
// region's own recorded origSize IS the size, taken on faith from the
// offline diff. Growing edits shift every subsequent string's offset, and
// those offsets live in this buffer's own internal chunk table (mad2iga/
// chunktable.go's "Section 0" fixup) -- patching the section alone applies
// the new bytes but leaves this table stale, which is why a same-length-
// only edit rendered live but a growing one silently didn't. This buffer
// is never expected to grow (mad2repack's own fixup is a same-size splice,
// not a length change), so only an in-place write is implemented.
void ApplyHeaderRegion(BYTE* base, const RegionEntry& region, const std::string& matchKey) {
    void* headerPtr = *reinterpret_cast<void**>(base + kOff_HeaderPtr);
    uint32_t ptrValue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(headerPtr));
    // Diagnostic -- the version discriminator and both candidate size
    // fields readSections' three branches use for this exact allocation
    // (this+0x5c for version<2, this+0x24 for version>=2), read directly
    // and unconditionally so the real allocated size can be determined
    // from a live log instead of assumed. A first attempt assumed this
    // buffer covers the whole file up to Section 1's start (10496 bytes)
    // and read clearly unrelated data (HLSL shader source text) past some
    // point, meaning the real allocation is smaller than that.
    uint32_t version = *reinterpret_cast<uint32_t*>(base + 0x14);
    uint32_t sizeIfLt2 = *reinterpret_cast<uint32_t*>(base + 0x5c);
    uint32_t alignIfLt2 = *reinterpret_cast<uint32_t*>(base + 0x60);
    uint32_t sizeIfGe2 = *reinterpret_cast<uint32_t*>(base + 0x24);
    uint32_t alignIfGe2 = *reinterpret_cast<uint32_t*>(base + 0x28);
    Log("%s: header ptr=0x%08x, version=%u sizeIfLt2=%u(align=%u) sizeIfGe2=%u(align=%u), looking for size=%u "
        "crc32=0x%08x\n",
        matchKey.c_str(), ptrValue, version, sizeIfLt2, alignIfLt2, sizeIfGe2, alignIfGe2, region.origSize,
        region.origCrc32);
    if (!LooksReadable(ptrValue, region.origSize)) {
        Log("  header ptr/size doesn't look safely readable -- skipping\n");
        return;
    }
    uint32_t crc = Mad2XDelta_Crc32(headerPtr, region.origSize);
    Log("  header crc32=0x%08x\n", crc);
    {
        // Diagnostic hex dump -- unconditional while this offset/size
        // assumption is still being validated live. Logs the first 64 and
        // last 64 bytes so a mismatch can be compared byte-for-byte
        // against the offline original (header_orig.bin) to tell apart
        // "wrong buffer entirely" from "right buffer, wrong boundary".
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(headerPtr);
        std::string hexFirst, hexLast;
        char buf[4];
        for (uint32_t i = 0; i < 64 && i < region.origSize; ++i) {
            snprintf(buf, sizeof(buf), "%02x", bytes[i]);
            hexFirst += buf;
        }
        for (uint32_t i = (region.origSize > 64 ? region.origSize - 64 : 0); i < region.origSize; ++i) {
            snprintf(buf, sizeof(buf), "%02x", bytes[i]);
            hexLast += buf;
        }
        Log("  header first64=%s\n", hexFirst.c_str());
        Log("  header last64=%s\n", hexLast.c_str());
    }
    if (crc != region.origCrc32) {
        Log("header patch %s matched filename but header crc32 didn't match -- leaving untouched\n",
            matchKey.c_str());
        return;
    }

    std::vector<uint8_t> outBuf;
    usize_t outSize = 0;
    if (!DecodeRegion(region, headerPtr, region.origSize, matchKey.c_str(), outBuf, outSize)) return;

    if (outSize != region.origSize) {
        Log("header patch %s decoded to %u bytes but expected exactly %u (this region never grows) -- leaving "
            "untouched\n",
            matchKey.c_str(), static_cast<unsigned>(outSize), region.origSize);
        return;
    }
    memcpy(headerPtr, outBuf.data(), outSize);
    Log("applied header patch %s in place (%u bytes)\n", matchKey.c_str(), region.origSize);
}

void TryPatchLoader(void* loader) {
    if (g_patches.empty()) return;
    BYTE* base = reinterpret_cast<BYTE*>(loader);

    const char* filename = *reinterpret_cast<char* const*>(base + kOff_Filename);
    if (!filename || !filename[0]) return;

    std::string matchKey = NormalizeMatchKey(filename);
    Log("parseSections: file=\"%s\" matchKey=%s\n", filename, matchKey.c_str());

    for (auto& patch : g_patches) {
        if (patch.matchKey != matchKey) continue;
        for (auto& region : patch.regions) {
            if (region.kind == MAD2XDELTA_REGION_HEADER) {
                ApplyHeaderRegion(base, region, patch.matchKey);
            } else {
                ApplySectionRegion(base, region, patch.matchKey);
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------
// Hook stub. Must be raw asm, not an ordinary C function: it's entered via
// a JMP (not a CALL) from the exact byte offset InstallInlineHook patched
// inside igIGZLoader::parseSections, with ECX = igIGZLoader* (thiscall)
// and the stack in whatever state parseSections' own caller left it --
// nothing here may assume or introduce a return address. It saves the
// caller-clobberable registers, calls Mad2XDeltaMod_OnParseSections(ecx)
// as an ordinary cdecl C function, restores those registers so the
// following stolen bytes execute in EXACTLY the state they would have at
// the real function's original entry, then jumps into the trampoline
// mad2hookutil built (the stolen bytes followed by a jump back into the
// untouched remainder of parseSections). g_trampoline and
// Mad2XDeltaMod_OnParseSections are declared extern "C" specifically so
// their linker symbol names ("_g_trampoline", "_Mad2XDeltaMod_OnParseSections"
// under this toolchain's cdecl name-mangling) are the literal, predictable
// names this asm references -- putting them in an anonymous namespace (as
// everything else in this file is) would mangle them unpredictably.
// ---------------------------------------------------------------------
extern "C" void* g_trampoline = nullptr;

extern "C" void Mad2XDeltaMod_OnParseSections(void* loader) {
    // Runs on whatever thread the game itself calls parseSections from --
    // keep this fast, and never let an exception escape into engine code.
    TryPatchLoader(loader);
}

extern "C" __attribute__((naked)) void Mad2XDeltaMod_HookStub() {
    asm volatile(
        "pushl %eax\n\t"
        "pushl %edx\n\t"
        "pushl %ecx\n\t"
        "pushl %ecx\n\t"
        "call _Mad2XDeltaMod_OnParseSections\n\t"
        "addl $4, %esp\n\t"
        "popl %ecx\n\t"
        "popl %edx\n\t"
        "popl %eax\n\t"
        "jmp *_g_trampoline\n\t");
}

namespace {

DWORD WINAPI InitThread(LPVOID) {
    LoadPatches();
    if (g_patches.empty()) return 0;  // nothing to apply -- don't bother hooking at all

    HMODULE igCore = nullptr;
    for (int attempt = 0; attempt < 600 && !igCore; ++attempt) {  // up to ~60s
        igCore = GetModuleHandleA("igCore.dll");
        if (!igCore) Sleep(100);
    }
    if (!igCore) {
        Log("igCore.dll never appeared -- giving up\n");
        return 0;
    }

    void* target = reinterpret_cast<BYTE*>(igCore) + kParseSectionsRva;
    void* trampoline = nullptr;
    if (!Mad2HookUtil_InstallInlineHook(target, reinterpret_cast<void*>(&Mad2XDeltaMod_HookStub),
                                         kParseSectionsStolenBytes, &trampoline)) {
        Log("InstallInlineHook failed on igIGZLoader::parseSections (%p) -- is aa_mad2hookutil.dll loaded?\n",
            target);
        return 0;
    }
    g_trampoline = trampoline;
    Log("hooked igIGZLoader::parseSections at %p, %zu patch(es) loaded\n", target, g_patches.size());
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            break;
        default:
            break;
    }
    return TRUE;
}
