#include "loadlibrary_hook.h"

#include <windows.h>

#include "file_redirect.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "log.h"

typedef HMODULE(WINAPI* LoadLibraryA_t)(LPCSTR);
typedef HMODULE(WINAPI* LoadLibraryW_t)(LPCWSTR);
typedef HMODULE(WINAPI* LoadLibraryExA_t)(LPCSTR, HANDLE, DWORD);
typedef HMODULE(WINAPI* LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);

static LoadLibraryA_t RealLoadLibraryA = nullptr;
static LoadLibraryW_t RealLoadLibraryW = nullptr;
static LoadLibraryExA_t RealLoadLibraryExA = nullptr;
static LoadLibraryExW_t RealLoadLibraryExW = nullptr;

// Patches just the module that was newly loaded, not a full rescan -- see
// file_redirect.h's comment on PatchFileRedirectHooksInModule for why that
// distinction matters (this used to call InstallFileRedirectHooks() here
// instead, an O(n) rescan on every single module load for the rest of the
// process's lifetime, which measurably stalled mod loading once more than
// one mod did the same thing).
static HMODULE WINAPI HookedLoadLibraryA(LPCSTR lpLibFileName) {
    HMODULE h = RealLoadLibraryA(lpLibFileName);
    if (h) PatchFileRedirectHooksInModule(h);
    return h;
}

static HMODULE WINAPI HookedLoadLibraryW(LPCWSTR lpLibFileName) {
    HMODULE h = RealLoadLibraryW(lpLibFileName);
    if (h) PatchFileRedirectHooksInModule(h);
    return h;
}

static HMODULE WINAPI HookedLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE h = RealLoadLibraryExA(lpLibFileName, hFile, dwFlags);
    if (h) PatchFileRedirectHooksInModule(h);
    return h;
}

static HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE h = RealLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (h) PatchFileRedirectHooksInModule(h);
    return h;
}

void InstallFileRedirectLoadLibraryHooks() {
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    RealLoadLibraryA = reinterpret_cast<LoadLibraryA_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryA")));
    RealLoadLibraryW = reinterpret_cast<LoadLibraryW_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryW")));
    RealLoadLibraryExA =
        reinterpret_cast<LoadLibraryExA_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryExA")));
    RealLoadLibraryExW =
        reinterpret_cast<LoadLibraryExW_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryExW")));

    void* prev = nullptr;
    int total = 0;

#define CHAIN(importName, hook, target)                                                              \
    prev = nullptr;                                                                                   \
    total += Mad2HookUtil_PatchIatAllModules("kernel32.dll", importName, reinterpret_cast<void*>(hook), &prev);     \
    if (prev) target = reinterpret_cast<decltype(target)>(prev);

    CHAIN("LoadLibraryA", HookedLoadLibraryA, RealLoadLibraryA)
    CHAIN("LoadLibraryW", HookedLoadLibraryW, RealLoadLibraryW)
    CHAIN("LoadLibraryExA", HookedLoadLibraryExA, RealLoadLibraryExA)
    CHAIN("LoadLibraryExW", HookedLoadLibraryExW, RealLoadLibraryExW)

#undef CHAIN

    Log("[mad2assetloader] LoadLibrary hooks installed in %d module(s) (catches engine DLLs loaded later)\n", total);
}
