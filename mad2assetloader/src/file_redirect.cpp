#include "file_redirect.h"

#include <windows.h>
#include <cctype>
#include <string>

#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "log.h"

typedef HANDLE(WINAPI* CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static CreateFileA_t RealCreateFileA = nullptr;
static CreateFileW_t RealCreateFileW = nullptr;

// Any path containing ".../Streams/win/..." is first checked under
// ".../Streams/mod/..." (case-insensitively, either slash direction).
// If a matching file exists there, it's preferred; otherwise we fall
// back to the original path untouched.
static const char kSearchSegA[] = "streams/win/";
static const char kReplaceSegA[] = "Streams\\mod\\";
static const wchar_t kSearchSegW[] = L"streams/win/";
static const wchar_t kReplaceSegW[] = L"Streams\\mod\\";

// Level content ships as a matched pair of archives (e.g. BraveNewWild.arc
// + BraveNewWild.bld) under Content/Streams/win/. Logging every open of
// one gives a precise, timestamped "level load" marker to correlate
// against other diagnostics (e.g. the shadowfix mod's shadow-pass log)
// without needing an external process to poll for it.
static bool HasLevelArchiveExtension(const std::string& normLower) {
    return normLower.size() >= 4 &&
           (normLower.compare(normLower.size() - 4, 4, ".arc") == 0 ||
            normLower.compare(normLower.size() - 4, 4, ".bld") == 0);
}

static bool HasLevelArchiveExtension(const std::wstring& normLower) {
    return normLower.size() >= 4 &&
           (normLower.compare(normLower.size() - 4, 4, L".arc") == 0 ||
            normLower.compare(normLower.size() - 4, 4, L".bld") == 0);
}

static HANDLE WINAPI HookedCreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                        LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                                        DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    if (lpFileName) {
        std::string norm(lpFileName);
        for (char& c : norm) {
            if (c == '\\') c = '/';
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        size_t idx = norm.find(kSearchSegA);
        if (idx != std::string::npos) {
            if (HasLevelArchiveExtension(norm)) {
                Log("[LevelLoad] Opening: %s\n", lpFileName);
            }

            std::string candidate = std::string(lpFileName).substr(0, idx) + kReplaceSegA +
                                     std::string(lpFileName).substr(idx + sizeof(kSearchSegA) - 1);
            for (char& c : candidate) {
                if (c == '/') c = '\\';
            }

            DWORD attrs = GetFileAttributesA(candidate.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                Log("[modloader] Redirect: %s -> %s\n", lpFileName, candidate.c_str());
                return RealCreateFileA(candidate.c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                        dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
            }
        }
    }
    return RealCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition,
                            dwFlagsAndAttributes, hTemplateFile);
}

static HANDLE WINAPI HookedCreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                        LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                                        DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    if (lpFileName) {
        std::wstring norm(lpFileName);
        for (wchar_t& c : norm) {
            if (c == L'\\') c = L'/';
            c = static_cast<wchar_t>(std::tolower(static_cast<unsigned char>(c)));
        }

        size_t idx = norm.find(kSearchSegW);
        if (idx != std::wstring::npos) {
            if (HasLevelArchiveExtension(norm)) {
                char mb[MAX_PATH] = {0};
                WideCharToMultiByte(CP_ACP, 0, lpFileName, -1, mb, sizeof(mb), NULL, NULL);
                Log("[LevelLoad] Opening: %s\n", mb);
            }

            std::wstring candidate = std::wstring(lpFileName).substr(0, idx) + kReplaceSegW +
                                      std::wstring(lpFileName).substr(idx + (sizeof(kSearchSegW) / sizeof(wchar_t)) - 1);
            for (wchar_t& c : candidate) {
                if (c == L'/') c = L'\\';
            }

            DWORD attrs = GetFileAttributesW(candidate.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                char orig_mb[MAX_PATH] = {0};
                char cand_mb[MAX_PATH] = {0};
                WideCharToMultiByte(CP_ACP, 0, lpFileName, -1, orig_mb, sizeof(orig_mb), NULL, NULL);
                WideCharToMultiByte(CP_ACP, 0, candidate.c_str(), -1, cand_mb, sizeof(cand_mb), NULL, NULL);
                Log("[modloader] Redirect: %s -> %s\n", orig_mb, cand_mb);
                return RealCreateFileW(candidate.c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                        dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
            }
        }
    }
    return RealCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition,
                            dwFlagsAndAttributes, hTemplateFile);
}

void PatchFileRedirectHooksInModule(HMODULE module) {
    Mad2HookUtil_PatchIat(module, "kernel32.dll", "CreateFileA", reinterpret_cast<void*>(HookedCreateFileA));
    Mad2HookUtil_PatchIat(module, "kernel32.dll", "CreateFileW", reinterpret_cast<void*>(HookedCreateFileW));
}

void InitFileRedirect() {
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    RealCreateFileA = reinterpret_cast<CreateFileA_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateFileA")));
    RealCreateFileW = reinterpret_cast<CreateFileW_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateFileW")));
}

// Safe to call more than once (see loadlibrary_hook.cpp, which does
// exactly that every time a new module loads): patching an already-
// patched module just overwrites its IAT thunk with the same hook address
// again, a no-op.
void InstallFileRedirectHooks() {
    // Chain onto whatever was in the IAT before us -- the real kernel32
    // function normally, or another mod's hook if it patched these
    // modules first (e.g. mad2saveloader, if it happens to load before
    // us). Keeps multiple file-hooking mods composable regardless of
    // mad2/mods/ load order, instead of the later one clobbering the
    // earlier one's hook outright.
    void* prevA = nullptr;
    void* prevW = nullptr;
    int a = Mad2HookUtil_PatchIatAllModules("kernel32.dll", "CreateFileA", reinterpret_cast<void*>(HookedCreateFileA), &prevA);
    int w = Mad2HookUtil_PatchIatAllModules("kernel32.dll", "CreateFileW", reinterpret_cast<void*>(HookedCreateFileW), &prevW);
    if (prevA) RealCreateFileA = reinterpret_cast<CreateFileA_t>(prevA);
    if (prevW) RealCreateFileW = reinterpret_cast<CreateFileW_t>(prevW);
    if (a > 0 || w > 0) {
        Log("[modloader] CreateFileA hooked in %d new module(s), CreateFileW hooked in %d new module(s)\n", a, w);
    }
}
