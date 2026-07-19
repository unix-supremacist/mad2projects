#pragma once
// Public client interface for aa_mad2hookutil.dll (mad2/mods/aa_mad2hookutil.dll).
//
// Shared IAT/vtable hooking primitives, factored out of 11 mods that each
// carried their own near-identical copy: PatchIat/PatchIatAllModules (the
// "walk a module's import directory, overwrite one resolved import
// pointer, chain onto whatever was there before" technique -- see
// mad2assetloader/src/iat_hook.cpp's original, most-thoroughly-commented
// version for the full explanation of how/why this works) and
// PatchVTableSlot (the simpler "overwrite one COM vtable slot" technique
// every D3D9-hooking overlay mod uses for CreateDevice/EndScene/Reset).
//
// DEPLOYED AS "aa_mad2hookutil.dll" -- NOT "mad2hookutil.dll" -- so it
// sorts alphabetically FIRST among mad2/mods/*.dll and is therefore always
// the first mod mod_loader.cpp's LoadMods() loads (mirroring
// zz_mad2graphicseffectmod.dll's sorts-LAST trick in reverse -- see
// CLAUDE.md's "D3D9 overlay hooking" section for that precedent). This
// matters because IAT hooking runs SYNCHRONOUSLY inside every consumer's
// own DllMain (installing the Direct3DCreate9/NtCreateFile/XInputGetState
// IAT hook immediately at load, not lazily on first frame) -- unlike the
// mad2xinput/mad2config/mad2effects "resolve lazily from your first
// per-frame hook or a background thread" pattern, a consumer's DllMain
// cannot defer resolving this API, so it must already be loaded and ready
// by the time ANY other mod's DllMain runs. Consumers therefore resolve it
// via GetModuleHandleA/GetProcAddress directly, synchronously, from their
// own DllMain -- see Mad2HookUtil_Resolve() below -- relying on the aa_
// prefix rather than a lazy-retry loop.
//
// Consumers: mad2assetloader, mad2saveloader, mad2igttimer,
// mad2playercoords, mad2inputdisplay, mad2twitchchat, mad2effectshud,
// mad2graphicseffectmod, mad2musichud, mad2shadowfix, mad2levelredirectmod
// (all via PatchIat/PatchIatAllModules and/or PatchVTableSlot), plus
// mad2xinput (via PatchIat/PatchIatAllModules's ordinal-aware parameter --
// see below).
#include <windows.h>

#define MAD2HOOKUTIL_DLL_NAME "aa_mad2hookutil.dll"

// Overwrites the resolved import-address-table entry for (importDll,
// importName) in `module`'s own import directory with `hookFunc`, chaining
// onto whatever was there before via `prevFunc` (so multiple mods hooking
// the SAME import compose correctly regardless of load order -- the
// second one installed calls through to the first one's hook instead of
// clobbering it). Matched by name unless matchByOrdinal is TRUE, in which
// case importOrdinal is used instead (importName is ignored) -- see
// mad2xinput's use of this for why: igDisplay.dll imports XInputGetState
// from xinput1_3.dll by ordinal, not by name.
//
// Has a self-chain guard: if the entry already points at hookFunc itself
// (e.g. a periodic re-scan pass re-patching modules loaded since last
// time), it's skipped rather than re-patched -- otherwise a caller that
// re-runs this later would see its own hook reflected back as "the
// previous function" and chain onto itself.
//
// Returns TRUE if at least one matching entry in `module` was patched.
typedef BOOL(WINAPI* Mad2HookUtil_PatchIat_t)(HMODULE module, const char* importDll, const char* importName,
                                               WORD importOrdinal, BOOL matchByOrdinal, void* hookFunc,
                                               void** prevFunc);

// Same as PatchIat, but applies it across every currently-loaded module in
// the process (via a Toolhelp32Snapshot of TH32CS_SNAPMODULE) instead of
// one specific module -- what every consumer actually calls, since the
// importing module (igDisplay.dll, ntdll.dll, d3d9.dll, ...) isn't
// necessarily loaded yet, or its identity isn't known ahead of time, when
// the hook is installed. Returns the number of modules patched.
typedef int(WINAPI* Mad2HookUtil_PatchIatAllModules_t)(const char* importDll, const char* importName,
                                                         WORD importOrdinal, BOOL matchByOrdinal, void* hookFunc,
                                                         void** prevFunc);

// Overwrites vtable slot `index` of the COM object `comObj` (any COM
// interface -- IDirect3D9, IDirect3DDevice9, IDirect3DQuery9, ...) with
// hookFunc, returning the previous entry via prevFunc. Used for
// CreateDevice[16]/EndScene[42]/Reset[16] (different interfaces, same
// slot-16 coincidence -- see mad2graphicseffectmod's Reset hook comment)
// by every D3D9-hooking overlay mod.
typedef void(WINAPI* Mad2HookUtil_PatchVTableSlot_t)(void* comObj, int index, void* hookFunc, void** prevFunc);

#define MAD2HOOKUTIL_PATCHIAT_PROC "Mad2HookUtil_PatchIat"
#define MAD2HOOKUTIL_PATCHIATALLMODULES_PROC "Mad2HookUtil_PatchIatAllModules"
#define MAD2HOOKUTIL_PATCHVTABLESLOT_PROC "Mad2HookUtil_PatchVTableSlot"

struct Mad2HookUtilApi {
    Mad2HookUtil_PatchIat_t PatchIat;
    Mad2HookUtil_PatchIatAllModules_t PatchIatAllModules;
    Mad2HookUtil_PatchVTableSlot_t PatchVTableSlot;
};

// Resolves aa_mad2hookutil.dll's exports by name. Cheap to call every time
// (resolves once, then returns the cached result). If aa_mad2hookutil.dll
// isn't loaded (e.g. missing from mad2/mods/), every member is null --
// callers must null-check before use, same convention as every other
// shared-API mod in this repo.
inline const Mad2HookUtilApi& Mad2HookUtil_Resolve() {
    static Mad2HookUtilApi api{};
    static bool resolved = false;
    if (!resolved) {
        if (HMODULE h = GetModuleHandleA(MAD2HOOKUTIL_DLL_NAME)) {
            api.PatchIat = reinterpret_cast<Mad2HookUtil_PatchIat_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2HOOKUTIL_PATCHIAT_PROC)));
            api.PatchIatAllModules = reinterpret_cast<Mad2HookUtil_PatchIatAllModules_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2HOOKUTIL_PATCHIATALLMODULES_PROC)));
            api.PatchVTableSlot = reinterpret_cast<Mad2HookUtil_PatchVTableSlot_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2HOOKUTIL_PATCHVTABLESLOT_PROC)));
            resolved = api.PatchIat != nullptr;
        }
    }
    return api;
}

// ---------------------------------------------------------------------
// Convenience wrappers -- match the call shape every consumer's own local
// PatchIat/PatchIatAllModules/PatchVTableSlot used to have, so migrating a
// call site is a straight rename (PatchIat(...) -> Mad2HookUtil_PatchIat(...))
// rather than a rewrite. No-ops (false/0/no-op) if aa_mad2hookutil.dll
// isn't resolved -- callers already null-check/ignore-on-failure the same
// way they did with the old local copies.
// ---------------------------------------------------------------------

inline bool Mad2HookUtil_PatchIat(HMODULE module, const char* importDll, const char* importName, void* hookFunc,
                                   void** prevFunc = nullptr) {
    const auto& api = Mad2HookUtil_Resolve();
    if (!api.PatchIat) return false;
    return api.PatchIat(module, importDll, importName, 0, FALSE, hookFunc, prevFunc) != FALSE;
}

inline int Mad2HookUtil_PatchIatAllModules(const char* importDll, const char* importName, void* hookFunc,
                                            void** prevFunc = nullptr) {
    const auto& api = Mad2HookUtil_Resolve();
    if (!api.PatchIatAllModules) return 0;
    return api.PatchIatAllModules(importDll, importName, 0, FALSE, hookFunc, prevFunc);
}

// Ordinal-matching variants -- see mad2xinput's use for why (igDisplay.dll
// imports XInputGetState from xinput1_3.dll by ordinal, not by name).
inline bool Mad2HookUtil_PatchIatOrdinal(HMODULE module, const char* importDll, const char* importName,
                                          WORD importOrdinal, void* hookFunc, void** prevFunc = nullptr) {
    const auto& api = Mad2HookUtil_Resolve();
    if (!api.PatchIat) return false;
    return api.PatchIat(module, importDll, importName, importOrdinal, TRUE, hookFunc, prevFunc) != FALSE;
}

inline int Mad2HookUtil_PatchIatAllModulesOrdinal(const char* importDll, const char* importName, WORD importOrdinal,
                                                   void* hookFunc, void** prevFunc = nullptr) {
    const auto& api = Mad2HookUtil_Resolve();
    if (!api.PatchIatAllModules) return 0;
    return api.PatchIatAllModules(importDll, importName, importOrdinal, TRUE, hookFunc, prevFunc);
}

inline void Mad2HookUtil_PatchVTableSlot(void* comObj, int index, void* hookFunc, void** prevFunc = nullptr) {
    const auto& api = Mad2HookUtil_Resolve();
    if (api.PatchVTableSlot) api.PatchVTableSlot(comObj, index, hookFunc, prevFunc);
}
