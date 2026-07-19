#include <windows.h>

#include "log.h"
#include "mod_loader.h"
#include "version_proxy.h"

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);

            InitLog();
            Log("[modloader] Attaching to process...\n");

            // Must happen first: our exported version.dll functions need
            // the real DLL resolved before anything can call them.
            InitVersionProxy();

            LoadMods();

            Log("[modloader] Init complete.\n");
            break;
        }
        case DLL_PROCESS_DETACH:
            Log("[modloader] Detaching.\n");
            break;
        default:
            break;
    }
    return TRUE;
}
