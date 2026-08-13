// Shared IAT/vtable hooking primitives for Madagascar 2 (mad2/mods/aa_mad2hookutil.dll).
//
// See include/mad2hookutil_api.h for the full client contract and why this
// is deployed as "aa_" (sorts-first, loaded before every other mod so
// consumers can resolve it synchronously from their own DllMain). This is
// the canonical, merged implementation of what used to be 11 near-
// identical copies of PatchIat/PatchIatAllModules (mad2assetloader,
// mad2saveloader, mad2igttimer, mad2playercoords, mad2inputdisplay,
// mad2twitchchat, mad2effectshud, mad2graphicseffectmod, mad2musichud,
// mad2shadowfix, mad2levelredirectmod) plus mad2xinput's separate
// ordinal-aware variant, and 7 copies of PatchVTableSlot.
//
// Merge notes:
//   - mad2saveloader/src/iat_hook.cpp had a self-chain guard (skip an
//     entry already pointing at hookFunc itself, so a later re-scan pass
//     doesn't chain onto its own hook) that mad2assetloader/src/iat_hook.cpp
//     lacked. Kept here -- see the comment at its one call site below.
//   - mad2xinput/src/xinput_api.cpp's copy added ordinal-matching (some
//     imports, e.g. igDisplay.dll's XInputGetState, are imported by
//     ordinal rather than name) via an extra importOrdinal parameter.
//     Folded into this single implementation as matchByOrdinal/importOrdinal,
//     defaulted off by every non-ordinal caller (see the header's plain
//     Mad2HookUtil_PatchIat wrapper).
// Deliberately does NOT include ../include/mad2hookutil_api.h: that header
// declares inline convenience wrappers (Mad2HookUtil_PatchIat, etc.) with
// the SAME names as the raw exported DLL functions below but different
// signatures -- fine for consumers (who only ever see the inline
// wrappers), but including it here would put both the export and the
// wrapper in the same translation unit's overload set for no benefit,
// since this file has no need for the typedefs/resolver it provides.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstring>

#include "../../mad2sharedlog/mad2sharedlog.h"

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2hookutil]", fmt, args);
    va_end(args);
}

extern "C" __declspec(dllexport) BOOL WINAPI Mad2HookUtil_PatchIat(HMODULE module, const char* importDll,
                                                                     const char* importName, WORD importOrdinal,
                                                                     BOOL matchByOrdinal, void* hookFunc,
                                                                     void** prevFunc) {
    if (!module) return FALSE;

    BYTE* base = reinterpret_cast<BYTE*>(module);
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;

    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;

    IMAGE_DATA_DIRECTORY& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0) return FALSE;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    BOOL patchedAny = FALSE;

    for (; desc->Name != 0; ++desc) {
        const char* dllName = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dllName, importDll) != 0) continue;

        // Without the original (name) thunk we can't safely identify entries
        // by name -- the first thunk has already been overwritten with the
        // resolved function addresses by the loader.
        if (!desc->OriginalFirstThunk) continue;

        auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* addrThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

        for (; nameThunk->u1.AddressOfData != 0; ++nameThunk, ++addrThunk) {
            if (matchByOrdinal) {
                if (!IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) continue;
                if (IMAGE_ORDINAL(nameThunk->u1.Ordinal) != importOrdinal) continue;
            } else {
                if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) continue;
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + nameThunk->u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char*>(byName->Name), importName) != 0) continue;
            }

            // Self-chain guard (from mad2saveloader's copy): already patched
            // to this exact hook (e.g. a periodic rehook pass re-scanning
            // modules loaded since last time) -- skip, so callers that
            // re-run this later never see their own hook reflected back as
            // "the previous function" and chain onto themselves.
            if (addrThunk->u1.Function == reinterpret_cast<ULONG_PTR>(hookFunc)) continue;

            if (prevFunc) *prevFunc = reinterpret_cast<void*>(addrThunk->u1.Function);

            DWORD oldProtect;
            if (!VirtualProtect(&addrThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) continue;
            addrThunk->u1.Function = reinterpret_cast<ULONG_PTR>(hookFunc);
            DWORD unused;
            VirtualProtect(&addrThunk->u1.Function, sizeof(void*), oldProtect, &unused);

            patchedAny = TRUE;
        }
    }

    return patchedAny;
}

extern "C" __declspec(dllexport) int WINAPI Mad2HookUtil_PatchIatAllModules(const char* importDll,
                                                                              const char* importName,
                                                                              WORD importOrdinal,
                                                                              BOOL matchByOrdinal, void* hookFunc,
                                                                              void** prevFunc) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    int patchedCount = 0;
    MODULEENTRY32 entry;
    entry.dwSize = sizeof(entry);

    if (Module32First(snapshot, &entry)) {
        do {
            void* prev = nullptr;
            if (Mad2HookUtil_PatchIat(entry.hModule, importDll, importName, importOrdinal, matchByOrdinal, hookFunc,
                                       &prev)) {
                ++patchedCount;
                if (prevFunc && prev) *prevFunc = prev;
                Log("Hooked %s!%s%s in %s\n", importDll, matchByOrdinal ? "#" : "", matchByOrdinal ? "" : importName,
                    entry.szModule);
            }
        } while (Module32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return patchedCount;
}

extern "C" __declspec(dllexport) void WINAPI Mad2HookUtil_PatchVTableSlot(void* comObj, int index, void* hookFunc,
                                                                            void** prevFunc) {
    void** vtable = *reinterpret_cast<void***>(comObj);
    if (prevFunc) *prevFunc = vtable[index];
    DWORD oldProtect;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect);
    vtable[index] = hookFunc;
    DWORD unused;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &unused);
}

extern "C" __declspec(dllexport) BOOL WINAPI Mad2HookUtil_InstallInlineHook(void* targetAddr, void* hookFunc,
                                                                              int stolenBytes, void** outTrampoline) {
    if (!targetAddr || !hookFunc || !outTrampoline || stolenBytes < 5) return FALSE;

    BYTE* target = reinterpret_cast<BYTE*>(targetAddr);

    // Trampoline layout: [stolenBytes original bytes][E9 rel32 back to target+stolenBytes].
    BYTE* trampoline = reinterpret_cast<BYTE*>(
        VirtualAlloc(nullptr, static_cast<SIZE_T>(stolenBytes) + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return FALSE;

    memcpy(trampoline, target, static_cast<size_t>(stolenBytes));
    trampoline[stolenBytes] = 0xE9;
    ptrdiff_t backRel = (target + stolenBytes) - (trampoline + stolenBytes + 5);
    memcpy(trampoline + stolenBytes + 1, &backRel, sizeof(INT32));

    DWORD oldProtect;
    if (!VirtualProtect(target, static_cast<SIZE_T>(stolenBytes), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return FALSE;
    }

    target[0] = 0xE9;
    ptrdiff_t fwdRel = reinterpret_cast<BYTE*>(hookFunc) - (target + 5);
    memcpy(target + 1, &fwdRel, sizeof(INT32));
    for (int i = 5; i < stolenBytes; ++i) target[i] = 0x90;  // NOP-pad any remaining stolen bytes.

    DWORD unused;
    VirtualProtect(target, static_cast<SIZE_T>(stolenBytes), oldProtect, &unused);
    FlushInstructionCache(GetCurrentProcess(), target, static_cast<SIZE_T>(stolenBytes));

    *outTrampoline = trampoline;
    Log("Installed inline hook at %p (stole %d bytes, trampoline %p)\n", target, stolenBytes, trampoline);
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            Log("aa_mad2hookutil loaded\n");
            break;
        default:
            break;
    }
    return TRUE;
}
