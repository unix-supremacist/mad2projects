// Shared live-settings registry (mad2/mods/mad2settings.dll) -- see
// include/mad2settings_api.h for the full client contract. Pure bookkeeping,
// same shape as mad2effects/src/effects_dll.cpp: this file never reads or
// writes through a registered valuePtr itself, and never calls onChanged --
// that's entirely mad2debugmenu.dll's job, on the render thread, right after
// a UI edit. This mod just stores/returns descriptors.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../include/mad2settings_api.h"

#include "../../mad2sharedlog/mad2sharedlog.h"

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2settings]", fmt, args);
    va_end(args);
}

struct SettingEntry {
    std::string category;
    std::string label;
    std::string configSection;
    std::string configKey;
    Mad2SettingType type = MAD2SETTING_BOOL;
    void* valuePtr = nullptr;
    float minValue = 0.0f, maxValue = 0.0f;
    Mad2Settings_ActionFn onChanged = nullptr;
    void* userdata = nullptr;
};

static std::vector<SettingEntry> g_Settings;
static CRITICAL_SECTION g_Lock;

static void FillInfo(const SettingEntry& e, Mad2SettingInfo& out) {
    snprintf(out.category, sizeof(out.category), "%s", e.category.c_str());
    snprintf(out.label, sizeof(out.label), "%s", e.label.c_str());
    snprintf(out.configSection, sizeof(out.configSection), "%s", e.configSection.c_str());
    snprintf(out.configKey, sizeof(out.configKey), "%s", e.configKey.c_str());
    out.type = e.type;
    out.valuePtr = e.valuePtr;
    out.minValue = e.minValue;
    out.maxValue = e.maxValue;
    out.onChanged = e.onChanged;
    out.userdata = e.userdata;
}

extern "C" __declspec(dllexport) BOOL WINAPI Mad2Settings_Register(const Mad2SettingDesc* desc) {
    if (!desc) return FALSE;
    bool isAction = desc->type == MAD2SETTING_ACTION;
    if (isAction && desc->valuePtr != nullptr) return FALSE;
    if (!isAction && desc->valuePtr == nullptr) return FALSE;

    SettingEntry e;
    e.category = desc->category ? desc->category : "";
    e.label = desc->label ? desc->label : "";
    e.configSection = desc->configSection ? desc->configSection : "";
    e.configKey = desc->configKey ? desc->configKey : "";
    e.type = desc->type;
    e.valuePtr = desc->valuePtr;
    e.minValue = desc->minValue;
    e.maxValue = desc->maxValue;
    e.onChanged = desc->onChanged;
    e.userdata = desc->userdata;

    EnterCriticalSection(&g_Lock);
    g_Settings.push_back(e);
    LeaveCriticalSection(&g_Lock);

    Log("Registered setting '%s / %s' (type=%d)\n", e.category.c_str(), e.label.c_str(), static_cast<int>(e.type));
    return TRUE;
}

extern "C" __declspec(dllexport) int WINAPI Mad2Settings_GetCount(void) {
    EnterCriticalSection(&g_Lock);
    int count = static_cast<int>(g_Settings.size());
    LeaveCriticalSection(&g_Lock);
    return count;
}

extern "C" __declspec(dllexport) BOOL WINAPI Mad2Settings_GetInfo(int index, Mad2SettingInfo* outInfo) {
    if (!outInfo) return FALSE;
    EnterCriticalSection(&g_Lock);
    if (index < 0 || index >= static_cast<int>(g_Settings.size())) {
        LeaveCriticalSection(&g_Lock);
        return FALSE;
    }
    FillInfo(g_Settings[index], *outInfo);
    LeaveCriticalSection(&g_Lock);
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            InitializeCriticalSection(&g_Lock);
            Log("mad2settings loaded.\n");
            break;
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}
