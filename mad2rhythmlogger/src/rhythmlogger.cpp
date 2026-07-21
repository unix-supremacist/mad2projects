// Diagnostic runtime logger for VolcanoRave's rhythm minigame -- NOT a
// gameplay mod, not meant to stay loaded permanently. See docs/SCRIPT_FORMAT.md
// ("Round 4") for the full reverse-engineering context this exists to
// unblock: static analysis of level.bld confirmed the entire compiled
// TFBScript opcode graph, its type-tag scheme, and its control-flow model,
// but which *local variable* a given OpSetValue/OpCheckValue/OpForEach
// touches only resolves through a genuine runtime arg-stack (confirmed by
// decompiling ScriptInfoLib.dll's own execute()/resolveArgStack methods --
// there is no static field anywhere in the file that says this). This mod
// hooks that resolution directly, in the running game process, while a
// song actually plays, instead of trying to statically simulate it.
//
// How: `execute()` is a virtual method, declared once on OpCode/OpBranch
// and overridden per concrete opcode class -- confirmed via Ghidra to sit
// at the SAME vtable slot (29, byte offset 0x74) in every subclass's own
// vtable (OpSetValue, OpCheckValue, OpForEach, OpFindVariable all checked
// directly). Patching that one slot on each of those 4 classes' vtables
// (all addresses below are static, Ghidra-confirmed offsets from
// ScriptInfoLib.dll's own preferred image base 0x10000000) intercepts
// every instance of that opcode class process-wide -- standard C++, one
// vtable shared by every instance, same technique this repo already uses
// for D3D9's CreateDevice/EndScene (see mad2hookutil's
// Mad2HookUtil_PatchVTableSlot; this mod computes its own vtable addresses
// directly instead of going through an object pointer, since we don't
// have a live instance to read the vtable pointer FROM until one exists --
// we want to hook the vtable before ANY instance's first execute() call).
//
// What gets logged: for each hooked opcode instance, the relevant fields
// read directly out of it (LHS/RHS/dir/etc, all at Ghidra/arkRegisterInitialize
// -confirmed static offsets -- see mad2iga/tfbscript_pc_field_schema.json)
// both BEFORE calling through to the original execute() (its own inputs,
// unchanged by the call) and AFTER (fields the real execute() populates as
// a side effect -- OpForEach.cachedObject, OpFindVariable.cachedObject,
// OpCheckValue.cachedType/cachedValue -- confirmed by decompiling each
// class's own execute() body). This is genuinely the only way to see
// OpForEach's per-tick "which pattern got picked" decision, since that's
// runtime-only state (an internal index/random-permutation the *compiled
// file* never stores).
//
// Post-processing: every logged pointer is a RUNTIME address in the
// running process, in a completely different address space than
// level.bld's own file offsets. To cross-reference a log line against
// `mad2repack script-dump`'s JSON (which uses file offsets), compute the
// constant delta between the two address spaces empirically: level.bld's
// entire Section 1 is loaded as one contiguous block preserving internal
// relative offsets (the same assumption mad2iga.ObjectGraph.ResolveSegPointer
// already relies on for static analysis), so `runtime_addr - file_addr` is
// the SAME constant for every object loaded from this file in this play
// session -- find it by matching any one frequently-hit logged address
// against the known static address set from a script-dump JSON, then apply
// that same delta to every other logged pointer. This mod does not attempt
// that translation itself -- it has no visibification into level.bld's own
// file layout at all, deliberately kept simple/low-risk since it runs
// inside the live game process.
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../mad2sharedlog/mad2sharedlog.h"
#include "../../mad2levelredirectmod/include/mad2levelredirect_api.h"
#include "../../mad2leveldetect/mad2leveldetect.h"

// Computes <exe dir>\logs\rhythmlogger.log, mirroring
// Mad2Log_GetPath's own reasoning (mad2sharedlog.h): the current working
// directory isn't reliably settled to the game's exe dir at DllMain time,
// so resolve it explicitly from the process's own module path instead of
// relying on a bare relative fopen(). Lands in the same logs/ folder as
// the shared mad2.log for discoverability.
static void GetRhythmLogPath(char* outPath, size_t outSize) {
    char exeDir[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    char* slash = strrchr(exeDir, '\\');
    char* slash2 = strrchr(exeDir, '/');
    if (slash2 && (!slash || slash2 > slash)) slash = slash2;
    if (slash) {
        *(slash + 1) = '\0';
    } else {
        exeDir[0] = '\0';
    }
    char logsDir[MAX_PATH];
    snprintf(logsDir, sizeof(logsDir), "%slogs", exeDir);
    CreateDirectoryA(logsDir, nullptr);
    snprintf(outPath, outSize, "%s\\rhythmlogger.log", logsDir);
}

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2rhythmlogger]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Static addresses, all Ghidra-confirmed against ScriptInfoLib.dll at its
// own preferred image base (0x10000000) -- see docs/SCRIPT_FORMAT.md
// "Round 4"/"Round 5" for how each was found (arkRegisterInitialize for
// field offsets, ??_7<Class>@TFBScriptInfo@Gap@@6B@ vtable symbols for the
// vtable addresses, direct symbol lookup for the two globals).
// ---------------------------------------------------------------------

static constexpr uintptr_t kPreferredBase = 0x10000000;

static constexpr uintptr_t kVtableOpSetValue = 0x10037a68;
static constexpr uintptr_t kVtableOpCheckValue = 0x10037df8;
static constexpr uintptr_t kVtableOpForEach = 0x100349a8;
static constexpr uintptr_t kVtableOpFindVariable = 0x10035598;
// Added for a second play session (docs/SCRIPT_FORMAT.md "Round 7") to widen
// coverage past OpForEach, which turned out to mostly resolve into a
// runtime scratch pool rather than the actual pattern data -- both
// confirmed at vtable slot 29 the same way as the first 4 classes.
static constexpr uintptr_t kVtableOpSlideValue = 0x10037bf8;
static constexpr uintptr_t kVtableOpStartSequence = 0x100364a8;
static constexpr int kExecuteVtableSlot = 29;  // byte offset 0x74 -- confirmed identical across all classes above.

// ScriptVariant::_resolvedToObj -- the shared global the whole
// resolveArgStack family (used by OpSetValue/OpCheckValue's LHS resolution)
// writes its result into on success. Read AFTER calling through to the
// real execute(), since that's when it's populated.
static constexpr uintptr_t kAddrResolvedToObj = 0x100a26d4;

static uintptr_t g_delta = 0;  // ScriptInfoLib.dll's actual runtime base minus kPreferredBase.
static bool g_ready = false;

// Round 8: the first full-session capture (all 6 opcodes, no level gating)
// produced an 18GB log and made the whole system hard to use while it ran.
// Level-gating (IsInVolcanoRave, below) fixed that independently of which
// opcodes are hooked -- it cuts out menus/other levels, which was most of
// the original volume. OpStartSequence's parent-chase (the reason
// SetValue/CheckValue/FindVariable were dropped in favor of it) turned out
// to be a dead end against real data (mostly resolves to no object, and the
// rare hits land on unrelated classes -- see docs/SCRIPT_FORMAT.md "Round
// 8"), so these three are back on for the next capture; OpForEach stays off
// -- Round 6 already established its target set resolves into an untracked
// runtime scratch pool, not level.bld's own data.
static constexpr bool kHookSetValue = true;
static constexpr bool kHookCheckValue = true;
static constexpr bool kHookForEach = false;
static constexpr bool kHookFindVariable = true;

static std::string g_CurrentLevel = "None";
static DWORD g_lastLevelCheckTick = 0;

// Cheap to call from a hot opcode-execute path: refreshes at most 4x/second
// (GetCurrentLevel itself is a fast cached-resolve + string copy, but no
// need to pay even that on every single opcode call).
static bool IsInVolcanoRave() {
    DWORD now = GetTickCount();
    if (now - g_lastLevelCheckTick > 250) {
        g_lastLevelCheckTick = now;
        Mad2LevelDetect_UpdateCurrentLevel(g_CurrentLevel);
    }
    // The game itself opens the archive as "VOLCANORAVE.ARC" (all caps) --
    // mad2levelredirectmod's GetCurrentLevel preserves that original case
    // verbatim (confirmed live, mad2.log's [LevelLoad] lines), not the
    // mixed-case "VolcanoRave" the archive's on-disk filename/this doc uses.
    return _stricmp(g_CurrentLevel.c_str(), "VolcanoRave") == 0;
}

static uintptr_t Rt(uintptr_t staticAddr) { return staticAddr + g_delta; }

static uint32_t FieldU32(void* thisPtr, uint32_t offset) {
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(thisPtr) + offset);
}

static uint32_t ReadGlobalU32(uintptr_t staticAddr) { return *reinterpret_cast<uint32_t*>(Rt(staticAddr)); }

// ---------------------------------------------------------------------
// Safe pointer-chasing helpers -- needed once, for OpStartSequence's
// cachedObject: unlike every other field this mod reads (this's own
// fields, or another opcode's own fields — all guaranteed valid, since a
// virtual call already dispatched through them), cachedObject is a
// runtime-allocated ClonedSequence this mod has never seen before, and
// following ITS OWN parent field one hop further into a second live
// object is exactly the kind of chain that crashes the game outright on a
// bad guess. Mirrors mad2gameeffectsmod's IsPlausiblePointer, plus
// IsBadReadPtr as a real safety net (not just a range heuristic) since
// this walks into unfamiliar runtime-allocated memory, not a well-trodden
// static chain.
// ---------------------------------------------------------------------

static bool IsPlausiblePointer(uintptr_t p) { return p > 0x10000 && p < 0x7FFFFFFF; }

static bool SafeToRead(const void* p, size_t size) {
    if (!IsPlausiblePointer(reinterpret_cast<uintptr_t>(p))) return false;
    return !IsBadReadPtr(p, size);
}

static uint32_t SafeReadU32(const void* p, uint32_t offset, bool* ok) {
    const uint8_t* addr = reinterpret_cast<const uint8_t*>(p) + offset;
    if (!SafeToRead(addr, sizeof(uint32_t))) {
        if (ok) *ok = false;
        return 0;
    }
    if (ok) *ok = true;
    return *reinterpret_cast<const uint32_t*>(addr);
}

// Reads up to maxLen-1 bytes starting at p as a plain null-terminated C
// string, stopping (and refusing to even start) the moment any touched
// byte isn't safely readable -- used for Sequence.mediaName, which is a
// live in-memory string once loaded (unlike the file-at-rest ordinal
// encoding OpCreateVariable.varName uses -- see docs/SCRIPT_FORMAT.md;
// this is a genuinely different, runtime-only representation, not the
// same field kind read a different way).
static bool SafeReadCString(const void* p, char* out, size_t maxLen) {
    if (maxLen == 0) return false;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(p);
    size_t i = 0;
    for (; i < maxLen - 1; ++i) {
        if (!SafeToRead(src + i, 1)) return false;
        char c = static_cast<char>(src[i]);
        if (c == '\0') break;
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return false;  // not printable ASCII
        out[i] = c;
    }
    out[i] = '\0';
    return true;
}

// ---------------------------------------------------------------------
// Own output file. Opened once, kept open for the whole session (unlike
// mad2sharedlog's deliberate open+append+close-per-line -- that pattern
// exists for cross-DLL diagnostic logging where call volume is low; this
// mod can plausibly log many times per second while a song plays, so
// paying open+close+mutex per line isn't worth it here).
// ---------------------------------------------------------------------

static FILE* g_out = nullptr;
static CRITICAL_SECTION g_outLock;
static long g_lineCount = 0;

static void WriteLine(const char* fmt, ...) {
    if (!g_out) return;
    EnterCriticalSection(&g_outLock);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_out, fmt, args);
    va_end(args);
    fputc('\n', g_out);
    if (++g_lineCount % 200 == 0) fflush(g_out);  // periodic flush, not every line
    LeaveCriticalSection(&g_outLock);
}

// ---------------------------------------------------------------------
// Hook functions. `execute()` is `public virtual void execute() const`,
// thiscall, no arguments beyond the implicit `this` in ECX -- GCC's
// thiscall attribute matches MSVC's ABI for this shape (no stack args to
// clean up either way).
// ---------------------------------------------------------------------

typedef void(__attribute__((thiscall)) * ExecuteFn)(void* thisPtr);

static ExecuteFn g_origExecOpSetValue = nullptr;
static ExecuteFn g_origExecOpCheckValue = nullptr;
static ExecuteFn g_origExecOpForEach = nullptr;
static ExecuteFn g_origExecOpFindVariable = nullptr;
static ExecuteFn g_origExecOpSlideValue = nullptr;
static ExecuteFn g_origExecOpStartSequence = nullptr;

// OpSetValue: LHS@0x24 (ValueStack, the assignment target), RHS@0x28
// (ValueRHSVariant -- its literal contents, if any, are already fully
// decodable statically from the file via the confirmed type-tag scheme,
// see SCRIPT_FORMAT.md -- no need to re-derive that at runtime). After the
// real execute() runs, ScriptVariant::_resolvedToObj holds whatever
// resolveValueArgStack resolved LHS to.
static void __attribute__((thiscall)) Hook_ExecOpSetValue(void* thisPtr) {
    if (!IsInVolcanoRave()) {
        if (g_origExecOpSetValue) g_origExecOpSetValue(thisPtr);
        return;
    }
    uint32_t lhs = thisPtr ? FieldU32(thisPtr, 0x24) : 0;
    uint32_t rhs = thisPtr ? FieldU32(thisPtr, 0x28) : 0;
    if (g_origExecOpSetValue) g_origExecOpSetValue(thisPtr);
    uint32_t resolved = g_ready ? ReadGlobalU32(kAddrResolvedToObj) : 0;
    WriteLine("SETVALUE this=0x%08X LHS=0x%08X RHS=0x%08X resolvedTarget=0x%08X",
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thisPtr)), lhs, rhs, resolved);
}

// OpCheckValue inherits OpAbstractCheckValue's LHS@0x28/relOperator@0x2c/
// RHS@0x30, and adds its own cachedType@0x34/cachedValue@0x38 -- both
// populated by the real execute() as a side effect (confirmed by
// decompiling it directly, see SCRIPT_FORMAT.md).
static void __attribute__((thiscall)) Hook_ExecOpCheckValue(void* thisPtr) {
    if (!IsInVolcanoRave()) {
        if (g_origExecOpCheckValue) g_origExecOpCheckValue(thisPtr);
        return;
    }
    uint32_t lhs = thisPtr ? FieldU32(thisPtr, 0x28) : 0;
    uint32_t relOp = thisPtr ? FieldU32(thisPtr, 0x2c) : 0;
    uint32_t rhs = thisPtr ? FieldU32(thisPtr, 0x30) : 0;
    if (g_origExecOpCheckValue) g_origExecOpCheckValue(thisPtr);
    uint32_t cachedType = thisPtr ? FieldU32(thisPtr, 0x34) : 0;
    uint32_t cachedValue = thisPtr ? FieldU32(thisPtr, 0x38) : 0;
    WriteLine("CHECKVALUE this=0x%08X LHS=0x%08X relOp=%u RHS=0x%08X cachedType=0x%08X cachedValue=0x%08X",
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thisPtr)), lhs, relOp, rhs, cachedType, cachedValue);
}

// OpForEach: LHS@0x2c (the SetStack -- which collection is being walked),
// dir@0x30 (0=forward/1=backward/2=random-shuffle, confirmed by
// decompiling execute() directly), RHS@0x34 (index expression). The single
// most valuable hook here: cachedObject@0x3c is populated with whichever
// ONE element got picked THIS call (execute() does not loop internally --
// see SCRIPT_FORMAT.md's "Round 4" -- so this is the only way to see the
// actual per-tick advance through a song's pattern data).
static void __attribute__((thiscall)) Hook_ExecOpForEach(void* thisPtr) {
    uint32_t lhs = thisPtr ? FieldU32(thisPtr, 0x2c) : 0;
    uint32_t dir = thisPtr ? FieldU32(thisPtr, 0x30) : 0;
    uint32_t rhs = thisPtr ? FieldU32(thisPtr, 0x34) : 0;
    if (g_origExecOpForEach) g_origExecOpForEach(thisPtr);
    uint32_t cachedObject = thisPtr ? FieldU32(thisPtr, 0x3c) : 0;
    WriteLine("FOREACH this=0x%08X LHS=0x%08X dir=%u RHS=0x%08X cachedObject=0x%08X",
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thisPtr)), lhs, dir, rhs, cachedObject);
}

// OpFindVariable: varContainerType@0x28/varContentsType@0x2c (type tags),
// varName@0x30 (a string-pool ordinal -- resolvable the same way
// mad2iga.ObjectGraph.ResolveStringOrdinal already does statically, no
// runtime work needed for this one field), owner@0x34. cachedObject@0x3c
// is populated with whichever variable it actually found by name.
static void __attribute__((thiscall)) Hook_ExecOpFindVariable(void* thisPtr) {
    if (!IsInVolcanoRave()) {
        if (g_origExecOpFindVariable) g_origExecOpFindVariable(thisPtr);
        return;
    }
    uint32_t containerType = thisPtr ? FieldU32(thisPtr, 0x28) : 0;
    uint32_t contentsType = thisPtr ? FieldU32(thisPtr, 0x2c) : 0;
    uint32_t varNameOrdinal = thisPtr ? FieldU32(thisPtr, 0x30) : 0;
    uint32_t owner = thisPtr ? FieldU32(thisPtr, 0x34) : 0;
    if (g_origExecOpFindVariable) g_origExecOpFindVariable(thisPtr);
    uint32_t cachedObject = thisPtr ? FieldU32(thisPtr, 0x3c) : 0;
    WriteLine(
        "FINDVAR this=0x%08X varNameOrdinal=%u owner=0x%08X containerType=0x%08X contentsType=0x%08X cachedObject=0x%08X",
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thisPtr)), varNameOrdinal, owner, containerType,
        contentsType, cachedObject);
}

// OpSlideValue: LHS@0x28 (ValueStack, the value being slid), RHS@0x2c
// (target value expression), secondsRHS@0x30 (slide duration expression),
// easeOutRHS@0x34/easeInRHS@0x38 (easing curve expressions). Confirmed by
// decompiling execute() directly (ScriptInfoLib.dll 0x10021100): it
// allocates a fresh `Slider` object from a memory pool each call (a
// runtime-only wrapper, not part of level.bld's own data -- expect
// cachedSlider to often land in the same untracked scratch-pool address
// range OpForEach.cachedObject did) and appends it the same
// push-onto-the-arg-stack way OpForEach/OpFindVariable do.
static void __attribute__((thiscall)) Hook_ExecOpSlideValue(void* thisPtr) {
    if (!IsInVolcanoRave()) {
        if (g_origExecOpSlideValue) g_origExecOpSlideValue(thisPtr);
        return;
    }
    uint32_t lhs = thisPtr ? FieldU32(thisPtr, 0x28) : 0;
    uint32_t rhs = thisPtr ? FieldU32(thisPtr, 0x2c) : 0;
    uint32_t secondsRHS = thisPtr ? FieldU32(thisPtr, 0x30) : 0;
    if (g_origExecOpSlideValue) g_origExecOpSlideValue(thisPtr);
    uint32_t cachedSlider = thisPtr ? FieldU32(thisPtr, 0x3c) : 0;
    WriteLine("SLIDEVALUE this=0x%08X LHS=0x%08X RHS=0x%08X secondsRHS=0x%08X cachedSlider=0x%08X",
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thisPtr)), lhs, rhs, secondsRHS, cachedSlider);
}

// OpStartSequence: LHS@0x28 (the sequence-group reference), indexRHS@0x2c
// (which sequence to start), cachedObject@0x30 (populated with the actual
// ClonedSequence instance started). This turned out to be the single most
// promising hook in Round 7 -- one static OpStartSequence firing
// repeatedly over a play session with steadily-increasing cachedObject
// addresses (one fresh ClonedSequence per call, allocated sequentially in
// a runtime pool) is exactly the shape of "start the next beat/pattern
// event." ClonedSequence is itself a runtime-only wrapper, but its
// `parent`@0x38 field points back at the STATIC `Sequence` template it was
// cloned from -- which has real, level.bld-resolvable data
// (`mediaName`@0x20, `playbackMode`@0x28, `playbackPercent`@0x2c). Chasing
// that one extra hop, safely (see SafeReadU32/SafeReadCString above), is
// what actually identifies *which* authored cue this specific call played.
static void __attribute__((thiscall)) Hook_ExecOpStartSequence(void* thisPtr) {
    if (!IsInVolcanoRave()) {
        if (g_origExecOpStartSequence) g_origExecOpStartSequence(thisPtr);
        return;
    }
    uint32_t lhs = thisPtr ? FieldU32(thisPtr, 0x28) : 0;
    uint32_t indexRHS = thisPtr ? FieldU32(thisPtr, 0x2c) : 0;
    if (g_origExecOpStartSequence) g_origExecOpStartSequence(thisPtr);
    uint32_t cachedObject = thisPtr ? FieldU32(thisPtr, 0x30) : 0;

    uint32_t parent = 0;
    uint32_t mediaNamePtr = 0;
    char mediaName[64] = {0};
    bool haveMediaName = false;
    uint32_t playbackMode = 0;
    float playbackPercent = 0.0f;
    if (cachedObject != 0 && SafeToRead(reinterpret_cast<void*>(cachedObject), 0x3c)) {
        bool ok = false;
        parent = SafeReadU32(reinterpret_cast<void*>(cachedObject), 0x38, &ok);
        if (ok && parent != 0 && SafeToRead(reinterpret_cast<void*>(parent), 0x30)) {
            mediaNamePtr = SafeReadU32(reinterpret_cast<void*>(parent), 0x20, &ok);
            if (ok && mediaNamePtr != 0) {
                haveMediaName = SafeReadCString(reinterpret_cast<void*>(mediaNamePtr), mediaName, sizeof(mediaName));
            }
            playbackMode = SafeReadU32(reinterpret_cast<void*>(parent), 0x28, &ok);
            uint32_t rawPercent = SafeReadU32(reinterpret_cast<void*>(parent), 0x2c, &ok);
            if (ok) memcpy(&playbackPercent, &rawPercent, sizeof(float));
        }
    }

    if (haveMediaName) {
        WriteLine(
            "STARTSEQUENCE this=0x%08X LHS=0x%08X indexRHS=0x%08X cachedObject=0x%08X parent=0x%08X "
            "mediaName=\"%s\" playbackMode=%u playbackPercent=%f",
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thisPtr)), lhs, indexRHS, cachedObject, parent,
            mediaName, playbackMode, playbackPercent);
    } else {
        WriteLine("STARTSEQUENCE this=0x%08X LHS=0x%08X indexRHS=0x%08X cachedObject=0x%08X parent=0x%08X",
                  static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thisPtr)), lhs, indexRHS, cachedObject, parent);
    }
}

// ---------------------------------------------------------------------
// Installation.
// ---------------------------------------------------------------------

static void PatchVTableSlotDirect(uintptr_t vtableAddr, int slot, void* hookFn, void** origOut) {
    void** vtable = reinterpret_cast<void**>(vtableAddr);
    if (origOut) *origOut = vtable[slot];
    DWORD oldProtect;
    VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &oldProtect);
    vtable[slot] = hookFn;
    DWORD unused;
    VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &unused);
}

static DWORD WINAPI InitThreadFunc(LPVOID) {
    HMODULE h = nullptr;
    for (int i = 0; i < 600 && !h; ++i) {  // poll up to ~60s for ScriptInfoLib.dll to load
        h = GetModuleHandleA("ScriptInfoLib.dll");
        if (!h) Sleep(100);
    }
    if (!h) {
        Log("ScriptInfoLib.dll never loaded, giving up\n");
        return 0;
    }

    g_delta = reinterpret_cast<uintptr_t>(h) - kPreferredBase;
    Log("ScriptInfoLib.dll base=0x%08X delta=0x%08X\n", static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h)),
        static_cast<uint32_t>(g_delta));

    void* origSetValue = nullptr;
    void* origCheckValue = nullptr;
    void* origForEach = nullptr;
    void* origFindVariable = nullptr;
    void* origSlideValue = nullptr;
    void* origStartSequence = nullptr;
    if (kHookSetValue)
        PatchVTableSlotDirect(Rt(kVtableOpSetValue), kExecuteVtableSlot, reinterpret_cast<void*>(&Hook_ExecOpSetValue),
                              &origSetValue);
    if (kHookCheckValue)
        PatchVTableSlotDirect(Rt(kVtableOpCheckValue), kExecuteVtableSlot,
                              reinterpret_cast<void*>(&Hook_ExecOpCheckValue), &origCheckValue);
    if (kHookForEach)
        PatchVTableSlotDirect(Rt(kVtableOpForEach), kExecuteVtableSlot, reinterpret_cast<void*>(&Hook_ExecOpForEach),
                              &origForEach);
    if (kHookFindVariable)
        PatchVTableSlotDirect(Rt(kVtableOpFindVariable), kExecuteVtableSlot,
                              reinterpret_cast<void*>(&Hook_ExecOpFindVariable), &origFindVariable);
    PatchVTableSlotDirect(Rt(kVtableOpSlideValue), kExecuteVtableSlot, reinterpret_cast<void*>(&Hook_ExecOpSlideValue),
                          &origSlideValue);
    PatchVTableSlotDirect(Rt(kVtableOpStartSequence), kExecuteVtableSlot,
                          reinterpret_cast<void*>(&Hook_ExecOpStartSequence), &origStartSequence);

    g_origExecOpSetValue = reinterpret_cast<ExecuteFn>(origSetValue);
    g_origExecOpCheckValue = reinterpret_cast<ExecuteFn>(origCheckValue);
    g_origExecOpForEach = reinterpret_cast<ExecuteFn>(origForEach);
    g_origExecOpFindVariable = reinterpret_cast<ExecuteFn>(origFindVariable);
    g_origExecOpSlideValue = reinterpret_cast<ExecuteFn>(origSlideValue);
    g_origExecOpStartSequence = reinterpret_cast<ExecuteFn>(origStartSequence);

    g_ready = true;
    Log("Hooks installed (OpSetValue/OpCheckValue/OpFindVariable/OpSlideValue/OpStartSequence execute() vtable "
        "slot %d; OpForEach disabled; logging gated to level=='VolcanoRave')\n",
        kExecuteVtableSlot);
    WriteLine("# hooks installed, delta=0x%08X, gated to VolcanoRave", static_cast<uint32_t>(g_delta));
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            InitializeCriticalSection(&g_outLock);
            char logPath[MAX_PATH];
            GetRhythmLogPath(logPath, sizeof(logPath));
            g_out = fopen(logPath, "w");
            if (!g_out) {
                Log("failed to open %s for writing\n", logPath);
            } else {
                Log("logging to %s\n", logPath);
            }
            Log("mad2rhythmlogger loaded\n");
            HANDLE hThread = CreateThread(nullptr, 0, InitThreadFunc, nullptr, 0, nullptr);
            if (hThread) {
                CloseHandle(hThread);
            } else {
                Log("CreateThread for init thread failed: %lu\n", GetLastError());
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            if (g_out) {
                fflush(g_out);
                fclose(g_out);
                g_out = nullptr;
            }
            DeleteCriticalSection(&g_outLock);
            break;
        default:
            break;
    }
    return TRUE;
}
