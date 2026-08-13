// Diagnostic runtime logger for the Duty Free shop's item-list/membership
// logic -- NOT a gameplay mod, not meant to stay loaded permanently (same
// status as mad2rhythmlogger, whose exact technique this copies).
//
// Why this exists: DutyFreeBLD's ENGLISH.pak has several purchasable/
// unlockable item strings (e.g. "Alex Rites Hat", "Gloria Swimsuit",
// "Concept Art", "Card Match - bonus card 1/2", "Mini-Golf Bonus Hole 3")
// that never appear as selectable options in the shipped shop UI, sitting
// right alongside sibling items that ARE selectable. Static analysis this
// session (mad2repack script-dump against level.bld.orig, using the new
// live-reflection-derived full field schema -- see mad2metadumper) found:
//   - Every OpForEach's RHS resolves to the SAME shared/empty
//     ValueRHSVariant/RHSValueStack object across every instance in the
//     file (a default/no-op placeholder, not a per-instance set
//     reference), and cachedSet/cachedObject are always the runtime-unbound
//     null sentinel in the shipped file.
//   - 393 OpChangeMembership and 83 OpFindSubSet opcodes exist in this
//     level (add/remove-from-set and subset-filter operations respectively
//     -- exactly the shape of "is this item available" logic), but neither
//     class's fields reveal anything about *which* items get included
//     without seeing them actually run.
// This is the identical shape of dead end mad2rhythmlogger's own "Round 6"
// hit for VolcanoRave's OpForEach (see that file's header) -- the fix is
// the same one that worked there: hook execute() live, in the actual
// running game, instead of trying to statically resolve runtime-only
// state.
//
// How: same technique as mad2rhythmlogger -- execute() is a virtual method
// at vtable slot 29 (byte offset 0x74) in every Op* subclass; patching that
// slot on each target class's own vtable intercepts every instance of that
// class process-wide. All vtable addresses and field offsets below are
// PC-native: the vtable addresses came from Ghidra against the real PC
// ScriptInfoLib.dll (this session), and the field offsets came from
// mad2metadumper's live reflection dump (mad2iga/live_reflection_dump.tsv)
// -- i.e. read directly out of the actual running game's own class
// registry, not decompiled/guessed and not ported from the Wii build.
//
// Also hooks OpCreateVariable -- unrelated to the shop-items question, but
// directly answers a second question from this session: DutyFreeBLD has 7
// OpCreateVariable declarations (a disabled "Debug_RandomMonkeySet" dev
// feature in the Monkey Gallery aquarium exhibit) with _internalFlags==4
// (IS_DISABLED) in the shipped file. Logging internalFlags for every
// OpCreateVariable actually executed while in DutyFree tells us directly
// whether the interpreter skips IS_DISABLED nodes at all (if a logged
// instance with the disabled flag never appears, or appears but its
// variable is never subsequently found/used, that's real evidence either
// way) -- this wasn't previously known; the compiled OpCode::execute() and
// OpBlock/OpIfElse::execute() bodies checked earlier all turned out to be
// no-ops with no obvious internalFlags check, but that doesn't rule out
// the check happening somewhere else in the call chain this mod doesn't
// hook.
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../mad2sharedlog/mad2sharedlog.h"
#include "../../mad2levelredirectmod/include/mad2levelredirect_api.h"
#include "../../mad2leveldetect/mad2leveldetect.h"

static void GetShopTraceLogPath(char* outPath, size_t outSize) {
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
    snprintf(outPath, outSize, "%s\\shoptrace.log", logsDir);
}

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2shoptrace]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Static addresses -- vtables Ghidra-confirmed against the real PC
// ScriptInfoLib.dll at its own preferred image base (0x10000000) this
// session; field offsets from mad2iga/live_reflection_dump.tsv (this
// session's mad2metadumper capture), not ported from Wii.
// ---------------------------------------------------------------------

static constexpr uintptr_t kPreferredBase = 0x10000000;

static constexpr uintptr_t kVtableOpForEach = 0x100349a8;
static constexpr uintptr_t kVtableOpChangeMembership = 0x10034928;
static constexpr uintptr_t kVtableOpFindSubSet = 0x10037e78;
static constexpr uintptr_t kVtableOpCheckMembership = 0x100348a8;
static constexpr uintptr_t kVtableOpCreateVariable = 0x10034368;
// Added for a second capture: the Apparel submenu's own OpForEach hits were
// sparse (dir=2 "random-shuffle", resolving cachedObject to a shared
// "Generic_Container" pooled name both times) while OpChangeMembership/
// OpFindSubSet/OpCheckMembership turned out to be thousands-of-calls-per-
// few-seconds per-frame UI polling, not one-time list construction --
// neither looks like the actual "populate N item slots" event. OpSpawn is
// the next candidate: "Generic_Container" strongly suggests a spawned,
// then customized, placeholder actor per visible item slot.
static constexpr uintptr_t kVtableOpSpawn = 0x10034ee8;
static constexpr int kExecuteVtableSlot = 29;  // byte offset 0x74 -- same slot for every Op* class, confirmed by mad2rhythmlogger.

static uintptr_t g_delta = 0;
static bool g_ready = false;

static std::string g_CurrentLevel = "None";
static DWORD g_lastLevelCheckTick = 0;

static bool IsInDutyFree() {
    DWORD now = GetTickCount();
    if (now - g_lastLevelCheckTick > 250) {
        g_lastLevelCheckTick = now;
        Mad2LevelDetect_UpdateCurrentLevel(g_CurrentLevel);
    }
    return _stricmp(g_CurrentLevel.c_str(), "DutyFree") == 0;
}

static uintptr_t Rt(uintptr_t staticAddr) { return staticAddr + g_delta; }

static uint32_t FieldU32(void* thisPtr, uint32_t offset) {
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(thisPtr) + offset);
}

// Safe pointer helpers (mirrors mad2rhythmlogger's own SafeToRead/
// SafeReadCString -- see that file for the reasoning) -- needed here
// because cachedObject/cachedSet point at runtime-allocated objects this
// mod has never seen before, unlike thisPtr's own fields (guaranteed valid
// since a virtual call already dispatched through it).
static bool IsPlausiblePointer(uintptr_t p) { return p > 0x10000 && p < 0x7FFFFFFF; }

static bool SafeToRead(const void* p, size_t size) {
    if (!IsPlausiblePointer(reinterpret_cast<uintptr_t>(p))) return false;
    return !IsBadReadPtr(p, size);
}

// Reads *(obj+8) as a plain igNamedObject-style char* name (the same
// convention docs/SCRIPT_FORMAT.md already established for ScriptObject:
// "*(this+8) as a plain char*, per igCore.dll") and returns it only if it
// looks like a real short printable identifier -- guards against
// cachedObject/cachedSet pointing at something that isn't an
// igNamedObject at all (e.g. a raw ActorInfo with no name field there).
static bool TryReadNamedObjectName(uint32_t objAddr, char* out, size_t outSize) {
    if (objAddr == 0 || !SafeToRead(reinterpret_cast<const void*>(objAddr), 12)) return false;
    uint32_t namePtr = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(objAddr) + 8);
    if (namePtr == 0 || !SafeToRead(reinterpret_cast<const void*>(namePtr), 1)) return false;
    size_t n = 0;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(namePtr);
    for (; n + 1 < outSize; ++n) {
        if (!SafeToRead(src + n, 1)) return false;
        char c = static_cast<char>(src[n]);
        if (c == '\0') break;
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return false;
        out[n] = c;
        if (n > 120) return false;
    }
    out[n] = '\0';
    return n > 0;
}

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
    if (++g_lineCount % 200 == 0) fflush(g_out);
    LeaveCriticalSection(&g_outLock);
}

typedef void(__attribute__((thiscall)) * ExecuteFn)(void* thisPtr);

static ExecuteFn g_origExecOpForEach = nullptr;
static ExecuteFn g_origExecOpSpawn = nullptr;
static ExecuteFn g_origExecOpChangeMembership = nullptr;
static ExecuteFn g_origExecOpFindSubSet = nullptr;
static ExecuteFn g_origExecOpCheckMembership = nullptr;
static ExecuteFn g_origExecOpCreateVariable = nullptr;

// OpForEach: _LHS@44 (SetStack), _dir@48, _RHS@52 (ValueRHSVariant),
// _cachedSet@56/_cachedObject@60 populated as a side effect of the real
// execute() -- the actual per-tick "which set / which element" resolution.
static void __attribute__((thiscall)) Hook_ExecOpForEach(void* thisPtr) {
    if (!IsInDutyFree()) {
        if (g_origExecOpForEach) g_origExecOpForEach(thisPtr);
        return;
    }
    uint32_t lhs = thisPtr ? FieldU32(thisPtr, 44) : 0;
    uint32_t dir = thisPtr ? FieldU32(thisPtr, 48) : 0;
    uint32_t rhs = thisPtr ? FieldU32(thisPtr, 52) : 0;
    if (g_origExecOpForEach) g_origExecOpForEach(thisPtr);
    uint32_t cachedSet = thisPtr ? FieldU32(thisPtr, 56) : 0;
    uint32_t cachedObject = thisPtr ? FieldU32(thisPtr, 60) : 0;

    char setName[128] = {0};
    char objName[128] = {0};
    bool haveSetName = TryReadNamedObjectName(cachedSet, setName, sizeof(setName));
    bool haveObjName = TryReadNamedObjectName(cachedObject, objName, sizeof(objName));

    // cachedSet is itself a ScriptSet (or SetVariant) -- if it's readable,
    // also log its OWN member count (ScriptGroupStack-style container:
    // count at +0x10, mirroring the shape mad2iga.ObjectGraph.
    // ReadObjectListContainer already established for this exact class
    // family) so we can see the FULL source set's size, not just which one
    // element this particular call landed on.
    int32_t setMemberCount = -1;
    if (cachedSet != 0 && SafeToRead(reinterpret_cast<const void*>(cachedSet), 0x24)) {
        uint32_t listContainer = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(cachedSet) + 0x20);
        if (listContainer != 0 && SafeToRead(reinterpret_cast<const void*>(listContainer), 0x14)) {
            setMemberCount = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(listContainer) + 0x10);
        }
    }

    WriteLine(
        "FOREACH this=0x%08X LHS=0x%08X dir=%u RHS=0x%08X cachedSet=0x%08X(name=%s,memberCount=%d) "
        "cachedObject=0x%08X(name=%s)",
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thisPtr)), lhs, dir, rhs, cachedSet,
        haveSetName ? setName : "?", setMemberCount, cachedObject, haveObjName ? objName : "?");
}

// OpSpawn: _LHS@48 (spawn source/template), _RHS@52 (position/count
// expression), _facingRHS@56, _cachedObject@40 (the actual spawned
// instance -- runtime only), _lastReceiver@44. Tries to read a name off
// both _cachedObject (what got spawned) and _LHS (the template it was
// spawned from) -- "Generic_Container" showing up as the FOREACH target's
// name suggested a spawned-then-customized placeholder is the real vehicle
// for each visible item slot.
static void __attribute__((thiscall)) Hook_ExecOpSpawn(void* thisPtr) {
    if (!IsInDutyFree()) {
        if (g_origExecOpSpawn) g_origExecOpSpawn(thisPtr);
        return;
    }
    uint32_t lhs = thisPtr ? FieldU32(thisPtr, 48) : 0;
    uint32_t rhs = thisPtr ? FieldU32(thisPtr, 52) : 0;
    uint32_t facingRHS = thisPtr ? FieldU32(thisPtr, 56) : 0;
    if (g_origExecOpSpawn) g_origExecOpSpawn(thisPtr);
    uint32_t cachedObject = thisPtr ? FieldU32(thisPtr, 40) : 0;
    uint32_t lastReceiver = thisPtr ? FieldU32(thisPtr, 44) : 0;

    char lhsName[128] = {0};
    char objName[128] = {0};
    char receiverName[128] = {0};
    bool haveLhsName = TryReadNamedObjectName(lhs, lhsName, sizeof(lhsName));
    bool haveObjName = TryReadNamedObjectName(cachedObject, objName, sizeof(objName));
    bool haveReceiverName = TryReadNamedObjectName(lastReceiver, receiverName, sizeof(receiverName));

    WriteLine(
        "SPAWN this=0x%08X LHS=0x%08X(name=%s) RHS=0x%08X facingRHS=0x%08X cachedObject=0x%08X(name=%s) "
        "lastReceiver=0x%08X(name=%s)",
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thisPtr)), lhs, haveLhsName ? lhsName : "?", rhs, facingRHS,
        cachedObject, haveObjName ? objName : "?", lastReceiver, haveReceiverName ? receiverName : "?");
}

// OpChangeMembership: _LHS@36 (the set being mutated), _combineOp@40
// (add/remove/etc, enum value -- meaning not yet decoded, log raw),
// _RHS@44 (what's being added/removed). No cached field on this class --
// it's a pure mutation, not a query.
static void __attribute__((thiscall)) Hook_ExecOpChangeMembership(void* thisPtr) {
    if (!IsInDutyFree()) {
        if (g_origExecOpChangeMembership) g_origExecOpChangeMembership(thisPtr);
        return;
    }
    uint32_t lhs = thisPtr ? FieldU32(thisPtr, 36) : 0;
    uint32_t combineOp = thisPtr ? FieldU32(thisPtr, 40) : 0;
    uint32_t rhs = thisPtr ? FieldU32(thisPtr, 44) : 0;
    if (g_origExecOpChangeMembership) g_origExecOpChangeMembership(thisPtr);
    WriteLine("CHANGEMEMBERSHIP this=0x%08X LHS=0x%08X combineOp=%u RHS=0x%08X",
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thisPtr)), lhs, combineOp, rhs);
}

// OpFindSubSet: _LHS@40/_relOperator@44/_RHS@48 (the filter condition),
// _setRemainder@52 (int), _cachedObject@56 (the resulting ScriptSet
// subset -- the single most valuable field here, populated only at
// runtime).
static void __attribute__((thiscall)) Hook_ExecOpFindSubSet(void* thisPtr) {
    if (!IsInDutyFree()) {
        if (g_origExecOpFindSubSet) g_origExecOpFindSubSet(thisPtr);
        return;
    }
    uint32_t lhs = thisPtr ? FieldU32(thisPtr, 40) : 0;
    uint32_t relOp = thisPtr ? FieldU32(thisPtr, 44) : 0;
    uint32_t rhs = thisPtr ? FieldU32(thisPtr, 48) : 0;
    uint32_t setRemainder = thisPtr ? FieldU32(thisPtr, 52) : 0;
    if (g_origExecOpFindSubSet) g_origExecOpFindSubSet(thisPtr);
    uint32_t cachedObject = thisPtr ? FieldU32(thisPtr, 56) : 0;
    WriteLine("FINDSUBSET this=0x%08X LHS=0x%08X relOp=%u RHS=0x%08X setRemainder=%u cachedObject=0x%08X",
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thisPtr)), lhs, relOp, rhs, setRemainder,
              cachedObject);
}

// OpCheckMembership: _LHS@40/_membershipOp@44/_RHS@48, _cachedType@52,
// _cachedObj@56 -- "is this item currently a member of that set" style
// query, populated at runtime.
static void __attribute__((thiscall)) Hook_ExecOpCheckMembership(void* thisPtr) {
    if (!IsInDutyFree()) {
        if (g_origExecOpCheckMembership) g_origExecOpCheckMembership(thisPtr);
        return;
    }
    uint32_t lhs = thisPtr ? FieldU32(thisPtr, 40) : 0;
    uint32_t membershipOp = thisPtr ? FieldU32(thisPtr, 44) : 0;
    uint32_t rhs = thisPtr ? FieldU32(thisPtr, 48) : 0;
    if (g_origExecOpCheckMembership) g_origExecOpCheckMembership(thisPtr);
    uint32_t cachedType = thisPtr ? FieldU32(thisPtr, 52) : 0;
    uint32_t cachedObj = thisPtr ? FieldU32(thisPtr, 56) : 0;
    WriteLine("CHECKMEMBERSHIP this=0x%08X LHS=0x%08X membershipOp=%u RHS=0x%08X cachedType=0x%08X cachedObj=0x%08X",
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thisPtr)), lhs, membershipOp, rhs, cachedType,
              cachedObj);
}

// OpCreateVariable: logs _internalFlags (bit 4 = IS_DISABLED) for every
// instance actually executed while in DutyFree -- see file header. If the
// 7 known-disabled "Debug_RandomMonkeySet" declarations (internalFlags==4
// in the shipped file) show up here at all, the interpreter does NOT skip
// disabled nodes; if they never show up, that's evidence it does (or that
// this script path just never runs at all -- can't fully distinguish
// without also confirming via varName, hence logging that too).
static void __attribute__((thiscall)) Hook_ExecOpCreateVariable(void* thisPtr) {
    if (!IsInDutyFree()) {
        if (g_origExecOpCreateVariable) g_origExecOpCreateVariable(thisPtr);
        return;
    }
    uint32_t internalFlags = thisPtr ? FieldU32(thisPtr, 32) : 0;
    uint32_t varNameOrdinal = thisPtr ? FieldU32(thisPtr, 44) : 0;
    if (g_origExecOpCreateVariable) g_origExecOpCreateVariable(thisPtr);
    if (internalFlags != 0) {
        WriteLine("CREATEVAR this=0x%08X internalFlags=%u varNameOrdinal=%u", static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thisPtr)),
                  internalFlags, varNameOrdinal);
    }
}

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
    for (int i = 0; i < 600 && !h; ++i) {
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

    void* origForEach = nullptr;
    void* origSpawn = nullptr;
    void* origChangeMembership = nullptr;
    void* origFindSubSet = nullptr;
    void* origCheckMembership = nullptr;
    void* origCreateVariable = nullptr;
    PatchVTableSlotDirect(Rt(kVtableOpForEach), kExecuteVtableSlot, reinterpret_cast<void*>(&Hook_ExecOpForEach),
                          &origForEach);
    PatchVTableSlotDirect(Rt(kVtableOpSpawn), kExecuteVtableSlot, reinterpret_cast<void*>(&Hook_ExecOpSpawn),
                          &origSpawn);
    PatchVTableSlotDirect(Rt(kVtableOpChangeMembership), kExecuteVtableSlot,
                          reinterpret_cast<void*>(&Hook_ExecOpChangeMembership), &origChangeMembership);
    PatchVTableSlotDirect(Rt(kVtableOpFindSubSet), kExecuteVtableSlot, reinterpret_cast<void*>(&Hook_ExecOpFindSubSet),
                          &origFindSubSet);
    PatchVTableSlotDirect(Rt(kVtableOpCheckMembership), kExecuteVtableSlot,
                          reinterpret_cast<void*>(&Hook_ExecOpCheckMembership), &origCheckMembership);
    PatchVTableSlotDirect(Rt(kVtableOpCreateVariable), kExecuteVtableSlot,
                          reinterpret_cast<void*>(&Hook_ExecOpCreateVariable), &origCreateVariable);

    g_origExecOpForEach = reinterpret_cast<ExecuteFn>(origForEach);
    g_origExecOpSpawn = reinterpret_cast<ExecuteFn>(origSpawn);
    g_origExecOpChangeMembership = reinterpret_cast<ExecuteFn>(origChangeMembership);
    g_origExecOpFindSubSet = reinterpret_cast<ExecuteFn>(origFindSubSet);
    g_origExecOpCheckMembership = reinterpret_cast<ExecuteFn>(origCheckMembership);
    g_origExecOpCreateVariable = reinterpret_cast<ExecuteFn>(origCreateVariable);

    g_ready = true;
    Log("Hooks installed (OpForEach/OpChangeMembership/OpFindSubSet/OpCheckMembership/OpCreateVariable execute() "
        "vtable slot %d; logging gated to level=='DutyFree')\n",
        kExecuteVtableSlot);
    WriteLine("# hooks installed, delta=0x%08X, gated to DutyFree", static_cast<uint32_t>(g_delta));
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            InitializeCriticalSection(&g_outLock);
            char logPath[MAX_PATH];
            GetShopTraceLogPath(logPath, sizeof(logPath));
            g_out = fopen(logPath, "w");
            if (!g_out) {
                Log("failed to open %s for writing\n", logPath);
            } else {
                Log("logging to %s\n", logPath);
            }
            Log("mad2shoptrace loaded\n");
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
