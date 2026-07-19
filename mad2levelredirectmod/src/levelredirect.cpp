// Level-load interception for Madagascar 2. Loaded by mad2modloader from
// mad2/mods/*.dll. Two responsibilities (see include/mad2levelredirect_api.h
// for the client-facing summary):
//
// 1. Level redirection. ../mad2rando5 is a level-logic randomizer: it
//    computes a Slot->Level mapping (which level's CONTENT should load
//    when the game asks for a given SLOT) and, unlike ../mad2mod's older
//    Go tool, doesn't rewrite or symlink any archive files itself -- it
//    just needs a way to get that mapping into the running game. This mod
//    is that mechanism: it reads level_redirects.txt (a flat `Slot=Level`
//    list, one per line, `#` comments and blank lines ignored -- written
//    by ../mad2rando5, see its -out flag) from the game exe dir at
//    startup, then intercepts the game's level archive opens and rewrites
//    the requested name before the real open.
//
//    IMPORTANT: this does NOT go through kernel32's CreateFileA/W, even
//    though that's the standard, well-precedented technique every other
//    file-redirecting mod in this repo uses (mad2assetloader,
//    mad2saveloader). Confirmed by direct instrumentation during
//    development: across multiple full sessions that demonstrably loaded
//    and played a level (ObjectShuffle's matched-object count jumping
//    from ~1 at the title screen to 20-30+ in-level is unambiguous), the
//    CreateFileA/W hook fired exactly once per session, at boot, and
//    never again -- the engine's level-archive streaming bypasses
//    kernel32's CreateFileA/W entirely. The actual interception point is
//    ntdll.dll's NtCreateFile -- the native NT API kernel32's own
//    CreateFileA/W is normally implemented on top of, but which a custom/
//    low-level I/O layer (exactly what a streaming asset system wants,
//    for control over caching/async behavior) can and evidently does
//    call directly. Same PatchIat/PatchIatAllModules IAT-patching
//    technique as everywhere else in this repo, just pointed at
//    ("ntdll.dll", "NtCreateFile") instead of ("kernel32.dll",
//    "CreateFileA"/"CreateFileW"). See HookedNtCreateFile below for the
//    one real wrinkle this introduces: NT paths can be relative to a
//    RootDirectory handle instead of fully qualified, so ParseArchivePath
//    (unlike its CreateFileA/W-era predecessor) has to tolerate a bare
//    "Name.arc" with no directory component at all. The CreateFileA/W
//    hooks are kept installed regardless, as a harmless (near-zero-cost,
//    called once at boot) defense-in-depth path in case some code path
//    does use them.
//
//    Composing with mad2assetloader is load-order-sensitive, and the
//    ordering it needs falls out of mod_loader.cpp's plain alphabetical
//    sort without any special naming trick: "mad2assetloader" sorts before
//    "mad2levelredirectmod", so mad2assetloader's hook installs first and
//    this mod's installs second, becoming the outer hook (the one the game
//    actually calls) for whichever of these two mechanisms mad2assetloader
//    itself turns out to also depend on. If this mod is ever renamed to
//    something that sorts before "mad2assetloader", that composition order
//    flips; there's nothing else enforcing it.
//
// 2. Current-level tracking. mad2gameeffectsmod's PlayerTeleport and
//    MangoMutate need to know which level's data to use (coordinates.yaml
//    is keyed by level, and MangoMutate is gated to levels that actually
//    have a mango counter). The obvious in-process source for "current
//    level" is ScriptInfoLib.dll's _levelLoadRequest pointer (see
//    project_documentation.md's "Level Transition system"), but that
//    pointer only holds a valid StringInfo pointer while a load is
//    actively in flight -- it resets to null/invalid once the load
//    completes, per ../mad2mod's own comment on the equivalent code path.
//    A mod that queries it outside that narrow transient window gets
//    nothing; in practice this was observed to work only intermittently
//    (one full session correctly reported 'IslandFever' throughout, an
//    otherwise-identical later session never got past 'None' despite
//    clearly being in a level -- see ObjectShuffle's object counts in
//    that session's gameeffects.log). This mod tracks current level a
//    different way that doesn't depend on catching a narrow window: every
//    level archive open (NtCreateFile, see above) IS the "level changed"
//    event, and it's automatically the actual CONTENT level rather than
//    the requested slot, so it's correct under redirection for free.
//    Exposed via Mad2LevelRedirect_GetCurrentLevel. mad2gameeffectsmod
//    prefers this over the ScriptInfoLib heuristic when available, and
//    falls back to the heuristic otherwise -- see its own file header.
//
// This mod works standalone even with no level_redirects.txt present
// (pure passthrough + tracking) -- randomization is opt-in, tracking is
// not.
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>

#include "../include/mad2levelredirect_api.h"

#include "../../mad2sharedlog/mad2sharedlog.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"

// ---------------------------------------------------------------------
// Logging -- this mod's Log() wrapper writes into the shared logs\mad2.log file (see mad2sharedlog.h), tagged so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2levelredirectmod]", fmt, args);
    va_end(args);
}

// IAT/vtable hooking now provided by aa_mad2hookutil.dll -- see
// ../../mad2hookutil/include/mad2hookutil_api.h. Resolved synchronously
// (not lazily) since aa_mad2hookutil.dll is guaranteed already loaded by
// the time this mod's own DllMain runs (see that header for why).

// ---------------------------------------------------------------------
// Mapping (level_redirects.txt).
// ---------------------------------------------------------------------

// Keyed lowercase (case-insensitive slot match); value preserves the
// mapping file's original casing for the level name, since that has to
// match the actual archive filenames on disk.
static std::unordered_map<std::string, std::string> g_Mapping;

static std::string ToLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

static std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Populated once here, before InstallHooks() below ever lets a
// CreateFileA/W call reach ResolveMapping() -- no lock needed for reads
// after startup.
static void LoadMapping() {
    std::ifstream in("level_redirects.txt");
    if (!in.is_open()) {
        Log("No level_redirects.txt found -- redirection inactive (current-level tracking still runs)\n");
        return;
    }

    int count = 0;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string slot = Trim(line.substr(0, eq));
        std::string level = Trim(line.substr(eq + 1));
        if (slot.empty() || level.empty()) continue;
        if (_stricmp(slot.c_str(), level.c_str()) == 0) continue;  // identity mapping -- nothing to do

        g_Mapping[ToLower(slot)] = level;
        ++count;
    }
    Log("Loaded %d level redirect(s) from level_redirects.txt\n", count);
}

// Returns the level whose content should actually load for `slot` --
// `slot` itself if it isn't in the mapping.
static const std::string& ResolveMapping(const std::string& slot) {
    auto it = g_Mapping.find(ToLower(slot));
    return it != g_Mapping.end() ? it->second : slot;
}

// ---------------------------------------------------------------------
// Current-level tracking.
// ---------------------------------------------------------------------

static CRITICAL_SECTION g_CurrentLevelLock;
static std::string g_CurrentLevel;  // empty until the first level archive is observed

static void SetCurrentLevel(const std::string& level) {
    EnterCriticalSection(&g_CurrentLevelLock);
    g_CurrentLevel = level;
    LeaveCriticalSection(&g_CurrentLevelLock);
}

extern "C" __declspec(dllexport) BOOL WINAPI Mad2LevelRedirect_GetCurrentLevel(char* outBuf, DWORD outBufSize) {
    if (!outBuf || outBufSize == 0) return FALSE;
    EnterCriticalSection(&g_CurrentLevelLock);
    bool has = !g_CurrentLevel.empty();
    if (has) {
        strncpy(outBuf, g_CurrentLevel.c_str(), outBufSize - 1);
        outBuf[outBufSize - 1] = '\0';
    }
    LeaveCriticalSection(&g_CurrentLevelLock);
    return has ? TRUE : FALSE;
}

// ---------------------------------------------------------------------
// Path parsing + diagnostics shared by every hook below.
// ---------------------------------------------------------------------

// Recognizes a level archive name: "<Name>.arc" or ".bld" (case-
// insensitive, either slash direction), not "global" (the shared,
// non-level archive -- see project_documentation.md's level-detection
// section for why that one is excluded). Unlike a plain CreateFileA/W
// path (always fully qualified), an NT path can be *relative to a
// RootDirectory handle* -- i.e. just "IslandFever.arc" with no directory
// component at all, the directory context living in a handle we never
// see as a string. So this deliberately does NOT require a
// "Content/Streams/win/" segment to be present: if there IS a directory
// component, it must look like a Streams/win path (avoids false-positive
// matches on an unrelated .arc file elsewhere, if one exists); if there
// ISN'T one, a bare "<Name>.arc"/".bld" is accepted on faith, since in
// practice this only comes from an NT open relative to a directory handle
// the engine already had open on the level archive folder. On match,
// splits `original` (preserving its own case and slash style) into the
// directory prefix (through the trailing slash, or empty for the bare
// case), the base name, and the extension.
static bool ParseArchivePath(const std::string& original, std::string& outDirPrefix, std::string& outBaseName,
                              std::string& outExt) {
    std::string norm = original;
    for (char& c : norm) {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    size_t dot = norm.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = norm.substr(dot);
    if (ext != ".arc" && ext != ".bld") return false;

    size_t lastSlash = norm.find_last_of('/');
    size_t nameStart = (lastSlash == std::string::npos) ? 0 : lastSlash + 1;
    if (dot < nameStart) return false;

    std::string base = norm.substr(nameStart, dot - nameStart);
    if (base.empty() || base == "global") return false;

    if (lastSlash != std::string::npos && norm.find("streams/win/") == std::string::npos) return false;

    outDirPrefix = original.substr(0, nameStart);
    outBaseName = original.substr(nameStart, dot - nameStart);
    outExt = original.substr(dot);
    return true;
}

// ---------------------------------------------------------------------
// CreateFileA/W hooks -- kept as a secondary/defense-in-depth path, but
// NOT the mechanism that actually catches level archive opens for this
// game; see this file's header comment for why.
// ---------------------------------------------------------------------

typedef HANDLE(WINAPI* CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static CreateFileA_t RealCreateFileA = nullptr;
static CreateFileW_t RealCreateFileW = nullptr;

static HANDLE WINAPI HookedCreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                        LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                                        DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    if (lpFileName) {
        std::string dirPrefix, baseName, ext;
        if (ParseArchivePath(lpFileName, dirPrefix, baseName, ext)) {
            const std::string& mappedLevel = ResolveMapping(baseName);
            SetCurrentLevel(mappedLevel);
            if (mappedLevel != baseName) {
                std::string candidate = dirPrefix + mappedLevel + ext;
                Log("[LevelLoad] Redirect (CreateFileA): %s -> %s\n", lpFileName, candidate.c_str());
                return RealCreateFileA(candidate.c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                        dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
            }
            Log("[LevelLoad] Opening (CreateFileA): %s\n", lpFileName);
        }
    }
    return RealCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition,
                            dwFlagsAndAttributes, hTemplateFile);
}

// Converts to narrow (CP_ACP) and reuses HookedCreateFileA's parsing/
// mapping logic rather than duplicating it in wide form -- level names are
// ASCII in practice (see project_documentation.md), and the only thing
// that actually needs to stay wide is the final path handed to the real
// W API. Unlike mad2assetloader's file_redirect.cpp (which does duplicate
// its logic per-encoding), there's no per-character wide-specific behavior
// here worth the duplication.
static HANDLE WINAPI HookedCreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                        LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                                        DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    if (lpFileName) {
        char narrow[MAX_PATH] = {0};
        WideCharToMultiByte(CP_ACP, 0, lpFileName, -1, narrow, sizeof(narrow), nullptr, nullptr);

        std::string dirPrefix, baseName, ext;
        if (ParseArchivePath(narrow, dirPrefix, baseName, ext)) {
            const std::string& mappedLevel = ResolveMapping(baseName);
            SetCurrentLevel(mappedLevel);
            if (mappedLevel != baseName) {
                std::string candidate = dirPrefix + mappedLevel + ext;
                wchar_t wcandidate[MAX_PATH] = {0};
                MultiByteToWideChar(CP_ACP, 0, candidate.c_str(), -1, wcandidate, MAX_PATH);
                Log("[LevelLoad] Redirect (CreateFileW): %s -> %s\n", narrow, candidate.c_str());
                return RealCreateFileW(wcandidate, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                        dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
            }
            Log("[LevelLoad] Opening (CreateFileW): %s\n", narrow);
        }
    }
    return RealCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition,
                            dwFlagsAndAttributes, hTemplateFile);
}

// ---------------------------------------------------------------------
// NtCreateFile hook -- the mechanism that actually catches level archive
// opens; see this file's header comment for how that was confirmed and
// why CreateFileA/W (above) doesn't. ObjectAttributes->ObjectName is a
// UNICODE_STRING (explicit Length in bytes, NOT necessarily NUL-
// terminated) that may be relative to ObjectAttributes->RootDirectory
// (a directory handle) instead of fully qualified -- ParseArchivePath is
// written to tolerate that (see its own comment).
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

static NTSTATUS NTAPI HookedNtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                                          POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
                                          PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
                                          ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer,
                                          ULONG EaLength) {
    if (ObjectAttributes && ObjectAttributes->ObjectName) {
        std::string path = UnicodeStringToNarrow(ObjectAttributes->ObjectName);
        if (!path.empty()) {
            std::string dirPrefix, baseName, ext;
            if (ParseArchivePath(path, dirPrefix, baseName, ext)) {
                const std::string& mappedLevel = ResolveMapping(baseName);
                SetCurrentLevel(mappedLevel);
                if (mappedLevel != baseName) {
                    std::string candidate = dirPrefix + mappedLevel + ext;
                    wchar_t wbuf[512] = {0};
                    int wn = MultiByteToWideChar(CP_ACP, 0, candidate.c_str(), static_cast<int>(candidate.size()),
                                                  wbuf, 511);
                    if (wn > 0) {
                        UNICODE_STRING newName;
                        newName.Length = static_cast<USHORT>(wn * sizeof(wchar_t));
                        newName.MaximumLength = static_cast<USHORT>(sizeof(wbuf));
                        newName.Buffer = wbuf;
                        // Shallow copy: keeps RootDirectory/SecurityDescriptor/Attributes from the
                        // original, only ObjectName is swapped -- wbuf/newName/newAttrs are all
                        // stack-local and only need to stay alive for this synchronous call.
                        OBJECT_ATTRIBUTES newAttrs = *ObjectAttributes;
                        newAttrs.ObjectName = &newName;
                        Log("[LevelLoad] Redirect (NtCreateFile): %s -> %s\n", path.c_str(), candidate.c_str());
                        return RealNtCreateFile(FileHandle, DesiredAccess, &newAttrs, IoStatusBlock, AllocationSize,
                                                 FileAttributes, ShareAccess, CreateDisposition, CreateOptions,
                                                 EaBuffer, EaLength);
                    }
                }
                Log("[LevelLoad] Opening (NtCreateFile): %s\n", path.c_str());
            }
        }
    }
    return RealNtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes,
                             ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
}

// Patches just one module (typically one just returned by LoadLibrary --
// see HookedLoadLibraryA/W/ExA/ExW below). Deliberately NOT a full
// InstallHooks() rescan: this used to call InstallHooks() again on every
// single module load, an O(n) Toolhelp32Snapshot + full IAT walk each
// time, which -- once mad2assetloader's own LoadLibrary hook did the same
// thing -- compounded into a measured mod-loading stall (mad2/mods/
// loading visibly stopped partway through). A freshly-returned module
// handle can't have been patched by anyone before this point (it didn't
// exist as a loaded module yet), so no prevFunc chaining is needed here --
// RealCreateFileA/W/RealNtCreateFile (set once in InstallHooks() below)
// are already the correct chain-through targets.
static void PatchModuleForCreateFile(HMODULE module) {
    Mad2HookUtil_PatchIat(module, "kernel32.dll", "CreateFileA", reinterpret_cast<void*>(HookedCreateFileA));
    Mad2HookUtil_PatchIat(module, "kernel32.dll", "CreateFileW", reinterpret_cast<void*>(HookedCreateFileW));
    Mad2HookUtil_PatchIat(module, "ntdll.dll", "NtCreateFile", reinterpret_cast<void*>(HookedNtCreateFile));
}

// One-time full scan (DllMain only -- see PatchModuleForCreateFile above
// for the per-new-module path used everywhere else).
static void InstallHooks() {
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    RealCreateFileA =
        reinterpret_cast<CreateFileA_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateFileA")));
    RealCreateFileW =
        reinterpret_cast<CreateFileW_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "CreateFileW")));
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    RealNtCreateFile =
        reinterpret_cast<NtCreateFile_t>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateFile")));

    // Chain onto whatever was in the IAT before us -- see this file's
    // header comment for why load order (specifically, loading after
    // mad2assetloader) matters for composing correctly with it.
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
        Log("Hooked CreateFileA in %d new module(s), CreateFileW in %d new module(s), NtCreateFile in %d new "
            "module(s)\n",
            a, w, nt);
    }
}

// ---------------------------------------------------------------------
// LoadLibrary hooks -- catch modules loaded AFTER this mod's own
// DllMain. This mod (like every IAT-hooking mod) installs its hooks by
// patching the IAT of every module loaded *so far*, enumerated via
// Toolhelp32Snapshot at InstallHooks()-call time. But mad2/mods/*.dll,
// including this one, load very early -- "before Mad2.exe's own entry
// point runs" (see the repo's CLAUDE.md) -- while the game's own engine
// DLLs (TfbHavokLibrary.dll, ScriptInfoLib.dll, ActorInfoLib.dll, and
// almost certainly whatever module actually opens level archives) load
// dynamically, well after that point. Without this, InstallHooks() above
// only ever sees the initial static-import module set and silently never
// catches the one CreateFileA/W call that actually matters -- which is
// exactly what was happening: zero "[LevelLoad]"-equivalent log activity
// even while a level was demonstrably loaded and played. Same technique
// as mad2saveloader/src/loadlibrary_hook.cpp (see that file for the
// original; this is a self-contained copy adapted to re-run InstallHooks
// instead of mad2saveloader's own install functions).
// ---------------------------------------------------------------------

typedef HMODULE(WINAPI* LoadLibraryA_t)(LPCSTR);
typedef HMODULE(WINAPI* LoadLibraryW_t)(LPCWSTR);
typedef HMODULE(WINAPI* LoadLibraryExA_t)(LPCSTR, HANDLE, DWORD);
typedef HMODULE(WINAPI* LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);

static LoadLibraryA_t RealLoadLibraryA = nullptr;
static LoadLibraryW_t RealLoadLibraryW = nullptr;
static LoadLibraryExA_t RealLoadLibraryExA = nullptr;
static LoadLibraryExW_t RealLoadLibraryExW = nullptr;

static HMODULE WINAPI HookedLoadLibraryA(LPCSTR lpLibFileName) {
    HMODULE h = RealLoadLibraryA(lpLibFileName);
    if (h) PatchModuleForCreateFile(h);
    return h;
}

static HMODULE WINAPI HookedLoadLibraryW(LPCWSTR lpLibFileName) {
    HMODULE h = RealLoadLibraryW(lpLibFileName);
    if (h) PatchModuleForCreateFile(h);
    return h;
}

static HMODULE WINAPI HookedLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE h = RealLoadLibraryExA(lpLibFileName, hFile, dwFlags);
    if (h) PatchModuleForCreateFile(h);
    return h;
}

static HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE h = RealLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (h) PatchModuleForCreateFile(h);
    return h;
}

static void InstallLoadLibraryHooks() {
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    RealLoadLibraryA =
        reinterpret_cast<LoadLibraryA_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryA")));
    RealLoadLibraryW =
        reinterpret_cast<LoadLibraryW_t>(reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryW")));
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

    Log("LoadLibrary hooks installed in %d module(s) (catches engine DLLs loaded later)\n", total);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            InitializeCriticalSection(&g_CurrentLevelLock);
            LoadMapping();
            InstallHooks();
            InstallLoadLibraryHooks();
            break;
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}
