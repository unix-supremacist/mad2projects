// Diagnostic runtime probe for Madagascar 2's "climb ability" and "monkey
// collectibles" reverse-engineering effort -- NOT a gameplay mod, not
// meant to stay loaded permanently (same status as mad2rhythmlogger -- see
// that mod's file header and docs/SCRIPT_FORMAT.md for the precedent).
//
// Both flags are now solved, in the save file at least: found by hooking
// the game's own save-file writes (this file's SaveWriteLog machinery,
// below) across a real playthrough and diffing consecutive writes -- see
// mad2launcher/saveeditor.go's ReadClimbAbility/ReadMonkeyCollectible for
// the confirmed offsets and mad2cheatapply.cpp's own kCheats table history
// for why several earlier, plausible-looking memory-diff candidates
// (logged in this file's git history) turned out to be false leads.
//
// This file's remaining job is the live-memory side: ScanForSaveStateObject
// (F3) finds the live object those same save-file offsets apply to
// (confirmed via a 13-independent-field pattern match, and cross-checked
// live via ToggleClimbLive/ToggleMonkeysLive actually working in-game) by
// scanning process memory rather than a fixed pointer chain -- a real
// static chain to it was searched for (backward pointer-walk through
// several levels of heap-allocated engine bookkeeping tables) and not
// found; the scan itself is the practical, working substitute, the same
// way mad2gameeffectsmod's ObjectShuffle already finds Havok objects by
// vtable-pattern scan instead of a chain.
//
// F9's DumpAll is what did the original hex-dump-and-diff investigation
// (still occasionally useful for further poking around); F5's
// ToggleAllCheats force-applies every mad2cheatapply-known cheat
// regardless of screen/level, confirming that struct is read live in every
// level, not just from the title screen.
#include <windows.h>
#include <winternl.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "../../mad2levelredirectmod/include/mad2levelredirect_api.h"
#include "../../mad2settings/include/mad2settings_api.h"

#include "../../mad2leveldetect/mad2leveldetect.h"
#include "../../mad2sharedlog/mad2sharedlog.h"

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2climbprobe]", fmt, args);
    va_end(args);
}

static bool IsPlausiblePointer(uintptr_t p) { return p > 0x10000 && p < 0x7FFFFFFF; }

// Same shape as every other mod's FollowChain, but keeps every intermediate
// hop instead of only the final address -- each hop is worth logging on
// its own so a failed/garbage capture is diagnosable after the fact.
static std::vector<uintptr_t> FollowChainVerbose(uintptr_t base, const uintptr_t* offsets, size_t count) {
    std::vector<uintptr_t> hops;
    if (!IsPlausiblePointer(base)) return hops;
    uintptr_t addr = *reinterpret_cast<uint32_t*>(base);
    hops.push_back(addr);
    for (size_t i = 0; i < count; ++i) {
        if (!IsPlausiblePointer(addr)) break;
        addr = *reinterpret_cast<uint32_t*>(addr + offsets[i]);
        hops.push_back(addr);
    }
    return hops;
}

static void LogHops(const char* label, const std::vector<uintptr_t>& hops) {
    for (size_t i = 0; i < hops.size(); ++i) {
        Log("  %s hop[%zu] = 0x%08X\n", label, i, static_cast<unsigned int>(hops[i]));
    }
}

// Dumps [start, start+length) 16 bytes/line, address-prefixed so two
// captures can be diffed directly. Stops (rather than crashing) the moment
// it hits a page that VirtualQuery reports as not committed/readable --
// this is a speculative "how big is this object really" probe, not a
// verified-safe fixed-size struct read.
static void SafeDumpRegion(uintptr_t start, size_t length, const char* label) {
    Log("---- DUMP %s addr=0x%08X len=0x%zX ----\n", label, static_cast<unsigned int>(start), length);
    size_t offset = 0;
    while (offset < length) {
        uintptr_t cur = start + offset;
        uintptr_t pageAddr = cur & ~static_cast<uintptr_t>(0xFFF);
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(pageAddr), &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) {
            Log("  [unreadable page at 0x%08X, stopping]\n", static_cast<unsigned int>(pageAddr));
            return;
        }
        uintptr_t pageEnd = pageAddr + 0x1000;
        uintptr_t chunkEnd = (start + length < pageEnd) ? (start + length) : pageEnd;

        for (uintptr_t addr = cur; addr < chunkEnd; addr += 16) {
            size_t n = static_cast<size_t>((chunkEnd - addr < 16) ? (chunkEnd - addr) : 16);
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(addr);
            char hex[64];
            char asc[20];
            size_t hp = 0, ap = 0;
            for (size_t i = 0; i < 16; ++i) {
                if (i < n) {
                    hp += static_cast<size_t>(snprintf(hex + hp, sizeof(hex) - hp, "%02X ", bytes[i]));
                    asc[ap++] = (bytes[i] >= 0x20 && bytes[i] < 0x7F) ? static_cast<char>(bytes[i]) : '.';
                } else {
                    hp += static_cast<size_t>(snprintf(hex + hp, sizeof(hex) - hp, "   "));
                }
            }
            asc[ap] = '\0';
            Log("  0x%08X: %s|%s|\n", static_cast<unsigned int>(addr), hex, asc);
        }
        offset = static_cast<size_t>(chunkEnd - start);
    }
}

// Dumps every intermediate hop of a chain, not just the final address --
// the first pass only dumped the shared ancestor and each chain's final
// leaf struct, but both chains pass through several unnamed intermediate
// objects in between that were never inspected. `seen` dedupes across both
// chains in one capture (hops[0]/hops[1]/the ancestor are identical
// addresses for both the position and mango chains), so each distinct
// object is only dumped once per capture even though DumpAll() walks both
// chains.
static void DumpHopsIfNew(std::vector<uintptr_t>& seen, const char* chainLabel,
                           const std::vector<uintptr_t>& hops, size_t dumpLen) {
    for (size_t i = 0; i < hops.size(); ++i) {
        uintptr_t addr = hops[i];
        if (!IsPlausiblePointer(addr)) continue;
        bool alreadyDumped = false;
        for (uintptr_t s : seen) {
            if (s == addr) {
                alreadyDumped = true;
                break;
            }
        }
        if (alreadyDumped) continue;
        seen.push_back(addr);

        char label[64];
        snprintf(label, sizeof(label), "%s_hop%zu", chainLabel, i);
        SafeDumpRegion(addr, dumpLen, label);
    }
}

// Same offsets mad2playercoords/mad2gameeffectsmod already use -- see file
// header. Kept duplicated (not shared) since this mod is temporary.
static const uintptr_t kPlayerPosOffsets[] = {0x1C, 0x20, 0x148, 0x8, 0x160, 0x54};
static const uintptr_t kMangoOffsets[] = {0x1C, 0x20, 0x104, 0x9D8, 0x98, 0x44};
static const uintptr_t kCheatStructOffsets[] = {0x38, 0x1C, 0x14, 0x240, 0xA0, 0x164};

// Verbatim copy of mad2cheatapply.cpp's kCheats table (name/offset only --
// see that file for descriptions). mad2cheatapply itself only ever writes
// these while IsOnTitleScreen() is true (a design choice ported from
// ../mad2mod, not a technical requirement) -- climbprobe's own captures
// already showed this struct resolves to the exact same absolute address
// in every level, every save, every session, so there's a live question of
// whether it's actually READ every frame from this same address (in which
// case cheats should work identically applied mid-level) or only read once
// at load into some separate per-level copy (in which case applying it
// mid-level would do nothing). F5 tests this directly by force-applying
// every known cheat's "on" value (2, matching mad2cheatapply's own
// convention) regardless of what screen/level is active.
struct CheatOffset {
    const char* name;
    int32_t offset;
};
static const CheatOffset kCheats[] = {
    {"infiniteSprint", -0x08},   {"superMangos", -0x60},     {"headOfTheGame", -0x34},
    {"penguinProjectile", 0x24}, {"unlockLevels", 0x50},     {"buddyPointer", 0xA8},
    {"debugMode", 0xD4},         {"fastMango", 0x100},       {"giantFrogs", 0x12C},
    {"superButtBounce", 0x158},  {"pepperAtWill", 0x184},    {"swimBackwards", 0x1B0},
    {"invulnAlex", 0x1DC},       {"invulnGloria", 0x208},    {"invulnMarty", 0x234},
    {"invisNewYork", 0x260},     {"golfHole", 0xEEC},        {"ballStop", 0xFC8},
};

// Body of the original DumpAll, factored out (no start/end capture-log
// wrapper of its own) so DumpGlobalData can bundle it into the same capture
// block as its own AWL/AlchemyCommonLib dumps instead of producing a
// separate, unaligned capture that diff_climbprobe.py would have to pair up
// by hand.
static void DumpKnownChains() {
    HMODULE actorInfoLib = GetModuleHandleA("ActorInfoLib.dll");
    if (!actorInfoLib) {
        Log("ActorInfoLib.dll not loaded -- skipping player-object dumps\n");
    } else {
        uintptr_t base = reinterpret_cast<uintptr_t>(actorInfoLib) + 0x0001A030;

        auto posHops = FollowChainVerbose(base, kPlayerPosOffsets, 6);
        LogHops("pos", posHops);
        if (posHops.size() == 7 && IsPlausiblePointer(posHops[2])) {
            // Shared "player ancestor" object -- both the position and
            // mango chains hang their own sub-object pointer off this one
            // (at +0x148 and +0x104 respectively). Dumping it wide is the
            // main hope of spotting an "abilities" field directly.
            SafeDumpRegion(posHops[2], 0x400, "PlayerAncestor(viaPosChain)");
        }
        if (posHops.size() == 7 && IsPlausiblePointer(posHops[6])) {
            // position floats live at +0xD0 off this -- dump some space
            // before/after in case an ability flag is a sibling field.
            uintptr_t structAddr = posHops[6];
            uintptr_t dumpStart = (structAddr > 0x40) ? (structAddr - 0x40) : structAddr;
            SafeDumpRegion(dumpStart, 0x300, "PositionHolder");
        }

        auto mangoHops = FollowChainVerbose(base, kMangoOffsets, 6);
        LogHops("mango", mangoHops);
        if (mangoHops.size() == 7 && IsPlausiblePointer(mangoHops[2])) {
            SafeDumpRegion(mangoHops[2], 0x400, "PlayerAncestor(viaMangoChain)");
        }
        if (mangoHops.size() == 7 && IsPlausiblePointer(mangoHops[6])) {
            // mango count lives at +0x114 off this.
            uintptr_t structAddr = mangoHops[6];
            uintptr_t dumpStart = (structAddr > 0x40) ? (structAddr - 0x40) : structAddr;
            SafeDumpRegion(dumpStart, 0x300, "MangoHolder");
        }

        // Round 1 only dumped the shared ancestor (hop[2]) and each chain's
        // FINAL leaf struct (hop[6]) -- neither chain's intermediate hops
        // (hop[0]/[1] before the ancestor, hop[3..5] between the ancestor
        // and the leaf) were ever inspected, and round 1's candidate turned
        // out to be heap noise once cross-checked against a second OFF
        // capture. Dump every intermediate object too, deduped so the
        // shared early hops aren't logged twice. "pos" is processed first
        // so hop[0]/hop[1]/hop[2] are consistently labeled "pos_hopN"
        // across every capture (mango's identical addresses at those
        // indices get skipped as already-seen) -- this matters for
        // diff_climbprobe.py's --groups mode, which aligns regions by name.
        std::vector<uintptr_t> seen;
        DumpHopsIfNew(seen, "pos", posHops, 0x200);
        DumpHopsIfNew(seen, "mango", mangoHops, 0x200);
    }

    HMODULE alchemyCommon = GetModuleHandleA("AlchemyCommonLib.dll");
    if (!alchemyCommon) {
        Log("AlchemyCommonLib.dll not loaded -- skipping cheat-struct dump\n");
    } else {
        uintptr_t base2 = reinterpret_cast<uintptr_t>(alchemyCommon) + 0x000535DC;
        auto cheatHops = FollowChainVerbose(
            base2, kCheatStructOffsets, sizeof(kCheatStructOffsets) / sizeof(kCheatStructOffsets[0]));
        LogHops("cheat", cheatHops);
        if (cheatHops.size() == 7 && IsPlausiblePointer(cheatHops[6])) {
            // Known cheats span [-0x60, +0xFC8] off this address (see
            // mad2cheatapply/src/cheatapply.cpp's kCheats table) -- dump a
            // superset of that range in case an ability flag sits in the
            // same struct at an offset not yet mapped to a named cheat.
            uintptr_t structAddr = cheatHops[6];
            uintptr_t dumpStart = (structAddr > 0x100) ? (structAddr - 0x100) : structAddr;
            SafeDumpRegion(dumpStart, 0x1200, "CheatStruct");
        }
    }
}

static void DumpAll() {
    std::string level = "None";
    Mad2LevelDetect_UpdateCurrentLevel(level);
    Log("==== capture start, level='%s' ====\n", level.c_str());
    DumpKnownChains();
    Log("==== capture end ====\n");
}

// Round 1 of the minigame-mode investigation: dumped the *entire* .data
// section of AWL.dll and AlchemyCommonLib.dll raw (both small enough --
// ~14KB and ~144KB -- to capture in full). Result: byte-identical between a
// title-screen capture and an active-minigame-level capture, across both
// whole regions. That rules out a plain global bool living directly in
// either module's .data -- confirms the same shape climb/monkeys turned out
// to have: real state lives on the heap, reached through a pointer that
// itself never changes (see this file's header, and the already-known
// cheat-struct pointer at AlchemyCommonLib.dll+0x535DC, which is exactly
// that shape -- "9 captures across 2 saves/4 levels ... the literal same
// absolute address every time").
//
// Round 2 (this function): treats every 4-byte-aligned slot in those same
// two .data regions as a candidate pointer, and for each one that (a) looks
// like a plausible pointer value and (b) actually resolves via VirtualQuery
// to a currently-committed, MEM_PRIVATE, read-write page (i.e. looks like a
// live heap allocation, not a pointer into some DLL's own .text/.rdata or
// into the stack) -- dumps a small window (0x40 bytes) of *what it points
// to*, labeled by the pointer's own stable .data offset (not the heap
// target address, which isn't stable across sessions) so
// diff_climbprobe.py's by-name region alignment still works across
// captures. This is a strict superset of round 1 -- also re-dumps the raw
// .data bytes themselves and folds in DumpKnownChains()'s existing
// chain-anchored regions -- so one capture (bound to 'M') now covers
// everything in a single aligned block instead of needing separate keypresses.
static void DumpDataPointerTargets(const char* moduleName, uintptr_t dataOffset, size_t dataLength,
                                    const char* label) {
    HMODULE mod = GetModuleHandleA(moduleName);
    if (!mod) {
        Log("%s not loaded -- skipping\n", moduleName);
        return;
    }
    uintptr_t base = reinterpret_cast<uintptr_t>(mod) + dataOffset;
    SafeDumpRegion(base, dataLength, label);

    size_t slotCount = dataLength / 4;
    size_t candidateCount = 0;
    for (size_t i = 0; i < slotCount; ++i) {
        uintptr_t slotAddr = base + i * 4;
        uint32_t val = *reinterpret_cast<const uint32_t*>(slotAddr);
        if (!IsPlausiblePointer(val)) continue;

        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(val)), &mbi, sizeof(mbi)) != sizeof(mbi)) {
            continue;
        }
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE) continue;
        if (mbi.Protect != PAGE_READWRITE && mbi.Protect != PAGE_EXECUTE_READWRITE) continue;

        char ptrLabel[80];
        snprintf(ptrLabel, sizeof(ptrLabel), "%s+0x%04zX_target", label, i * 4);
        SafeDumpRegion(val, 0x40, ptrLabel);
        ++candidateCount;
    }
    Log("%s: %zu candidate heap pointer target(s) dumped\n", label, candidateCount);
}

static void DumpGlobalData() {
    std::string level = "None";
    Mad2LevelDetect_UpdateCurrentLevel(level);
    Log("==== capture start, level='%s' ====\n", level.c_str());

    DumpKnownChains();
    DumpDataPointerTargets("AWL.dll", 0x40000, 0x387C, "AWL_data");
    DumpDataPointerTargets("AlchemyCommonLib.dll", 0x31000, 0x2340F, "AlchemyCommonLib_data");

    Log("==== capture end ====\n");
}

// Mad2.exe on disk is SecuROM-wrapped -- its real code/data sections
// (renamed by the wrapper's own linker to avoid colliding with the outer
// stub's own .text/.rdata/.data: Stext/Sitext/Srdata/Sdata/Sidata, per
// `objdump -h`) are encrypted at rest, so Ghidra's static import only finds
// a handful of functions in them. None of that encryption survives into a
// *running* process, though -- by the time this DLL is loaded, the SecuROM
// stub has already decrypted everything into memory on its way to the real
// OEP. Confirmed via `objdump -h`: every section's file offset is exactly
// (VMA - 0x400000), i.e. flat/unrelocated, so a live dump of each section's
// bytes can be written straight back into the matching byte range of a copy
// of the on-disk .exe, producing a file Ghidra can actually analyze
// properly (real function prologues instead of encrypted noise). Written as
// raw binary files (not through the shared hex-text logger -- these are
// several MB each, far too big for that format) to
// <exeDir>\mad2climbprobe_dump_<section>.bin. Bound to 'U'. Safe to press
// at any point in-game (even the title screen) since this is static code,
// not a live game-state snapshot -- unlike every other capture in this
// file, timing doesn't matter here.
struct SectionDumpSpec {
    const char* name;
    uintptr_t vma;
    size_t size;
};
static const SectionDumpSpec kMad2ExeSections[] = {
    {"Stext", 0x00413000, 0x006B3000},
    {"Sitext", 0x00AC6000, 0x00007000},
    {"Srdata", 0x00ACD000, 0x0005C000},
    {"Sdata", 0x00B29000, 0x002FE000},
    {"Sidata", 0x00E27000, 0x00006000},
};

static void DumpUnpackedSections() {
    HMODULE exeBase = GetModuleHandleA(nullptr);
    uintptr_t runtimeBase = reinterpret_cast<uintptr_t>(exeBase);
    if (runtimeBase != 0x00400000) {
        Log("DumpUnpackedSections: exe loaded at 0x%08X, not the expected 0x00400000 -- "
            "dumping anyway using runtimeBase + (vma - 0x400000), but double-check the result\n",
            static_cast<unsigned int>(runtimeBase));
    }

    char exeDir[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    char* slash = strrchr(exeDir, '\\');
    if (slash) *(slash + 1) = '\0';

    for (const auto& sec : kMad2ExeSections) {
        uintptr_t liveAddr = runtimeBase + (sec.vma - 0x00400000);
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%smad2climbprobe_dump_%s.bin", exeDir, sec.name);

        FILE* f = fopen(path, "wb");
        if (!f) {
            Log("DumpUnpackedSections: could not open %s for write\n", path);
            continue;
        }
        // Written in page-sized chunks, stopping (not crashing) at the
        // first unreadable page, same defensive shape as SafeDumpRegion --
        // a section that's smaller in memory than its on-disk SizeOfRawData
        // (common for a .bss-like tail) shouldn't take the whole dump down.
        size_t written = 0;
        while (written < sec.size) {
            uintptr_t cur = liveAddr + written;
            uintptr_t pageAddr = cur & ~static_cast<uintptr_t>(0xFFF);
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(reinterpret_cast<LPCVOID>(pageAddr), &mbi, sizeof(mbi)) != sizeof(mbi) ||
                mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) {
                Log("DumpUnpackedSections: %s unreadable at +0x%zX, stopping early (%zu/%zu bytes written)\n",
                    sec.name, written, written, sec.size);
                break;
            }
            uintptr_t pageEnd = pageAddr + 0x1000;
            size_t chunk = static_cast<size_t>((liveAddr + sec.size < pageEnd) ? (sec.size - written)
                                                                                : (pageEnd - cur));
            fwrite(reinterpret_cast<const void*>(cur), 1, chunk, f);
            written += chunk;
        }
        fclose(f);
        Log("DumpUnpackedSections: wrote %s (%zu/%zu bytes) from live addr 0x%08X\n", path, written, sec.size,
            static_cast<unsigned int>(liveAddr));
    }
}

// Resolves the same cheat-struct address mad2cheatapply.cpp already uses
// (AlchemyCommonLib.dll+0x535DC, offsets {0x38,0x1C,0x14,0x240,0xA0,0x164}).
// Confirmed via 9 captures across 2 saves/4 levels this session to resolve
// to the literal same absolute address every time -- a persistent global,
// not reallocated per level/session.
static uintptr_t ResolveCheatStructAddr() {
    HMODULE alchemyCommon = GetModuleHandleA("AlchemyCommonLib.dll");
    if (!alchemyCommon) return 0;
    uintptr_t base2 = reinterpret_cast<uintptr_t>(alchemyCommon) + 0x000535DC;
    auto hops =
        FollowChainVerbose(base2, kCheatStructOffsets, sizeof(kCheatStructOffsets) / sizeof(kCheatStructOffsets[0]));
    if (hops.size() != 7 || !IsPlausiblePointer(hops[6])) return 0;
    return hops[6];
}

// F3: scans all committed, private, read/write process memory for a
// candidate base address where every known climb/monkeys save-file offset
// (see saveeditor.go's ReadClimbAbility/ReadMonkeyCollectible for the
// derivation) simultaneously holds its expected UNLOCKED value. With 13
// independent bytes across 9 distinct relative offsets all required to
// match at once, a false positive here is essentially impossible -- any
// hit is almost certainly the live object the save serializer reads from
// (the WriteFile buffer itself was confirmed transient heap, freshly
// built per save, not this). Mirrors gameeffects.cpp's ObjectShuffle scan
// technique (VirtualQuery over the whole address space, MEM_PRIVATE +
// committed + read/write regions only).
struct FieldCheck {
    uintptr_t offset;
    uint32_t value;
    int width;  // 1 or 4 bytes
};
static const FieldCheck kSaveStateFieldChecks[] = {
    {0x70, 1, 1},        // climb ability
    {0x18, 0, 4},        // monkeys uint32 (0 = unlocked)
    {0x30, 1, 1}, {0x48, 1, 1}, {0x58, 1, 1}, {0x68, 2, 1},
    {0x80, 1, 1}, {0xA8, 1, 1}, {0x3A0, 1, 1}, {0x3A4, 1, 1},
};

static bool MatchesAtBase(uintptr_t base, uintptr_t regionEnd) {
    for (const auto& c : kSaveStateFieldChecks) {
        uintptr_t addr = base + c.offset;
        if (addr + static_cast<uintptr_t>(c.width) > regionEnd) return false;
        uint32_t actual = (c.width == 4) ? *reinterpret_cast<const uint32_t*>(addr)
                                          : *reinterpret_cast<const unsigned char*>(addr);
        if (actual != c.value) return false;
    }
    return true;
}

// Describes an address: which module (if any) it falls inside, and the
// offset from that module's base. Used both for the WriteFile buffer
// (confirmed transient heap, not this) and for whatever ScanForPointers-
// ToSaveStateObject (below) finds pointing at the live save-state object.
static std::string DescribeAddress(const void* addr) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(addr), &mod)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "0x%p (no containing module -- heap/stack)", addr);
        return buf;
    }
    char modName[MAX_PATH] = {0};
    GetModuleFileNameA(mod, modName, sizeof(modName));
    const char* base = strrchr(modName, '\\');
    base = base ? base + 1 : modName;
    uintptr_t offset = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
    char buf[128];
    snprintf(buf, sizeof(buf), "0x%p (%s+0x%zX)", addr, base, offset);
    return buf;
}

// [start, end) of one committed, readable region.
using AddrRange = std::pair<uintptr_t, uintptr_t>;

// Enumerates every committed, MEM_PRIVATE, read-write region ONCE and
// returns [start,end) ranges in increasing address order (VirtualQuery
// naturally walks the address space that way, so no sort needed). A first
// version of this scan called VirtualQuery again for every individual
// candidate pointer instead of doing this once -- since IsPlausiblePointer's
// bound (0x10000 < p < 0x7FFFFFFF) is loose enough that nearly every 4-byte
// word in gigabytes of memory satisfies it, that meant a syscall for nearly
// every word in the whole process's writable memory, which along with the
// unchecked-boundary bug below is what actually crashed the game. Building
// this table once and binary-searching it in memory (FindContainingRangeEnd)
// is the fix for both: no more per-candidate syscalls, and every read is
// checked against a real range before it happens.
static std::vector<AddrRange> EnumerateReadableRanges() {
    std::vector<AddrRange> ranges;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    const uintptr_t kMaxAddr = 0x7FFF0000;
    const SIZE_T kMaxRegionSize = 600ull * 1024 * 1024;
    while (addr < kMaxAddr) {
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        if (mbi.RegionSize == 0) break;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE) &&
            mbi.RegionSize < kMaxRegionSize) {
            uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            ranges.emplace_back(start, start + mbi.RegionSize);
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return ranges;
}

// Binary search for the range containing [addr, addr+minLen). Returns the
// range's end address (so the caller can clamp a variable-length read to
// it) or 0 if addr isn't inside any recorded range, or the range is too
// short for minLen.
static uintptr_t FindContainingRangeEnd(const std::vector<AddrRange>& ranges, uintptr_t addr, size_t minLen) {
    size_t lo = 0, hi = ranges.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ranges[mid].second <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo >= ranges.size()) return 0;
    const AddrRange& r = ranges[lo];
    if (addr < r.first || addr + minLen > r.second) return 0;
    return r.second;
}

// Checks whether addr looks like a plausible printable, null-terminated
// ASCII name (2-63 chars, mostly letters/digits/spaces/underscores/hyphens)
// -- used to validate a candidate igNamedObject::getName() string pointer,
// not just that it's in-bounds. The read never goes past the end of addr's
// own already-validated range (clamped via FindContainingRangeEnd), so this
// can't cross into unmapped memory the way the first version of this scan
// did.
static bool LooksLikeName(const std::vector<AddrRange>& ranges, uintptr_t addr, size_t maxLen) {
    uintptr_t rangeEnd = FindContainingRangeEnd(ranges, addr, 1);
    if (rangeEnd == 0) return false;
    size_t avail = static_cast<size_t>(rangeEnd - addr);
    size_t limit = (avail < maxLen) ? avail : maxLen;
    const char* s = reinterpret_cast<const char*>(addr);
    size_t len = 0;
    while (len < limit) {
        char c = s[len];
        if (c == '\0') break;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
                  c == '_' || c == '-';
        if (!ok) return false;
        ++len;
    }
    return len >= 2 && len < limit;
}

// Minigame-mode investigation, live-object-naming approach: per
// docs/CLASS_SCHEMA.md's "Naming investigation" section, mad2shoptrace
// already confirmed that *live, runtime-spawned* UI objects' `+8` field
// (the igNamedObject convention -- `*(this+8)` as a plain char*, confirmed
// real on PC) resolves to genuine human-readable names ("Menu Manager",
// "Generic_Container", etc.), unlike the same field read off static file
// objects (small useless sequential ordinals there). Rather than
// vtable-patching a specific opcode class the way mad2shoptrace does, this
// scans all committed private RW memory directly: for every 4-byte-aligned
// candidate object address, treat +8 as a candidate name pointer, and if it
// resolves to a plausible printable name *containing "minigame"*
// (case-insensitive -- narrow enough that a false positive from pure
// chance is very unlikely), log the object address, its vtable pointer
// (the first 4 bytes of any C++ object with virtual functions -- most
// igObject-derived classes have one), and DescribeAddress() of both so a
// vtable landing inside a known DLL's .rdata identifies the real
// implementing class directly. Bound to 'J' ('N' collides with
// mad2nesmod's N64Sm64Challenge TestTriggerKey default -- confirmed the
// hard way, live, when pressing 'N' launched SM64 instead of running this
// scan). Timing matters here, unlike
// 'U' -- press while the (legitimate or wrongly-spawned) minigame UI is
// actually on screen, the same way ScanForSaveStateObject (F3) needs the
// target object to actually exist.
static void ScanForNamedObjects() {
    Log("ScanForNamedObjects: enumerating readable ranges...\n");
    std::vector<AddrRange> ranges = EnumerateReadableRanges();
    Log("ScanForNamedObjects: %zu range(s) -- scanning for a live object whose name contains "
        "\"minigame\"...\n",
        ranges.size());

    std::vector<uintptr_t> hits;
    for (const AddrRange& range : ranges) {
        uintptr_t start = range.first;
        uintptr_t end = range.second;
        if (end < start + 12) continue;                // need objAddr+8..+11 to exist
        uintptr_t lastObj = end - 12;                   // last objAddr whose +8..+11 is still in-range
        for (uintptr_t objAddr = start; objAddr <= lastObj; objAddr += 4) {
            uint32_t namePtr = *reinterpret_cast<const uint32_t*>(objAddr + 8);
            if (!IsPlausiblePointer(namePtr)) continue;
            if (!LooksLikeName(ranges, namePtr, 64)) continue;
            const char* name = reinterpret_cast<const char*>(static_cast<uintptr_t>(namePtr));
            bool hasMinigame = false;
            for (const char* p = name; *p; ++p) {
                if ((p[0] == 'm' || p[0] == 'M') && _strnicmp(p, "minigame", 8) == 0) {
                    hasMinigame = true;
                    break;
                }
            }
            if (!hasMinigame) continue;
            hits.push_back(objAddr);
            uint32_t vtable = *reinterpret_cast<const uint32_t*>(objAddr);  // objAddr is in-range, safe
            Log("  HIT obj=%s name=\"%s\" vtable=%s\n", DescribeAddress(reinterpret_cast<void*>(objAddr)).c_str(),
                name, DescribeAddress(reinterpret_cast<void*>(static_cast<uintptr_t>(vtable))).c_str());
            // "Minigame Launch Mode" is the literal candidate flag name --
            // raw-dump its object wide enough to cover OpCreateVariable's
            // base fields (52 bytes per the live reflection schema) plus
            // whatever a derived value-holding subclass appends after that,
            // so the actual stored value (not just its name) is visible.
            if (_stricmp(name, "Minigame Launch Mode") == 0) {
                SafeDumpRegion(objAddr, 96, "MinigameLaunchModeVar");
            }
        }
    }
    Log("ScanForNamedObjects: %zu hit(s)\n", hits.size());
}

// Forward declaration -- real definition is further down this file
// (originally written for RefineValueScan/F7), reused by
// TickForceMinigameModeOff below to verify a cached address is still safe
// to write to before every poke.
static bool IsAddressSafeToRead(uintptr_t addr);

// Confirmed via two ScanForNamedObjects captures (title screen vs. an
// active minigame): every byte of the "Minigame Launch Mode" object is
// identical between the two states except a 4-byte field at +0x24
// (0=title/story, 1=active minigame) -- see this file's own capture log
// history for the exact before/after bytes. This finds that same object
// (by exact name, not substring) and returns its address, or 0 if it isn't
// currently spawned.
static uintptr_t FindMinigameLaunchModeObject() {
    std::vector<AddrRange> ranges = EnumerateReadableRanges();
    for (const AddrRange& range : ranges) {
        uintptr_t start = range.first;
        uintptr_t end = range.second;
        if (end < start + 12) continue;
        uintptr_t lastObj = end - 12;
        for (uintptr_t objAddr = start; objAddr <= lastObj; objAddr += 4) {
            uint32_t namePtr = *reinterpret_cast<const uint32_t*>(objAddr + 8);
            if (!IsPlausiblePointer(namePtr)) continue;
            if (!LooksLikeName(ranges, namePtr, 64)) continue;
            const char* name = reinterpret_cast<const char*>(static_cast<uintptr_t>(namePtr));
            if (_stricmp(name, "Minigame Launch Mode") == 0) return objAddr;
        }
    }
    return 0;
}

static const uintptr_t kMinigameLaunchModeValueOffset = 0x24;

// Experiment (per user request): force "Minigame Launch Mode" to 0 at all
// times, regardless of what the game itself thinks it should be, to
// observe what happens loading arbitrary levels with it permanently off --
// separate from actually fixing the root cause (whatever spawns the wrong
// UI in the first place). 'K' toggles this on/off. When first enabled, the
// object is located via FindMinigameLaunchModeObject() (a ~0.5s scan) and
// its address cached; from then on the poll loop just writes a 0 there
// every ~50ms tick, cheap enough to do continuously for the rest of the
// session. If the object ever stops being at the cached address (freed/
// reallocated -- observed stable across several level loads this session,
// but not guaranteed forever), the write target is verified live and the
// cache is dropped/re-scanned rather than blindly writing to stale memory.
static bool g_ForceMinigameModeOff = false;
static uintptr_t g_MinigameModeObjAddr = 0;
static ULONGLONG g_NextMinigameModeRescanTick = 0;

static void ToggleForceMinigameModeOff() {
    g_ForceMinigameModeOff = !g_ForceMinigameModeOff;
    if (g_ForceMinigameModeOff) {
        Log("ForceMinigameModeOff: ENABLED -- locating \"Minigame Launch Mode\" object...\n");
        g_MinigameModeObjAddr = FindMinigameLaunchModeObject();
        g_NextMinigameModeRescanTick = 0;
        if (g_MinigameModeObjAddr == 0) {
            Log("ForceMinigameModeOff: not found yet (not spawned?) -- will keep retrying periodically\n");
        } else {
            Log("ForceMinigameModeOff: found at %s -- forcing +0x%zX to 0 every tick from now on\n",
                DescribeAddress(reinterpret_cast<void*>(g_MinigameModeObjAddr)).c_str(),
                kMinigameLaunchModeValueOffset);
        }
    } else {
        Log("ForceMinigameModeOff: DISABLED\n");
        g_MinigameModeObjAddr = 0;
    }
}

// Called every poll-thread tick (~50ms) while g_ForceMinigameModeOff is on.
static void TickForceMinigameModeOff() {
    if (g_MinigameModeObjAddr != 0) {
        uintptr_t valueAddr = g_MinigameModeObjAddr + kMinigameLaunchModeValueOffset;
        if (IsAddressSafeToRead(valueAddr)) {
            *reinterpret_cast<uint32_t*>(valueAddr) = 0;
            return;
        }
        // Cached object no longer valid -- drop it and fall through to
        // re-scan below (rate-limited, not every tick).
        g_MinigameModeObjAddr = 0;
    }
    ULONGLONG now = GetTickCount64();
    if (now < g_NextMinigameModeRescanTick) return;
    g_NextMinigameModeRescanTick = now + 2000;  // retry at most every 2s, the scan itself isn't free
    g_MinigameModeObjAddr = FindMinigameLaunchModeObject();
    if (g_MinigameModeObjAddr != 0) {
        Log("ForceMinigameModeOff: (re)found at %s\n",
            DescribeAddress(reinterpret_cast<void*>(g_MinigameModeObjAddr)).c_str());
    }
}

// Set by ScanForSaveStateObject when exactly one candidate is found;
// consumed by ToggleClimbLive/ToggleMonkeysLive (F1/F2) via
// EnsureSaveStateObjectFound, further down.
static uintptr_t g_LastFoundSaveStateAddr = 0;

static void ScanForSaveStateObject() {
    Log("ScanForSaveStateObject: scanning committed private RW memory for climb/monkeys field pattern...\n");
    std::vector<uintptr_t> hits;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    const uintptr_t kMaxAddr = 0x7FFF0000;
    const SIZE_T kMaxRegionSize = 600ull * 1024 * 1024;
    // Cheapest check first (climb byte, offset 0x70) lets most candidate
    // addresses fail on one comparison instead of all thirteen.
    while (addr < kMaxAddr) {
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        if (mbi.RegionSize == 0) break;

        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE) &&
            mbi.RegionSize < kMaxRegionSize) {
            uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            uintptr_t regionEnd = regionStart + mbi.RegionSize;
            if (regionEnd > regionStart + 0x3A8) {
                // 4-byte stride, not every byte -- a real C++ object's base
                // is virtually always at least 4-byte aligned (heap
                // allocators typically align to 8/16 anyway), and this
                // matches gameeffects.cpp's own ObjectShuffle scan
                // precedent for the same performance reason.
                for (uintptr_t base = regionStart; base < regionEnd - 0x3A8; base += 4) {
                    if (*reinterpret_cast<const unsigned char*>(base + 0x70) != 1) continue;
                    if (MatchesAtBase(base, regionEnd)) hits.push_back(base);
                }
            }
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    Log("ScanForSaveStateObject: %zu candidate address(es) found\n", hits.size());
    for (uintptr_t h : hits) {
        Log("  candidate base = 0x%08X\n", static_cast<unsigned int>(h));
    }
    // Originally required exactly one hit before caching -- too strict in
    // practice: a second, spurious hit can appear alongside the real one
    // (observed live: the real object plus one coincidental match), and
    // requiring uniqueness just silently blocked F1/F2 instead of proceeding
    // with the (still overwhelmingly likely correct) first hit. hits[0] is
    // not guaranteed to be the "real" one if there's ever a genuine tie, but
    // every session so far has shown the real object landing first.
    if (!hits.empty()) {
        g_LastFoundSaveStateAddr = hits[0];
        if (hits.size() > 1) {
            Log("ScanForSaveStateObject: %zu hits (ambiguous) -- using the first, 0x%08X\n", hits.size(),
                static_cast<unsigned int>(hits[0]));
        } else {
            Log("ScanForSaveStateObject: stored as g_LastFoundSaveStateAddr for F1/F2\n");
        }
    }
}

// F5: toggles every known cheat between "on" (2) and "off" (0) regardless
// of current screen/level -- see kCheats' comment for why. Confirmed live
// (this session) that force-applying while already in a level actually
// works, not just from the title screen. g_CheatsOn tracks this mod's own
// idea of the current toggle state (not read back from memory) since
// that's simpler and this is a single-hotkey debug tool, not a real UI.
static bool g_CheatsOn = false;

static void ToggleAllCheats() {
    std::string level = "None";
    Mad2LevelDetect_UpdateCurrentLevel(level);

    uintptr_t structAddr = ResolveCheatStructAddr();
    if (!structAddr) {
        Log("ToggleAllCheats: cheat-struct chain unresolved (level='%s')\n", level.c_str());
        return;
    }
    g_CheatsOn = !g_CheatsOn;
    int32_t value = g_CheatsOn ? 2 : 0;
    Log("ToggleAllCheats: structAddr=0x%08X level='%s' -> %s\n", static_cast<unsigned int>(structAddr),
        level.c_str(), g_CheatsOn ? "ON" : "OFF");
    for (const auto& c : kCheats) {
        int32_t* addr = reinterpret_cast<int32_t*>(structAddr + c.offset);
        int32_t before = *addr;
        *addr = value;
        Log("  %s (offset %+d): before=%d after=%d\n", c.name, c.offset, before, *addr);
    }
}

// Live climb/monkeys toggles -- confirmed offsets, same ones
// mad2launcher/saveeditor.go's ReadClimbAbility/WriteClimbAbility and
// ReadMonkeyCollectible/WriteMonkeyCollectible use against the save file.
// The live object ScanForSaveStateObject (F3) finds turned out to share
// the exact same relative field layout as the save file -- unsurprising in
// hindsight, since the save is very likely a raw serialization of this
// object's own memory region -- so these offsets are reused verbatim, not
// rederived.
//
// EnsureSaveStateObjectFound() only re-scans if no address is cached yet.
// This matters, not just as an optimization: ScanForSaveStateObject's
// pattern match requires climb==1 AND the monkeys bundle==unlocked as part
// of its 13-field signature (see kSaveStateFieldChecks), so a fresh scan
// after toggling either OFF would fail to relocate the object at all.
// Caching the address once and reusing it for every subsequent toggle
// sidesteps that entirely.
static bool EnsureSaveStateObjectFound() {
    if (g_LastFoundSaveStateAddr) return true;
    ScanForSaveStateObject();
    return g_LastFoundSaveStateAddr != 0;
}

static const uintptr_t kLiveClimbOffset = 0x70;

// F1: reads the current live value and flips it -- confirms (or refutes)
// that ScanForSaveStateObject's found object is genuinely the live game
// state, not just a coincidentally-matching heap pattern.
static void ToggleClimbLive() {
    if (!EnsureSaveStateObjectFound()) {
        Log("ToggleClimbLive: save-state object not found -- press F3 first (needs climb+monkeys both already "
            "unlocked to locate it)\n");
        return;
    }
    uintptr_t addr = g_LastFoundSaveStateAddr + kLiveClimbOffset;
    unsigned char current = *reinterpret_cast<unsigned char*>(addr);
    unsigned char next = current ? 0 : 1;
    *reinterpret_cast<unsigned char*>(addr) = next;
    Log("ToggleClimbLive: addr=0x%08X %u -> %u (now %s)\n", static_cast<unsigned int>(addr), current, next,
        next ? "ON" : "OFF");
}

static const uintptr_t kLiveMonkeysUint32Offset = 0x18;
static const std::pair<uintptr_t, unsigned char> kLiveMonkeysByteFields[] = {
    {0x30, 1}, {0x48, 1}, {0x58, 1}, {0x68, 2}, {0x80, 1}, {0xA8, 1}, {0x3A0, 1}, {0x3A4, 1},
};

// F2: same idea as ToggleClimbLive, for the whole monkeys bundle at once
// (see saveeditor.go's WriteMonkeyCollectible for why the bundle is always
// written together rather than field-by-field).
static void ToggleMonkeysLive() {
    if (!EnsureSaveStateObjectFound()) {
        Log("ToggleMonkeysLive: save-state object not found -- press F3 first (needs climb+monkeys both already "
            "unlocked to locate it)\n");
        return;
    }
    uintptr_t base = g_LastFoundSaveStateAddr;
    uint32_t currentU32 = *reinterpret_cast<uint32_t*>(base + kLiveMonkeysUint32Offset);
    bool unlockedNow = (currentU32 == 0);
    bool makeUnlocked = !unlockedNow;

    *reinterpret_cast<uint32_t*>(base + kLiveMonkeysUint32Offset) = makeUnlocked ? 0x00000000u : 0xFFFFFFFFu;
    for (const auto& field : kLiveMonkeysByteFields) {
        *reinterpret_cast<unsigned char*>(base + field.first) = makeUnlocked ? field.second : 0;
    }
    Log("ToggleMonkeysLive: base=0x%08X %s -> %s\n", static_cast<unsigned int>(base),
        unlockedNow ? "unlocked" : "locked", makeUnlocked ? "unlocked" : "locked");
}

// ---------------------------------------------------------------------
// Generic int32 value scan (classic "Cheat Engine" successive-narrowing
// technique) -- for hunting values with no known offset at all, unlike
// climb/monkeys above (which started from known save-file offsets). Used
// for the global monkey/coin counters: F6 does a first scan (every
// committed private RW int32 equal to the target value), F7 refines the
// existing candidate list down to whichever ones still equal a new target
// value, F8 resets. The target value itself is read from a plain text
// file in the exe dir rather than prompted interactively (this mod has no
// UI) -- written externally (by whoever is driving this session) before
// each hotkey press.
// ---------------------------------------------------------------------

static std::vector<uintptr_t> g_ScanCandidates;

// <exe dir>\mad2climbprobe_scan_target.txt -- a single decimal integer,
// nothing else. Recomputed on every call rather than cached, same
// reasoning as mad2sharedlog.h's own Mad2Log_GetPath.
static bool ReadScanTargetValue(int32_t& outValue) {
    char exeDir[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    char* slash = strrchr(exeDir, '\\');
    if (slash) {
        *(slash + 1) = '\0';
    } else {
        exeDir[0] = '\0';
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%smad2climbprobe_scan_target.txt", exeDir);

    FILE* f = fopen(path, "r");
    if (!f) {
        Log("ReadScanTargetValue: could not open %s\n", path);
        return false;
    }
    char buf[64] = {0};
    bool ok = fgets(buf, sizeof(buf), f) != nullptr;
    fclose(f);
    if (!ok) {
        Log("ReadScanTargetValue: %s is empty\n", path);
        return false;
    }
    outValue = atoi(buf);
    return true;
}

// F6: fresh scan -- every committed, private, read/write int32 (4-byte
// aligned) equal to the current target value becomes the new candidate set.
static void FirstValueScan() {
    int32_t target;
    if (!ReadScanTargetValue(target)) return;
    Log("FirstValueScan: target=%d\n", target);

    g_ScanCandidates.clear();
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    const uintptr_t kMaxAddr = 0x7FFF0000;
    const SIZE_T kMaxRegionSize = 600ull * 1024 * 1024;
    while (addr < kMaxAddr) {
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        if (mbi.RegionSize == 0) break;

        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE) &&
            mbi.RegionSize < kMaxRegionSize) {
            uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            SIZE_T wordCount = mbi.RegionSize / 4;
            const int32_t* words = reinterpret_cast<const int32_t*>(regionStart);
            for (SIZE_T i = 0; i < wordCount; ++i) {
                if (words[i] == target) g_ScanCandidates.push_back(regionStart + i * 4);
            }
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    Log("FirstValueScan: %zu candidate(s)\n", g_ScanCandidates.size());
    if (g_ScanCandidates.size() <= 20) {
        for (uintptr_t c : g_ScanCandidates) Log("  0x%08X\n", static_cast<unsigned int>(c));
    }
}

// F7: narrows the existing candidate set down to whichever addresses
// currently hold the new target value. Run this after doing/observing
// whatever changed the real value (collect a coin/monkey, etc.) and
// updating the target file to the new known value.
// Candidates were only ever verified valid at FirstValueScan time -- by
// the time RefineValueScan runs, arbitrary time (and possibly a level
// transition, which frees/reallocates huge swaths of heap) may have
// passed. A candidate whose page is no longer committed/readable is
// silently dropped (it can't still hold the target value if it doesn't
// exist), not dereferenced -- an earlier version of this function skipped
// this check entirely and crashed the game doing exactly that.
static bool IsAddressSafeToRead(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
    return true;
}

static void RefineValueScan() {
    if (g_ScanCandidates.empty()) {
        Log("RefineValueScan: no candidates yet -- press F6 first\n");
        return;
    }
    int32_t target;
    if (!ReadScanTargetValue(target)) return;
    Log("RefineValueScan: target=%d, refining %zu candidate(s)...\n", target, g_ScanCandidates.size());

    std::vector<uintptr_t> kept;
    size_t droppedUnsafe = 0;
    for (uintptr_t addr : g_ScanCandidates) {
        if (!IsAddressSafeToRead(addr)) {
            ++droppedUnsafe;
            continue;
        }
        if (*reinterpret_cast<const int32_t*>(addr) == target) kept.push_back(addr);
    }
    g_ScanCandidates = kept;

    Log("RefineValueScan: %zu candidate(s) remain (%zu dropped as no-longer-valid memory)\n",
        g_ScanCandidates.size(), droppedUnsafe);
    if (g_ScanCandidates.size() <= 20) {
        for (uintptr_t c : g_ScanCandidates) Log("  0x%08X\n", static_cast<unsigned int>(c));
    }
}

// F8: clears the candidate set to start a fresh hunt (e.g. switching from
// hunting the monkey counter to the coin counter).
static void ResetValueScan() {
    Log("ResetValueScan: cleared %zu candidate(s)\n", g_ScanCandidates.size());
    g_ScanCandidates.clear();
}

// ---------------------------------------------------------------------
// Save-file write logging -- hooks CreateFileA/WriteFile/CloseHandle to
// log the exact bytes the game writes to its own save_gameN file, live,
// at the moment it happens. This is a different angle from the earlier
// "diff two finished save files" approach: it catches every write as it
// happens (in case the game writes in multiple stages, or writes more
// often than just the visible "autosave" points), tagged with the current
// level so a write can be correlated to a specific in-game moment (e.g.
// "the write that happens the instant you leave Fix the Plane").
//
// Path matching mirrors mad2saveloader/src/save_redirect.cpp's own
// redirect logic conceptually, but simpler: by the time this mod's own
// CreateFileA hook runs, mad2saveloader (if loaded) has already redirected
// the path to the real on-disk save_gameN location (IAT hooks chain, and
// "mad2climbprobe" sorts before "mad2saveloader" alphabetically, so
// mad2saveloader's hook installs later/outermost and runs first) -- so
// just matching "save_game" in the final path is enough, no need to
// duplicate the documents/activision/... matching mad2saveloader does.
// ---------------------------------------------------------------------

typedef HANDLE(WINAPI* CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL(WINAPI* WriteFile_t)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI* CloseHandle_t)(HANDLE);

static CreateFileA_t RealCreateFileA_SaveLog = nullptr;
static CreateFileW_t RealCreateFileW_SaveLog = nullptr;
static WriteFile_t RealWriteFile_SaveLog = nullptr;
static CloseHandle_t RealCloseHandle_SaveLog = nullptr;

static CRITICAL_SECTION g_SaveLogCs;
static std::vector<HANDLE> g_TrackedSaveHandles;

static bool PathLooksLikeSaveFile(const char* path) {
    if (!path) return false;
    std::string norm(path);
    for (char& c : norm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return norm.find("save_game") != std::string::npos;
}

static bool PathLooksLikeSaveFileW(const wchar_t* path) {
    if (!path) return false;
    std::wstring norm(path);
    for (wchar_t& c : norm) c = static_cast<wchar_t>(std::tolower(static_cast<unsigned char>(c)));
    return norm.find(L"save_game") != std::wstring::npos;
}

static void TrackHandle(HANDLE h) {
    EnterCriticalSection(&g_SaveLogCs);
    g_TrackedSaveHandles.push_back(h);
    LeaveCriticalSection(&g_SaveLogCs);
}

static bool IsTrackedHandle(HANDLE h) {
    EnterCriticalSection(&g_SaveLogCs);
    bool found = false;
    for (HANDLE t : g_TrackedSaveHandles) {
        if (t == h) {
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_SaveLogCs);
    return found;
}

static void UntrackHandle(HANDLE h) {
    EnterCriticalSection(&g_SaveLogCs);
    for (size_t i = 0; i < g_TrackedSaveHandles.size(); ++i) {
        if (g_TrackedSaveHandles[i] == h) {
            g_TrackedSaveHandles.erase(g_TrackedSaveHandles.begin() + static_cast<long>(i));
            break;
        }
    }
    LeaveCriticalSection(&g_SaveLogCs);
}

// Same 16-bytes/line hex+ascii format as SafeDumpRegion, but over a plain
// caller-provided buffer (the WriteFile call's own lpBuffer) instead of
// live process memory -- no VirtualQuery needed, this data is guaranteed
// valid for the call's duration.
static void LogWriteBuffer(const unsigned char* data, DWORD len) {
    for (DWORD i = 0; i < len; i += 16) {
        DWORD n = (len - i < 16) ? (len - i) : 16;
        char hex[64];
        char asc[20];
        size_t hp = 0, ap = 0;
        for (DWORD j = 0; j < 16; ++j) {
            if (j < n) {
                hp += static_cast<size_t>(snprintf(hex + hp, sizeof(hex) - hp, "%02X ", data[i + j]));
                asc[ap++] = (data[i + j] >= 0x20 && data[i + j] < 0x7F) ? static_cast<char>(data[i + j]) : '.';
            } else {
                hp += static_cast<size_t>(snprintf(hex + hp, sizeof(hex) - hp, "   "));
            }
        }
        asc[ap] = '\0';
        Log("  +0x%04lX: %s|%s|\n", i, hex, asc);
    }
}

static HANDLE WINAPI HookedCreateFileA_SaveLog(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                                LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                                                DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
                                                HANDLE hTemplateFile) {
    HANDLE h = RealCreateFileA_SaveLog(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                        dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    if (h != INVALID_HANDLE_VALUE && PathLooksLikeSaveFile(lpFileName)) {
        TrackHandle(h);
        Log("[SaveWriteLog] Tracking handle for %s\n", lpFileName);
    }
    return h;
}

static HANDLE WINAPI HookedCreateFileW_SaveLog(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                                LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                                                DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
                                                HANDLE hTemplateFile) {
    HANDLE h = RealCreateFileW_SaveLog(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                        dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    if (h != INVALID_HANDLE_VALUE && PathLooksLikeSaveFileW(lpFileName)) {
        TrackHandle(h);
        char narrow[MAX_PATH] = {0};
        WideCharToMultiByte(CP_ACP, 0, lpFileName, -1, narrow, sizeof(narrow), nullptr, nullptr);
        Log("[SaveWriteLog] Tracking handle for %s\n", narrow);
    }
    return h;
}

static BOOL WINAPI HookedWriteFile_SaveLog(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                                            LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped) {
    if (IsTrackedHandle(hFile)) {
        LARGE_INTEGER zero{};
        LARGE_INTEGER pos{};
        SetFilePointerEx(hFile, zero, &pos, FILE_CURRENT);  // query only -- distance 0 doesn't move the pointer

        std::string level = "None";
        Mad2LevelDetect_UpdateCurrentLevel(level);
        Log("---- SAVE WRITE handle=%p level='%s' filePos=0x%llX len=%lu buffer=%s ----\n", hFile, level.c_str(),
            static_cast<unsigned long long>(pos.QuadPart), static_cast<unsigned long>(nNumberOfBytesToWrite),
            DescribeAddress(lpBuffer).c_str());
        LogWriteBuffer(reinterpret_cast<const unsigned char*>(lpBuffer), nNumberOfBytesToWrite);
    }
    return RealWriteFile_SaveLog(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
}

static BOOL WINAPI HookedCloseHandle_SaveLog(HANDLE hObject) {
    UntrackHandle(hObject);
    return RealCloseHandle_SaveLog(hObject);
}

// NtCreateFile hook -- same reasoning as mad2levelredirectmod/src/
// levelredirect.cpp's own version: this game's file opens (level archives,
// and evidently the save file too, confirmed this session -- CreateFileA/W
// installed and traced zero opens for a real save write) route through
// ntdll's native NtCreateFile, bypassing kernel32's CreateFileA/CreateFileW
// entirely. ObjectAttributes->ObjectName is a UNICODE_STRING (explicit
// Length, not necessarily NUL-terminated) that may be relative to
// ObjectAttributes->RootDirectory instead of fully qualified -- doesn't
// matter here since we're only substring-matching "save_game", not
// resolving a full path.
typedef NTSTATUS(NTAPI* NtCreateFile_t)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                         ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
static NtCreateFile_t RealNtCreateFile_SaveLog = nullptr;

static std::string UnicodeStringToNarrow(const UNICODE_STRING* us) {
    if (!us || !us->Buffer || us->Length == 0) return "";
    int wlen = us->Length / sizeof(WCHAR);
    char buf[512] = {0};
    int n = WideCharToMultiByte(CP_ACP, 0, us->Buffer, wlen, buf, sizeof(buf) - 1, nullptr, nullptr);
    if (n <= 0) return "";
    return std::string(buf, static_cast<size_t>(n));
}

static NTSTATUS NTAPI HookedNtCreateFile_SaveLog(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                                                  POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
                                                  PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                                                  ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions,
                                                  PVOID EaBuffer, ULONG EaLength) {
    NTSTATUS status = RealNtCreateFile_SaveLog(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                                                AllocationSize, FileAttributes, ShareAccess, CreateDisposition,
                                                CreateOptions, EaBuffer, EaLength);
    if (status >= 0 && FileHandle && ObjectAttributes && ObjectAttributes->ObjectName) {
        std::string path = UnicodeStringToNarrow(ObjectAttributes->ObjectName);
        if (PathLooksLikeSaveFile(path.c_str())) {
            TrackHandle(*FileHandle);
            Log("[SaveWriteLog] Tracking handle for %s (via NtCreateFile)\n", path.c_str());
        }
    }
    return status;
}

static void InstallSaveWriteLogHooks() {
    InitializeCriticalSection(&g_SaveLogCs);

    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    RealCreateFileA_SaveLog =
        reinterpret_cast<CreateFileA_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateFileA")));
    RealCreateFileW_SaveLog =
        reinterpret_cast<CreateFileW_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateFileW")));
    RealWriteFile_SaveLog = reinterpret_cast<WriteFile_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "WriteFile")));
    RealCloseHandle_SaveLog =
        reinterpret_cast<CloseHandle_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "CloseHandle")));

    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    RealNtCreateFile_SaveLog =
        reinterpret_cast<NtCreateFile_t>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateFile")));

    void* prevCreateA = nullptr;
    void* prevCreateW = nullptr;
    void* prevWrite = nullptr;
    void* prevClose = nullptr;
    void* prevNt = nullptr;
    int ca = Mad2HookUtil_PatchIatAllModules("kernel32.dll", "CreateFileA",
                                              reinterpret_cast<void*>(HookedCreateFileA_SaveLog), &prevCreateA);
    int cw = Mad2HookUtil_PatchIatAllModules("kernel32.dll", "CreateFileW",
                                              reinterpret_cast<void*>(HookedCreateFileW_SaveLog), &prevCreateW);
    int w = Mad2HookUtil_PatchIatAllModules("kernel32.dll", "WriteFile",
                                             reinterpret_cast<void*>(HookedWriteFile_SaveLog), &prevWrite);
    int cl = Mad2HookUtil_PatchIatAllModules("kernel32.dll", "CloseHandle",
                                              reinterpret_cast<void*>(HookedCloseHandle_SaveLog), &prevClose);
    int nt = Mad2HookUtil_PatchIatAllModules("ntdll.dll", "NtCreateFile",
                                              reinterpret_cast<void*>(HookedNtCreateFile_SaveLog), &prevNt);
    if (prevCreateA) RealCreateFileA_SaveLog = reinterpret_cast<CreateFileA_t>(prevCreateA);
    if (prevCreateW) RealCreateFileW_SaveLog = reinterpret_cast<CreateFileW_t>(prevCreateW);
    if (prevWrite) RealWriteFile_SaveLog = reinterpret_cast<WriteFile_t>(prevWrite);
    if (prevClose) RealCloseHandle_SaveLog = reinterpret_cast<CloseHandle_t>(prevClose);
    if (prevNt) RealNtCreateFile_SaveLog = reinterpret_cast<NtCreateFile_t>(prevNt);
    Log("[SaveWriteLog] Hooked CreateFileA in %d module(s), CreateFileW in %d, WriteFile in %d, CloseHandle in %d, "
        "NtCreateFile in %d\n",
        ca, cw, w, cl, nt);
}

// ---------------------------------------------------------------------
// mad2settings.dll registration -- every one of this diagnostic mod's 12
// hotkeys, reachable from the debug menu instead of needing to remember
// which F-key/letter does what.
// ---------------------------------------------------------------------

static void WINAPI ToggleForceMinigameModeOffAction(void*) { ToggleForceMinigameModeOff(); }
static void WINAPI ScanForNamedObjectsAction(void*) { ScanForNamedObjects(); }
static void WINAPI DumpUnpackedSectionsAction(void*) { DumpUnpackedSections(); }
static void WINAPI DumpGlobalDataAction(void*) { DumpGlobalData(); }
static void WINAPI DumpAllAction(void*) { DumpAll(); }
static void WINAPI ResetValueScanAction(void*) { ResetValueScan(); }
static void WINAPI RefineValueScanAction(void*) { RefineValueScan(); }
static void WINAPI FirstValueScanAction(void*) { FirstValueScan(); }
static void WINAPI ToggleAllCheatsAction(void*) { ToggleAllCheats(); }
static void WINAPI ScanForSaveStateObjectAction(void*) { ScanForSaveStateObject(); }
static void WINAPI ToggleMonkeysLiveAction(void*) { ToggleMonkeysLive(); }
static void WINAPI ToggleClimbLiveAction(void*) { ToggleClimbLive(); }

static void RegisterSettings() {
    const auto& settings = Mad2Settings_Resolve();
    if (!settings.Register) {
        Log("[Dependency] mad2settings.dll not available -- these hotkeys won't appear in the debug menu\n");
        return;
    }
    struct { const char* label; Mad2Settings_ActionFn fn; } actions[] = {
        {"Toggle Minigame Mode Force-Off", ToggleForceMinigameModeOffAction},
        {"Scan For Named Objects", ScanForNamedObjectsAction},
        {"Dump Unpacked Sections", DumpUnpackedSectionsAction},
        {"Dump Global Data", DumpGlobalDataAction},
        {"Dump All", DumpAllAction},
        {"Reset Value Scan", ResetValueScanAction},
        {"Refine Value Scan", RefineValueScanAction},
        {"First Value Scan", FirstValueScanAction},
        {"Toggle All Cheats", ToggleAllCheatsAction},
        {"Scan For Save-State Object", ScanForSaveStateObjectAction},
        {"Toggle Monkeys Live", ToggleMonkeysLiveAction},
        {"Toggle Climb Live", ToggleClimbLiveAction},
    };
    for (auto& a : actions) {
        Mad2SettingDesc desc{};
        desc.category = "ClimbProbe (diagnostic)";
        desc.label = a.label;
        desc.type = MAD2SETTING_ACTION;
        desc.onChanged = a.fn;
        settings.Register(&desc);
    }
    Log("Registered settings with mad2settings.dll\n");
}

static DWORD WINAPI PollThreadFunc(LPVOID) {
    Log("Ready -- F9 dumps memory regions, F8 resets the value-scan candidate list, "
        "F7 refines the value-scan candidate list against mad2climbprobe_scan_target.txt, "
        "F6 does a fresh value scan against that same file, "
        "F5 toggles ALL known cheats on/off regardless of screen/level, "
        "F3 scans process memory for the live climb/monkeys save-state object, "
        "F2 toggles monkey collectibles live, F1 toggles climb ability live, "
        "M dumps AWL.dll/AlchemyCommonLib.dll's full .data sections plus every plausible "
        "heap pointer found in them, and the known DumpAll chains, all in one capture "
        "(minigame-mode hunt), "
        "U dumps Mad2.exe's live (unpacked) code/data sections to raw .bin files, "
        "J scans live memory for a named object whose name contains \"minigame\" and reports "
        "its vtable/class, "
        "K toggles continuously forcing \"Minigame Launch Mode\" to 0 regardless of level\n");
    bool wasKPressed = false;
    bool wasJPressed = false;
    bool wasUPressed = false;
    bool wasMPressed = false;
    bool wasF9Pressed = false;
    bool wasF8Pressed = false;
    bool wasF7Pressed = false;
    bool wasF6Pressed = false;
    bool wasF5Pressed = false;
    bool wasF3Pressed = false;
    bool wasF2Pressed = false;
    bool wasF1Pressed = false;
    bool settingsRegistered = false;
    for (;;) {
        Sleep(50);

        if (!settingsRegistered && Mad2Settings_Resolve().Register) {
            RegisterSettings();
            settingsRegistered = true;
        }
        bool isKPressed = (GetAsyncKeyState('K') & 0x8000) != 0;
        if (isKPressed && !wasKPressed) ToggleForceMinigameModeOff();
        wasKPressed = isKPressed;
        if (g_ForceMinigameModeOff) TickForceMinigameModeOff();

        bool isJPressed = (GetAsyncKeyState('J') & 0x8000) != 0;
        if (isJPressed && !wasJPressed) ScanForNamedObjects();
        wasJPressed = isJPressed;

        bool isUPressed = (GetAsyncKeyState('U') & 0x8000) != 0;
        if (isUPressed && !wasUPressed) DumpUnpackedSections();
        wasUPressed = isUPressed;

        bool isMPressed = (GetAsyncKeyState('M') & 0x8000) != 0;
        if (isMPressed && !wasMPressed) DumpGlobalData();
        wasMPressed = isMPressed;

        bool isF9Pressed = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (isF9Pressed && !wasF9Pressed) DumpAll();
        wasF9Pressed = isF9Pressed;

        bool isF8Pressed = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (isF8Pressed && !wasF8Pressed) ResetValueScan();
        wasF8Pressed = isF8Pressed;

        bool isF7Pressed = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (isF7Pressed && !wasF7Pressed) RefineValueScan();
        wasF7Pressed = isF7Pressed;

        bool isF6Pressed = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (isF6Pressed && !wasF6Pressed) FirstValueScan();
        wasF6Pressed = isF6Pressed;

        bool isF5Pressed = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (isF5Pressed && !wasF5Pressed) ToggleAllCheats();
        wasF5Pressed = isF5Pressed;

        bool isF3Pressed = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        if (isF3Pressed && !wasF3Pressed) ScanForSaveStateObject();
        wasF3Pressed = isF3Pressed;

        bool isF2Pressed = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        if (isF2Pressed && !wasF2Pressed) ToggleMonkeysLive();
        wasF2Pressed = isF2Pressed;

        bool isF1Pressed = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        if (isF1Pressed && !wasF1Pressed) ToggleClimbLive();
        wasF1Pressed = isF1Pressed;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            // Synchronous, not lazy -- aa_mad2hookutil.dll is guaranteed
            // already loaded (its "aa_" prefix sorts first), and IAT hooks
            // need to be installed immediately, not deferred to a first
            // per-frame call like the other hotkeys/pointer-chain probes
            // in this mod.
            InstallSaveWriteLogHooks();
            HANDLE h = CreateThread(nullptr, 0, PollThreadFunc, nullptr, 0, nullptr);
            if (h) {
                CloseHandle(h);
            } else {
                Log("CreateThread for poll thread failed: %lu\n", GetLastError());
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}
