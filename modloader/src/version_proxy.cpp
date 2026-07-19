#include "version_proxy.h"

#include <windows.h>
#include <string>

#include "log.h"

// This DLL is deployed as version.dll so it gets loaded in place of the
// real one (Mad2.exe -> AWL.dll -> version.dll). Everything below just
// forwards to the genuine version.dll, resolved by full path from the
// system directory so we don't recursively load ourselves.

static HMODULE g_realVersion = nullptr;

static FARPROC g_GetFileVersionInfoA = nullptr;
static FARPROC g_GetFileVersionInfoExA = nullptr;
static FARPROC g_GetFileVersionInfoExW = nullptr;
static FARPROC g_GetFileVersionInfoSizeA = nullptr;
static FARPROC g_GetFileVersionInfoSizeExA = nullptr;
static FARPROC g_GetFileVersionInfoSizeExW = nullptr;
static FARPROC g_GetFileVersionInfoSizeW = nullptr;
static FARPROC g_GetFileVersionInfoW = nullptr;
static FARPROC g_VerFindFileA = nullptr;
static FARPROC g_VerFindFileW = nullptr;
static FARPROC g_VerInstallFileA = nullptr;
static FARPROC g_VerInstallFileW = nullptr;
static FARPROC g_VerLanguageNameA = nullptr;
static FARPROC g_VerLanguageNameW = nullptr;
static FARPROC g_VerQueryValueA = nullptr;
static FARPROC g_VerQueryValueW = nullptr;

void InitVersionProxy() {
    char sysDir[MAX_PATH] = {0};
    GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string realPath = std::string(sysDir) + "\\version.dll";

    g_realVersion = LoadLibraryA(realPath.c_str());
    if (!g_realVersion) {
        Log("[modloader] FATAL: failed to load real version.dll from %s (err=%lu)\n", realPath.c_str(), GetLastError());
        return;
    }
    Log("[modloader] Loaded real version.dll from %s\n", realPath.c_str());

    g_GetFileVersionInfoA = GetProcAddress(g_realVersion, "GetFileVersionInfoA");
    g_GetFileVersionInfoExA = GetProcAddress(g_realVersion, "GetFileVersionInfoExA");
    g_GetFileVersionInfoExW = GetProcAddress(g_realVersion, "GetFileVersionInfoExW");
    g_GetFileVersionInfoSizeA = GetProcAddress(g_realVersion, "GetFileVersionInfoSizeA");
    g_GetFileVersionInfoSizeExA = GetProcAddress(g_realVersion, "GetFileVersionInfoSizeExA");
    g_GetFileVersionInfoSizeExW = GetProcAddress(g_realVersion, "GetFileVersionInfoSizeExW");
    g_GetFileVersionInfoSizeW = GetProcAddress(g_realVersion, "GetFileVersionInfoSizeW");
    g_GetFileVersionInfoW = GetProcAddress(g_realVersion, "GetFileVersionInfoW");
    g_VerFindFileA = GetProcAddress(g_realVersion, "VerFindFileA");
    g_VerFindFileW = GetProcAddress(g_realVersion, "VerFindFileW");
    g_VerInstallFileA = GetProcAddress(g_realVersion, "VerInstallFileA");
    g_VerInstallFileW = GetProcAddress(g_realVersion, "VerInstallFileW");
    g_VerLanguageNameA = GetProcAddress(g_realVersion, "VerLanguageNameA");
    g_VerLanguageNameW = GetProcAddress(g_realVersion, "VerLanguageNameW");
    g_VerQueryValueA = GetProcAddress(g_realVersion, "VerQueryValueA");
    g_VerQueryValueW = GetProcAddress(g_realVersion, "VerQueryValueW");
}

extern "C" {

BOOL WINAPI GetFileVersionInfoA(LPCSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
    if (!g_GetFileVersionInfoA) return FALSE;
    return reinterpret_cast<BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID)>(g_GetFileVersionInfoA)(lptstrFilename, dwHandle, dwLen, lpData);
}

BOOL WINAPI GetFileVersionInfoExA(DWORD dwFlags, LPCSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
    if (!g_GetFileVersionInfoExA) return FALSE;
    return reinterpret_cast<BOOL(WINAPI*)(DWORD, LPCSTR, DWORD, DWORD, LPVOID)>(g_GetFileVersionInfoExA)(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData);
}

BOOL WINAPI GetFileVersionInfoExW(DWORD dwFlags, LPCWSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
    if (!g_GetFileVersionInfoExW) return FALSE;
    return reinterpret_cast<BOOL(WINAPI*)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID)>(g_GetFileVersionInfoExW)(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData);
}

DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR lptstrFilename, LPDWORD lpdwHandle) {
    if (!g_GetFileVersionInfoSizeA) return 0;
    return reinterpret_cast<DWORD(WINAPI*)(LPCSTR, LPDWORD)>(g_GetFileVersionInfoSizeA)(lptstrFilename, lpdwHandle);
}

DWORD WINAPI GetFileVersionInfoSizeExA(DWORD dwFlags, LPCSTR lpwstrFilename, LPDWORD lpdwHandle) {
    if (!g_GetFileVersionInfoSizeExA) return 0;
    return reinterpret_cast<DWORD(WINAPI*)(DWORD, LPCSTR, LPDWORD)>(g_GetFileVersionInfoSizeExA)(dwFlags, lpwstrFilename, lpdwHandle);
}

DWORD WINAPI GetFileVersionInfoSizeExW(DWORD dwFlags, LPCWSTR lpwstrFilename, LPDWORD lpdwHandle) {
    if (!g_GetFileVersionInfoSizeExW) return 0;
    return reinterpret_cast<DWORD(WINAPI*)(DWORD, LPCWSTR, LPDWORD)>(g_GetFileVersionInfoSizeExW)(dwFlags, lpwstrFilename, lpdwHandle);
}

DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR lptstrFilename, LPDWORD lpdwHandle) {
    if (!g_GetFileVersionInfoSizeW) return 0;
    return reinterpret_cast<DWORD(WINAPI*)(LPCWSTR, LPDWORD)>(g_GetFileVersionInfoSizeW)(lptstrFilename, lpdwHandle);
}

BOOL WINAPI GetFileVersionInfoW(LPCWSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
    if (!g_GetFileVersionInfoW) return FALSE;
    return reinterpret_cast<BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID)>(g_GetFileVersionInfoW)(lptstrFilename, dwHandle, dwLen, lpData);
}

DWORD WINAPI VerFindFileA(DWORD uFlags, LPSTR szFileName, LPSTR szWinDir, LPSTR szAppDir, LPSTR szCurDir,
                           PUINT lpuCurDirLen, LPSTR szDestDir, PUINT lpuDestDirLen) {
    if (!g_VerFindFileA) return 0;
    return reinterpret_cast<DWORD(WINAPI*)(DWORD, LPSTR, LPSTR, LPSTR, LPSTR, PUINT, LPSTR, PUINT)>(g_VerFindFileA)(
        uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen);
}

DWORD WINAPI VerFindFileW(DWORD uFlags, LPWSTR szFileName, LPWSTR szWinDir, LPWSTR szAppDir, LPWSTR szCurDir,
                           PUINT lpuCurDirLen, LPWSTR szDestDir, PUINT lpuDestDirLen) {
    if (!g_VerFindFileW) return 0;
    return reinterpret_cast<DWORD(WINAPI*)(DWORD, LPWSTR, LPWSTR, LPWSTR, LPWSTR, PUINT, LPWSTR, PUINT)>(g_VerFindFileW)(
        uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen);
}

DWORD WINAPI VerInstallFileA(DWORD uFlags, LPSTR szSrcFileName, LPSTR szDestFileName, LPSTR szSrcDir, LPSTR szDestDir,
                              LPSTR szCurDir, LPSTR szTmpFile, PUINT lpuTmpFileLen) {
    if (!g_VerInstallFileA) return 0;
    return reinterpret_cast<DWORD(WINAPI*)(DWORD, LPSTR, LPSTR, LPSTR, LPSTR, LPSTR, LPSTR, PUINT)>(g_VerInstallFileA)(
        uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen);
}

DWORD WINAPI VerInstallFileW(DWORD uFlags, LPWSTR szSrcFileName, LPWSTR szDestFileName, LPWSTR szSrcDir, LPWSTR szDestDir,
                              LPWSTR szCurDir, LPWSTR szTmpFile, PUINT lpuTmpFileLen) {
    if (!g_VerInstallFileW) return 0;
    return reinterpret_cast<DWORD(WINAPI*)(DWORD, LPWSTR, LPWSTR, LPWSTR, LPWSTR, LPWSTR, LPWSTR, PUINT)>(g_VerInstallFileW)(
        uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen);
}

DWORD WINAPI VerLanguageNameA(DWORD wLang, LPSTR szLang, DWORD cchLang) {
    if (!g_VerLanguageNameA) return 0;
    return reinterpret_cast<DWORD(WINAPI*)(DWORD, LPSTR, DWORD)>(g_VerLanguageNameA)(wLang, szLang, cchLang);
}

DWORD WINAPI VerLanguageNameW(DWORD wLang, LPWSTR szLang, DWORD cchLang) {
    if (!g_VerLanguageNameW) return 0;
    return reinterpret_cast<DWORD(WINAPI*)(DWORD, LPWSTR, DWORD)>(g_VerLanguageNameW)(wLang, szLang, cchLang);
}

BOOL WINAPI VerQueryValueA(LPCVOID pBlock, LPCSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen) {
    if (!g_VerQueryValueA) return FALSE;
    return reinterpret_cast<BOOL(WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT)>(g_VerQueryValueA)(pBlock, lpSubBlock, lplpBuffer, puLen);
}

BOOL WINAPI VerQueryValueW(LPCVOID pBlock, LPCWSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen) {
    if (!g_VerQueryValueW) return FALSE;
    return reinterpret_cast<BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT)>(g_VerQueryValueW)(pBlock, lpSubBlock, lplpBuffer, puLen);
}

}  // extern "C"
