// Diagnostic runtime probe for the "switch playable character at any time"
// feasibility question -- NOT a gameplay mod, not meant to stay loaded
// permanently (same status as mad2climbprobe/mad2rhythmlogger/
// mad2metadumper/mad2shoptrace -- see those files' headers for the
// precedent).
//
// Background (see docs/CLASS_SCHEMA.md's "PC-verified fields" section):
// ActorInfo has a real, PC-confirmed field `_controller` at `+0x140`, an
// igSmartPointer<igController> (Display namespace) -- confirmed live via
// Ghidra by decompiling ActorInfoLib.dll's `usingController()`
// (`return *(this+0x140);`) and `setControllerFromVariant()` (which calls
// `igSmartPointerAssign(this+0x140, newController)`, not a raw store).
// `igController` itself is a tiny class (just a button map + isMapped flag),
// consistent with there being one shared "the local player's input" instance
// that gets attached to whichever ActorInfo the game currently treats as the
// player -- CLASS_SCHEMA.md's own data shows every ActorInfo's `controller`
// field is null in serialized level data, populated only at runtime.
//
// `igSmartPointerAssign` (`Gap::igSmartPointerAssign`, decompiled directly:
// `ref(new); release(*slot); *slot = new;`, both `ref`/`release` null-check
// internally) is a real function `igCore.dll` exports by its mangled name --
// resolvable via GetProcAddress like every other cross-DLL call in this
// codebase, and is the ENGINE'S OWN safe way to reassign a ref-counted slot.
// This probe calls it directly rather than poking the raw pointer, so a
// successful swap leaves refcounts exactly as if the game had done it.
//
// The open question this probe exists to answer (can't be settled by static
// analysis alone -- usingController() is a vtable virtual, so its real
// callers aren't visible without live tracing): does moving the controller
// away from ActorInfo A and onto ActorInfo B actually make B respond to
// input, get followed by the camera, etc, or do those systems track "the
// player" via some separate mechanism this field doesn't touch?
//
// Hotkeys (background poll thread, GetAsyncKeyState, same idiom as
// mad2climbprobe). Deliberately NOT function keys: every single F1-F12 is
// already claimed by some other simultaneously-loaded mod in this repo
// (mad2climbprobe alone owns F1/F2/F3/F5/F6/F7/F8/F9; mad2metadumper=F4;
// mad2igttimer/mad2podcastmod=F9; mad2nesmod=F5/F6/F7/F8;
// mad2companionmod=F10/F11/F12) -- confirmed the hard way, live: this
// probe's own F4 was silently double-firing mad2metadumper's full
// reflection dump on every press, and F5 launched mad2nesmod's NES
// challenge instead of (or alongside) this probe's export scan. Using
// letters not already claimed by mad2climbprobe (K/J/U/M) or mad2nesmod
// (R) instead, same precedent mad2nesmod's own RefreshGameKey set for
// itself (see that mod's own comment on why it picked 'R', not an F-key).
//   O -- scan all committed private RW memory for objects whose first 4
//        bytes equal ActorInfo's real vtable pointer (ActorInfoLib.dll+
//        0x8A38, confirmed via the `??_7ActorInfo@TFBActorInfo@Gap@@6B@`
//        export), logging each candidate's address, controller field,
//        position (`+0x50`, the world-matrix translation row -- see
//        CLASS_SCHEMA.md's "+80 reconciled" section), model (`+0x144`) and
//        gameActor (`+0x160`) fields, and a PLAUSIBLE flag (see below) so
//        found addresses can be correlated to visible in-game characters.
//   P -- cycle the selected candidate (wraps around).
//   L -- move the controller onto the selected candidate via
//        igSmartPointerAssign, clearing the source. Play and see what
//        actually happens.
//
// v2, after the first live test crashed the game (see below for what that
// run's own log revealed):
// - Identifying "who currently holds the controller" no longer guesses from
//   "the first candidate with a non-null controller field" -- a real capture
//   showed 3 candidates simultaneously non-null (evidently `_controller`
//   isn't exclusively "the human input device"; likely AI actors get some
//   controller object too), so that guess isn't reliable. Instead
//   `ResolvePlayerActorInfo()` re-walks the same pointer chain
//   mad2playercoords/mad2gameeffectsmod already use to read the live
//   player's position (`ActorInfoLib.dll+0x1A030`, then `+0x1C`, `+0x20`),
//   which independent reasoning (the actorParameters offset the rest of
//   that chain uses next lines up exactly with ActorInfo's own PC-verified
//   `+0x148`) suggests lands ON the player's own ActorInfo -- and this file
//   can now directly confirm that guess by checking the resolved address's
//   own vtable matches ActorInfo's, something the older mods never checked
//   since they didn't know the vtable offset yet.
// - A candidate is only offered as a swap TARGET if it looks like a real,
//   placed, fully-set-up actor: position isn't exactly (0,0,0) (a strong
//   uninitialized-object signal), and both `model` (`+0x144`) and
//   `gameActor` (`+0x160`) are non-null. The crashed run's target candidate
//   was `pos=(0,0,0)` -- almost certainly an uninitialized template object,
//   not a live character -- which is a much more likely crash cause than
//   the core controller-reassignment mechanism itself.
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../mad2levelredirectmod/include/mad2levelredirect_api.h"
#include "../../mad2settings/include/mad2settings_api.h"

#include "../../mad2leveldetect/mad2leveldetect.h"
#include "../../mad2sharedlog/mad2sharedlog.h"

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2characterswitchprobe]", fmt, args);
    va_end(args);
}

static bool IsPlausiblePointer(uintptr_t p) { return p > 0x10000 && p < 0x7FFFFFFF; }

static bool IsAddressSafeToRead(uintptr_t addr, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return addr + len <= regionEnd;
}

// Stricter than IsPlausiblePointer -- also requires the address to actually
// resolve (via VirtualQuery) to committed, MEM_PRIVATE, read-write memory
// (i.e. a real heap allocation), not just "some number in a plausible
// range". Added after a live crash traced to a false-positive vtable match:
// the second live test's target candidate had a `gameActor` value that
// passed IsPlausiblePointer's loose range check but actually pointed inside
// ViewerCommonLib.dll's own image (MEM_IMAGE, not a heap object) -- proof
// that candidate's vtable-pointer match itself was a coincidental collision
// with unrelated memory, not a real ActorInfo at all. This check would have
// caught it: a real model/gameActor delegate is always a heap allocation.
static bool IsPlausibleHeapPointer(uintptr_t p) {
    if (!IsPlausiblePointer(p)) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE) return false;
    return mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE;
}

static std::string DescribeAddress(uintptr_t addr) {
    if (addr == 0) return "null";
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(addr), &mod)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(addr));
        return buf;
    }
    char modName[MAX_PATH] = {0};
    GetModuleFileNameA(mod, modName, sizeof(modName));
    const char* base = strrchr(modName, '\\');
    base = base ? base + 1 : modName;
    char buf[128];
    snprintf(buf, sizeof(buf), "0x%08X (%s+0x%zX)", static_cast<unsigned int>(addr), base,
              addr - reinterpret_cast<uintptr_t>(mod));
    return buf;
}

// PC-verified against ActorInfoLib.dll -- see this file's header and
// docs/CLASS_SCHEMA.md's "PC-verified fields (ActorInfo...)" section /
// the "arkRegisterInitialize@ActorInfo" follow-up.
static const uintptr_t kActorInfoVtableOffset = 0x8A38;  // ??_7ActorInfo@TFBActorInfo@Gap@@6B@
static const uintptr_t kControllerOffset = 0x140;        // igSmartPointer<igController>
static const uintptr_t kModelOffset = 0x144;              // ModelInfo*
static const uintptr_t kPositionOffset = 0x50;            // worldMatrix row 3 (Vec3f x/y/z)
static const uintptr_t kGameActorOffset = 0x160;          // gameActor delegate pointer
static const uintptr_t kScanReadSpan = kGameActorOffset + 4;  // widest field read during a scan

// ---------------------------------------------------------------------
// F1: scan for live ActorInfo instances by vtable pattern (same technique
// as mad2gameeffectsmod's ObjectShuffle uses for Havok objects).
// ---------------------------------------------------------------------

struct Candidate {
    uintptr_t addr;
    uint32_t controller;
    float pos[3];
    uint32_t model;
    uint32_t gameActor;
    bool plausible;    // looks like a real, placed, fully-set-up actor -- see file header
    bool isPlayer;      // matches the chain-resolved, vtable-confirmed player address
    float distToPlayer;  // squared distance to the player's position; -1 if unknown
    int modelShareCount;  // how many OTHER scanned candidates share this exact model pointer
};

// A real, unique, switchable character (Alex/Marty/Gloria/a named NPC) has
// its own distinct model. Ordinary scenery -- coins, foliage, small props --
// shares one model across dozens/hundreds of instances. A live capture
// (this session) showed "nearest to player" reliably landing on a cluster
// of near-identical-position, same-model objects a few units from the
// player -- clutter, not other characters -- because raw distance alone
// doesn't distinguish the two. `modelShareCount` (populated by
// ScanForActorInfoInstances after the full candidate list is built) lets
// selection deprioritize commonly-repeated models instead.
static const int kCommonModelThreshold = 4;  // more than this many sightings of the same model = clutter, not a character
static bool LooksLikeCharacter(const Candidate& c) { return c.plausible && c.modelShareCount <= kCommonModelThreshold; }

static float DistSq(const float* a, const float* b) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

static std::vector<Candidate> g_Candidates;
static size_t g_SelectedIndex = 0;

static bool IsZeroVec3(const float* v) { return v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f; }

static Candidate ReadCandidate(uintptr_t addr) {
    Candidate c{};
    c.addr = addr;
    c.controller = *reinterpret_cast<const uint32_t*>(addr + kControllerOffset);
    const float* pos = reinterpret_cast<const float*>(addr + kPositionOffset);
    c.pos[0] = pos[0];
    c.pos[1] = pos[1];
    c.pos[2] = pos[2];
    c.model = *reinterpret_cast<const uint32_t*>(addr + kModelOffset);
    c.gameActor = *reinterpret_cast<const uint32_t*>(addr + kGameActorOffset);
    c.plausible = !IsZeroVec3(c.pos) && IsPlausibleHeapPointer(c.model) && IsPlausibleHeapPointer(c.gameActor);
    c.isPlayer = false;
    c.distToPlayer = -1.0f;  // unknown until the scan resolves the player's own position
    c.modelShareCount = 0;   // filled in by ScanForActorInfoInstances once the full list is known
    return c;
}

static void LogCandidate(size_t i, const Candidate& c, bool selected) {
    char distBuf[32] = "?";
    if (c.distToPlayer >= 0.0f) snprintf(distBuf, sizeof(distBuf), "%.1f", sqrtf(c.distToPlayer));
    Log("  [%zu]%s addr=0x%08X controller=%s pos=(%.2f, %.2f, %.2f) model=%s(x%d) gameActor=%s dist=%s %s%s%s\n", i,
        selected ? " *" : "  ", static_cast<unsigned int>(c.addr), DescribeAddress(c.controller).c_str(), c.pos[0],
        c.pos[1], c.pos[2], DescribeAddress(c.model).c_str(), c.modelShareCount + 1, DescribeAddress(c.gameActor).c_str(),
        distBuf, c.plausible ? "PLAUSIBLE" : "(not plausible -- uninitialized?)",
        (c.plausible && !LooksLikeCharacter(c)) ? " (common model -- likely clutter)" : "", c.isPlayer ? " PLAYER" : "");
}

// Re-walks the same pointer chain mad2playercoords/mad2gameeffectsmod use to
// read the live player's position (ActorInfoLib.dll+0x1A030, then +0x1C,
// +0x20) -- see this file's header for why that's believed to land on the
// player's own ActorInfo. Returns 0 if unresolved.
static uintptr_t ResolvePlayerActorInfo() {
    HMODULE actorInfoLib = GetModuleHandleA("ActorInfoLib.dll");
    if (!actorInfoLib) return 0;
    uintptr_t base = reinterpret_cast<uintptr_t>(actorInfoLib) + 0x0001A030;
    if (!IsAddressSafeToRead(base, 4)) return 0;
    uintptr_t addr = *reinterpret_cast<const uint32_t*>(base);
    static const uintptr_t offsets[] = {0x1C, 0x20};
    for (uintptr_t off : offsets) {
        if (!IsPlausiblePointer(addr) || !IsAddressSafeToRead(addr + off, 4)) return 0;
        addr = *reinterpret_cast<const uint32_t*>(addr + off);
    }
    return addr;
}

// Address of playerSet's single occupied element slot -- see F4's own
// header comment. Live-dumped and confirmed this session:
// ActorInfoLib.dll+0x1A030 -> deref -> resolver; resolver+0x1C = playerSet
// object; playerSet is a plain igObjectList (count=1 @+0x08, capacity=8
// @+0x0C, data pointer @+0x14 pointing at its own inline array starting at
// +0x20); element [0] at +0x20 holds the player's ActorInfo. This is a
// plain array slot, NOT a ref-counted igSmartPointer field the way
// ActorInfo::_controller is (confirmed: count/capacity don't change when
// this slot's value does) -- so a direct pointer write here is the correct
// operation, unlike _controller which needed igSmartPointerAssign. Returns
// 0 if any hop is unresolved.
static uintptr_t ResolvePlayerSetSlotAddr() {
    HMODULE actorInfoLib = GetModuleHandleA("ActorInfoLib.dll");
    if (!actorInfoLib) return 0;
    uintptr_t resolverSlot = reinterpret_cast<uintptr_t>(actorInfoLib) + 0x0001A030;
    if (!IsAddressSafeToRead(resolverSlot, 4)) return 0;
    uintptr_t resolver = *reinterpret_cast<const uint32_t*>(resolverSlot);
    if (!IsPlausiblePointer(resolver) || !IsAddressSafeToRead(resolver + 0x1C, 4)) return 0;
    uintptr_t playerSet = *reinterpret_cast<const uint32_t*>(resolver + 0x1C);
    if (!IsPlausiblePointer(playerSet)) return 0;
    uintptr_t slotAddr = playerSet + 0x20;
    if (!IsAddressSafeToRead(slotAddr, 4)) return 0;
    return slotAddr;
}

static void ScanForActorInfoInstances() {
    HMODULE actorInfoLib = GetModuleHandleA("ActorInfoLib.dll");
    if (!actorInfoLib) {
        Log("ScanForActorInfoInstances: ActorInfoLib.dll not loaded\n");
        return;
    }
    uint32_t vtableTarget = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(actorInfoLib) + kActorInfoVtableOffset);

    std::string level = "None";
    Mad2LevelDetect_UpdateCurrentLevel(level);
    Log("ScanForActorInfoInstances: scanning (level='%s', vtableTarget=0x%08X)...\n", level.c_str(), vtableTarget);

    std::vector<uintptr_t> found;
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
            uintptr_t regionEnd = regionStart + mbi.RegionSize;
            // Need objAddr+kScanReadSpan to still be in-bounds (covers every
            // field ReadCandidate() will later read off this address).
            if (regionEnd > regionStart + kScanReadSpan) {
                const uint32_t* words = reinterpret_cast<const uint32_t*>(regionStart);
                size_t wordCount = (regionEnd - regionStart - kScanReadSpan) / 4;
                for (size_t i = 0; i < wordCount; ++i) {
                    if (words[i] == vtableTarget) found.push_back(regionStart + i * 4);
                }
            }
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    g_Candidates.clear();
    for (uintptr_t obj : found) g_Candidates.push_back(ReadCandidate(obj));

    uintptr_t chainPlayer = ResolvePlayerActorInfo();
    bool chainPlayerIsRealActorInfo =
        chainPlayer != 0 && IsAddressSafeToRead(chainPlayer, 4) && *reinterpret_cast<const uint32_t*>(chainPlayer) == vtableTarget;
    Log("ScanForActorInfoInstances: %zu candidate(s) found; chain-resolved player=0x%08X (%s)\n", found.size(),
        static_cast<unsigned int>(chainPlayer),
        chainPlayerIsRealActorInfo ? "confirmed real ActorInfo" : "NOT a confirmed ActorInfo -- see log below");

    // Tag the player and, if we know its position, compute every other
    // candidate's distance to it -- lets F2/the initial selection prefer
    // the nearest real actor instead of scan order (usually memory
    // allocation order, which has no relation to where anything actually
    // is in the level -- see this file's header for why that mattered).
    const float* playerPos = nullptr;
    for (Candidate& c : g_Candidates) {
        if (chainPlayerIsRealActorInfo && c.addr == chainPlayer) {
            c.isPlayer = true;
            playerPos = c.pos;
            break;
        }
    }
    if (playerPos) {
        for (Candidate& c : g_Candidates) {
            if (!c.isPlayer) c.distToPlayer = DistSq(c.pos, playerPos);
        }
    } else {
        Log("ScanForActorInfoInstances: player position unknown -- distances unavailable, showing scan order\n");
    }

    // Tally how many candidates share each model pointer -- see
    // LooksLikeCharacter's comment. Real characters have their own unique
    // model; ordinary clutter (coins, foliage, small props) shares one
    // model across many instances, which "nearest to player" alone can't
    // tell apart from another character standing nearby.
    std::unordered_map<uint32_t, int> modelCounts;
    for (const Candidate& c : g_Candidates) {
        if (c.model != 0) ++modelCounts[c.model];
    }
    for (Candidate& c : g_Candidates) {
        if (c.model != 0) c.modelShareCount = modelCounts[c.model] - 1;
    }

    // Sort nearest-first; unknown distances (-1) and the player itself sort
    // to the end since neither is a useful switch target.
    std::sort(g_Candidates.begin(), g_Candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.isPlayer != b.isPlayer) return b.isPlayer;
        if ((a.distToPlayer < 0) != (b.distToPlayer < 0)) return a.distToPlayer >= 0;
        return a.distToPlayer < b.distToPlayer;
    });

    // Auto-select the nearest candidate that LOOKS LIKE A CHARACTER (plausible
    // AND not a commonly-repeated clutter model) -- falls back to any
    // plausible candidate if the level has nothing better nearby.
    g_SelectedIndex = 0;
    bool foundCharacterLike = false;
    for (size_t i = 0; i < g_Candidates.size(); ++i) {
        if (g_Candidates[i].isPlayer) continue;
        if (LooksLikeCharacter(g_Candidates[i])) {
            g_SelectedIndex = i;
            foundCharacterLike = true;
            break;
        }
    }
    if (!foundCharacterLike) {
        for (size_t i = 0; i < g_Candidates.size(); ++i) {
            if (g_Candidates[i].plausible && !g_Candidates[i].isPlayer) {
                g_SelectedIndex = i;
                break;
            }
        }
    }

    for (size_t i = 0; i < g_Candidates.size(); ++i) {
        LogCandidate(i, g_Candidates[i], i == g_SelectedIndex);
    }
    Log("ScanForActorInfoInstances: auto-selected the nearest PLAUSIBLE candidate (marked *) -- P cycles to the "
        "next-nearest, L moves the controller onto the selection\n");
}

// ---------------------------------------------------------------------
// F2: cycle selection. Skips straight to the next PLAUSIBLE candidate --
// with a scan often turning up hundreds of hits (most of them template/
// unplaced objects), stepping through every single one to find a real actor
// was too slow to be usable. Refreshes each candidate it passes over (not
// just the one it lands on) since plausibility can change between the scan
// and now (an object freed/reallocated, or a real actor that only just got
// positioned).
// ---------------------------------------------------------------------

static void CycleSelection() {
    if (g_Candidates.empty()) {
        Log("CycleSelection: no candidates -- press O first\n");
        return;
    }
    size_t count = g_Candidates.size();
    // Two passes: first only candidates that LOOK LIKE A CHARACTER (skips
    // commonly-repeated clutter models -- see LooksLikeCharacter), then fall
    // back to any plausible candidate if the level has nothing better.
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t step = 1; step <= count; ++step) {
            size_t i = (g_SelectedIndex + step) % count;
            Candidate& c = g_Candidates[i];
            if (!IsAddressSafeToRead(c.addr, kScanReadSpan)) continue;
            bool wasPlayer = c.isPlayer;
            float dist = c.distToPlayer;
            int modelShareCount = c.modelShareCount;
            c = ReadCandidate(c.addr);  // refresh -- state may have changed since the scan
            c.isPlayer = wasPlayer;
            c.distToPlayer = dist;              // stale (from the last F1 scan), but still useful context
            c.modelShareCount = modelShareCount;  // same -- model-frequency tally isn't recomputed here
            if (c.isPlayer) continue;
            if (pass == 0 ? !LooksLikeCharacter(c) : !c.plausible) continue;
            g_SelectedIndex = i;
            LogCandidate(g_SelectedIndex, c, true);
            return;
        }
    }
    Log("CycleSelection: no PLAUSIBLE candidate found among %zu -- re-scan with O, or the level may not have "
        "another placed actor right now\n",
        count);
}

// ---------------------------------------------------------------------
// F3: move the live controller (found on whichever candidate currently has
// one) onto the selected candidate, via the engine's own real
// igSmartPointerAssign -- not a raw pointer poke, see this file's header.
// ---------------------------------------------------------------------

typedef void(__cdecl* igSmartPointerAssign_t)(void** slot, void* newValue);
static igSmartPointerAssign_t g_igSmartPointerAssign = nullptr;
static bool g_ResolvedAssign = false;

static igSmartPointerAssign_t ResolveSmartPointerAssign() {
    if (g_ResolvedAssign) return g_igSmartPointerAssign;
    g_ResolvedAssign = true;
    HMODULE igCore = GetModuleHandleA("igCore.dll");
    if (!igCore) {
        Log("[Dependency] igCore.dll not loaded -- cannot resolve igSmartPointerAssign\n");
        return nullptr;
    }
    g_igSmartPointerAssign = reinterpret_cast<igSmartPointerAssign_t>(
        reinterpret_cast<void*>(GetProcAddress(igCore, "?igSmartPointerAssign@Gap@@YAXAAPAVigObject@Core@1@PAV231@@Z")));
    if (!g_igSmartPointerAssign) {
        Log("[Dependency] igCore.dll!igSmartPointerAssign export not found (DWORD=%lu)\n", GetLastError());
    }
    return g_igSmartPointerAssign;
}

static void SwitchControllerToSelected() {
    if (g_Candidates.empty()) {
        Log("SwitchControllerToSelected: no candidates -- press O first\n");
        return;
    }
    igSmartPointerAssign_t assign = ResolveSmartPointerAssign();
    if (!assign) return;

    uintptr_t target = g_Candidates[g_SelectedIndex].addr;
    if (!IsAddressSafeToRead(target, kScanReadSpan)) {
        Log("SwitchControllerToSelected: selected candidate 0x%08X no longer valid -- re-scan with O\n",
            static_cast<unsigned int>(target));
        return;
    }
    Candidate targetNow = ReadCandidate(target);
    if (!targetNow.plausible) {
        Log("SwitchControllerToSelected: refusing -- selected candidate 0x%08X doesn't look like a real placed "
            "actor (pos=(%.2f,%.2f,%.2f) model=%s gameActor=%s). Cycle with P to a PLAUSIBLE candidate instead.\n",
            static_cast<unsigned int>(target), targetNow.pos[0], targetNow.pos[1], targetNow.pos[2],
            DescribeAddress(targetNow.model).c_str(), DescribeAddress(targetNow.gameActor).c_str());
        return;
    }

    // Primary: the same pointer chain mad2playercoords/mad2gameeffectsmod
    // already use to read the live player's position -- cross-checked here
    // against ActorInfo's real vtable, something those older mods never
    // did. This replaces v1's "first candidate with a non-null controller"
    // guess, which a real capture showed is ambiguous (3 simultaneous hits).
    uint32_t vtableTarget = 0;
    HMODULE actorInfoLib = GetModuleHandleA("ActorInfoLib.dll");
    if (actorInfoLib) vtableTarget = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(actorInfoLib) + kActorInfoVtableOffset);

    uintptr_t holder = ResolvePlayerActorInfo();
    bool holderConfirmed = holder != 0 && IsAddressSafeToRead(holder, kScanReadSpan) &&
                            *reinterpret_cast<const uint32_t*>(holder) == vtableTarget;
    if (holderConfirmed) {
        uint32_t holderController = *reinterpret_cast<const uint32_t*>(holder + kControllerOffset);
        if (!IsPlausibleHeapPointer(holderController)) {
            Log("SwitchControllerToSelected: chain-resolved player 0x%08X has no valid controller (%s) -- falling "
                "back to the ambiguous scan\n",
                static_cast<unsigned int>(holder), DescribeAddress(holderController).c_str());
            holderConfirmed = false;
        }
    }
    if (holderConfirmed) {
        Log("SwitchControllerToSelected: using chain-resolved, vtable-confirmed player 0x%08X as source\n",
            static_cast<unsigned int>(holder));
    } else {
        // Fallback: scan candidates for a non-null, real-heap-pointer
        // controller, among PLAUSIBLE candidates only (v1 didn't filter
        // this at all, and didn't validate the controller value either --
        // a live crash traced to a false-positive vtable match showed why
        // both checks matter, see this file's header).
        holder = 0;
        int holderCount = 0;
        for (const Candidate& c : g_Candidates) {
            if (!c.plausible) continue;
            if (!IsAddressSafeToRead(c.addr, kScanReadSpan)) continue;
            uint32_t controller = *reinterpret_cast<const uint32_t*>(c.addr + kControllerOffset);
            if (IsPlausibleHeapPointer(controller)) {
                if (holder == 0) holder = c.addr;
                ++holderCount;
            }
        }
        if (holderCount > 1) {
            Log("SwitchControllerToSelected: fallback scan: %d PLAUSIBLE candidates have a valid non-null "
                "controller (still ambiguous) -- using the first, 0x%08X\n",
                holderCount, static_cast<unsigned int>(holder));
        } else if (holderCount == 1) {
            Log("SwitchControllerToSelected: fallback scan found exactly one holder, 0x%08X\n",
                static_cast<unsigned int>(holder));
        }
    }
    if (holder == 0) {
        Log("SwitchControllerToSelected: could not identify a current controller holder -- nothing to move\n");
        return;
    }
    if (holder == target) {
        Log("SwitchControllerToSelected: selected candidate 0x%08X already holds the controller -- no-op\n",
            static_cast<unsigned int>(target));
        return;
    }

    uint32_t controllerVal = *reinterpret_cast<const uint32_t*>(holder + kControllerOffset);
    Log("SwitchControllerToSelected: moving controller %s from 0x%08X to 0x%08X\n",
        DescribeAddress(controllerVal).c_str(), static_cast<unsigned int>(holder), static_cast<unsigned int>(target));

    assign(reinterpret_cast<void**>(target + kControllerOffset), reinterpret_cast<void*>(controllerVal));
    assign(reinterpret_cast<void**>(holder + kControllerOffset), nullptr);

    uint32_t targetAfter = *reinterpret_cast<const uint32_t*>(target + kControllerOffset);
    uint32_t holderAfter = *reinterpret_cast<const uint32_t*>(holder + kControllerOffset);
    Log("SwitchControllerToSelected: done -- target controller now %s, source controller now %s\n",
        DescribeAddress(targetAfter).c_str(), DescribeAddress(holderAfter).c_str());

    // Also replace playerSet's single element -- see ResolvePlayerSetSlotAddr's
    // comment. This is what a live capture (F4) showed camera-follow/HUD/
    // ability-dispatch most likely actually key off, distinct from the
    // low-level _controller field just moved above. Plain pointer write, not
    // ref-counted -- confirmed via the same F4 capture (count/capacity fields
    // sit right next to it and aren't smart-pointer managed).
    uintptr_t playerSetSlot = ResolvePlayerSetSlotAddr();
    if (playerSetSlot) {
        uint32_t before = *reinterpret_cast<const uint32_t*>(playerSetSlot);
        *reinterpret_cast<uint32_t*>(playerSetSlot) = static_cast<uint32_t>(target);
        Log("SwitchControllerToSelected: also replaced playerSet[0] (slot=0x%08X) %s -> 0x%08X\n",
            static_cast<unsigned int>(playerSetSlot), DescribeAddress(before).c_str(),
            static_cast<unsigned int>(target));
    } else {
        Log("SwitchControllerToSelected: could not resolve playerSet slot -- controller moved but playerSet "
            "unchanged\n");
    }
}

// ---------------------------------------------------------------------
// I: read-only structure dump of the playerSet/activeSet mechanism --
// added after live testing showed F3's controller swap does NOT bring the
// camera along. Ghidra research (this session) found the pointer chain this
// file already uses to find "the player" doesn't actually start from the
// player at all: ActorInfoLib.dll+0x1A030 is `ActorInfo::__interface`, a
// SHARED static `igSmartPointer<ActorInterfaceResolver>` -- decompiling
// `getPlayerSetToVariant`/`getActiveSetToVariant` (ActorInfoLib.dll) shows
// they read `*(resolver+0x1C)` ("playerSet") and `*(resolver+0x20)`
// ("activeSet") respectively off that ONE shared resolver object, not off
// `this`. `playerSet`/`activeSet` are real Set-typed fields (script-exposed
// as SetVariant) -- almost certainly what camera-follow/ability-dispatch
// actually key off, since they're a much higher-level "who is the player"
// concept than the low-level `_controller` field F3 moves.
//
// The real mutator is the `OpChangeMembership` script opcode (decompiled
// live, ScriptInfoLib.dll -- same class mad2shoptrace already vtable-hooks
// `execute()` on): its `execute()` resolves `_LHS`/`_RHS` through
// `resolveArgStack` (the script expression-evaluation subsystem) before
// calling plain `igObjectList::set`/`setCount`/`remove` on whatever list
// that resolves to -- i.e. `playerSet` isn't a raw pointer to be swapped in
// place; it's a real Set OBJECT this file hasn't confirmed the layout of
// yet. This dump is step one before attempting any write: read what's
// actually there, right now, with exactly one real player, so the
// structure can be read off real bytes instead of guessed from offsets in
// unrelated code paths (OpForEach's own cachedSet/listContainer nesting,
// logged in mad2shoptrace, may or may not be the same shape).
// ---------------------------------------------------------------------

static void SafeDumpRegion(uintptr_t start, size_t length, const char* label) {
    Log("---- DUMP %s addr=0x%08X len=0x%zX ----\n", label, static_cast<unsigned int>(start), length);
    if (!IsAddressSafeToRead(start, length)) {
        Log("  [unreadable]\n");
        return;
    }
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(start);
    for (size_t off = 0; off < length; off += 16) {
        size_t n = (length - off < 16) ? (length - off) : 16;
        char hex[64];
        size_t hp = 0;
        for (size_t i = 0; i < 16; ++i) {
            hp += static_cast<size_t>(
                (i < n) ? snprintf(hex + hp, sizeof(hex) - hp, "%02X ", bytes[off + i])
                        : snprintf(hex + hp, sizeof(hex) - hp, "   "));
        }
        Log("  +0x%02zX: %s\n", off, hex);
    }
}

static void DumpPlayerSetStructure() {
    HMODULE actorInfoLib = GetModuleHandleA("ActorInfoLib.dll");
    if (!actorInfoLib) {
        Log("DumpPlayerSetStructure: ActorInfoLib.dll not loaded\n");
        return;
    }
    uint32_t vtableTarget = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(actorInfoLib) + kActorInfoVtableOffset);
    uintptr_t resolverSlot = reinterpret_cast<uintptr_t>(actorInfoLib) + 0x0001A030;
    if (!IsAddressSafeToRead(resolverSlot, 4)) {
        Log("DumpPlayerSetStructure: resolver slot 0x%08X unreadable\n", static_cast<unsigned int>(resolverSlot));
        return;
    }
    uintptr_t resolver = *reinterpret_cast<const uint32_t*>(resolverSlot);
    Log("DumpPlayerSetStructure: resolverSlot=0x%08X resolver=%s\n", static_cast<unsigned int>(resolverSlot),
        DescribeAddress(resolver).c_str());
    if (!IsPlausiblePointer(resolver)) return;
    SafeDumpRegion(resolver, 0x30, "ActorInterfaceResolver");

    if (IsAddressSafeToRead(resolver + 0x1C, 4)) {
        uintptr_t playerSet = *reinterpret_cast<const uint32_t*>(resolver + 0x1C);
        Log("DumpPlayerSetStructure: playerSet (resolver+0x1C) = %s\n", DescribeAddress(playerSet).c_str());
        if (IsPlausiblePointer(playerSet)) {
            SafeDumpRegion(playerSet, 0x40, "playerSet_object");
            if (IsAddressSafeToRead(playerSet + 0x20, 4)) {
                uintptr_t inner = *reinterpret_cast<const uint32_t*>(playerSet + 0x20);
                Log("DumpPlayerSetStructure: playerSet+0x20 = %s%s\n", DescribeAddress(inner).c_str(),
                    (IsAddressSafeToRead(inner, 4) && *reinterpret_cast<const uint32_t*>(inner) == vtableTarget)
                        ? " <-- vtable MATCHES ActorInfo (this is the chain's existing 'player' hop)"
                        : "");
                if (IsPlausiblePointer(inner)) SafeDumpRegion(inner, 0x40, "playerSet+0x20_target");
            }
        }
    }

    if (IsAddressSafeToRead(resolver + 0x20, 4)) {
        uintptr_t activeSet = *reinterpret_cast<const uint32_t*>(resolver + 0x20);
        Log("DumpPlayerSetStructure: activeSet (resolver+0x20) = %s\n", DescribeAddress(activeSet).c_str());
        if (IsPlausiblePointer(activeSet)) SafeDumpRegion(activeSet, 0x40, "activeSet_object");
    }
}

// ---------------------------------------------------------------------
// H: the user reported the camera stays static after a switch until some
// discrete state change (cutscene, being attacked, dying) -- not a smooth
// catch-up lag, a genuine "someone has to tell the camera to retarget"
// gap. The live reflection dump (mad2metadumper) shows a real class,
// `SetCameraTargetMessage` (base igMessage, one field: `_target`), plus a
// `GetCameraCurrentInfo` message -- exactly the shape of an explicit
// "retarget the camera" event the game's own cutscene/damage/respawn logic
// almost certainly sends. Manually opening each of ~30 DLLs in Ghidra one
// at a time to find which one exports it is slow; instead, ask the ACTUAL
// running process directly -- walk every loaded module's own PE export
// table (read-only, no hooking, no writes) for names containing
// "CameraTarget", so whichever DLL really has it is found in one pass.
// ---------------------------------------------------------------------

static bool ContainsCaseInsensitive(const char* haystack, const char* needle) {
    size_t needleLen = strlen(needle);
    for (const char* p = haystack; *p; ++p) {
        size_t i = 0;
        for (; i < needleLen && p[i]; ++i) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) break;
        }
        if (i == needleLen) return true;
    }
    return false;
}

static void ScanModuleExportsForSubstring(HMODULE mod, const char* modName, const char* substr) {
    uintptr_t base = reinterpret_cast<uintptr_t>(mod);
    if (!IsAddressSafeToRead(base, sizeof(IMAGE_DOS_HEADER))) return;
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    uintptr_t ntAddr = base + static_cast<uintptr_t>(dos->e_lfanew);
    if (!IsAddressSafeToRead(ntAddr, sizeof(IMAGE_NT_HEADERS))) return;
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(ntAddr);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    if (IMAGE_DIRECTORY_ENTRY_EXPORT >= nt->OptionalHeader.NumberOfRvaAndSizes) return;
    IMAGE_DATA_DIRECTORY exportDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDir.VirtualAddress == 0) return;
    uintptr_t exportsAddr = base + exportDir.VirtualAddress;
    if (!IsAddressSafeToRead(exportsAddr, sizeof(IMAGE_EXPORT_DIRECTORY))) return;
    const IMAGE_EXPORT_DIRECTORY* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(exportsAddr);

    uintptr_t namesAddr = base + exports->AddressOfNames;
    for (uint32_t i = 0; i < exports->NumberOfNames; ++i) {
        if (!IsAddressSafeToRead(namesAddr + i * 4, 4)) break;
        uint32_t nameRva = reinterpret_cast<const uint32_t*>(namesAddr)[i];
        uintptr_t nameAddr = base + nameRva;
        if (!IsAddressSafeToRead(nameAddr, 1)) continue;
        const char* name = reinterpret_cast<const char*>(nameAddr);
        if (ContainsCaseInsensitive(name, substr)) {
            Log("  [%s] %s\n", modName, name);
        }
    }
}

static void ScanAllModuleExportsFor(const char* substr) {
    Log("ScanAllModuleExportsFor(\"%s\"): walking every loaded module's export table...\n", substr);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        Log("ScanAllModuleExportsFor: CreateToolhelp32Snapshot failed: %lu\n", GetLastError());
        return;
    }
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    int moduleCount = 0;
    if (Module32First(snap, &me)) {
        do {
            ScanModuleExportsForSubstring(me.hModule, me.szModule, substr);
            ++moduleCount;
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    Log("ScanAllModuleExportsFor: done (%d modules scanned)\n", moduleCount);
}

// ---------------------------------------------------------------------
// Poll thread -- same idiom as mad2climbprobe (GetAsyncKeyState, 50ms,
// edge-detected).
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// mad2settings.dll registration -- reachable from the debug menu instead
// of needing to remember the O/P/L/I/H hotkeys.
// ---------------------------------------------------------------------

static void WINAPI ScanForActorInfoInstancesAction(void*) { ScanForActorInfoInstances(); }
static void WINAPI CycleSelectionAction(void*) { CycleSelection(); }
static void WINAPI SwitchControllerToSelectedAction(void*) { SwitchControllerToSelected(); }
static void WINAPI DumpPlayerSetStructureAction(void*) { DumpPlayerSetStructure(); }
static void WINAPI ScanForCameraTargetExportsAction(void*) { ScanAllModuleExportsFor("CameraTarget"); }

static void RegisterSettings() {
    const auto& settings = Mad2Settings_Resolve();
    if (!settings.Register) {
        Log("[Dependency] mad2settings.dll not available -- these hotkeys won't appear in the debug menu\n");
        return;
    }
    struct { const char* label; Mad2Settings_ActionFn fn; } actions[] = {
        {"Scan For ActorInfo Instances", ScanForActorInfoInstancesAction},
        {"Cycle Selection", CycleSelectionAction},
        {"Switch Controller To Selected", SwitchControllerToSelectedAction},
        {"Dump PlayerSet Structure", DumpPlayerSetStructureAction},
        {"Scan Module Exports For \"CameraTarget\"", ScanForCameraTargetExportsAction},
    };
    for (auto& a : actions) {
        Mad2SettingDesc desc{};
        desc.category = "CharacterSwitchProbe (diagnostic)";
        desc.label = a.label;
        desc.type = MAD2SETTING_ACTION;
        desc.onChanged = a.fn;
        settings.Register(&desc);
    }
    Log("Registered settings with mad2settings.dll\n");
}

static DWORD WINAPI PollThreadFunc(LPVOID) {
    Log("Ready -- O scans for live ActorInfo instances, P cycles the selected candidate, "
        "L moves the live controller onto the selected candidate, I dumps the playerSet/activeSet "
        "structure (read-only), H scans every loaded module's exports for \"CameraTarget\" (read-only) "
        "-- deliberately not F-keys, see file header (every F1-F12 is already claimed by another mod)\n");
    bool wasO = false, wasP = false, wasL = false, wasI = false, wasH = false;
    bool settingsRegistered = false;
    for (;;) {
        Sleep(50);

        if (!settingsRegistered && Mad2Settings_Resolve().Register) {
            RegisterSettings();
            settingsRegistered = true;
        }
        bool isO = (GetAsyncKeyState('O') & 0x8000) != 0;
        if (isO && !wasO) ScanForActorInfoInstances();
        wasO = isO;

        bool isP = (GetAsyncKeyState('P') & 0x8000) != 0;
        if (isP && !wasP) CycleSelection();
        wasP = isP;

        bool isL = (GetAsyncKeyState('L') & 0x8000) != 0;
        if (isL && !wasL) SwitchControllerToSelected();
        wasL = isL;

        bool isI = (GetAsyncKeyState('I') & 0x8000) != 0;
        if (isI && !wasI) DumpPlayerSetStructure();
        wasI = isI;

        bool isH = (GetAsyncKeyState('H') & 0x8000) != 0;
        if (isH && !wasH) ScanAllModuleExportsFor("CameraTarget");
        wasH = isH;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
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
