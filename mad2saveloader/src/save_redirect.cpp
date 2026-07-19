#include "save_redirect.h"

#include <windows.h>
#include <cctype>
#include <cstring>
#include <string>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "log.h"

typedef HANDLE(WINAPI* CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static CreateFileA_t RealCreateFileA = nullptr;
static CreateFileW_t RealCreateFileW = nullptr;

// The game stores saves under the Wine prefix's fake per-user Documents
// folder (...\Documents\Activision\Madagascar 2\save_gameN), and its
// graphics config under the fake per-user AppData\Local folder
// (...\AppData\Local\Activision\Madagascar 2\Config.xml). Both are
// awkward to find and get wiped if the prefix is ever recreated. Any path
// containing one of these segments (case-insensitively, either slash
// direction) is unconditionally rewritten to "<exe dir>\saves\..." --
// unlike mad2assetloader's asset override, this never falls back to the
// original path, since the whole point is for saves/config to always
// live next to the install.
//
// Note: Config.xml's CreateFile calls come from a plugin DLL that isn't
// a static import of Mad2.exe and gets LoadLibrary'd well after our
// one-shot hook install -- see loadlibrary_hook.h for how that's
// covered (hooking LoadLibrary itself to rehook immediately whenever a
// new module is loaded, rather than a one-time startup scan).
// The demo install uses "Madagascar 2 Demo" as its folder/key name
// instead of "Madagascar 2", hence the separate entries below.
static const char* const kSearchSegsA[] = {
    "documents/activision/madagascar 2/",
    "documents/activision/madagascar 2 demo/",
    "appdata/local/activision/madagascar 2/",
    "appdata/local/activision/madagascar 2 demo/",
};
static const wchar_t* const kSearchSegsW[] = {
    L"documents/activision/madagascar 2/",
    L"documents/activision/madagascar 2 demo/",
    L"appdata/local/activision/madagascar 2/",
    L"appdata/local/activision/madagascar 2 demo/",
};

static std::string g_ExeDirA;
static std::wstring g_ExeDirW;

static void InitExeDir() {
    char exePathA[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exePathA, MAX_PATH);
    g_ExeDirA.assign(exePathA);
    size_t slashA = g_ExeDirA.find_last_of("\\/");
    if (slashA != std::string::npos) g_ExeDirA.resize(slashA + 1);

    wchar_t exePathW[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePathW, MAX_PATH);
    g_ExeDirW.assign(exePathW);
    size_t slashW = g_ExeDirW.find_last_of(L"\\/");
    if (slashW != std::wstring::npos) g_ExeDirW.resize(slashW + 1);
}

// [SaveLoader] SaveDir from mad2config.dll (relative to the exe dir),
// resolved lazily on the first actual hooked CreateFile call rather than
// from DllMain/InitSaveRedirect -- see mad2config/include/mad2config_api.h
// for why: mad2/mods/*.dll load order isn't guaranteed, so mad2config.dll
// might not be loaded yet this early. By the time any hook installed by
// any mod actually fires, LoadMods() has already finished loading every
// mods/*.dll, so this is always safe here.
static std::string g_SaveDirA = "saves";
static std::wstring g_SaveDirW = L"saves";
static bool g_SaveDirResolved = false;

static void EnsureSaveDirResolved() {
    if (g_SaveDirResolved) return;
    g_SaveDirResolved = true;

    const auto& api = Mad2Config_Resolve();
    if (api.GetString) {
        char buf[MAX_PATH];
        if (api.GetString("SaveLoader", "SaveDir", "saves",
                           "Where saves and the graphics config live, relative to the game install directory.",
                           buf, sizeof(buf))) {
            g_SaveDirA.assign(buf);
            g_SaveDirW.assign(g_SaveDirA.begin(), g_SaveDirA.end());
        }
    } else {
        Log("[mad2saveloader] [Dependency] mad2config.dll not found or missing exports -- using built-in "
            "default SaveDir=saves, settings in config.cfg will be ignored (place mad2config.dll in "
            "mad2/mods/)\n");
    }

    CreateDirectoryA((g_ExeDirA + g_SaveDirA).c_str(), nullptr);
    Log("[mad2saveloader] SaveDir resolved to %s\n", (g_ExeDirA + g_SaveDirA).c_str());
}

// mkdir -p for the directory portion of `path` (a full file path). The
// redirected save path may include subfolders the game expects to
// create itself (e.g. per-profile folders), which CreateFile won't
// create on its own.
static void EnsureDirectoryRecursiveA(const std::string& dir) {
    if (dir.size() <= 3) return;  // stop at "C:\"
    DWORD attrs = GetFileAttributesA(dir.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) return;

    size_t parentEnd = dir.find_last_of('\\', dir.size() - 2);
    if (parentEnd != std::string::npos) {
        EnsureDirectoryRecursiveA(dir.substr(0, parentEnd + 1));
    }
    CreateDirectoryA(dir.c_str(), nullptr);
}

static void EnsureParentDirectoryA(const std::string& filePath) {
    size_t slash = filePath.find_last_of('\\');
    if (slash == std::string::npos) return;
    EnsureDirectoryRecursiveA(filePath.substr(0, slash + 1));
}

static void EnsureDirectoryRecursiveW(const std::wstring& dir) {
    if (dir.size() <= 3) return;
    DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) return;

    size_t parentEnd = dir.find_last_of(L'\\', dir.size() - 2);
    if (parentEnd != std::wstring::npos) {
        EnsureDirectoryRecursiveW(dir.substr(0, parentEnd + 1));
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

static void EnsureParentDirectoryW(const std::wstring& filePath) {
    size_t slash = filePath.find_last_of(L'\\');
    if (slash == std::wstring::npos) return;
    EnsureDirectoryRecursiveW(filePath.substr(0, slash + 1));
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

        for (const char* seg : kSearchSegsA) {
            size_t segLen = std::strlen(seg);
            size_t idx = norm.find(seg);
            if (idx == std::string::npos) continue;
            EnsureSaveDirResolved();
            std::string candidate = g_ExeDirA + g_SaveDirA + "\\" + std::string(lpFileName).substr(idx + segLen);
            for (char& c : candidate) {
                if (c == '/') c = '\\';
            }
            EnsureParentDirectoryA(candidate);
            Log("[mad2saveloader] Redirect: %s -> %s\n", lpFileName, candidate.c_str());
            return RealCreateFileA(candidate.c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                    dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
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

        for (const wchar_t* seg : kSearchSegsW) {
            size_t segLen = std::wcslen(seg);
            size_t idx = norm.find(seg);
            if (idx == std::wstring::npos) continue;
            EnsureSaveDirResolved();
            std::wstring candidate = g_ExeDirW + g_SaveDirW + L"\\" + std::wstring(lpFileName).substr(idx + segLen);
            for (wchar_t& c : candidate) {
                if (c == L'/') c = L'\\';
            }
            EnsureParentDirectoryW(candidate);

            char orig_mb[MAX_PATH] = {0};
            char cand_mb[MAX_PATH] = {0};
            WideCharToMultiByte(CP_ACP, 0, lpFileName, -1, orig_mb, sizeof(orig_mb), NULL, NULL);
            WideCharToMultiByte(CP_ACP, 0, candidate.c_str(), -1, cand_mb, sizeof(cand_mb), NULL, NULL);
            Log("[mad2saveloader] Redirect: %s -> %s\n", orig_mb, cand_mb);

            return RealCreateFileW(candidate.c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                    dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
        }
    }
    return RealCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition,
                            dwFlagsAndAttributes, hTemplateFile);
}

void InitSaveRedirect() {
    InitExeDir();

    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    RealCreateFileA = reinterpret_cast<CreateFileA_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateFileA")));
    RealCreateFileW = reinterpret_cast<CreateFileW_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateFileW")));

    // SaveDir (and the directory it names) is resolved lazily on first
    // actual redirect -- see EnsureSaveDirResolved above.
}

void InstallSaveRedirectHooks() {
    // Chain onto whatever was in the IAT before us -- the real kernel32
    // function normally, or another mod's hook (e.g. mad2assetloader) if
    // it patched these modules first. Keeps multiple file-hooking mods
    // composable regardless of mad2/mods/ load order.
    void* prevA = nullptr;
    void* prevW = nullptr;
    int a = Mad2HookUtil_PatchIatAllModules("kernel32.dll", "CreateFileA", reinterpret_cast<void*>(HookedCreateFileA), &prevA);
    int w = Mad2HookUtil_PatchIatAllModules("kernel32.dll", "CreateFileW", reinterpret_cast<void*>(HookedCreateFileW), &prevW);
    if (prevA) RealCreateFileA = reinterpret_cast<CreateFileA_t>(prevA);
    if (prevW) RealCreateFileW = reinterpret_cast<CreateFileW_t>(prevW);
    if (a > 0 || w > 0) {
        Log("[mad2saveloader] CreateFileA hooked in %d new module(s), CreateFileW hooked in %d new module(s)\n", a,
            w);
    }
}
