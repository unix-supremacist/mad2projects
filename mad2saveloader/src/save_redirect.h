#pragma once

// Resolves the real (unhooked) CreateFileA/CreateFileW from kernel32.
void InitSaveRedirect();

// Scans every currently loaded module and redirects its CreateFileA/W
// imports to our hooks. Chains onto whatever was there before (the real
// function, or another mod's hook) so this composes with other
// file-hooking mods regardless of mad2/mods/ load order. Safe to call
// again later to pick up newly loaded modules.
void InstallSaveRedirectHooks();
