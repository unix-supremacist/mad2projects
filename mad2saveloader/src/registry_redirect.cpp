#include "registry_redirect.h"

#include <windows.h>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <string>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "log.h"

typedef LONG(WINAPI* RegOpenKeyExA_t)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG(WINAPI* RegOpenKeyExW_t)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
typedef LONG(WINAPI* RegCreateKeyExA_t)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY,
                                         LPDWORD);
typedef LONG(WINAPI* RegCreateKeyExW_t)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY,
                                         LPDWORD);
typedef LONG(WINAPI* RegQueryValueExA_t)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG(WINAPI* RegQueryValueExW_t)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG(WINAPI* RegSetValueExA_t)(HKEY, LPCSTR, DWORD, DWORD, CONST BYTE*, DWORD);
typedef LONG(WINAPI* RegSetValueExW_t)(HKEY, LPCWSTR, DWORD, DWORD, CONST BYTE*, DWORD);
typedef LONG(WINAPI* RegCloseKey_t)(HKEY);

static RegOpenKeyExA_t RealRegOpenKeyExA = nullptr;
static RegOpenKeyExW_t RealRegOpenKeyExW = nullptr;
static RegCreateKeyExA_t RealRegCreateKeyExA = nullptr;
static RegCreateKeyExW_t RealRegCreateKeyExW = nullptr;
static RegQueryValueExA_t RealRegQueryValueExA = nullptr;
static RegQueryValueExW_t RealRegQueryValueExW = nullptr;
static RegSetValueExA_t RealRegSetValueExA = nullptr;
static RegSetValueExW_t RealRegSetValueExW = nullptr;
static RegCloseKey_t RealRegCloseKey = nullptr;

// The game reads/writes its UI language under
// HKLM\Software\Activision\Madagascar 2\Language (a plain "en"/"fr"/...
// string). That's awkward to find and edit, and gets reset if the prefix
// is ever recreated, so we intercept opens of that specific key and serve
// its value from mad2config.dll's shared config.cfg instead ([SaveLoader]
// Language) -- never touching the real registry for it. Any handle we hand
// back for that key is this sentinel value, never a real HKEY.
static const HKEY kVirtualKey = reinterpret_cast<HKEY>(static_cast<ULONG_PTR>(0x4D414432));  // "MAD2"

// Resolved lazily on first actual registry hook call, not from
// DllMain/InitRegistryRedirect -- see mad2config/include/mad2config_api.h
// for why: mad2/mods/*.dll load order isn't guaranteed, so mad2config.dll
// might not be loaded yet this early.
static bool g_ConfigApiWarned = false;
static std::string g_LegacyConfigPath;  // pre-mad2config "<exe dir>saves\registry.cfg", migration-only

static void InitLegacyConfigPath() {
    char exePathA[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exePathA, MAX_PATH);
    std::string exeDir(exePathA);
    size_t slash = exeDir.find_last_of("\\/");
    if (slash != std::string::npos) exeDir.resize(slash + 1);
    g_LegacyConfigPath = exeDir + "saves\\registry.cfg";
}

// One-time best-effort read of the value this mod used to keep in its own
// saves\registry.cfg, before it moved to mad2config's shared config.cfg.
// Only ever consulted as the seed default the first time [SaveLoader]
// Language is read via mad2config -- see Mad2Config_GetString's "read or
// seed default" semantics -- so an existing non-English selection isn't
// silently reset back to "en" on upgrade.
static std::string ReadLegacyLanguage() {
    std::ifstream in(g_LegacyConfigPath);
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.compare(0, eq, "Language") == 0) {
            std::string val = line.substr(eq + 1);
            while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();
            return val;
        }
    }
    return "en";
}

static std::string GetLanguage() {
    const auto& api = Mad2Config_Resolve();
    if (!api.GetString) {
        if (!g_ConfigApiWarned) {
            g_ConfigApiWarned = true;
            Log("[mad2saveloader] [Dependency] mad2config.dll not found or missing exports -- language "
                "selection will not persist, settings in config.cfg will be ignored (place mad2config.dll in "
                "mad2/mods/)\n");
        }
        return "en";
    }
    char buf[64];
    api.GetString("SaveLoader", "Language", ReadLegacyLanguage().c_str(),
                  "The game's UI language (e.g. en, fr, de, es, it, ru -- ru added for the Russian\n"
                  "localization install).\n"
                  "@select(en,fr,de,es,it,ru)",
                  buf, sizeof(buf));
    return std::string(buf);
}

static void SetLanguage(const std::string& value) {
    const auto& api = Mad2Config_Resolve();
    if (!api.SetString) return;
    api.SetString("SaveLoader", "Language", value.c_str());
}

// The demo install uses "Madagascar 2 Demo" as its key name instead of
// "Madagascar 2", hence checking both.
static const char* const kTargetKeys[] = {
    "software\\activision\\madagascar 2",
    "software\\activision\\madagascar 2 demo",
};

static bool IsTargetSubKeyA(LPCSTR sub) {
    if (!sub) return false;
    for (const char* kTarget : kTargetKeys) {
        size_t i = 0;
        for (; sub[i] != '\0' && kTarget[i] != '\0'; ++i) {
            if (std::tolower(static_cast<unsigned char>(sub[i])) != kTarget[i]) break;
        }
        if (sub[i] == '\0' && kTarget[i] == '\0') return true;
    }
    return false;
}

static bool IsTargetSubKeyW(LPCWSTR sub) {
    if (!sub) return false;
    for (const char* kTarget : kTargetKeys) {
        size_t i = 0;
        for (; sub[i] != L'\0' && kTarget[i] != '\0'; ++i) {
            if (std::towlower(sub[i]) != static_cast<wchar_t>(kTarget[i])) break;
        }
        if (sub[i] == L'\0' && kTarget[i] == '\0') return true;
    }
    return false;
}

static bool IsRootHKey(HKEY hKey) {
    return hKey == HKEY_LOCAL_MACHINE || hKey == HKEY_CURRENT_USER || hKey == HKEY_CLASSES_ROOT ||
           hKey == HKEY_USERS;
}

static bool IsLanguageValueA(LPCSTR name) { return name && _stricmp(name, "Language") == 0; }
static bool IsLanguageValueW(LPCWSTR name) { return name && _wcsicmp(name, L"Language") == 0; }

static LONG WINAPI HookedRegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired,
                                        PHKEY phkResult) {
    if (IsRootHKey(hKey) && IsTargetSubKeyA(lpSubKey)) {
        Log("[mad2saveloader] Redirect registry key: %s -> config.cfg [SaveLoader]\n", lpSubKey);
        if (phkResult) *phkResult = kVirtualKey;
        return ERROR_SUCCESS;
    }
    if (hKey == kVirtualKey) return ERROR_FILE_NOT_FOUND;
    return RealRegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult);
}

static LONG WINAPI HookedRegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired,
                                        PHKEY phkResult) {
    if (IsRootHKey(hKey) && IsTargetSubKeyW(lpSubKey)) {
        Log("[mad2saveloader] Redirect registry key (W) -> config.cfg [SaveLoader]\n");
        if (phkResult) *phkResult = kVirtualKey;
        return ERROR_SUCCESS;
    }
    if (hKey == kVirtualKey) return ERROR_FILE_NOT_FOUND;
    return RealRegOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult);
}

static LONG WINAPI HookedRegCreateKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass, DWORD dwOptions,
                                          REGSAM samDesired, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                                          PHKEY phkResult, LPDWORD lpdwDisposition) {
    if (IsRootHKey(hKey) && IsTargetSubKeyA(lpSubKey)) {
        Log("[mad2saveloader] Redirect registry key (create): %s -> config.cfg [SaveLoader]\n", lpSubKey);
        if (phkResult) *phkResult = kVirtualKey;
        if (lpdwDisposition) *lpdwDisposition = REG_OPENED_EXISTING_KEY;
        return ERROR_SUCCESS;
    }
    if (hKey == kVirtualKey) return ERROR_FILE_NOT_FOUND;
    return RealRegCreateKeyExA(hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes,
                                phkResult, lpdwDisposition);
}

static LONG WINAPI HookedRegCreateKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD Reserved, LPWSTR lpClass,
                                          DWORD dwOptions, REGSAM samDesired,
                                          LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult,
                                          LPDWORD lpdwDisposition) {
    if (IsRootHKey(hKey) && IsTargetSubKeyW(lpSubKey)) {
        Log("[mad2saveloader] Redirect registry key (create, W) -> config.cfg [SaveLoader]\n");
        if (phkResult) *phkResult = kVirtualKey;
        if (lpdwDisposition) *lpdwDisposition = REG_OPENED_EXISTING_KEY;
        return ERROR_SUCCESS;
    }
    if (hKey == kVirtualKey) return ERROR_FILE_NOT_FOUND;
    return RealRegCreateKeyExW(hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes,
                                phkResult, lpdwDisposition);
}

static LONG WINAPI HookedRegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType,
                                           LPBYTE lpData, LPDWORD lpcbData) {
    if (hKey == kVirtualKey) {
        if (!IsLanguageValueA(lpValueName)) return ERROR_FILE_NOT_FOUND;
        std::string val = GetLanguage();
        DWORD needed = static_cast<DWORD>(val.size() + 1);
        if (lpType) *lpType = REG_SZ;
        if (lpData) {
            if (!lpcbData || *lpcbData < needed) {
                if (lpcbData) *lpcbData = needed;
                return ERROR_MORE_DATA;
            }
            std::memcpy(lpData, val.c_str(), needed);
        }
        if (lpcbData) *lpcbData = needed;
        Log("[mad2saveloader] Registry read Language=%s\n", val.c_str());
        return ERROR_SUCCESS;
    }
    return RealRegQueryValueExA(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
}

static LONG WINAPI HookedRegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType,
                                           LPBYTE lpData, LPDWORD lpcbData) {
    if (hKey == kVirtualKey) {
        if (!IsLanguageValueW(lpValueName)) return ERROR_FILE_NOT_FOUND;
        std::string val = GetLanguage();
        std::wstring wval(val.begin(), val.end());
        DWORD needed = static_cast<DWORD>((wval.size() + 1) * sizeof(wchar_t));
        if (lpType) *lpType = REG_SZ;
        if (lpData) {
            if (!lpcbData || *lpcbData < needed) {
                if (lpcbData) *lpcbData = needed;
                return ERROR_MORE_DATA;
            }
            std::memcpy(lpData, wval.c_str(), needed);
        }
        if (lpcbData) *lpcbData = needed;
        Log("[mad2saveloader] Registry read Language=%s (W)\n", val.c_str());
        return ERROR_SUCCESS;
    }
    return RealRegQueryValueExW(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
}

static LONG WINAPI HookedRegSetValueExA(HKEY hKey, LPCSTR lpValueName, DWORD Reserved, DWORD dwType,
                                         CONST BYTE* lpData, DWORD cbData) {
    if (hKey == kVirtualKey) {
        if (IsLanguageValueA(lpValueName) && lpData) {
            std::string val(reinterpret_cast<const char*>(lpData), cbData > 0 ? cbData - 1 : 0);
            SetLanguage(val);
            Log("[mad2saveloader] Registry write Language=%s\n", val.c_str());
        }
        (void)dwType;
        return ERROR_SUCCESS;
    }
    return RealRegSetValueExA(hKey, lpValueName, Reserved, dwType, lpData, cbData);
}

static LONG WINAPI HookedRegSetValueExW(HKEY hKey, LPCWSTR lpValueName, DWORD Reserved, DWORD dwType,
                                         CONST BYTE* lpData, DWORD cbData) {
    if (hKey == kVirtualKey) {
        if (IsLanguageValueW(lpValueName) && lpData) {
            const wchar_t* wstr = reinterpret_cast<const wchar_t*>(lpData);
            size_t wlen = cbData / sizeof(wchar_t);
            if (wlen > 0 && wstr[wlen - 1] == L'\0') wlen--;
            std::wstring wval(wstr, wlen);
            std::string val(wval.begin(), wval.end());
            SetLanguage(val);
            Log("[mad2saveloader] Registry write Language=%s (W)\n", val.c_str());
        }
        (void)dwType;
        return ERROR_SUCCESS;
    }
    return RealRegSetValueExW(hKey, lpValueName, Reserved, dwType, lpData, cbData);
}

static LONG WINAPI HookedRegCloseKey(HKEY hKey) {
    if (hKey == kVirtualKey) return ERROR_SUCCESS;
    return RealRegCloseKey(hKey);
}

void InitRegistryRedirect() {
    InitLegacyConfigPath();

    HMODULE advapi32 = GetModuleHandleA("advapi32.dll");
    RealRegOpenKeyExA = reinterpret_cast<RegOpenKeyExA_t>(reinterpret_cast<void*>(GetProcAddress(advapi32, "RegOpenKeyExA")));
    RealRegOpenKeyExW = reinterpret_cast<RegOpenKeyExW_t>(reinterpret_cast<void*>(GetProcAddress(advapi32, "RegOpenKeyExW")));
    RealRegCreateKeyExA =
        reinterpret_cast<RegCreateKeyExA_t>(reinterpret_cast<void*>(GetProcAddress(advapi32, "RegCreateKeyExA")));
    RealRegCreateKeyExW =
        reinterpret_cast<RegCreateKeyExW_t>(reinterpret_cast<void*>(GetProcAddress(advapi32, "RegCreateKeyExW")));
    RealRegQueryValueExA =
        reinterpret_cast<RegQueryValueExA_t>(reinterpret_cast<void*>(GetProcAddress(advapi32, "RegQueryValueExA")));
    RealRegQueryValueExW =
        reinterpret_cast<RegQueryValueExW_t>(reinterpret_cast<void*>(GetProcAddress(advapi32, "RegQueryValueExW")));
    RealRegSetValueExA =
        reinterpret_cast<RegSetValueExA_t>(reinterpret_cast<void*>(GetProcAddress(advapi32, "RegSetValueExA")));
    RealRegSetValueExW =
        reinterpret_cast<RegSetValueExW_t>(reinterpret_cast<void*>(GetProcAddress(advapi32, "RegSetValueExW")));
    RealRegCloseKey = reinterpret_cast<RegCloseKey_t>(reinterpret_cast<void*>(GetProcAddress(advapi32, "RegCloseKey")));
}

void InstallRegistryRedirectHooks() {
    void* prev = nullptr;
    int totalPatched = 0;

#define CHAIN(importName, hook, target)                                                             \
    prev = nullptr;                                                                                  \
    totalPatched += Mad2HookUtil_PatchIatAllModules("advapi32.dll", importName, reinterpret_cast<void*>(hook), &prev); \
    if (prev) target = reinterpret_cast<decltype(target)>(prev);

    CHAIN("RegOpenKeyExA", HookedRegOpenKeyExA, RealRegOpenKeyExA)
    CHAIN("RegOpenKeyExW", HookedRegOpenKeyExW, RealRegOpenKeyExW)
    CHAIN("RegCreateKeyExA", HookedRegCreateKeyExA, RealRegCreateKeyExA)
    CHAIN("RegCreateKeyExW", HookedRegCreateKeyExW, RealRegCreateKeyExW)
    CHAIN("RegQueryValueExA", HookedRegQueryValueExA, RealRegQueryValueExA)
    CHAIN("RegQueryValueExW", HookedRegQueryValueExW, RealRegQueryValueExW)
    CHAIN("RegSetValueExA", HookedRegSetValueExA, RealRegSetValueExA)
    CHAIN("RegSetValueExW", HookedRegSetValueExW, RealRegSetValueExW)
    CHAIN("RegCloseKey", HookedRegCloseKey, RealRegCloseKey)

#undef CHAIN

    if (totalPatched > 0) {
        Log("[mad2saveloader] Registry redirect hooks installed in %d new module(s) for "
            "Software\\Activision\\Madagascar 2\n",
            totalPatched);
    }
}
