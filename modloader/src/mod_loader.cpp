#include "mod_loader.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "log.h"

// mad2/mods/*.dll are loaded in sorted-filename order -- deliberately, not
// incidentally. D3D9-hooking mods each chain their IAT/vtable patches onto
// whatever was already there (see any of their file headers, e.g.
// mad2igttimer's), which means whichever mod's hook installs LAST ends up
// as the OUTERMOST hook: the game (and every earlier-installed mod's
// RealEndScene chain) reaches it FIRST, so it draws/captures first and
// every earlier-loaded mod's drawing happens later, safely on top of
// whatever the later-loaded mod already did.
//
// mad2graphicseffectmod's effects pipeline destructively captures and
// redraws the ENTIRE backbuffer, so any HUD-drawing mod loaded AFTER it
// gets its drawing captured and transformed right along with everything
// else -- it needs to be the outermost hook (load LAST) for every other
// overlay mod to be safe. Sorting alphabetically and naming its deployed
// DLL "zz_mad2graphicseffectmod.dll" (see its CMakeLists.txt) is what
// guarantees that, deterministically, instead of leaving it to whatever
// order FindFirstFileA/FindNextFileA happen to return (not sorted by the
// Windows API's own contract, even though it was observed to be alphabetical
// in this environment before this fix -- which is exactly what let mods
// alphabetically after "mad2graphicseffectmod" get silently clobbered).
static bool CaseInsensitiveLess(const std::string& a, const std::string& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = static_cast<unsigned char>(tolower(a[i]));
        unsigned char cb = static_cast<unsigned char>(tolower(b[i]));
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

void LoadMods() {
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir.resize(slash + 1);

    std::string modsDir = dir + "mods\\";
    std::string searchPattern = modsDir + "*.dll";

    std::vector<std::string> fileNames;
    WIN32_FIND_DATAA findData;
    HANDLE find = FindFirstFileA(searchPattern.c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE) {
        Log("[modloader] No mod DLLs found in %s\n", modsDir.c_str());
        return;
    }
    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        fileNames.emplace_back(findData.cFileName);
    } while (FindNextFileA(find, &findData));
    FindClose(find);

    std::sort(fileNames.begin(), fileNames.end(), CaseInsensitiveLess);

    for (const auto& fileName : fileNames) {
        std::string modPath = modsDir + fileName;
        HMODULE mod = LoadLibraryA(modPath.c_str());
        if (mod) {
            Log("[modloader] Loaded mod DLL: %s\n", fileName.c_str());
        } else {
            Log("[modloader] Failed to load mod DLL %s (err=%lu)\n", fileName.c_str(), GetLastError());
        }
    }
}
