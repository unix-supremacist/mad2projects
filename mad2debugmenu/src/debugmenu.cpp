// In-game Nuklear debug menu (mad2/mods/aab_mad2debugmenu.dll) -- a single
// place to see/edit the live settings other mods register with
// mad2settings.dll (see mad2settings/include/mad2settings_api.h), trigger/
// clear mad2effects.dll's registered chaos effects, and view
// mad2achievements.dll's registered achievements, instead of memorizing a
// growing pile of independent per-mod hotkeys.
//
// D3D9 hook: same IAT Direct3DCreate9 -> vtable CreateDevice[16] ->
// EndScene[42] chain as every other overlay mod (see
// mad2effectshud/src/effectshud.cpp for the identical technique), plus a
// Reset[16] hook to rebuild Nuklear's device-dependent resources (font
// texture, state block) around a device Reset.
//
// Deployed as "aab_mad2debugmenu.dll" -- sorts immediately after
// "aa_mad2hookutil.dll" and before every other mod. Per CLAUDE.md's "D3D9
// overlay hooking" section, the EARLIEST-loaded D3D9-hooking mod ends up
// invoked LAST in the per-frame hook chain (closest to the real
// EndScene/Present) -- so this always draws on top of every other overlay
// AND is immune to zz_mad2graphicseffectmod's backbuffer-capture-and-warp
// (which runs first in the chain, well before this mod's own draw).
//
// Input capture is new territory for this repo (every other mod either
// polls GetAsyncKeyState or reads mad2xinput's cached state -- nothing
// hooks the window procedure). HookCreateDevice captures hFocusWindow
// (already passed into the real CreateDevice call, just unused elsewhere)
// and installs a GWLP_WNDPROC hook: while the menu is closed, every message
// is immediately chained through untouched (zero behavioral change, same
// as if this hook didn't exist). While the menu is open, mouse/keyboard
// messages are fed into Nuklear via the vendored backend's own
// nk_d3d9_handle_event and then swallowed (never reach the game) -- paired
// with a full XInput block on every controller slot via mad2xinput's
// layered-override system (the same "block Mad2's own input while
// something else owns it" precedent mad2companionmod's SM64/Jak1 effects
// already use) -- so opening the menu behaves like a clean pause, not
// double input.
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS  // must be defined before THIS translation unit's first
                     // <d3d9.h> include -- nuklear_d3d9.h's implementation
                     // section re-includes <d3d9.h> with COBJMACROS itself,
                     // but the header guard means that second inclusion is
                     // a no-op; the COBJMACROS-gated IDirect3DDevice9_Xxx
                     // macros it needs only exist if COBJMACROS was already
                     // defined the first time <d3d9.h> was ever parsed in
                     // this TU, which is the include right below.
#include <windows.h>
#include <d3d9.h>
#include <xinput.h>
#include <dinput.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../../mad2sharedlog/mad2sharedlog.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2xinput/include/mad2xinput_api.h"
#include "../../mad2settings/include/mad2settings_api.h"
#include "../../mad2effects/include/mad2effects_api.h"
#include "../../mad2achievements/include/mad2achievements_api.h"

// ---------------------------------------------------------------------
// Nuklear + its official D3D9 demo backend (vendored verbatim, see
// third_party/nuklear/ -- fixed-function only, no shaders, matching this
// repo's existing D3DX/shader-avoidance policy). Compiled directly into
// this translation unit, single-header-library style -- NK_IMPLEMENTATION/
// NK_D3D9_IMPLEMENTATION must only ever be defined in exactly one .cpp
// across the whole DLL, and this is it.
// ---------------------------------------------------------------------
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_IMPLEMENTATION
#include "../third_party/nuklear/nuklear.h"
#define NK_D3D9_IMPLEMENTATION
#include "../third_party/nuklear/nuklear_d3d9.h"

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2debugmenu]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config (mad2config.dll's [DebugMenu] section), resolved lazily on the
// first EndScene call like every other overlay mod.
// ---------------------------------------------------------------------

struct Config {
    int toggleVk = 0xC0;  // VK_OEM_3 -- grave/tilde, the conventional debug-console key
};
static Config g_Config;
static bool g_ConfigLoaded = false;

static void EnsureConfigLoaded() {
    if (g_ConfigLoaded) return;
    g_ConfigLoaded = true;
    const auto& api = Mad2Config_Resolve();
    if (!api.GetVirtualKey) {
        Log("[Dependency] mad2config.dll not found or missing exports -- using built-in default toggle key\n");
        return;
    }
    g_Config.toggleVk = api.GetVirtualKey(
        "DebugMenu", "ToggleKey", g_Config.toggleVk,
        "Keyboard key that opens/closes the in-game debug menu (settings, chaos effects,\n"
        "achievements). Default is the grave/tilde key. Virtual-key code name with the VK_\n"
        "prefix stripped (e.g. F9), or a 0xNN hex code.\n"
        "See https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes");
    Log("Config loaded: toggleVk=0x%X\n", g_Config.toggleVk);
}

// ---------------------------------------------------------------------
// Menu open/closed state -- gates both WndProc's input swallowing and
// whether the render hook builds/draws anything at all.
// ---------------------------------------------------------------------

static std::atomic<bool> g_MenuOpen{false};
static Mad2XInputOverrideHandle g_XInputBlockHandles[XUSER_MAX_COUNT] = {0, 0, 0, 0};
static HWND g_GameHwnd = nullptr;  // captured in InstallWndProcHook, reused here for ClipCursor

static void SetMenuOpen(bool open) {
    if (g_MenuOpen.exchange(open) == open) return;
    const auto& xinput = Mad2XInput_Resolve();
    if (open) {
        if (xinput.AddOverride) {
            Mad2XInputOverride block{};
            block.forceBlockButtons = 0xFFFF;
            for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) g_XInputBlockHandles[i] = xinput.AddOverride(i, &block);
        }
        // Best-effort extra layer on top of the DirectInput mouse hook
        // below and WndProc's WM_MOUSE*/WM_KEY* swallow: confines the OS
        // cursor to the game window so anything reading raw screen-space
        // cursor position (GetCursorPos) rather than device deltas stays
        // sane while the menu covers the screen.
        if (g_GameHwnd) {
            RECT rect{};
            if (GetWindowRect(g_GameHwnd, &rect)) ClipCursor(&rect);
        }
        Log("Menu opened\n");
    } else {
        if (xinput.RemoveOverride) {
            for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
                if (g_XInputBlockHandles[i]) xinput.RemoveOverride(g_XInputBlockHandles[i]);
                g_XInputBlockHandles[i] = 0;
            }
        }
        ClipCursor(nullptr);
        Log("Menu closed\n");
    }
}

// ---------------------------------------------------------------------
// WndProc hook -- see file header. Only these message classes are ever
// swallowed, and only while the menu is open; everything else (WM_SIZE,
// WM_ACTIVATE, WM_PAINT, ...) always passes straight through unconditionally.
// ---------------------------------------------------------------------

static WNDPROC RealWndProc = nullptr;

static bool IsInputMessage(UINT msg) {
    switch (msg) {
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        case WM_MOUSEWHEEL: case WM_MOUSEMOVE:
            return true;
        default:
            return false;
    }
}

// The window's message pump isn't guaranteed to run on the same thread as
// EndScene (nothing in this codebase establishes that the two are the same
// thread for this game) -- so WndProc must NOT call directly into Nuklear
// (nk_d3d9_handle_event mutates the shared nk_context with no locking of
// its own). Instead every input message is queued here and drained/applied
// from HookEndScene, which is always the same thread nk_d3d9_render runs
// on, right before nk_input_end -- see HookEndScene below.
struct QueuedInputEvent {
    HWND hwnd;
    UINT msg;
    WPARAM wparam;
    LPARAM lparam;
};
static CRITICAL_SECTION g_InputQueueLock;
static std::vector<QueuedInputEvent> g_InputQueue;

static LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (g_MenuOpen.load() && IsInputMessage(msg)) {
        EnterCriticalSection(&g_InputQueueLock);
        g_InputQueue.push_back({hwnd, msg, wparam, lparam});
        LeaveCriticalSection(&g_InputQueueLock);
        return 0;  // swallowed -- never reaches the game while the menu is open
    }
    return CallWindowProcA(RealWndProc, hwnd, msg, wparam, lparam);
}

// Called from HookEndScene, on the render thread -- the only place
// nk_d3d9_handle_event (and therefore any nk_input_* call) ever runs.
static void DrainInputQueue() {
    EnterCriticalSection(&g_InputQueueLock);
    std::vector<QueuedInputEvent> events;
    events.swap(g_InputQueue);
    LeaveCriticalSection(&g_InputQueueLock);
    for (auto& e : events) nk_d3d9_handle_event(e.hwnd, e.msg, e.wparam, e.lparam);
}

static void InstallWndProcHook(HWND hwnd) {
    if (RealWndProc || !hwnd) return;
    g_GameHwnd = hwnd;
    RealWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc)));
    Log("Installed WndProc hook (hwnd=%p)\n", (void*)hwnd);
}

// ---------------------------------------------------------------------
// Rebind capture for MAD2SETTING_VKEY entries: no WndProc dependency --
// just scans GetAsyncKeyState every frame while armed, same polling style
// every other mod's hotkey detection already uses. Armed by clicking a
// setting's "Rebind" button; VK_ESCAPE cancels without changing anything;
// any other freshly-pressed key (that isn't a mouse button already driving
// the menu itself) becomes the new binding.
// ---------------------------------------------------------------------

static void* g_CapturingValuePtr = nullptr;  // non-null while armed for a VKEY rebind (see RenderSettingRow)
// Same rebind-capture idea as g_CapturingValuePtr, for a Raw Config vkey
// entry -- there's no live pointer to write through there, just a
// "section\x01key" map key to persist via Mad2Config_SetString once a key
// is captured. Mutually exclusive with g_CapturingValuePtr in practice
// (only one Rebind button can be clicked at a time).
static std::string g_CapturingRawConfigKey;

static int ScanForFreshKeyPress() {
    for (int vk = 1; vk < 255; ++vk) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;  // menu itself uses these
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    }
    return 0;
}

// ---------------------------------------------------------------------
// Small display helpers.
// ---------------------------------------------------------------------

static std::string VkKeyName(int vk) {
    if (vk == 0) return "(unbound)";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) {
        char buf[8];
        snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1);
        return buf;
    }
    switch (vk) {
        case VK_SPACE: return "SPACE";
        case VK_RETURN: return "ENTER";
        case VK_ESCAPE: return "ESCAPE";
        case VK_TAB: return "TAB";
        case VK_BACK: return "BACKSPACE";
        case VK_OEM_3: return "`";
        default: {
            char buf[8];
            snprintf(buf, sizeof(buf), "0x%02X", vk);
            return buf;
        }
    }
}

struct NamedButton {
    const char* name;
    WORD mask;
};
static const NamedButton kNamedButtons[] = {
    {"DPAD_UP", XINPUT_GAMEPAD_DPAD_UP},     {"DPAD_DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
    {"DPAD_LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"DPAD_RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
    {"START", XINPUT_GAMEPAD_START},         {"BACK", XINPUT_GAMEPAD_BACK},
    {"L_THUMB", XINPUT_GAMEPAD_LEFT_THUMB},  {"R_THUMB", XINPUT_GAMEPAD_RIGHT_THUMB},
    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER},    {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"A", XINPUT_GAMEPAD_A},                 {"B", XINPUT_GAMEPAD_B},
    {"X", XINPUT_GAMEPAD_X},                 {"Y", XINPUT_GAMEPAD_Y},
};

// Renders "LEFT_SHOULDER,START"-style text matching mad2config's own
// GetGamepadButtonMask parser (which accepts either short or long names --
// long names used here purely so the persisted config.cfg stays consistent
// with what a hand-editing user would expect from CLAUDE.md's own examples).
static std::string FormatGamepadMask(WORD mask) {
    struct LongName { WORD bit; const char* name; };
    static const LongName kLong[] = {
        {XINPUT_GAMEPAD_DPAD_UP, "DPAD_UP"},       {XINPUT_GAMEPAD_DPAD_DOWN, "DPAD_DOWN"},
        {XINPUT_GAMEPAD_DPAD_LEFT, "DPAD_LEFT"},   {XINPUT_GAMEPAD_DPAD_RIGHT, "DPAD_RIGHT"},
        {XINPUT_GAMEPAD_START, "START"},           {XINPUT_GAMEPAD_BACK, "BACK"},
        {XINPUT_GAMEPAD_LEFT_THUMB, "LEFT_THUMB"}, {XINPUT_GAMEPAD_RIGHT_THUMB, "RIGHT_THUMB"},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, "LEFT_SHOULDER"}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, "RIGHT_SHOULDER"},
        {XINPUT_GAMEPAD_A, "A"}, {XINPUT_GAMEPAD_B, "B"}, {XINPUT_GAMEPAD_X, "X"}, {XINPUT_GAMEPAD_Y, "Y"},
    };
    std::string out;
    for (auto& b : kLong) {
        if (mask & b.bit) {
            if (!out.empty()) out += ",";
            out += b.name;
        }
    }
    return out;
}

// ---------------------------------------------------------------------
// Persist an edited setting back to config.cfg and run its onChanged hook.
// ---------------------------------------------------------------------

static void PersistAndApply(const Mad2SettingInfo& info) {
    const auto& cfg = Mad2Config_Resolve();
    char buf[64];
    switch (info.type) {
        case MAD2SETTING_BOOL:
            if (cfg.SetString) cfg.SetString(info.configSection, info.configKey,
                                              (*static_cast<bool*>(info.valuePtr)) ? "true" : "false");
            break;
        case MAD2SETTING_INT:
            snprintf(buf, sizeof(buf), "%d", *static_cast<int*>(info.valuePtr));
            if (cfg.SetString) cfg.SetString(info.configSection, info.configKey, buf);
            break;
        case MAD2SETTING_FLOAT:
            snprintf(buf, sizeof(buf), "%g", *static_cast<float*>(info.valuePtr));
            if (cfg.SetString) cfg.SetString(info.configSection, info.configKey, buf);
            break;
        case MAD2SETTING_VKEY:
            snprintf(buf, sizeof(buf), "0x%02X", *static_cast<int*>(info.valuePtr));
            if (cfg.SetString) cfg.SetString(info.configSection, info.configKey, buf);
            break;
        case MAD2SETTING_GAMEPAD_MASK: {
            std::string combo = FormatGamepadMask(*static_cast<WORD*>(info.valuePtr));
            if (cfg.SetString) cfg.SetString(info.configSection, info.configKey, combo.c_str());
            break;
        }
        case MAD2SETTING_ACTION:
            break;  // nothing to persist -- onChanged IS the action
    }
    if (info.onChanged) info.onChanged(info.userdata);
}

// ---------------------------------------------------------------------
// UI: category list (mad2settings categories + the two fixed special
// tabs) on the left, selected category's content on the right.
// ---------------------------------------------------------------------

static std::string g_SelectedCategory;

static std::vector<std::string> BuildCategoryList() {
    std::vector<std::string> categories;
    const auto& settings = Mad2Settings_Resolve();
    if (settings.GetCount) {
        int count = settings.GetCount();
        for (int i = 0; i < count; ++i) {
            Mad2SettingInfo info{};
            if (!settings.GetInfo(i, &info)) continue;
            std::string cat = info.category;
            bool seen = false;
            for (auto& c : categories) {
                if (c == cat) { seen = true; break; }
            }
            if (!seen) categories.push_back(cat);
        }
    }
    categories.push_back("Chaos Effects");
    categories.push_back("Achievements");
    categories.push_back("Config");
    categories.push_back("Save Editor");
    categories.push_back("Live Cheats");
    if (g_SelectedCategory.empty() && !categories.empty()) g_SelectedCategory = categories.front();
    return categories;
}

static void RenderSettingRow(struct nk_context* ctx, const Mad2SettingInfo& info) {
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_label(ctx, info.label, NK_TEXT_LEFT);

    switch (info.type) {
        case MAD2SETTING_BOOL: {
            int checked = (*static_cast<bool*>(info.valuePtr)) ? 1 : 0;
            int before = checked;
            nk_checkbox_label(ctx, checked ? "On" : "Off", &checked);
            if (checked != before) {
                *static_cast<bool*>(info.valuePtr) = checked != 0;
                PersistAndApply(info);
            }
            break;
        }
        case MAD2SETTING_INT: {
            int val = *static_cast<int*>(info.valuePtr);
            int before = val;
            int minV = static_cast<int>(info.minValue), maxV = static_cast<int>(info.maxValue);
            if (minV == maxV) { minV = -1000000; maxV = 1000000; }  // unranged -- registrant didn't set a slider range
            nk_property_int(ctx, "#", minV, &val, maxV, 1, 1.0f);
            if (val != before) {
                *static_cast<int*>(info.valuePtr) = val;
                PersistAndApply(info);
            }
            break;
        }
        case MAD2SETTING_FLOAT: {
            float val = *static_cast<float*>(info.valuePtr);
            float before = val;
            float minV = info.minValue, maxV = info.maxValue;
            if (minV == maxV) { minV = -1000000.0f; maxV = 1000000.0f; }
            nk_property_float(ctx, "#", minV, &val, maxV, 0.01f, 0.001f);
            if (val != before) {
                *static_cast<float*>(info.valuePtr) = val;
                PersistAndApply(info);
            }
            break;
        }
        case MAD2SETTING_VKEY: {
            bool capturingThis = (g_CapturingValuePtr == info.valuePtr);
            std::string text = capturingThis ? "press a key... (ESC cancels)" : VkKeyName(*static_cast<int*>(info.valuePtr));
            nk_label(ctx, text.c_str(), NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 22, 1);
            if (nk_button_label(ctx, capturingThis ? "Cancel" : "Rebind")) {
                g_CapturingValuePtr = capturingThis ? nullptr : info.valuePtr;
            }
            break;
        }
        case MAD2SETTING_GAMEPAD_MASK: {
            WORD mask = *static_cast<WORD*>(info.valuePtr);
            WORD before = mask;
            nk_layout_row_dynamic(ctx, 22, 7);
            for (auto& b : kNamedButtons) {
                int checked = (mask & b.mask) ? 1 : 0;
                nk_checkbox_label(ctx, b.name, &checked);
                if (checked) mask |= b.mask; else mask &= ~b.mask;
            }
            if (mask != before) {
                *static_cast<WORD*>(info.valuePtr) = mask;
                PersistAndApply(info);
            }
            break;
        }
        case MAD2SETTING_ACTION: {
            if (nk_button_label(ctx, "Run")) PersistAndApply(info);
            break;
        }
    }
}

static void RenderSettingsCategory(struct nk_context* ctx, const std::string& category) {
    const auto& settings = Mad2Settings_Resolve();
    if (!settings.GetCount) {
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_label(ctx, "[Dependency] mad2settings.dll not available", NK_TEXT_LEFT);
        return;
    }
    int count = settings.GetCount();
    bool any = false;
    for (int i = 0; i < count; ++i) {
        Mad2SettingInfo info{};
        if (!settings.GetInfo(i, &info)) continue;
        if (category != info.category) continue;
        any = true;
        RenderSettingRow(ctx, info);
    }
    if (!any) {
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_label(ctx, "(nothing registered in this category)", NK_TEXT_LEFT);
    }
}

// ---------------------------------------------------------------------
// Raw Config: every key in config.cfg, not just what a mod explicitly
// registered with mad2settings.dll -- ports mad2launcher's iniconfig.go/
// ui_category.go behavior (a separate, independent implementation of the
// same directive format, since that one runs in a different process): a
// bool-looking value becomes a checkbox, an "@select(a,b,c)" directive
// line in the seeded comment becomes a cycle-button over those options, an
// "@indexed(Prefix1,Prefix2,...)" directive on a count key becomes a
// repeating Add/Remove group of <Prefix><N> fields, everything else is a
// free-text field. Mad2Config_GetEntryCount() is called once per frame
// this category is visible (refreshing the whole parse), not once per
// entry -- see mad2config_api.h's own comment on why.
// ---------------------------------------------------------------------

static std::string TrimStr(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::vector<std::string> SplitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (std::getline(iss, tok, ',')) {
        tok = TrimStr(tok);
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

struct ParsedComment {
    std::string helpText;
    std::vector<std::string> selectOptions;   // from an "@select(...)" directive line
    std::vector<std::string> indexedGroups;   // from an "@indexed(...)" directive line
};

static ParsedComment ParseRawComment(const char* raw) {
    ParsedComment out;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.rfind("@select(", 0) == 0 && !line.empty() && line.back() == ')') {
            out.selectOptions = SplitCsv(line.substr(8, line.size() - 9));
        } else if (line.rfind("@indexed(", 0) == 0 && !line.empty() && line.back() == ')') {
            out.indexedGroups = SplitCsv(line.substr(9, line.size() - 10));
        } else {
            if (!out.helpText.empty()) out.helpText += "\n";
            out.helpText += line;
        }
    }
    return out;
}

static bool LooksBool(const std::string& v) {
    std::string lower = v;
    for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return lower == "true" || lower == "false";
}

// Persistent text-edit buffers, keyed by "section\x01key" -- nk_edit_string
// needs a mutable buffer that survives across frames while being typed
// into; seeded from the entry's current value the first time it's
// rendered, never resynced from disk afterward (same "you're the only
// editor" assumption mad2launcher's own autosaving Entry widgets make).
struct RawEditBuffer {
    char data[256];
    int len;
};
static std::map<std::string, RawEditBuffer> g_RawEditBuffers;

static RawEditBuffer& GetOrSeedEditBuffer(const std::string& mapKey, const std::string& initial) {
    auto it = g_RawEditBuffers.find(mapKey);
    if (it != g_RawEditBuffers.end()) return it->second;
    RawEditBuffer buf{};
    size_t n = initial.size();
    if (n >= sizeof(buf.data)) n = sizeof(buf.data) - 1;
    memcpy(buf.data, initial.data(), n);
    buf.len = static_cast<int>(n);
    return g_RawEditBuffers.emplace(mapKey, buf).first->second;
}

static std::string g_SelectedRawSection;

// Accepts both the short names kNamedButtons renders (LB/RB/L_THUMB/
// R_THUMB) and the long names FormatGamepadMask/mad2config's own seeded
// defaults use (LEFT_SHOULDER/RIGHT_SHOULDER/LEFT_THUMB/RIGHT_THUMB), so a
// value written by either side parses back correctly regardless of which
// convention produced it.
static WORD ParseGamepadMaskValue(const std::string& value) {
    struct Alias {
        const char* name;
        WORD mask;
    };
    static const Alias kAliases[] = {
        {"DPAD_UP", XINPUT_GAMEPAD_DPAD_UP},         {"DPAD_DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
        {"DPAD_LEFT", XINPUT_GAMEPAD_DPAD_LEFT},     {"DPAD_RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
        {"START", XINPUT_GAMEPAD_START},             {"BACK", XINPUT_GAMEPAD_BACK},
        {"LEFT_THUMB", XINPUT_GAMEPAD_LEFT_THUMB},   {"L_THUMB", XINPUT_GAMEPAD_LEFT_THUMB},
        {"RIGHT_THUMB", XINPUT_GAMEPAD_RIGHT_THUMB}, {"R_THUMB", XINPUT_GAMEPAD_RIGHT_THUMB},
        {"LEFT_SHOULDER", XINPUT_GAMEPAD_LEFT_SHOULDER},   {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER},
        {"RIGHT_SHOULDER", XINPUT_GAMEPAD_RIGHT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
    };
    WORD mask = 0;
    std::istringstream iss(value);
    std::string tok;
    while (std::getline(iss, tok, ',')) {
        tok = TrimStr(tok);
        for (char& c : tok) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        for (auto& a : kAliases) {
            if (tok == a.name) {
                mask |= a.mask;
                break;
            }
        }
    }
    return mask;
}

static void RenderRawConfigEntry(struct nk_context* ctx, const Mad2ConfigEntry& e) {
    ParsedComment comment = ParseRawComment(e.comment);
    std::string mapKey = std::string(e.section) + "\x01" + e.key;
    const auto& cfg = Mad2Config_Resolve();

    nk_layout_row_dynamic(ctx, 24, 2);
    nk_label(ctx, e.key, NK_TEXT_LEFT);

    if (strcmp(e.typeHint, MAD2CONFIG_TYPEHINT_VKEY) == 0) {
        // Same press-a-key capture UX as a registered MAD2SETTING_VKEY
        // (RenderSettingRow) -- see HookEndScene's key-scan block, which
        // handles g_CapturingRawConfigKey the same way it handles
        // g_CapturingValuePtr.
        bool capturingThis = (g_CapturingRawConfigKey == mapKey);
        nk_label(ctx, capturingThis ? "press a key... (ESC cancels)" : e.value, NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_button_label(ctx, capturingThis ? "Cancel" : "Rebind")) {
            g_CapturingRawConfigKey = capturingThis ? "" : mapKey;
        }
    } else if (strcmp(e.typeHint, MAD2CONFIG_TYPEHINT_GAMEPADMASK) == 0) {
        WORD mask = ParseGamepadMaskValue(e.value);
        WORD before = mask;
        nk_layout_row_dynamic(ctx, 22, 7);
        for (auto& b : kNamedButtons) {
            int checked = (mask & b.mask) ? 1 : 0;
            nk_checkbox_label(ctx, b.name, &checked);
            if (checked)
                mask |= b.mask;
            else
                mask &= ~b.mask;
        }
        if (mask != before) {
            std::string combo = FormatGamepadMask(mask);
            if (cfg.SetString) cfg.SetString(e.section, e.key, combo.c_str());
        }
    } else if (!comment.indexedGroups.empty()) {
        int count = atoi(e.value);
        if (count < 0) count = 0;
        char label[64];
        snprintf(label, sizeof(label), "%d mapping(s)", count);
        nk_label(ctx, label, NK_TEXT_LEFT);

        nk_layout_row_dynamic(ctx, 22, 2);
        if (nk_button_label(ctx, "+ Add")) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", count + 1);
            if (cfg.SetString) cfg.SetString(e.section, e.key, buf);
        }
        if (count > 0 && nk_button_label(ctx, "- Remove Last")) {
            for (auto& prefix : comment.indexedGroups) {
                std::string k = prefix + std::to_string(count);
                g_RawEditBuffers.erase(std::string(e.section) + "\x01" + k);
            }
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", count - 1);
            if (cfg.SetString) cfg.SetString(e.section, e.key, buf);
        }

        for (int i = 1; i <= count; ++i) {
            for (auto& prefix : comment.indexedGroups) {
                std::string k = prefix + std::to_string(i);
                char current[MAD2CONFIG_MAX_VALUE] = {0};
                if (cfg.GetString) cfg.GetString(e.section, k.c_str(), "", "", current, sizeof(current));
                std::string subKey = std::string(e.section) + "\x01" + k;
                RawEditBuffer& buf = GetOrSeedEditBuffer(subKey, current);

                nk_layout_row_dynamic(ctx, 22, 2);
                nk_label(ctx, k.c_str(), NK_TEXT_LEFT);
                int before = buf.len;
                std::string beforeText(buf.data, buf.len);
                nk_edit_string(ctx, NK_EDIT_FIELD, buf.data, &buf.len, sizeof(buf.data), nk_filter_default);
                std::string afterText(buf.data, buf.len);
                if (buf.len != before || afterText != beforeText) {
                    if (cfg.SetString) cfg.SetString(e.section, k.c_str(), afterText.c_str());
                }
            }
        }
    } else if (!comment.selectOptions.empty()) {
        size_t curIdx = 0;
        for (size_t i = 0; i < comment.selectOptions.size(); ++i) {
            if (comment.selectOptions[i] == e.value) { curIdx = i; break; }
        }
        if (nk_button_label(ctx, e.value)) {
            size_t nextIdx = (curIdx + 1) % comment.selectOptions.size();
            if (cfg.SetString) cfg.SetString(e.section, e.key, comment.selectOptions[nextIdx].c_str());
        }
    } else if (LooksBool(e.value)) {
        bool isTrue = _stricmp(e.value, "true") == 0;
        int checked = isTrue ? 1 : 0;
        int before = checked;
        nk_checkbox_label(ctx, checked ? "true" : "false", &checked);
        if (checked != before) {
            if (cfg.SetString) cfg.SetString(e.section, e.key, checked ? "true" : "false");
        }
    } else {
        RawEditBuffer& buf = GetOrSeedEditBuffer(mapKey, e.value);
        std::string beforeText(buf.data, buf.len);
        int beforeLen = buf.len;
        nk_edit_string(ctx, NK_EDIT_FIELD, buf.data, &buf.len, sizeof(buf.data), nk_filter_default);
        std::string afterText(buf.data, buf.len);
        if (buf.len != beforeLen || afterText != beforeText) {
            if (cfg.SetString) cfg.SetString(e.section, e.key, afterText.c_str());
        }
    }

    if (!comment.helpText.empty()) {
        nk_layout_row_dynamic(ctx, 16, 1);
        nk_label_colored(ctx, comment.helpText.c_str(), NK_TEXT_LEFT, nk_rgb(160, 160, 160));
    }
}

// Two layers, like the outer window's own category|content split: a
// vertical list of config.cfg sections on the left (same nk_selectable_label
// style as the outer category list, not a horizontal tab bar), the
// selected section's keys on the right. availableHeight is the same
// live-resize-aware height BuildUI computed for its own outer split (see
// its own comment on nk_window_get_bounds) -- this inner split needs an
// explicit row height too, Nuklear groups don't inherit "fill remaining
// space" automatically.
static void RenderConfigCategory(struct nk_context* ctx, float availableHeight) {
    const auto& cfg = Mad2Config_Resolve();
    if (!cfg.GetEntryCount || !cfg.GetEntryInfo) {
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_label(ctx, "[Dependency] mad2config.dll not available (or missing the entry-enumeration exports)",
                  NK_TEXT_LEFT);
        return;
    }

    int count = cfg.GetEntryCount();  // refreshes the whole parse -- once per frame, not per entry

    std::vector<std::string> sections;
    std::vector<Mad2ConfigEntry> entries(count);
    for (int i = 0; i < count; ++i) {
        if (!cfg.GetEntryInfo(i, &entries[i])) continue;
        bool seen = false;
        for (auto& s : sections) {
            if (s == entries[i].section) { seen = true; break; }
        }
        if (!seen) sections.push_back(entries[i].section);
    }
    if (g_SelectedRawSection.empty() && !sections.empty()) g_SelectedRawSection = sections.front();

    float ratios[2] = {0.32f, 0.68f};
    nk_layout_row(ctx, NK_DYNAMIC, availableHeight, 2, ratios);

    if (nk_group_begin(ctx, "config_sections", NK_WINDOW_BORDER)) {
        nk_layout_row_dynamic(ctx, 24, 1);
        for (auto& s : sections) {
            int selected = (s == g_SelectedRawSection) ? 1 : 0;
            if (nk_selectable_label(ctx, s.c_str(), NK_TEXT_LEFT, &selected) && selected) {
                g_SelectedRawSection = s;
            }
        }
        nk_group_end(ctx);
    }

    if (nk_group_begin(ctx, "config_content", NK_WINDOW_BORDER)) {
        for (auto& e : entries) {
            if (g_SelectedRawSection == e.section) RenderRawConfigEntry(ctx, e);
        }
        nk_group_end(ctx);
    }
}

// ---------------------------------------------------------------------
// Save Editor: ports mad2launcher/saveeditor.go's cheat-toggle mechanism
// verbatim -- a fixed 13568-byte save struct, PC (little-endian) or Wii
// (big-endian, offsets from 0x128 on shifted -8 relative to PC's own extra
// 8 zero bytes at 0x120), CRC32-IEEE-then-XORed footer. Distinct from
// mad2cheatapply's per-cheat live toggles (RenderSettingsCategory's "Cheat
// Apply" category): this edits the SAVE FILE on disk directly, so a toggle
// here persists across a fresh load without needing mad2cheatapply's
// runtime pointer-chain write to run again. See saveeditor.go for the
// original Go implementation this is a straight C++ port of (same table,
// same offsets, same checksum math) -- kept independent rather than
// shared, same as this repo's other Go/C++ format-parser pairs.
// ---------------------------------------------------------------------

struct SaveCheatDef {
    const char* name;
    int pcOffset;
    const char* desc;
};
// Verbatim from mad2launcher/saveeditor.go's cheatsList (name/offset/desc
// only -- that file's own comment documents the provenance: confirmed by
// diffing two real PC saves, cross-checked against mad2cheatapply.cpp's
// live-memory offsets).
static const SaveCheatDef kSaveCheats[] = {
    {"Super Mangos", 0x128, "Increases standard health limits"},
    {"Head of the Game", 0x12c, "Enables bobble head scaling"},
    {"Infinite Sprint", 0x130, "Sprint without depleting energy"},
    {"Penguin Projectile", 0x134, "Allows throwing penguin projectiles"},
    {"Unlock Levels", 0x138, "Unlocks stage select levels"},
    {"Buddy Pointer", 0x140, "Shows compass/arrow helpers"},
    {"Debug Mode", 0x144, "Developer debug logs/mode"},
    {"Fast Mango", 0x148, "Increases gameplay speed"},
    {"Giant Frogs", 0x14c, "Scales frog targets larger"},
    {"Super Butt Bounce", 0x150, "Improves ground pound velocity"},
    {"Pepper at Will", 0x154, "Marty gets infinite pepper sprints"},
    {"Swim Backwards", 0x158, "Reverses swimming speed"},
    {"Invulnerable Alex", 0x15c, "Invincibility for Alex"},
    {"Invulnerable Gloria", 0x160, "Invincibility for Gloria"},
    {"Invulnerable Marty", 0x164, "Invincibility for Marty"},
    {"Invis New York", 0x168, "Turns character invisible"},
    {"Golf Bonus Hole", 0x28c, "Easier golf physics"},
    {"Stop Ball", 0x2a0, "Allows stopping ball instantly"},
};
static const size_t kSaveCheatCount = sizeof(kSaveCheats) / sizeof(kSaveCheats[0]);
static const long kSaveFileSize = 13568;
static const int kPcGapOffset = 0x120, kPcGapSize = 8;

static uint32_t Crc32Ieee(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool tableReady = false;
    if (!tableReady) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        tableReady = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// Matches saveeditor.go's saveChecksum exactly: the standard CRC32-IEEE of
// everything but the trailing 4 bytes, XORed with 0xFFFFFFFF once more
// (this format stores the raw CRC register state before the algorithm's
// own usual final inversion, not the everyday "CRC32 of this data" value).
static uint32_t SaveChecksum(const uint8_t* data, size_t len) { return Crc32Ieee(data, len - 4) ^ 0xFFFFFFFFu; }

static uint32_t ReadU32(const uint8_t* data, int off, bool isWii) {
    if (isWii) {
        return (static_cast<uint32_t>(data[off]) << 24) | (static_cast<uint32_t>(data[off + 1]) << 16) |
               (static_cast<uint32_t>(data[off + 2]) << 8) | data[off + 3];
    }
    return (static_cast<uint32_t>(data[off + 3]) << 24) | (static_cast<uint32_t>(data[off + 2]) << 16) |
           (static_cast<uint32_t>(data[off + 1]) << 8) | data[off];
}

static void WriteU32(uint8_t* data, int off, uint32_t val, bool isWii) {
    if (isWii) {
        data[off] = static_cast<uint8_t>(val >> 24);
        data[off + 1] = static_cast<uint8_t>(val >> 16);
        data[off + 2] = static_cast<uint8_t>(val >> 8);
        data[off + 3] = static_cast<uint8_t>(val);
    } else {
        data[off] = static_cast<uint8_t>(val);
        data[off + 1] = static_cast<uint8_t>(val >> 8);
        data[off + 2] = static_cast<uint8_t>(val >> 16);
        data[off + 3] = static_cast<uint8_t>(val >> 24);
    }
}

static bool DetectWiiSave(const uint8_t* data, size_t len) {
    if (len < 0x14) return false;
    uint32_t verBE = ReadU32(data, 0x10, true);
    uint32_t verLE = ReadU32(data, 0x10, false);
    return verBE == 17 && verLE != 17;
}

static int CheatFileOffset(int pcOffset, bool isWii) {
    if (isWii && pcOffset >= kPcGapOffset + kPcGapSize) return pcOffset - kPcGapSize;
    return pcOffset;
}

static std::string GetSaveDir() {
    char exePathA[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exePathA, MAX_PATH);
    std::string exeDir(exePathA);
    size_t slash = exeDir.find_last_of("\\/");
    if (slash != std::string::npos) exeDir.resize(slash + 1);

    std::string saveDirName = "saves";
    const auto& cfg = Mad2Config_Resolve();
    if (cfg.GetString) {
        char buf[MAX_PATH] = {0};
        if (cfg.GetString("SaveLoader", "SaveDir", "saves",
                           "Where saves and the graphics config live, relative to the game install directory.",
                           buf, sizeof(buf))) {
            saveDirName = buf;
        }
    }
    return exeDir + saveDirName + "\\";
}

static bool IsSaveSlotFilename(const std::string& name) {
    if (name.rfind("save_game", 0) != 0) return false;
    if (name.size() <= 9) return false;
    for (size_t i = 9; i < name.size(); ++i) {
        if (!isdigit(static_cast<unsigned char>(name[i]))) return false;
    }
    return true;
}

static std::vector<std::string> ListSaveSlots() {
    std::vector<std::string> slots;
    std::string pattern = GetSaveDir() + "*";
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return slots;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsSaveSlotFilename(fd.cFileName)) {
            slots.emplace_back(fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(slots.begin(), slots.end());
    return slots;
}

static std::string g_SelectedSaveSlot;
static std::vector<uint8_t> g_SaveFileBuffer;
static bool g_SaveFileIsWii = false;
static bool g_SaveFileLoadFailed = false;

static void LoadSelectedSaveFile() {
    g_SaveFileBuffer.clear();
    g_SaveFileLoadFailed = false;
    if (g_SelectedSaveSlot.empty()) return;

    std::string path = GetSaveDir() + g_SelectedSaveSlot;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        g_SaveFileLoadFailed = true;
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < kSaveFileSize) {
        g_SaveFileLoadFailed = true;
        fclose(f);
        return;
    }
    g_SaveFileBuffer.resize(static_cast<size_t>(size));
    fread(g_SaveFileBuffer.data(), 1, static_cast<size_t>(size), f);
    fclose(f);
    g_SaveFileIsWii = DetectWiiSave(g_SaveFileBuffer.data(), g_SaveFileBuffer.size());
    Log("Loaded save slot '%s' (%ld bytes, %s)\n", g_SelectedSaveSlot.c_str(), size,
        g_SaveFileIsWii ? "Wii/big-endian" : "PC/little-endian");
}

// Recomputes the CRC32 footer over the current g_SaveFileBuffer and writes
// the whole buffer back to the selected slot's file. Shared by every
// field-specific write function below (cheats, climb ability, monkey
// collectibles, amount spent, level coins) so the checksum/write-back step
// only exists once.
static void CommitSaveFileBuffer(const char* logContext) {
    if (g_SaveFileBuffer.empty() || g_SelectedSaveSlot.empty()) return;

    uint32_t checksum = SaveChecksum(g_SaveFileBuffer.data(), g_SaveFileBuffer.size());
    WriteU32(g_SaveFileBuffer.data(), static_cast<int>(g_SaveFileBuffer.size()) - 4, checksum, g_SaveFileIsWii);

    std::string path = GetSaveDir() + g_SelectedSaveSlot;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        Log("CommitSaveFileBuffer(%s): failed to open %s for write\n", logContext, path.c_str());
        return;
    }
    fwrite(g_SaveFileBuffer.data(), 1, g_SaveFileBuffer.size(), f);
    fclose(f);
}

// value is the exact 0 (Lock) / 1 (Unlock) / 2 (Enable) to write -- NOT a
// boolean. Conflating this to on/off used to silently make state 1
// unreachable from this editor entirely.
static void WriteSelectedSaveCheat(size_t cheatIndex, uint32_t value) {
    if (g_SaveFileBuffer.empty() || g_SelectedSaveSlot.empty() || cheatIndex >= kSaveCheatCount) return;
    int off = CheatFileOffset(kSaveCheats[cheatIndex].pcOffset, g_SaveFileIsWii);
    WriteU32(g_SaveFileBuffer.data(), off, value, g_SaveFileIsWii);
    CommitSaveFileBuffer("cheat");
    Log("Save slot '%s': %s = %u\n", g_SelectedSaveSlot.c_str(), kSaveCheats[cheatIndex].name, value);
}

// ---------------------------------------------------------------------
// Story-progress ability flags + coin economy -- ported verbatim from
// mad2launcher/saveeditor.go's own ReadClimbAbility/WriteClimbAbility,
// ReadMonkeyCollectible/WriteMonkeyCollectible, ReadAmountSpent/
// WriteAmountSpent, ReadLevelCoins/WriteLevelCoins (same offsets, same
// bundled-fields behavior for monkey collectibles -- see that file's own
// extensive comments for how each of these was discovered/verified; not
// re-derived here, just re-implemented in C++ against the same live
// g_SaveFileBuffer/CommitSaveFileBuffer this tab already uses for cheats).
// ---------------------------------------------------------------------

static const int kClimbAbilityOffset = 0x70;

// monkeyCollectibleByteFields' bundle -- see saveeditor.go's own comment on
// why the write path always touches all of it together, and why 0x68 is 2
// instead of 1 (never further investigated upstream either).
struct MonkeyByteField {
    int offset;
    uint8_t value;
};
static const MonkeyByteField kMonkeyByteFields[] = {
    {0x30, 1}, {0x48, 1}, {0x58, 1}, {0x68, 2}, {0x80, 1}, {0xA8, 1}, {0x3A0, 1}, {0x3A4, 1},
};
static const int kMonkeyUint32Offset = 0x18;
static const int kAmountSpentOffset = 0x0;

struct LevelCoinDef {
    const char* level;
    int offset;
    uint32_t maxCoins;  // 0 = no known cap (e.g. Unknown0x19C)
};
// Order matches the save file's own physical table layout (0x16C-0x1B0),
// same as saveeditor.go's LevelCoinLevels -- also the order this tab lists
// them in.
static const LevelCoinDef kLevelCoins[] = {
    {"IslandFever", 0x16C, 100},          {"Prepare2Launch", 0x170, 15},
    {"BraveNewWild", 0x174, 100},         {"Waterhole", 0x178, 120},
    {"penguins", 0x17C, 85},              {"FixThePlane", 0x180, 300},
    {"RitesOfPassage", 0x184, 100},       {"ConvoyChase", 0x188, 100},
    {"Wooing_Gloria", 0x18C, 100},        {"VolcanoRave", 0x190, 100},
    {"Morts_Adventure", 0x194, 100},      {"Dam_Busters", 0x198, 100},
    {"Unknown0x19C", 0x19C, 0},           {"Watercaves", 0x1A0, 100},
    {"DrMelman", 0x1A4, 52},              {"MartyRace", 0x1A8, 28},
    {"penguins2", 0x1AC, 15},             {"Prepare2Launch_Plane", 0x1B0, 85},
};
static const size_t kLevelCoinCount = sizeof(kLevelCoins) / sizeof(kLevelCoins[0]);

static bool ReadClimbAbilityLive() { return !g_SaveFileBuffer.empty() && g_SaveFileBuffer[kClimbAbilityOffset] != 0; }

static void WriteClimbAbilityLive(bool unlocked) {
    if (g_SaveFileBuffer.empty()) return;
    g_SaveFileBuffer[kClimbAbilityOffset] = unlocked ? 1 : 0;
    CommitSaveFileBuffer("climb ability");
    Log("Save slot '%s': Climb Ability = %d\n", g_SelectedSaveSlot.c_str(), unlocked);
}

static bool ReadMonkeyCollectibleLive() {
    if (g_SaveFileBuffer.empty()) return false;
    return ReadU32(g_SaveFileBuffer.data(), kMonkeyUint32Offset, g_SaveFileIsWii) == 0;
}

static void WriteMonkeyCollectibleLive(bool unlocked) {
    if (g_SaveFileBuffer.empty()) return;
    if (unlocked) {
        WriteU32(g_SaveFileBuffer.data(), kMonkeyUint32Offset, 0x00000000u, g_SaveFileIsWii);
        for (auto& f : kMonkeyByteFields) g_SaveFileBuffer[f.offset] = f.value;
    } else {
        WriteU32(g_SaveFileBuffer.data(), kMonkeyUint32Offset, 0xFFFFFFFFu, g_SaveFileIsWii);
        for (auto& f : kMonkeyByteFields) g_SaveFileBuffer[f.offset] = 0;
    }
    CommitSaveFileBuffer("monkey collectible");
    Log("Save slot '%s': Monkey Collectible = %d\n", g_SelectedSaveSlot.c_str(), unlocked);
}

static uint32_t ReadAmountSpentLive() {
    if (g_SaveFileBuffer.empty()) return 0;
    return ReadU32(g_SaveFileBuffer.data(), kAmountSpentOffset, g_SaveFileIsWii);
}

static void WriteAmountSpentLive(uint32_t value) {
    if (g_SaveFileBuffer.empty()) return;
    WriteU32(g_SaveFileBuffer.data(), kAmountSpentOffset, value, g_SaveFileIsWii);
    CommitSaveFileBuffer("amount spent");
    Log("Save slot '%s': Amount Spent = %u\n", g_SelectedSaveSlot.c_str(), value);
}

static uint32_t ReadLevelCoinsLive(size_t levelIndex) {
    if (g_SaveFileBuffer.empty() || levelIndex >= kLevelCoinCount) return 0;
    return ReadU32(g_SaveFileBuffer.data(), kLevelCoins[levelIndex].offset, g_SaveFileIsWii);
}

static void WriteLevelCoinsLive(size_t levelIndex, uint32_t value) {
    if (g_SaveFileBuffer.empty() || levelIndex >= kLevelCoinCount) return;
    WriteU32(g_SaveFileBuffer.data(), kLevelCoins[levelIndex].offset, value, g_SaveFileIsWii);
    CommitSaveFileBuffer("level coins");
    Log("Save slot '%s': %s coins = %u\n", g_SelectedSaveSlot.c_str(), kLevelCoins[levelIndex].level, value);
}

static void RenderSaveEditorCategory(struct nk_context* ctx) {
    std::vector<std::string> slots = ListSaveSlots();
    if (slots.empty()) {
        nk_layout_row_dynamic(ctx, 26, 1);
        char msg[256];
        snprintf(msg, sizeof(msg), "No save_game<N> files found under %s yet.", GetSaveDir().c_str());
        nk_label(ctx, msg, NK_TEXT_LEFT);
        return;
    }

    for (size_t i = 0; i < slots.size(); i += 4) {
        int cols = static_cast<int>(std::min<size_t>(4, slots.size() - i));
        nk_layout_row_dynamic(ctx, 24, cols);
        for (size_t j = i; j < i + static_cast<size_t>(cols); ++j) {
            if (slots[j] == g_SelectedSaveSlot) {
                nk_label_colored(ctx, slots[j].c_str(), NK_TEXT_CENTERED, nk_rgb(255, 215, 90));
            } else if (nk_button_label(ctx, slots[j].c_str())) {
                g_SelectedSaveSlot = slots[j];
                LoadSelectedSaveFile();
            }
        }
    }

    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);

    if (g_SelectedSaveSlot.empty()) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, "Pick a save slot above.", NK_TEXT_LEFT);
        return;
    }
    if (g_SaveFileLoadFailed || g_SaveFileBuffer.empty()) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, "Failed to load that save file (wrong size, or not a Madagascar 2 save).", NK_TEXT_LEFT);
        return;
    }

    char header[128];
    snprintf(header, sizeof(header), "%s -- %s", g_SelectedSaveSlot.c_str(),
             g_SaveFileIsWii ? "Wii (big-endian)" : "PC (little-endian)");
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label(ctx, header, NK_TEXT_LEFT);

    nk_layout_row_dynamic(ctx, 16, 1);
    nk_label_colored(ctx, "0 = Lock, 1 = Unlock, 2 = Enable", NK_TEXT_LEFT, nk_rgb(160, 160, 160));

    for (size_t i = 0; i < kSaveCheatCount; ++i) {
        uint32_t rawVal = ReadU32(g_SaveFileBuffer.data(), CheatFileOffset(kSaveCheats[i].pcOffset, g_SaveFileIsWii),
                                   g_SaveFileIsWii);
        int val = static_cast<int>(rawVal);
        int before = val;
        nk_layout_row_dynamic(ctx, 24, 2);
        nk_label(ctx, kSaveCheats[i].name, NK_TEXT_LEFT);
        nk_property_int(ctx, "#", 0, &val, 2, 1, 1.0f);
        if (val != before) WriteSelectedSaveCheat(i, static_cast<uint32_t>(val));
    }

    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label(ctx, "Story Progress", NK_TEXT_LEFT);

    {
        bool climb = ReadClimbAbilityLive();
        int checked = climb ? 1 : 0;
        int before = checked;
        nk_layout_row_dynamic(ctx, 24, 2);
        nk_label(ctx, "Climb Ability", NK_TEXT_LEFT);
        nk_checkbox_label(ctx, checked ? "Unlocked" : "Locked", &checked);
        if (checked != before) WriteClimbAbilityLive(checked != 0);
    }
    {
        bool monkeys = ReadMonkeyCollectibleLive();
        int checked = monkeys ? 1 : 0;
        int before = checked;
        nk_layout_row_dynamic(ctx, 24, 2);
        nk_label(ctx, "Monkey Collectibles", NK_TEXT_LEFT);
        nk_checkbox_label(ctx, checked ? "Unlocked" : "Locked", &checked);
        if (checked != before) WriteMonkeyCollectibleLive(checked != 0);
    }

    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label(ctx, "Coin Economy", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 16, 1);
    nk_label_colored(
        ctx,
        "Shop total = (sum of every level's coins below) - Amount Spent -- both operands are editable, not the total.",
        NK_TEXT_LEFT, nk_rgb(160, 160, 160));

    {
        uint32_t spent = ReadAmountSpentLive();
        int val = static_cast<int>(spent);
        int before = val;
        nk_layout_row_dynamic(ctx, 24, 2);
        nk_label(ctx, "Amount Spent", NK_TEXT_LEFT);
        nk_property_int(ctx, "#", 0, &val, 999999, 10, 1.0f);
        if (val != before) WriteAmountSpentLive(static_cast<uint32_t>(val));
    }

    for (size_t i = 0; i < kLevelCoinCount; ++i) {
        uint32_t coins = ReadLevelCoinsLive(i);
        int val = static_cast<int>(coins);
        int before = val;
        int maxCoins = kLevelCoins[i].maxCoins > 0 ? static_cast<int>(kLevelCoins[i].maxCoins) : 999;
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, kLevelCoins[i].level, NK_TEXT_LEFT);
        nk_property_int(ctx, "#", 0, &val, maxCoins, 1, 1.0f);
        if (val != before) WriteLevelCoinsLive(i, static_cast<uint32_t>(val));
    }
}

// ---------------------------------------------------------------------
// Live Cheats: replaces the now-removed mad2cheatapply.dll entirely --
// per-cheat 0/1/2 value editing directly against the running game's own
// memory, live, from any level. Ported from mad2climbprobe/src/
// climbprobe.cpp's proven ResolveCheatStructAddr/kCheats (that mod
// confirmed, live in-game, that this exact struct address is a persistent
// global safely writable from any level/screen, not just the title
// screen mad2cheatapply used to gate itself to -- see that file's own
// comments for the 9-capture/2-save/4-level verification). Each cheat's
// real value is 0 (Lock) / 1 (Unlock) / 2 (Enable), NOT a boolean -- the
// old mad2cheatapply/RenderSettingsCategory implementation only ever
// wrote 0 or 2, silently unable to express state 1 at all.
// ---------------------------------------------------------------------

struct LiveCheatDef {
    const char* name;
    int32_t offset;
    const char* desc;
};
// Name/offset/desc verbatim from the old mad2cheatapply.cpp's kCheats
// table (live-memory offsets from the resolved cheat struct -- NOT the
// same numbers as kSaveCheats' PCOffset above, which are save-FILE byte
// offsets; the two tables are unrelated despite naming the same 18 cheats).
static const LiveCheatDef kLiveCheats[] = {
    {"Infinite Sprint", -0x08, "Sprint without depleting energy"},
    {"Super Mangos", -0x60, "Increases standard health limits"},
    {"Head of the Game", -0x34, "Enables bobble head scaling"},
    {"Penguin Projectile", 0x24, "Allows throwing penguin projectiles"},
    {"Unlock Levels", 0x50, "Unlocks stage select levels"},
    {"Buddy Pointer", 0xA8, "Shows compass/arrow helpers"},
    {"Debug Mode", 0xD4, "Developer debug logs/mode"},
    {"Fast Mango", 0x100, "Increases gameplay speed"},
    {"Giant Frogs", 0x12C, "Scales frog targets larger"},
    {"Super Butt Bounce", 0x158, "Improves ground pound velocity"},
    {"Pepper at Will", 0x184, "Marty gets infinite pepper sprints"},
    {"Swim Backwards", 0x1B0, "Reverses swimming speed"},
    {"Invulnerable Alex", 0x1DC, "Invincibility for Alex"},
    {"Invulnerable Gloria", 0x208, "Invincibility for Gloria"},
    {"Invulnerable Marty", 0x234, "Invincibility for Marty"},
    {"Invis New York", 0x260, "Turns character invisible"},
    {"Golf Bonus Hole", 0xEEC, "Easier golf physics"},
    {"Stop Ball", 0xFC8, "Allows stopping ball instantly"},
};
static const size_t kLiveCheatCount = sizeof(kLiveCheats) / sizeof(kLiveCheats[0]);

static bool IsPlausiblePointer(uintptr_t p) { return p > 0x10000 && p < 0x7FFFFFFF; }

static uintptr_t FollowCheatChain(uintptr_t base, const uintptr_t* offsets, size_t count) {
    if (!IsPlausiblePointer(base)) return 0;
    uintptr_t addr = *reinterpret_cast<uint32_t*>(base);
    for (size_t i = 0; i < count; ++i) {
        if (!IsPlausiblePointer(addr)) return 0;
        addr = *reinterpret_cast<uint32_t*>(addr + offsets[i]);
    }
    return addr;
}

// AlchemyCommonLib.dll+0x535DC, then this chain -- identical to the old
// mad2cheatapply.cpp/mad2climbprobe.cpp's own ResolveCheatStructPtr.
static const uintptr_t kCheatStructOffsets[] = {0x38, 0x1C, 0x14, 0x240, 0xA0, 0x164};

static uintptr_t ResolveCheatStructAddr() {
    HMODULE alchemyCommon = GetModuleHandleA("AlchemyCommonLib.dll");
    if (!alchemyCommon) return 0;
    uintptr_t base = reinterpret_cast<uintptr_t>(alchemyCommon) + 0x000535DC;
    return FollowCheatChain(base, kCheatStructOffsets, sizeof(kCheatStructOffsets) / sizeof(kCheatStructOffsets[0]));
}

static void RenderLiveCheatsCategory(struct nk_context* ctx) {
    uintptr_t structAddr = ResolveCheatStructAddr();
    if (!IsPlausiblePointer(structAddr)) {
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_label(ctx, "Cheat struct not resolved yet (game may still be loading).", NK_TEXT_LEFT);
        return;
    }

    char header[64];
    snprintf(header, sizeof(header), "Cheat struct: 0x%08X", static_cast<unsigned int>(structAddr));
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label(ctx, header, NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 16, 1);
    nk_label_colored(ctx, "0 = Lock, 1 = Unlock, 2 = Enable", NK_TEXT_LEFT, nk_rgb(160, 160, 160));

    for (size_t i = 0; i < kLiveCheatCount; ++i) {
        int32_t* addr = reinterpret_cast<int32_t*>(structAddr + kLiveCheats[i].offset);
        int val = *addr;
        int before = val;

        nk_layout_row_dynamic(ctx, 24, 2);
        nk_label(ctx, kLiveCheats[i].name, NK_TEXT_LEFT);
        nk_property_int(ctx, "#", 0, &val, 2, 1, 1.0f);
        if (val != before) {
            *addr = val;
            Log("Live cheat '%s' = %d (offset %+d)\n", kLiveCheats[i].name, val, kLiveCheats[i].offset);
        }
    }
}

static void RenderChaosEffectsCategory(struct nk_context* ctx) {
    const auto& effects = Mad2Effects_Resolve();
    if (!effects.GetCount) {
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_label(ctx, "[Dependency] mad2effects.dll not available", NK_TEXT_LEFT);
        return;
    }
    int count = effects.GetCount();
    for (int i = 0; i < count; ++i) {
        Mad2EffectInfo info{};
        if (!effects.GetInfo(i, &info)) continue;
        nk_layout_row_dynamic(ctx, 26, 3);
        char label[192];
        snprintf(label, sizeof(label), "%s%s", info.displayName, info.isActive ? "  [ACTIVE]" : "");
        nk_label(ctx, label, NK_TEXT_LEFT);
        if (nk_button_label(ctx, "Trigger")) effects.Trigger(info.name, 0);
        if (info.hasDuration) {
            if (nk_button_label(ctx, "Clear")) effects.ClearByName(info.name);
        } else {
            nk_label(ctx, "", NK_TEXT_LEFT);
        }
    }
}

static void RenderAchievementsCategory(struct nk_context* ctx) {
    const auto& ach = Mad2Achievements_Resolve();
    if (!ach.GetCount) {
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_label(ctx, "[Dependency] mad2achievements.dll not available", NK_TEXT_LEFT);
        return;
    }
    int count = ach.GetCount();
    for (int i = 0; i < count; ++i) {
        Mad2AchievementInfo info{};
        if (!ach.GetInfo(i, &info)) continue;
        nk_layout_row_dynamic(ctx, 22, 2);
        char label[192];
        snprintf(label, sizeof(label), "%s -- %s", info.displayName, info.unlocked ? "Unlocked" : "Locked");
        nk_label(ctx, label, NK_TEXT_LEFT);
        if (!info.unlocked) {
            if (nk_button_label(ctx, "Force Unlock (debug)")) ach.Unlock(info.name);
        } else {
            nk_label(ctx, "", NK_TEXT_LEFT);
        }
        nk_layout_row_dynamic(ctx, 18, 1);
        nk_label(ctx, info.description, NK_TEXT_LEFT);
    }
}

static void BuildUI(struct nk_context* ctx, int screenW, int screenH) {
    float w = 820.0f, h = 560.0f;
    if (w > screenW - 40) w = static_cast<float>(screenW - 40);
    if (h > screenH - 40) h = static_cast<float>(screenH - 40);
    float x = (screenW - w) * 0.5f, y = (screenH - h) * 0.5f;

    if (nk_begin(ctx, "Mad2 Debug Menu", nk_rect(x, y, w, h),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        std::vector<std::string> categories = BuildCategoryList();

        // nk_begin's rect argument only seeds the window's INITIAL bounds --
        // once the user drags the NK_WINDOW_SCALABLE resize handle, Nuklear
        // tracks the window's own live size internally from then on, and our
        // own locally recomputed `h` above (clamped once against the
        // screen, at the top of this function) stops reflecting it. Read
        // the window's actual current bounds back so the split row -- and
        // therefore every page that scales off it, e.g. Live Cheats/Config
        // needing more room -- actually grows/shrinks with a live resize
        // instead of staying pinned to whatever the first frame computed.
        struct nk_rect bounds = nk_window_get_bounds(ctx);
        float contentHeight = bounds.h - 60.0f;
        if (contentHeight < 100.0f) contentHeight = 100.0f;

        float ratios[2] = {0.28f, 0.72f};
        nk_layout_row(ctx, NK_DYNAMIC, contentHeight, 2, ratios);

        if (nk_group_begin(ctx, "categories", NK_WINDOW_BORDER)) {
            nk_layout_row_dynamic(ctx, 26, 1);
            for (auto& cat : categories) {
                int selected = (cat == g_SelectedCategory) ? 1 : 0;
                if (nk_selectable_label(ctx, cat.c_str(), NK_TEXT_LEFT, &selected) && selected) {
                    g_SelectedCategory = cat;
                }
            }
            nk_group_end(ctx);
        }

        if (nk_group_begin(ctx, "content", NK_WINDOW_BORDER)) {
            if (g_SelectedCategory == "Chaos Effects") {
                RenderChaosEffectsCategory(ctx);
            } else if (g_SelectedCategory == "Achievements") {
                RenderAchievementsCategory(ctx);
            } else if (g_SelectedCategory == "Config") {
                RenderConfigCategory(ctx, contentHeight - 4.0f);
            } else if (g_SelectedCategory == "Save Editor") {
                RenderSaveEditorCategory(ctx);
            } else if (g_SelectedCategory == "Live Cheats") {
                RenderLiveCheatsCategory(ctx);
            } else {
                RenderSettingsCategory(ctx, g_SelectedCategory);
            }
            nk_group_end(ctx);
        }
    }
    nk_end(ctx);
}

// ---------------------------------------------------------------------
// D3D9 hook chain: Direct3DCreate9 (IAT) -> CreateDevice[16] -> EndScene[42],
// plus Reset[16] to rebuild Nuklear's device-dependent resources. Same
// technique as every other overlay mod -- see mad2effectshud/src/effectshud.cpp.
// ---------------------------------------------------------------------

static struct nk_context* g_NkCtx = nullptr;
static int g_ScreenW = 0, g_ScreenH = 0;
static bool g_NkReady = false;

typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
static EndScene_t RealEndScene = nullptr;
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static Reset_t RealReset = nullptr;

static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* This) {
    EnsureConfigLoaded();

    static bool wasToggleDown = false;
    bool toggleDown = g_Config.toggleVk != 0 && (GetAsyncKeyState(g_Config.toggleVk) & 0x8000) != 0;
    if (toggleDown && !wasToggleDown) SetMenuOpen(!g_MenuOpen.load());
    wasToggleDown = toggleDown;

    if (g_MenuOpen.load() && g_NkReady) {
        if (g_CapturingValuePtr) {
            int vk = ScanForFreshKeyPress();
            if (vk == VK_ESCAPE) {
                g_CapturingValuePtr = nullptr;
            } else if (vk != 0) {
                *static_cast<int*>(g_CapturingValuePtr) = vk;
                // Find the owning setting to persist/onChanged -- linear
                // scan is fine, this only runs once per successful rebind.
                const auto& settings = Mad2Settings_Resolve();
                if (settings.GetCount) {
                    int count = settings.GetCount();
                    for (int i = 0; i < count; ++i) {
                        Mad2SettingInfo info{};
                        if (settings.GetInfo(i, &info) && info.valuePtr == g_CapturingValuePtr) {
                            PersistAndApply(info);
                            break;
                        }
                    }
                }
                g_CapturingValuePtr = nullptr;
            }
        }
        if (!g_CapturingRawConfigKey.empty()) {
            int vk = ScanForFreshKeyPress();
            if (vk == VK_ESCAPE) {
                g_CapturingRawConfigKey.clear();
            } else if (vk != 0) {
                char buf[16];
                snprintf(buf, sizeof(buf), "0x%02X", vk);
                size_t sep = g_CapturingRawConfigKey.find('\x01');
                if (sep != std::string::npos) {
                    std::string section = g_CapturingRawConfigKey.substr(0, sep);
                    std::string key = g_CapturingRawConfigKey.substr(sep + 1);
                    const auto& cfg = Mad2Config_Resolve();
                    if (cfg.SetString) cfg.SetString(section.c_str(), key.c_str(), buf);
                }
                g_CapturingRawConfigKey.clear();
            }
        }

        DrainInputQueue();
        nk_input_end(g_NkCtx);
        BuildUI(g_NkCtx, g_ScreenW, g_ScreenH);
        nk_d3d9_render(NK_ANTI_ALIASING_ON);
        nk_input_begin(g_NkCtx);
    }

    return RealEndScene(This);
}

static HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_NkReady) nk_d3d9_release();
    HRESULT hr = RealReset(dev, pp);
    if (SUCCEEDED(hr) && g_NkReady && pp) {
        g_ScreenW = static_cast<int>(pp->BackBufferWidth);
        g_ScreenH = static_cast<int>(pp->BackBufferHeight);
        nk_d3d9_resize(g_ScreenW, g_ScreenH);
        Log("Rebuilt Nuklear D3D9 resources after Reset (%dx%d)\n", g_ScreenW, g_ScreenH);
    }
    return hr;
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

        void* prevReset = nullptr;
        Mad2HookUtil_PatchVTableSlot(*ppReturnedDeviceInterface, 16, reinterpret_cast<void*>(HookReset), &prevReset);
        RealReset = reinterpret_cast<Reset_t>(prevReset);

        InstallWndProcHook(hFocusWindow);

        g_ScreenW = pPresentationParameters ? static_cast<int>(pPresentationParameters->BackBufferWidth) : 1280;
        g_ScreenH = pPresentationParameters ? static_cast<int>(pPresentationParameters->BackBufferHeight) : 720;

        g_NkCtx = nk_d3d9_init(*ppReturnedDeviceInterface, g_ScreenW, g_ScreenH);
        struct nk_font_atlas* atlas = nullptr;
        nk_d3d9_font_stash_begin(&atlas);
        struct nk_font* defaultFont = nk_font_atlas_add_default(atlas, 15.0f, nullptr);
        nk_d3d9_font_stash_end();
        // nk_font_atlas_bake only auto-populates atlas->default_font (which
        // nk_d3d9_font_stash_end would otherwise use to set the style font)
        // when NO fonts were added before baking -- see nuklear.h's own
        // nk_font_atlas_bake, gated behind "if (!atlas->font_num)". Since we
        // just added one ourselves, that fallback never fires and
        // ctx->style.font is left null, which crashes the very first time
        // Nuklear tries to draw any text at all (confirmed live: the debug
        // menu crashed the game deterministically on every first open before
        // this fix). Set it explicitly instead of relying on that fallback.
        if (defaultFont) nk_style_set_font(g_NkCtx, &defaultFont->handle);
        nk_input_begin(g_NkCtx);
        g_NkReady = true;

        Log("Hooked IDirect3DDevice9::EndScene/Reset, initialized Nuklear (%dx%d, device=%p)\n", g_ScreenW, g_ScreenH,
            (void*)*ppReturnedDeviceInterface);
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

// ---------------------------------------------------------------------
// DirectInput8 mouse block: igDisplay.dll imports DINPUT8.dll directly
// (confirmed via `objdump -p` against the real game DLL, not assumed --
// alongside XINPUT1_3.dll, which mad2xinput.dll's own hook already
// covers). Neither mad2xinput's XInputGetState hook nor WndProc's own
// WM_MOUSE*/WM_KEY* swallow (see HookWndProc above) stops a
// DirectInput-polled device: IDirectInputDevice8::GetDeviceState/
// GetDeviceData read straight from the driver, bypassing the window
// message queue entirely -- if the game reads its mouse this way (very
// likely, given the import), that's exactly how mouse input was leaking
// through while the menu is open. Hooked the same shape as mad2xinput's
// own XInputGetState hook: IAT-hook the factory function, vtable-patch
// the two methods that actually deliver input on whichever device turns
// out to be the system mouse (GUID_SysMouse), chain to the real call
// whenever the menu isn't open. Vtable slot indices (CreateDevice=3,
// GetDeviceState=9, GetDeviceData=10) verified directly against this
// toolchain's own dinput.h interface declaration order, not recalled from
// memory -- IDirectInputDevice8A repeats the full inherited method list
// from IDirectInputDeviceA on up, so GetDeviceState/GetDeviceData sit at
// the same slot regardless of which derived interface actually got
// returned.
// ---------------------------------------------------------------------

// GUID_SysMouse's bytes, hardcoded from dinput.h's own DEFINE_GUID rather
// than linking dxguid.lib for one constant.
static const GUID kGuidSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

static bool IsGuidEqual(REFGUID a, const GUID& b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }

typedef HRESULT(STDMETHODCALLTYPE* DIGetDeviceState_t)(IDirectInputDevice8A*, DWORD, LPVOID);
static DIGetDeviceState_t RealMouseGetDeviceState = nullptr;

static HRESULT STDMETHODCALLTYPE HookMouseGetDeviceState(IDirectInputDevice8A* This, DWORD cbData, LPVOID lpvData) {
    if (g_MenuOpen.load()) {
        if (lpvData && cbData) memset(lpvData, 0, cbData);
        return DI_OK;
    }
    return RealMouseGetDeviceState(This, cbData, lpvData);
}

typedef HRESULT(STDMETHODCALLTYPE* DIGetDeviceData_t)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD,
                                                        DWORD);
static DIGetDeviceData_t RealMouseGetDeviceData = nullptr;

static HRESULT STDMETHODCALLTYPE HookMouseGetDeviceData(IDirectInputDevice8A* This, DWORD cbObjectData,
                                                          LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut,
                                                          DWORD flags) {
    if (g_MenuOpen.load()) {
        if (pdwInOut) *pdwInOut = 0;
        return DI_OK;
    }
    return RealMouseGetDeviceData(This, cbObjectData, rgdod, pdwInOut, flags);
}

typedef HRESULT(STDMETHODCALLTYPE* DICreateDevice_t)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
static DICreateDevice_t RealDICreateDevice = nullptr;

static HRESULT STDMETHODCALLTYPE HookDICreateDevice(IDirectInput8A* This, REFGUID rguid,
                                                      LPDIRECTINPUTDEVICE8A* lplpDevice, LPUNKNOWN pUnkOuter) {
    HRESULT hr = RealDICreateDevice(This, rguid, lplpDevice, pUnkOuter);
    if (SUCCEEDED(hr) && lplpDevice && *lplpDevice && IsGuidEqual(rguid, kGuidSysMouse) && !RealMouseGetDeviceState) {
        void* prevState = nullptr;
        Mad2HookUtil_PatchVTableSlot(*lplpDevice, 9, reinterpret_cast<void*>(HookMouseGetDeviceState), &prevState);
        RealMouseGetDeviceState = reinterpret_cast<DIGetDeviceState_t>(prevState);

        void* prevData = nullptr;
        Mad2HookUtil_PatchVTableSlot(*lplpDevice, 10, reinterpret_cast<void*>(HookMouseGetDeviceData), &prevData);
        RealMouseGetDeviceData = reinterpret_cast<DIGetDeviceData_t>(prevData);

        Log("Hooked DirectInput mouse device GetDeviceState/GetDeviceData (device=%p)\n", (void*)*lplpDevice);
    }
    return hr;
}

typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DirectInput8Create_t RealDirectInput8Create = nullptr;

static HRESULT WINAPI HookDirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid, LPVOID* ppvOut,
                                              LPUNKNOWN outer) {
    if (!RealDirectInput8Create) {
        Log("ERROR: RealDirectInput8Create is null\n");
        return E_FAIL;
    }
    HRESULT hr = RealDirectInput8Create(hinst, version, riid, ppvOut, outer);
    if (SUCCEEDED(hr) && ppvOut && *ppvOut && !RealDICreateDevice) {
        void* prevCreate = nullptr;
        Mad2HookUtil_PatchVTableSlot(*ppvOut, 3, reinterpret_cast<void*>(HookDICreateDevice), &prevCreate);
        RealDICreateDevice = reinterpret_cast<DICreateDevice_t>(prevCreate);
        Log("Hooked IDirectInput8::CreateDevice (vtable[3], dinput=%p)\n", *ppvOut);
    }
    return hr;
}

static void InstallDirectInputHook() {
    void* prev = nullptr;
    int n = Mad2HookUtil_PatchIatAllModules("dinput8.dll", "DirectInput8Create",
                                             reinterpret_cast<void*>(HookDirectInput8Create), &prev);
    if (prev) RealDirectInput8Create = reinterpret_cast<DirectInput8Create_t>(prev);
    Log("Patched DirectInput8Create in %d module(s) (chained real=%p)\n", n, (void*)RealDirectInput8Create);
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

    InstallDirectInputHook();
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            InitializeCriticalSection(&g_InputQueueLock);
            InstallHooks();
            break;
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}
