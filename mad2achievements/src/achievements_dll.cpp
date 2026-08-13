// Achievement registry + dedicated-file persistence + unlock-toast overlay
// (mad2/mods/mad2achievements.dll) -- see include/mad2achievements_api.h for
// the full client contract. Same "registry, not owner" role as
// mad2effects.dll: this file holds no game-specific detection logic at all,
// just bookkeeping, an unlock flag per achievement, and a small D3D9
// overlay that shows a toast when Unlock() actually changes something.
// mad2achievementtracker.dll is the first (and so far only) detector
// calling Unlock().
//
// Persistence is a small dedicated file, "<exe dir>achievements.dat" --
// NOT config.cfg. Achievement unlocks are save-progress data, not a
// tunable setting, and don't belong interleaved with mod configuration
// (mad2config.dll's own file, meant for hand-editable settings with seeded
// comments) -- same reasoning that already gives mad2igttimer's IGT/RTA/
// Playtime state its own dedicated files instead of stuffing them into
// config.cfg. Format: one unlocked achievement name per line, plain text,
// append-only. Loaded once at DllMain (this file is exclusively ours, no
// "might not be loaded yet" dependency the way mad2config.dll has), so
// every Register/Unlock/IsUnlocked/GetInfo call below is a synchronous,
// already-in-memory lookup -- no lazy per-entry sync, no cross-DLL call
// needed at all for persistence.
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../include/mad2achievements_api.h"
#include "../../mad2textrenderer/include/mad2textrenderer_api.h"

#include "../../mad2sharedlog/mad2sharedlog.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2achievements]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Dedicated persistence file -- see file header for why this isn't
// config.cfg. Read once (LoadUnlockedNames, from DllMain, before any
// other thread in this DLL can be running yet); appended to (one line)
// on every fresh unlock.
// ---------------------------------------------------------------------

static std::string g_FilePath;  // "<exe dir>achievements.dat"
static std::vector<std::string> g_UnlockedNames;

static void InitFilePath() {
    char exePathA[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exePathA, MAX_PATH);
    std::string exeDir(exePathA);
    size_t slash = exeDir.find_last_of("\\/");
    if (slash != std::string::npos) exeDir.resize(slash + 1);
    g_FilePath = exeDir + "achievements.dat";
}

static void LoadUnlockedNames() {
    FILE* f = fopen(g_FilePath.c_str(), "r");
    if (!f) return;  // no file yet -- nothing unlocked so far, not an error
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len > 0) g_UnlockedNames.emplace_back(line);
    }
    fclose(f);
    Log("Loaded %zu unlocked achievement(s) from %s\n", g_UnlockedNames.size(), g_FilePath.c_str());
}

static void AppendUnlockedNameToFile(const std::string& name) {
    FILE* f = fopen(g_FilePath.c_str(), "a");
    if (!f) {
        Log("Failed to open %s for append -- unlock will not survive a restart\n", g_FilePath.c_str());
        return;
    }
    fprintf(f, "%s\n", name.c_str());
    fclose(f);
}

// ---------------------------------------------------------------------
// Registry.
// ---------------------------------------------------------------------

struct AchievementEntry {
    std::string name;
    std::string displayName;
    std::string description;
    bool unlocked = false;
};

static std::vector<AchievementEntry> g_Achievements;
static CRITICAL_SECTION g_Lock;

// Caller must hold g_Lock.
static int FindIndexLocked(const char* name) {
    for (size_t i = 0; i < g_Achievements.size(); ++i) {
        if (_stricmp(g_Achievements[i].name.c_str(), name) == 0) return static_cast<int>(i);
    }
    return -1;
}

// Caller must hold g_Lock. g_UnlockedNames is populated once at startup and
// only ever grows, so this is a plain read, no locking of its own needed.
static bool WasUnlockedAtStartupOrSinceLocked(const char* name) {
    for (auto& n : g_UnlockedNames) {
        if (_stricmp(n.c_str(), name) == 0) return true;
    }
    return false;
}

static void ArmToast(const std::string& displayName);  // forward decl, defined near the overlay below

static void FillInfo(const AchievementEntry& e, Mad2AchievementInfo& out) {
    snprintf(out.name, sizeof(out.name), "%s", e.name.c_str());
    snprintf(out.displayName, sizeof(out.displayName), "%s", e.displayName.c_str());
    snprintf(out.description, sizeof(out.description), "%s", e.description.c_str());
    out.unlocked = e.unlocked;
}

extern "C" __declspec(dllexport) BOOL WINAPI Mad2Achievements_Register(const Mad2AchievementDesc* desc) {
    if (!desc || !desc->name || !*desc->name) return FALSE;

    EnterCriticalSection(&g_Lock);
    int idx = FindIndexLocked(desc->name);
    if (idx < 0) {
        g_Achievements.emplace_back();
        idx = static_cast<int>(g_Achievements.size()) - 1;
    }
    AchievementEntry& e = g_Achievements[idx];
    e.name = desc->name;
    e.displayName = desc->displayName ? desc->displayName : desc->name;
    e.description = desc->description ? desc->description : "";
    e.unlocked = WasUnlockedAtStartupOrSinceLocked(desc->name);
    LeaveCriticalSection(&g_Lock);

    Log("Registered achievement '%s' (display='%s', unlocked=%d)\n", desc->name, e.displayName.c_str(), e.unlocked);
    return TRUE;
}

extern "C" __declspec(dllexport) int WINAPI Mad2Achievements_GetCount(void) {
    EnterCriticalSection(&g_Lock);
    int count = static_cast<int>(g_Achievements.size());
    LeaveCriticalSection(&g_Lock);
    return count;
}

extern "C" __declspec(dllexport) BOOL WINAPI Mad2Achievements_GetInfo(int index, Mad2AchievementInfo* outInfo) {
    if (!outInfo) return FALSE;
    EnterCriticalSection(&g_Lock);
    if (index < 0 || index >= static_cast<int>(g_Achievements.size())) {
        LeaveCriticalSection(&g_Lock);
        return FALSE;
    }
    FillInfo(g_Achievements[index], *outInfo);
    LeaveCriticalSection(&g_Lock);
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI Mad2Achievements_IsUnlocked(const char* name) {
    if (!name) return FALSE;
    EnterCriticalSection(&g_Lock);
    int idx = FindIndexLocked(name);
    BOOL unlocked = (idx >= 0) ? (g_Achievements[idx].unlocked ? TRUE : FALSE) : FALSE;
    LeaveCriticalSection(&g_Lock);
    return unlocked;
}

extern "C" __declspec(dllexport) void WINAPI Mad2Achievements_Unlock(const char* name) {
    if (!name) return;

    EnterCriticalSection(&g_Lock);
    int idx = FindIndexLocked(name);
    if (idx < 0) {
        LeaveCriticalSection(&g_Lock);
        Log("Unlock('%s') requested but no such achievement is registered\n", name);
        return;
    }
    if (g_Achievements[idx].unlocked) {
        LeaveCriticalSection(&g_Lock);
        return;  // already unlocked -- no-op
    }
    g_Achievements[idx].unlocked = true;
    std::string displayName = g_Achievements[idx].displayName;
    g_UnlockedNames.emplace_back(name);
    LeaveCriticalSection(&g_Lock);

    AppendUnlockedNameToFile(name);

    Log("Unlocked achievement '%s' (display='%s')\n", name, displayName.c_str());
    ArmToast(displayName);
}

// ---------------------------------------------------------------------
// Unlock-toast overlay: standard D3D9 IAT/vtable-hook chain (see
// mad2effectshud/src/effectshud.cpp for the identical technique this
// mirrors), drawing via mad2textrenderer.dll. Ordinary alphabetical load
// position is fine here (no zz_/aab_ prefix needed) -- "mad2achievements"
// sorts well before "zz_mad2graphicseffectmod", so per CLAUDE.md's "D3D9
// overlay hooking" section this mod always loads (and hooks) earlier than
// that mod, meaning it always draws AFTER graphicseffectmod's backbuffer
// capture/warp each frame -- safe from being captured, same guarantee
// every other non-zz_-prefixed overlay mod already relies on.
// ---------------------------------------------------------------------

struct PendingToast {
    std::string displayName;
};
static std::mutex g_ToastQueueMutex;
static std::vector<PendingToast> g_ToastQueue;

static void ArmToast(const std::string& displayName) {
    std::lock_guard<std::mutex> lock(g_ToastQueueMutex);
    g_ToastQueue.push_back({displayName});
}

static const DWORD kToastDurationMs = 4500;
static const DWORD kToastFadeMs = 600;  // fade-in/out portion of the duration above

static bool g_ToastActive = false;
static std::string g_ToastText;
static ULONGLONG g_ToastStartTick = 0;

static Mad2TextSurface g_ToastSurface = nullptr;
static bool g_ToastTextureDirty = true;

static bool EnsureToastFont() {
    if (g_ToastSurface) return true;
    const auto& api = Mad2TextRenderer_Resolve();
    if (!api.CreateSurface) return false;
    g_ToastSurface = api.CreateSurface(28);
    return g_ToastSurface != nullptr;
}

// Advances the toast state machine: pulls a new toast off the queue if
// nothing is currently showing and one is pending, and expires the current
// one after kToastDurationMs.
static void PumpToastQueue() {
    ULONGLONG now = GetTickCount64();
    if (g_ToastActive && (now - g_ToastStartTick) >= kToastDurationMs) {
        g_ToastActive = false;
    }
    if (!g_ToastActive) {
        std::lock_guard<std::mutex> lock(g_ToastQueueMutex);
        if (!g_ToastQueue.empty()) {
            g_ToastText = "Achievement Unlocked: " + g_ToastQueue.front().displayName;
            g_ToastQueue.erase(g_ToastQueue.begin());
            g_ToastActive = true;
            g_ToastStartTick = now;
            g_ToastTextureDirty = true;
        }
    }
}

// 0..255, ramping in/out at the head/tail of the toast's lifetime.
static BYTE ToastAlpha() {
    ULONGLONG elapsed = GetTickCount64() - g_ToastStartTick;
    if (elapsed < kToastFadeMs) return static_cast<BYTE>(255 * elapsed / kToastFadeMs);
    ULONGLONG remaining = (elapsed < kToastDurationMs) ? (kToastDurationMs - elapsed) : 0;
    if (remaining < kToastFadeMs) return static_cast<BYTE>(255 * remaining / kToastFadeMs);
    return 255;
}

typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
static EndScene_t RealEndScene = nullptr;

static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* This) {
    PumpToastQueue();

    if (g_ToastActive && EnsureToastFont()) {
        const auto& api = Mad2TextRenderer_Resolve();
        if (g_ToastTextureDirty) {
            int w = api.MeasureTextWidth(g_ToastSurface, g_ToastText.c_str());
            int lineH = api.GetLineHeight(g_ToastSurface);
            int padding = 10;
            if (api.EnsureSize(g_ToastSurface, This, w + padding * 2, lineH + padding)) {
                Mad2TextRun run{padding, padding / 2, g_ToastText.c_str(), RGB(255, 215, 90)};
                api.Render(g_ToastSurface, &run, 1);
                g_ToastTextureDirty = false;
            }
        }

        int texW = 0, texH = 0;
        api.GetSize(g_ToastSurface, &texW, &texH);
        if (texW > 0 && texH > 0) {
            D3DVIEWPORT9 vp{};
            This->GetViewport(&vp);
            float x = (static_cast<float>(vp.Width) - texW) * 0.5f;
            float y = static_cast<float>(vp.Height) * 0.12f;

            BYTE alpha = ToastAlpha();
            DWORD tint = (static_cast<DWORD>(alpha) << 24) | 0x00FFFFFF;

            Mad2TextRendererSavedState saved;
            api.SaveState(This, &saved);
            IDirect3DTexture9* tex = api.GetTexture(g_ToastSurface);
            if (tex) api.DrawTexturedQuad(This, tex, x, y, static_cast<float>(texW), static_cast<float>(texH), 0.0f, 0.0f, 1.0f, 1.0f, tint);
            api.RestoreState(This, &saved);
        }
    }

    return RealEndScene(This);
}

typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static CreateDevice_t RealCreateDevice = nullptr;

static HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* This, UINT Adapter, D3DDEVTYPE DeviceType,
                                                    HWND hFocusWindow, DWORD BehaviorFlags,
                                                    D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                    IDirect3DDevice9** ppReturnedDeviceInterface) {
    HRESULT hr = RealCreateDevice(This, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
                                   ppReturnedDeviceInterface);
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface && !RealEndScene) {
        void* prevEndScene = nullptr;
        Mad2HookUtil_PatchVTableSlot(*ppReturnedDeviceInterface, 42, reinterpret_cast<void*>(HookEndScene), &prevEndScene);
        RealEndScene = reinterpret_cast<EndScene_t>(prevEndScene);
        Log("Hooked IDirect3DDevice9::EndScene (vtable[42], device=%p)\n", (void*)*ppReturnedDeviceInterface);
    }
    return hr;
}

typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT);
static Direct3DCreate9_t RealDirect3DCreate9 = nullptr;

static IDirect3D9* WINAPI HookDirect3DCreate9(UINT SDKVersion) {
    if (!RealDirect3DCreate9) {
        Log("ERROR: RealDirect3DCreate9 is null\n");
        return nullptr;
    }
    IDirect3D9* pReal = RealDirect3DCreate9(SDKVersion);
    if (!pReal) return pReal;

    if (!RealCreateDevice) {
        void* prevCreateDevice = nullptr;
        Mad2HookUtil_PatchVTableSlot(pReal, 16, reinterpret_cast<void*>(HookCreateDevice), &prevCreateDevice);
        RealCreateDevice = reinterpret_cast<CreateDevice_t>(prevCreateDevice);
        Log("Hooked IDirect3D9::CreateDevice (vtable[16], d3d9=%p)\n", (void*)pReal);
    }
    return pReal;
}

static void InstallHooks() {
    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (!hD3D9) {
        Log("ERROR: d3d9.dll not loaded yet; cannot hook Direct3DCreate9\n");
        return;
    }
    RealDirect3DCreate9 =
        reinterpret_cast<Direct3DCreate9_t>(reinterpret_cast<void*>(GetProcAddress(hD3D9, "Direct3DCreate9")));

    void* prev = nullptr;
    int n = Mad2HookUtil_PatchIatAllModules("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(HookDirect3DCreate9), &prev);
    if (prev) RealDirect3DCreate9 = reinterpret_cast<Direct3DCreate9_t>(prev);
    Log("Patched Direct3DCreate9 in %d module(s) (chained real=%p)\n", n, (void*)RealDirect3DCreate9);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            InitializeCriticalSection(&g_Lock);
            InitFilePath();
            LoadUnlockedNames();
            InstallHooks();
            Log("mad2achievements loaded.\n");
            break;
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}
