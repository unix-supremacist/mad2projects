// Shared config file mod (mad2/mods/mad2config.dll). Owns a single
// human-editable <exe dir>\config.cfg that other mods read their
// user-facing settings from -- see include/mad2config_api.h for the
// client-side contract and the "read or seed default" semantics every
// Get* follows.
//
// File format is a small hand-rolled "[Section]" + "Key=Value" + "#
// comment" format (generalizes the flat Key=Value parser that used to live
// in mad2saveloader/src/registry_redirect.cpp), not a full INI library --
// this is a MinGW-cross-compiled static-runtime target, so pulling in a
// third-party parser isn't worth it for something this small.
#include <windows.h>
#include <xinput.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../mad2sharedlog/mad2sharedlog.h"

// ---------------------------------------------------------------------
// Logging -- this mod's Log() wrapper writes into the shared logs\mad2.log file (see mad2sharedlog.h), tagged so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2config]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config file path + serialization primitives.
// ---------------------------------------------------------------------

static std::string g_ConfigPath;  // "<exe dir>config.cfg"
static CRITICAL_SECTION g_FileLock;

static void InitConfigPath() {
    char exePathA[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exePathA, MAX_PATH);
    std::string exeDir(exePathA);
    size_t slash = exeDir.find_last_of("\\/");
    if (slash != std::string::npos) exeDir.resize(slash + 1);
    g_ConfigPath = exeDir + "config.cfg";
}

static std::string Trim(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

static std::vector<std::string> ReadLines() {
    std::vector<std::string> lines;
    std::ifstream in(g_ConfigPath);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

static void WriteLines(const std::vector<std::string>& lines) {
    std::ofstream out(g_ConfigPath, std::ios::trunc);
    for (const auto& line : lines) out << line << "\n";
}

// Splits a (possibly multi-line, '\n'-separated) comment string into
// "# ..." lines ready to insert directly above a key=value line.
static std::vector<std::string> RenderComment(const char* comment) {
    std::vector<std::string> out;
    if (!comment || !*comment) return out;
    std::istringstream in(comment);
    std::string line;
    while (std::getline(in, line)) out.push_back("# " + line);
    return out;
}

// Finds the existing "section/key" line, if any. On success, *outLineIdx is
// the index of the "key=value" line and *outValue is the trimmed value.
static bool FindKey(const std::vector<std::string>& lines, const std::string& section, const std::string& key,
                     size_t* outLineIdx, std::string* outValue) {
    std::string currentSection;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string trimmed = Trim(lines[i]);
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            currentSection = trimmed.substr(1, trimmed.size() - 2);
            continue;
        }
        if (currentSection != section) continue;
        size_t eq = lines[i].find('=');
        if (eq == std::string::npos) continue;
        if (Trim(lines[i].substr(0, eq)) != key) continue;
        *outLineIdx = i;
        *outValue = Trim(lines[i].substr(eq + 1));
        return true;
    }
    return false;
}

// Finds the index just past the last line belonging to `section` (i.e.
// where a new key should be appended to stay grouped under it), or
// std::string::npos if that section doesn't exist yet.
static size_t FindSectionInsertPoint(const std::vector<std::string>& lines, const std::string& section) {
    bool inSection = false;
    size_t insertAt = std::string::npos;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string trimmed = Trim(lines[i]);
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            inSection = (trimmed.substr(1, trimmed.size() - 2) == section);
            if (inSection) insertAt = i + 1;
            continue;
        }
        if (inSection) insertAt = i + 1;
    }
    return insertAt;
}

// Core "read or seed default" primitive all the typed Get* wrap. Returns
// the existing string value, or writes `defaultValue` (with `comment`
// above it) and returns that.
static std::string GetOrSeedString(const std::string& section, const std::string& key,
                                    const std::string& defaultValue, const char* comment) {
    EnterCriticalSection(&g_FileLock);
    std::vector<std::string> lines = ReadLines();

    size_t lineIdx = 0;
    std::string value;
    if (FindKey(lines, section, key, &lineIdx, &value)) {
        LeaveCriticalSection(&g_FileLock);
        return value;
    }

    std::vector<std::string> commentLines = RenderComment(comment);
    std::vector<std::string> insert = commentLines;
    insert.push_back(key + "=" + defaultValue);

    size_t insertAt = FindSectionInsertPoint(lines, section);
    if (insertAt == std::string::npos) {
        if (!lines.empty() && !lines.back().empty()) lines.push_back("");
        lines.push_back("[" + section + "]");
        insertAt = lines.size();
    }
    lines.insert(lines.begin() + insertAt, insert.begin(), insert.end());
    WriteLines(lines);
    Log("Seeded [%s] %s=%s\n", section.c_str(), key.c_str(), defaultValue.c_str());

    LeaveCriticalSection(&g_FileLock);
    return defaultValue;
}

static void SetString(const std::string& section, const std::string& key, const std::string& value) {
    EnterCriticalSection(&g_FileLock);
    std::vector<std::string> lines = ReadLines();

    size_t lineIdx = 0;
    std::string existing;
    if (FindKey(lines, section, key, &lineIdx, &existing)) {
        lines[lineIdx] = key + "=" + value;
        WriteLines(lines);
        LeaveCriticalSection(&g_FileLock);
        return;
    }

    size_t insertAt = FindSectionInsertPoint(lines, section);
    if (insertAt == std::string::npos) {
        if (!lines.empty() && !lines.back().empty()) lines.push_back("");
        lines.push_back("[" + section + "]");
        insertAt = lines.size();
    }
    lines.insert(lines.begin() + insertAt, key + "=" + value);
    WriteLines(lines);
    LeaveCriticalSection(&g_FileLock);
}

// ---------------------------------------------------------------------
// Virtual-key name table (A-Z, 0-9, F1-F24, and the handful of named keys
// worth typing instead of hunting down a hex code).
// ---------------------------------------------------------------------

struct NamedKey {
    const char* name;
    int vk;
};
static const NamedKey kNamedKeys[] = {
    {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"ESCAPE", VK_ESCAPE}, {"TAB", VK_TAB}, {"BACKSPACE", VK_BACK},
};

static bool ParseVirtualKeyName(const std::string& name, int* outVk) {
    if (name.size() == 1) {
        char c = static_cast<char>(toupper(static_cast<unsigned char>(name[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            *outVk = c;  // VK codes for '0'-'9'/'A'-'Z' equal their ASCII values.
            return true;
        }
    }
    if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f')) {
        int n = atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) {
            *outVk = VK_F1 + (n - 1);
            return true;
        }
    }
    std::string upper = name;
    for (char& c : upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (upper == "ESC") upper = "ESCAPE";
    if (upper == "RETURN") upper = "ENTER";
    for (const auto& k : kNamedKeys) {
        if (upper == k.name) {
            *outVk = k.vk;
            return true;
        }
    }
    return false;
}

// Reverse of ParseVirtualKeyName, used to render a friendly, directly
// hand-editable name for a seeded default (e.g. "F9" instead of "0x78")
// wherever one exists; falls back to a hex literal otherwise.
static std::string VirtualKeyToName(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) {
        char buf[8];
        snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1);
        return buf;
    }
    for (const auto& k : kNamedKeys) {
        if (vk == k.vk) return k.name;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

static int ParseVirtualKey(const std::string& raw, int fallback) {
    std::string value = Trim(raw);
    if (value.empty()) return fallback;
    char* end = nullptr;
    long asNumber = strtol(value.c_str(), &end, 0);  // base 0: accepts "0x..." and plain decimal
    if (end && *end == '\0') return static_cast<int>(asNumber);

    std::string name = value;
    if (name.size() > 3 && (name.compare(0, 3, "VK_") == 0 || name.compare(0, 3, "vk_") == 0)) {
        name = name.substr(3);
    }
    int vk = 0;
    if (ParseVirtualKeyName(name, &vk)) return vk;
    return fallback;
}

// ---------------------------------------------------------------------
// XINPUT_GAMEPAD_* button name table.
// ---------------------------------------------------------------------

struct NamedButton {
    const char* name;
    WORD mask;
};
static const NamedButton kNamedButtons[] = {
    {"DPAD_UP", XINPUT_GAMEPAD_DPAD_UP},
    {"DPAD_DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
    {"DPAD_LEFT", XINPUT_GAMEPAD_DPAD_LEFT},
    {"DPAD_RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
    {"START", XINPUT_GAMEPAD_START},
    {"BACK", XINPUT_GAMEPAD_BACK},
    {"LEFT_THUMB", XINPUT_GAMEPAD_LEFT_THUMB},
    {"RIGHT_THUMB", XINPUT_GAMEPAD_RIGHT_THUMB},
    {"LEFT_SHOULDER", XINPUT_GAMEPAD_LEFT_SHOULDER},
    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER},
    {"RIGHT_SHOULDER", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"A", XINPUT_GAMEPAD_A},
    {"B", XINPUT_GAMEPAD_B},
    {"X", XINPUT_GAMEPAD_X},
    {"Y", XINPUT_GAMEPAD_Y},
};

static WORD ParseGamepadButtonMask(const std::string& raw) {
    WORD mask = 0;
    std::stringstream ss(raw);
    std::string token;
    while (std::getline(ss, token, ',')) {
        std::string name = Trim(token);
        for (char& c : name) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if (name.size() > 15 && name.compare(0, 15, "XINPUT_GAMEPAD_") == 0) name = name.substr(15);
        for (const auto& b : kNamedButtons) {
            if (name == b.name) {
                mask |= b.mask;
                break;
            }
        }
    }
    return mask;
}

// ---------------------------------------------------------------------
// Exported API -- see include/mad2config_api.h for the client contract.
// ---------------------------------------------------------------------

extern "C" __declspec(dllexport) BOOL WINAPI Mad2Config_GetString(const char* section, const char* key,
                                                                    const char* defaultValue, const char* comment,
                                                                    char* outBuf, DWORD outBufSize) {
    if (!section || !key || !outBuf || outBufSize == 0) return FALSE;
    std::string value = GetOrSeedString(section, key, defaultValue ? defaultValue : "", comment);
    DWORD copyLen = static_cast<DWORD>(value.size());
    if (copyLen >= outBufSize) copyLen = outBufSize - 1;
    memcpy(outBuf, value.data(), copyLen);
    outBuf[copyLen] = '\0';
    return TRUE;
}

extern "C" __declspec(dllexport) void WINAPI Mad2Config_SetString(const char* section, const char* key,
                                                                    const char* value) {
    if (!section || !key || !value) return;
    SetString(section, key, value);
}

extern "C" __declspec(dllexport) int WINAPI Mad2Config_GetInt(const char* section, const char* key,
                                                                int defaultValue, const char* comment) {
    if (!section || !key) return defaultValue;
    char defBuf[32];
    snprintf(defBuf, sizeof(defBuf), "%d", defaultValue);
    std::string value = GetOrSeedString(section, key, defBuf, comment);
    char* end = nullptr;
    long parsed = strtol(value.c_str(), &end, 0);
    if (!end || end == value.c_str()) return defaultValue;
    return static_cast<int>(parsed);
}

extern "C" __declspec(dllexport) float WINAPI Mad2Config_GetFloat(const char* section, const char* key,
                                                                    float defaultValue, const char* comment) {
    if (!section || !key) return defaultValue;
    char defBuf[32];
    snprintf(defBuf, sizeof(defBuf), "%g", defaultValue);
    std::string value = GetOrSeedString(section, key, defBuf, comment);
    char* end = nullptr;
    float parsed = strtof(value.c_str(), &end);
    if (!end || end == value.c_str()) return defaultValue;
    return parsed;
}

extern "C" __declspec(dllexport) BOOL WINAPI Mad2Config_GetBool(const char* section, const char* key,
                                                                  BOOL defaultValue, const char* comment) {
    if (!section || !key) return defaultValue;
    std::string value = GetOrSeedString(section, key, defaultValue ? "true" : "false", comment);
    for (char& c : value) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (value == "true" || value == "1" || value == "yes") return TRUE;
    if (value == "false" || value == "0" || value == "no") return FALSE;
    return defaultValue;
}

extern "C" __declspec(dllexport) int WINAPI Mad2Config_GetVirtualKey(const char* section, const char* key,
                                                                       int defaultVk, const char* comment) {
    if (!section || !key) return defaultVk;
    std::string value = GetOrSeedString(section, key, VirtualKeyToName(defaultVk), comment);
    return ParseVirtualKey(value, defaultVk);
}

extern "C" __declspec(dllexport) WORD WINAPI Mad2Config_GetGamepadButtonMask(const char* section, const char* key,
                                                                               const char* defaultCombo,
                                                                               const char* comment) {
    if (!section || !key) return defaultCombo ? ParseGamepadButtonMask(defaultCombo) : 0;
    std::string value = GetOrSeedString(section, key, defaultCombo ? defaultCombo : "", comment);
    return ParseGamepadButtonMask(value);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            InitializeCriticalSection(&g_FileLock);
            InitConfigPath();
            Log("mad2config loaded. Config path: %s\n", g_ConfigPath.c_str());
            break;
        case DLL_PROCESS_DETACH:
            DeleteCriticalSection(&g_FileLock);
            break;
        default:
            break;
    }
    return TRUE;
}
