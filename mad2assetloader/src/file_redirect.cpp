#include "file_redirect.h"

#include <windows.h>
#include <winternl.h>
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

// The actual "Streams/win -> Streams/mod" redirect policy, in one place so
// every hook below (CreateFileA, and the NtCreateFile hook further down)
// makes the same decision instead of each reimplementing the substring
// match + existence check. Operates on narrow (char*/std::string) paths --
// HookedCreateFileW keeps its own wide-string duplicate (see that
// function's comment for why), but the NtCreateFile hook's paths are
// already converted to narrow before reaching here, so this is the natural
// shared form. Returns true and fills `outCandidate` (backslash-separated,
// same case as `original` outside the substituted segment) only if a
// "Streams/win/" segment was found AND a real, non-directory file exists at
// the corresponding "Streams/mod/" location.
static bool ComputeRedirectCandidate(const std::string& original, std::string& outCandidate) {
    std::string norm = original;
    for (char& c : norm) {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    size_t idx = norm.find(kSearchSegA);
    if (idx == std::string::npos) return false;

    if (HasLevelArchiveExtension(norm)) {
        Log("[LevelLoad] Opening: %s\n", original.c_str());
    }

    std::string candidate =
        original.substr(0, idx) + kReplaceSegA + original.substr(idx + sizeof(kSearchSegA) - 1);
    for (char& c : candidate) {
        if (c == '/') c = '\\';
    }

    DWORD attrs = GetFileAttributesA(candidate.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;

    outCandidate = candidate;
    return true;
}

static HANDLE WINAPI HookedCreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                        LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                                        DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    if (lpFileName) {
        std::string candidate;
        if (ComputeRedirectCandidate(lpFileName, candidate)) {
            Log("[modloader] Redirect: %s -> %s\n", lpFileName, candidate.c_str());
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

// ---------------------------------------------------------------------
// NtCreateFile hook.
//
// CreateFileA/W (above) is NOT the whole story for this game: per
// mad2levelredirectmod's own confirmed finding (see its levelredirect.cpp
// file header), the engine's level-archive streaming bypasses kernel32's
// CreateFileA/W entirely after boot and opens .arc/.bld files straight
// through ntdll.dll's NtCreateFile instead -- which is why a working
// Streams/mod/*.bld override never got picked up even though the exact
// same substitution logic above already works for at least one non-level
// asset (Content/Streams/mod/global.arc). This hook gives this mod the
// same interception point mad2levelredirectmod already proved out, wired
// to THIS mod's own Streams/win->Streams/mod substitution (ComputeRedirectCandidate
// above) rather than that mod's unrelated slot/level-name mapping -- two
// independent features that just happen to need the same underlying hook.
// ---------------------------------------------------------------------

typedef NTSTATUS(NTAPI* NtCreateFile_t)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                         ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);

static NtCreateFile_t RealNtCreateFile = nullptr;

static std::string UnicodeStringToNarrow(const UNICODE_STRING* us) {
    if (!us || !us->Buffer || us->Length == 0) return "";
    int wlen = us->Length / sizeof(WCHAR);
    char buf[512] = {0};
    int n = WideCharToMultiByte(CP_ACP, 0, us->Buffer, wlen, buf, sizeof(buf) - 1, nullptr, nullptr);
    if (n <= 0) return "";
    return std::string(buf, static_cast<size_t>(n));
}

// Resolves an already-open directory HANDLE to its own absolute DOS path
// (e.g. "\\?\C:\...\mad2\Content\Streams\win") via GetFinalPathNameByHandleA
// -- a plain query against a handle the engine already opened, not a new
// permission requirement. Returns "" on failure.
static std::string ResolveDirectoryHandlePath(HANDLE dir) {
    char dirPath[900] = {0};
    // 0 == VOLUME_NAME_DOS | FILE_NAME_NORMALIZED, i.e. the defaults --
    // spelled out as a literal since neither symbol is worth an extra
    // header just to name a value that's already zero.
    DWORD n = GetFinalPathNameByHandleA(dir, dirPath, sizeof(dirPath), 0);
    if (n == 0 || n >= sizeof(dirPath)) return "";
    return std::string(dirPath);
}

// Converts an absolute DOS path -- either a plain "C:\..." one or
// GetFinalPathNameByHandleA's own "\\?\C:\..." form -- into the NT-native
// "\??\C:\..." form NtCreateFile's ObjectName needs once RootDirectory is
// cleared. These look similar but name different things in the NT object
// namespace: "\??" is the per-session DosDevices symlink directory NT
// paths actually use; the superficially similar "\\?\" is a Win32-subsystem
// convention kernel32 translates on the way in, which NtCreateFile itself
// (called directly, bypassing kernel32) does not understand.
static std::wstring ToNtPath(const std::string& dosPath) {
    std::string p = dosPath;
    if (p.compare(0, 4, "\\\\?\\") == 0) {
        p = p.substr(4);
    }
    std::string ntPath = "\\??\\" + p;
    wchar_t wbuf[600] = {0};
    int n = MultiByteToWideChar(CP_ACP, 0, ntPath.c_str(), -1, wbuf, 599);
    if (n <= 0) return L"";
    return std::wstring(wbuf);
}

static NTSTATUS NTAPI HookedNtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                                          POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
                                          PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
                                          ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer,
                                          ULONG EaLength) {
    if (ObjectAttributes && ObjectAttributes->ObjectName) {
        std::string rawName = UnicodeStringToNarrow(ObjectAttributes->ObjectName);
        if (!rawName.empty()) {
            std::string candidate;
            bool matched = ComputeRedirectCandidate(rawName, candidate);
            bool viaRootDir = false;

            // rawName had no "streams/win/" segment on its own. NT paths can
            // be relative to ObjectAttributes->RootDirectory (an already-open
            // directory handle) instead of fully qualified -- e.g. just
            // "VolcanoRave.bld" with no directory component at all, the
            // "Streams/win" context living entirely in that handle (see
            // mad2levelredirectmod/src/levelredirect.cpp's own comment on
            // this exact wrinkle -- confirmed for this engine's own level
            // archive opens). Resolve the handle and retry, but ONLY for
            // this narrow, empirically-relevant case (a bare level-archive
            // filename) -- not for every relative NtCreateFile in the
            // process, which would add a real per-open cost (an extra
            // kernel query) for the overwhelming majority of opens that have
            // nothing to do with Streams/win at all.
            if (!matched && ObjectAttributes->RootDirectory && rawName.find_first_of("\\/") == std::string::npos) {
                std::string rawLower = rawName;
                for (char& c : rawLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (HasLevelArchiveExtension(rawLower)) {
                    std::string resolvedDir = ResolveDirectoryHandlePath(ObjectAttributes->RootDirectory);
                    if (!resolvedDir.empty()) {
                        std::string fullPath = resolvedDir + "\\" + rawName;
                        Log("[LevelLoad] NtCreateFile (root-relative): %s (root=%s)\n", rawName.c_str(),
                            resolvedDir.c_str());
                        matched = ComputeRedirectCandidate(fullPath, candidate);
                        viaRootDir = matched;
                    }
                }
            }

            if (matched) {
                std::wstring ntPath;
                OBJECT_ATTRIBUTES newAttrs = *ObjectAttributes;
                if (viaRootDir) {
                    // candidate is a full absolute path we built from the
                    // resolved RootDirectory -- it needs the NT DosDevices
                    // prefix and no longer needs (or can use) RootDirectory,
                    // since it's a sibling directory ("mod") the original
                    // handle wasn't opened on.
                    ntPath = ToNtPath(candidate);
                    newAttrs.RootDirectory = nullptr;
                } else {
                    // candidate is still relative to whatever RootDirectory
                    // already was (commonly null/fully-qualified, same shape
                    // as the CreateFileA/W hooks above) -- just the wide form
                    // of the same substitution, RootDirectory untouched.
                    wchar_t wbuf[512] = {0};
                    int wn = MultiByteToWideChar(CP_ACP, 0, candidate.c_str(), -1, wbuf, 511);
                    if (wn > 0) ntPath = wbuf;
                }

                if (!ntPath.empty()) {
                    UNICODE_STRING newName;
                    newName.Length = static_cast<USHORT>(ntPath.size() * sizeof(wchar_t));
                    newName.MaximumLength = static_cast<USHORT>((ntPath.size() + 1) * sizeof(wchar_t));
                    newName.Buffer = const_cast<wchar_t*>(ntPath.c_str());
                    newAttrs.ObjectName = &newName;

                    Log("[modloader] Redirect (NtCreateFile): %s -> %s\n", rawName.c_str(), candidate.c_str());
                    return RealNtCreateFile(FileHandle, DesiredAccess, &newAttrs, IoStatusBlock, AllocationSize,
                                             FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer,
                                             EaLength);
                }
            }
        }
    }
    return RealNtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes,
                             ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
}

void PatchFileRedirectHooksInModule(HMODULE module) {
    Mad2HookUtil_PatchIat(module, "kernel32.dll", "CreateFileA", reinterpret_cast<void*>(HookedCreateFileA));
    Mad2HookUtil_PatchIat(module, "kernel32.dll", "CreateFileW", reinterpret_cast<void*>(HookedCreateFileW));
    Mad2HookUtil_PatchIat(module, "ntdll.dll", "NtCreateFile", reinterpret_cast<void*>(HookedNtCreateFile));
}

void InitFileRedirect() {
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    RealCreateFileA = reinterpret_cast<CreateFileA_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateFileA")));
    RealCreateFileW = reinterpret_cast<CreateFileW_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateFileW")));
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    RealNtCreateFile =
        reinterpret_cast<NtCreateFile_t>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateFile")));
}

// Safe to call more than once (see loadlibrary_hook.cpp, which does
// exactly that every time a new module loads): patching an already-
// patched module just overwrites its IAT thunk with the same hook address
// again, a no-op.
void InstallFileRedirectHooks() {
    // Chain onto whatever was in the IAT before us -- the real kernel32/
    // ntdll function normally, or another mod's hook if it patched these
    // modules first (e.g. mad2saveloader for CreateFileA/W, or
    // mad2levelredirectmod for NtCreateFile, if either happens to load
    // before us). Keeps multiple file-hooking mods composable regardless of
    // mad2/mods/ load order, instead of the later one clobbering the
    // earlier one's hook outright. See CLAUDE.md's "The IAT hooking
    // pattern" section for how this composes specifically with
    // mad2levelredirectmod's own NtCreateFile hook.
    void* prevA = nullptr;
    void* prevW = nullptr;
    void* prevNt = nullptr;
    int a = Mad2HookUtil_PatchIatAllModules("kernel32.dll", "CreateFileA", reinterpret_cast<void*>(HookedCreateFileA), &prevA);
    int w = Mad2HookUtil_PatchIatAllModules("kernel32.dll", "CreateFileW", reinterpret_cast<void*>(HookedCreateFileW), &prevW);
    int nt = Mad2HookUtil_PatchIatAllModules("ntdll.dll", "NtCreateFile", reinterpret_cast<void*>(HookedNtCreateFile), &prevNt);
    if (prevA) RealCreateFileA = reinterpret_cast<CreateFileA_t>(prevA);
    if (prevW) RealCreateFileW = reinterpret_cast<CreateFileW_t>(prevW);
    if (prevNt) RealNtCreateFile = reinterpret_cast<NtCreateFile_t>(prevNt);
    if (a > 0 || w > 0 || nt > 0) {
        Log("[modloader] CreateFileA hooked in %d new module(s), CreateFileW hooked in %d new module(s), "
            "NtCreateFile hooked in %d new module(s)\n",
            a, w, nt);
    }
}
