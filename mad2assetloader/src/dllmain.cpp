#include <windows.h>

#include "file_redirect.h"
#include "loadlibrary_hook.h"
#include "log.h"

// Loaded by mad2modloader from mad2/mods/*.dll, same as any other mod.
// Split out from the core version.dll shim so the Content/Streams asset
// redirection (see file_redirect.cpp) can be disabled or swapped
// independently of the base mod loader.
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);

            InitLog();
            Log("[mad2assetloader] Attaching...\n");

            InitFileRedirect();
            InstallFileRedirectHooks();
            InstallFileRedirectLoadLibraryHooks();

            Log("[mad2assetloader] Init complete.\n");
            break;
        }
        default:
            break;
    }
    return TRUE;
}
