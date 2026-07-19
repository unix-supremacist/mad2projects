#include "loadlibrary_hook.h"

#include <windows.h>

#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "log.h"
#include "registry_redirect.h"
#include "save_redirect.h"

typedef HMODULE(WINAPI* LoadLibraryA_t)(LPCSTR);
typedef HMODULE(WINAPI* LoadLibraryW_t)(LPCWSTR);
typedef HMODULE(WINAPI* LoadLibraryExA_t)(LPCSTR, HANDLE, DWORD);
typedef HMODULE(WINAPI* LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);

static LoadLibraryA_t RealLoadLibraryA = nullptr;
static LoadLibraryW_t RealLoadLibraryW = nullptr;
static LoadLibraryExA_t RealLoadLibraryExA = nullptr;
static LoadLibraryExW_t RealLoadLibraryExW = nullptr;

// Re-scan for our redirect targets right after a new module is mapped in,
// before returning control to whatever code just loaded it (and might
// immediately start doing file/registry I/O of its own).
static void RehookNow() {
    InstallSaveRedirectHooks();
    InstallRegistryRedirectHooks();
}

static HMODULE WINAPI HookedLoadLibraryA(LPCSTR lpLibFileName) {
    HMODULE h = RealLoadLibraryA(lpLibFileName);
    if (h) RehookNow();
    return h;
}

static HMODULE WINAPI HookedLoadLibraryW(LPCWSTR lpLibFileName) {
    HMODULE h = RealLoadLibraryW(lpLibFileName);
    if (h) RehookNow();
    return h;
}

static HMODULE WINAPI HookedLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE h = RealLoadLibraryExA(lpLibFileName, hFile, dwFlags);
    if (h) RehookNow();
    return h;
}

static HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE h = RealLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (h) RehookNow();
    return h;
}

void InstallLoadLibraryHooks() {
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    RealLoadLibraryA = reinterpret_cast<LoadLibraryA_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryA")));
    RealLoadLibraryW = reinterpret_cast<LoadLibraryW_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryW")));
    RealLoadLibraryExA =
        reinterpret_cast<LoadLibraryExA_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryExA")));
    RealLoadLibraryExW =
        reinterpret_cast<LoadLibraryExW_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryExW")));

    void* prev = nullptr;
    int total = 0;

#define CHAIN(importName, hook, target)                                                       \
    prev = nullptr;                                                                             \
    total += Mad2HookUtil_PatchIatAllModules("kernel32.dll", importName, reinterpret_cast<void*>(hook), &prev); \
    if (prev) target = reinterpret_cast<decltype(target)>(prev);

    CHAIN("LoadLibraryA", HookedLoadLibraryA, RealLoadLibraryA)
    CHAIN("LoadLibraryW", HookedLoadLibraryW, RealLoadLibraryW)
    CHAIN("LoadLibraryExA", HookedLoadLibraryExA, RealLoadLibraryExA)
    CHAIN("LoadLibraryExW", HookedLoadLibraryExW, RealLoadLibraryExW)

#undef CHAIN

    Log("[mad2saveloader] LoadLibrary hooks installed in %d module(s) (catches plugin DLLs loaded later)\n", total);
}
