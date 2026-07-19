#include <windows.h>

#include "loadlibrary_hook.h"
#include "log.h"
#include "registry_redirect.h"
#include "save_redirect.h"

// Loaded by mad2modloader from mad2/mods/*.dll, same as any other mod.
// Redirects the game's save files and graphics config -- normally
// written under the Wine prefix's fake per-user Documents/AppData
// folders -- to a "saves" folder next to Mad2.exe instead, so they live
// with the rest of the install and survive a prefix wipe. Also redirects
// its Software\Activision\Madagascar 2 registry key (just the Language
// value, for now) to a text file in that same folder, so language can be
// changed by editing a file instead of digging through the registry.
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);

            InitLog();
            Log("[mad2saveloader] Attaching...\n");

            InitSaveRedirect();
            InstallSaveRedirectHooks();

            InitRegistryRedirect();
            InstallRegistryRedirectHooks();

            InstallLoadLibraryHooks();

            Log("[mad2saveloader] Init complete.\n");
            break;
        }
        default:
            break;
    }
    return TRUE;
}
