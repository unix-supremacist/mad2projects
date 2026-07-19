#pragma once

// Resolves the real (unhooked) Reg*/advapi32 functions used as fallbacks.
void InitRegistryRedirect();

// Scans every currently loaded module and redirects its registry-function
// imports (RegOpenKeyExA/W, RegCreateKeyExA/W, RegQueryValueExA/W,
// RegSetValueExA/W, RegCloseKey) to our hooks, so that opens of
// "Software\Activision\Madagascar 2" are served from a local config file
// instead of the real Windows registry. Chains onto whatever hook (if
// any) another mod already installed on the same import, same as
// InstallSaveRedirectHooks. Safe to call again later to pick up newly
// loaded modules.
void InstallRegistryRedirectHooks();
